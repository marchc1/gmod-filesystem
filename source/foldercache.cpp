#include "foldercache_internal.h"

#include <algorithm>
#include <cstring>

// FOLDERCACHE_STANDALONE builds the cache without tier0/tier1, for testing a backend on its own
#ifdef FOLDERCACHE_STANDALONE
#include <cstdlib>

static bool IsTurnedOff()
{
	return getenv( "FOLDERCACHE_DISABLE" ) != nullptr;
}

static bool IsFindTurnedOff()
{
	return getenv( "FOLDERCACHE_NOFIND" ) != nullptr;
}

static bool IsStatTurnedOff()
{
	return getenv( "FOLDERCACHE_NOSTAT" ) != nullptr;
}
#else
#include "tier0/icommandline.h"
#include "tier1/convar.h"

static ConVar filesystem_foldercache( "filesystem_foldercache", "1", 0, "Keep listings of loose search path folders in memory to skip lookups of missing files" );

static bool IsTurnedOff()
{
	static const bool s_bCommandLine = CommandLine()->FindParm( "-nofoldercache" ) != 0;
	return s_bCommandLine || !filesystem_foldercache.GetBool();
}

static ConVar filesystem_findcache( "filesystem_findcache", "1", 0, "Answer FindFirst on loose folders from listings kept in memory" );

static bool IsFindTurnedOff()
{
	return !filesystem_findcache.GetBool();
}

static ConVar filesystem_statcache( "filesystem_statcache", "1", 0, "Answer stat on listed files from what the folder listing said" );

static bool IsStatTurnedOff()
{
	return !filesystem_statcache.GetBool();
}
#endif

namespace FolderCache::Detail
{
	std::shared_mutex g_Lock;
	std::vector<Root *> g_Roots;
}

using namespace FolderCache::Detail;

namespace
{
	enum class Result
	{
		Missing,
		Present,
		NeedList,
		Unknown,
	};

	std::vector<std::unique_ptr<Root>> g_AllRoots; // dropped ones stay alive, a backend may still be finishing with them
	FolderCache::ChangeFn g_pChangeCallback = nullptr;
	bool g_bShutdown = false;
}

bool FolderCache::Detail::IsDisabled()
{
	return IsTurnedOff();
}

//-----------------------------------------------------------------------------
// Paths
//-----------------------------------------------------------------------------
static bool IsSimpleRelative( std::string_view rel )
{
	if ( rel.empty() )
		return false;

	size_t pos = 0;
	while ( true )
	{
		size_t sep = rel.find( kSep, pos );
		if ( !IsSimpleComponent( rel.substr( pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos ) ) )
			return false;
		if ( sep == std::string_view::npos )
			return true;
		pos = sep + 1;
	}
}

// Longest match, roots behind links sit inside other roots
static Root *FindRoot( std::string_view path, bool bAllowEqual )
{
	Root *best = nullptr;
	for ( Root *root : g_Roots )
	{
		const std::string &rp = root->m_Path;
		const bool bLongEnough = bAllowEqual ? path.size() >= rp.size() : path.size() > rp.size();
		if ( bLongEnough && memcmp( path.data(), rp.data(), rp.size() ) == 0 && ( !best || rp.size() > best->m_Path.size() ) )
			best = root;
	}
	return best;
}

//-----------------------------------------------------------------------------
// Tree
//-----------------------------------------------------------------------------
// The folder rel is in, rel itself and everything under it (a folder that went away or got renamed)
static void DropFindListings( Root &root, std::string_view rel )
{
	++root.m_nChangeSeq;

	auto &listings = root.m_FindListings;
	if ( listings.empty() )
		return;

	if ( rel.empty() )
	{
		listings.clear();
		return;
	}

	const size_t sep = rel.rfind( kSep );
	auto parent = listings.find( sep == std::string_view::npos ? std::string_view() : rel.substr( 0, sep ) );
	if ( parent != listings.end() )
		listings.erase( parent );

	for ( auto it = listings.lower_bound( rel ); it != listings.end() && it->first.compare( 0, rel.size(), rel ) == 0; )
	{
		if ( it->first.size() == rel.size() || it->first[rel.size()] == kSep )
			it = listings.erase( it );
		else
			++it;
	}
}

static void NotifyChanged( Root &root, std::string_view rel )
{
	DropFindListings( root, rel );

	if ( !g_pChangeCallback )
		return;

	char buf[kMaxPath * 2];
	if ( root.m_Path.size() + rel.size() >= sizeof( buf ) )
	{
		// Too long to say exactly, the whole root it is
		g_pChangeCallback( root.m_Path.c_str(), root.m_Path.size() );
		return;
	}

	memcpy( buf, root.m_Path.data(), root.m_Path.size() );
	memcpy( buf + root.m_Path.size(), rel.data(), rel.size() );
	buf[root.m_Path.size() + rel.size()] = '\0';
	g_pChangeCallback( buf, root.m_Path.size() + rel.size() );
}

void FolderCache::Detail::ClearNode( Node &node )
{
	node.m_bListed = false;
	node.m_nUnsure = 0;
	node.m_Names.clear();
	node.m_Folded.clear();
	node.m_Data.clear();
}

void FolderCache::Detail::ResetTree( Root &root )
{
	ClearNode( root.m_Tree );
	root.m_Tree.m_Children.clear();
	NotifyChanged( root, {} );
}

void FolderCache::Detail::AddEntry( Node &node, std::string_view name, std::string_view folded, unsigned char flags )
{
	auto add = [flags]( NameMap &map, std::string_view key ) {
		auto it = map.find( key );
		if ( it != map.end() )
			it->second |= flags | kFlagUnsure; // two names land on the same key
		else
			map.emplace( std::string( key ), flags );
	};

	if constexpr ( kExactFolders )
	{
		add( node.m_Names, name );

		// Case variants of one name are normal here, the filesystem picks one. Present either way
		auto it = node.m_Folded.find( folded );
		if ( it != node.m_Folded.end() )
			it->second |= flags;
		else
			node.m_Folded.emplace( std::string( folded ), flags );
	}
	else
	{
		add( node.m_Names, folded );
	}
}

// Folder part of a lookup
static const unsigned char *FindFolderEntry( const Node &node, std::string_view comp )
{
	auto it = node.m_Names.find( comp );
	return it == node.m_Names.end() ? nullptr : &it->second;
}

// Last part of a lookup
static const unsigned char *FindFinalEntry( const Node &node, std::string_view comp )
{
	if constexpr ( !kExactFolders )
		return FindFolderEntry( node, comp );

	// The exact open or the case-insensitive retry, either one finding it counts
	char buf[kMaxPath];
	if ( comp.size() >= sizeof( buf ) )
		return nullptr;

	for ( size_t i = 0; i < comp.size(); ++i )
	{
		char c = comp[i];
		buf[i] = ( c >= 'A' && c <= 'Z' ) ? (char)( c + 'a' - 'A' ) : c;
	}

	auto it = node.m_Folded.find( std::string_view( buf, comp.size() ) );
	return it == node.m_Folded.end() ? nullptr : &it->second;
}

static Result Walk( Root &root, std::string_view rel, bool bAllowList )
{
	Node *node = &root.m_Tree;
	size_t pos = 0;
	while ( true )
	{
		size_t sep = rel.find( kSep, pos );
		std::string_view comp = rel.substr( pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos );

		if ( !node->m_bListed )
		{
			if ( !bAllowList )
				return Result::NeedList;
			if ( !ListNode( root, *node, rel.substr( 0, pos ? pos - 1 : 0 ) ) )
				return Result::Unknown;
		}

		if ( sep == std::string_view::npos )
			return FindFinalEntry( *node, comp ) ? Result::Present : Result::Missing;

		const unsigned char *pFlags = FindFolderEntry( *node, comp );
		if ( !pFlags )
			return Result::Missing;

		if ( *pFlags & kFlagUnsure )
			return Result::Unknown;

		// Going through a file
		if ( !( *pFlags & kFlagDir ) )
			return Result::Missing;

		auto child = node->m_Children.find( comp );
		if ( child == node->m_Children.end() )
		{
			if ( !bAllowList )
				return Result::NeedList;
			child = node->m_Children.emplace( std::string( comp ), std::make_unique<Node>() ).first;
		}

		node = child->second.get();
		pos = sep + 1;
	}
}

// rel is a folder under root, empty = the root itself
static FolderCache::DirState WalkDir( Root &root, std::string_view rel, Node **ppNode )
{
	Node *node = &root.m_Tree;
	size_t pos = 0;
	while ( pos < rel.size() )
	{
		size_t sep = rel.find( kSep, pos );
		std::string_view comp = rel.substr( pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos );

		if ( !node->m_bListed && !ListNode( root, *node, rel.substr( 0, pos ? pos - 1 : 0 ) ) )
			return FolderCache::DirState::Unknown;

		const unsigned char *pFlags = FindFolderEntry( *node, comp );
		if ( !pFlags )
			return FolderCache::DirState::Missing;
		if ( *pFlags & kFlagUnsure )
			return FolderCache::DirState::Unknown;
		if ( !( *pFlags & kFlagDir ) )
			return FolderCache::DirState::Missing;

		auto child = node->m_Children.find( comp );
		if ( child == node->m_Children.end() )
			child = node->m_Children.emplace( std::string( comp ), std::make_unique<Node>() ).first;

		node = child->second.get();
		pos = sep == std::string_view::npos ? rel.size() : sep + 1;
	}

	if ( !node->m_bListed && !ListNode( root, *node, rel ) )
		return FolderCache::DirState::Unknown;

	*ppNode = node;
	return FolderCache::DirState::Listed;
}

static void MarkUnsure( NameMap &map, std::string_view key )
{
	auto it = map.find( key );
	if ( it != map.end() )
		it->second |= kFlagUnsure;
	else
		map.emplace( std::string( key ), kFlagUnsure );
}

void FolderCache::Detail::MarkChanged( Root &root, std::string_view rel )
{
	while ( !rel.empty() && rel.back() == kSep )
		rel.remove_suffix( 1 );

	if ( rel.empty() )
	{
		ResetTree( root );
		return;
	}

	NotifyChanged( root, rel );

	Node *node = &root.m_Tree;
	size_t pos = 0;
	while ( true )
	{
		size_t sep = rel.find( kSep, pos );
		if ( sep == std::string_view::npos )
		{
			std::string_view last = rel.substr( pos );
			node->m_Data.erase( last );
			if ( node->m_bListed )
			{
				// Could exist now or not, the OS decides until we list again
				MarkUnsure( node->m_Names, last );
				if constexpr ( kExactFolders )
				{
					std::string folded;
					FoldName( last, folded );
					MarkUnsure( node->m_Folded, folded );
				}

				if ( ++node->m_nUnsure > kMaxUnsurePerNode )
					ClearNode( *node );
			}

			auto child = node->m_Children.find( last );
			if ( child != node->m_Children.end() )
				node->m_Children.erase( child );
			return;
		}

		auto child = node->m_Children.find( rel.substr( pos, sep - pos ) );
		if ( child == node->m_Children.end() )
			return;

		node = child->second.get();
		pos = sep + 1;
	}
}

// Exclusive lock held. The listed folder rel lives in, nullptr if we never listed it
static Node *FindParentNode( Root &root, std::string_view rel, std::string_view &last )
{
	Node *node = &root.m_Tree;
	size_t pos = 0;
	while ( true )
	{
		size_t sep = rel.find( kSep, pos );
		if ( sep == std::string_view::npos )
		{
			last = rel.substr( pos );
			return node->m_bListed ? node : nullptr;
		}

		auto child = node->m_Children.find( rel.substr( pos, sep - pos ) );
		if ( child == node->m_Children.end() )
			return nullptr;

		node = child->second.get();
		pos = sep + 1;
	}
}

static void TrimSeparators( std::string_view &rel )
{
	while ( !rel.empty() && rel.back() == kSep )
		rel.remove_suffix( 1 );
}

static void ClearDataBelow( Node &node )
{
	node.m_Data.clear();
	for ( auto &child : node.m_Children )
		ClearDataBelow( *child.second );
}

void FolderCache::Detail::ClearAllData( Root &root )
{
	ClearDataBelow( root.m_Tree );
}

void FolderCache::Detail::MarkDataChanged( Root &root, std::string_view rel )
{
	TrimSeparators( rel );
	if ( rel.empty() )
		return;

	std::string_view last;
	if ( Node *node = FindParentNode( root, rel, last ) )
		node->m_Data.erase( last );
}

void FolderCache::Detail::MarkRemoved( Root &root, std::string_view rel )
{
	TrimSeparators( rel );
	if ( rel.empty() )
	{
		ResetTree( root );
		return;
	}

	NotifyChanged( root, rel );

	std::string_view last;
	Node *node = FindParentNode( root, rel, last );
	if ( !node )
		return;

	node->m_Data.erase( last );

	auto child = node->m_Children.find( last );
	if ( child != node->m_Children.end() )
		node->m_Children.erase( child );

	auto it = node->m_Names.find( last );
	if ( it == node->m_Names.end() )
		return;

	if constexpr ( kExactFolders )
	{
		node->m_Names.erase( it );

		// Whatever else still folds to the same key keeps it alive
		std::string folded, other;
		FoldName( last, folded );
		unsigned char flags = 0;
		bool bAny = false;
		for ( auto &entry : node->m_Names )
		{
			FoldName( entry.first, other );
			if ( other == folded )
			{
				flags |= entry.second;
				bAny = true;
			}
		}

		if ( bAny )
			node->m_Folded[folded] = flags;
		else
			node->m_Folded.erase( folded );
	}
	else
	{
		// The key might stand for more than one real name, then the OS decides
		if ( it->second & kFlagUnsure )
			return;
		node->m_Names.erase( it );
	}
}

void FolderCache::Detail::MarkAdded( Root &root, std::string_view rel, unsigned char flags )
{
	TrimSeparators( rel );
	if ( rel.empty() )
	{
		ResetTree( root );
		return;
	}

	NotifyChanged( root, rel );

	std::string_view last;
	Node *node = FindParentNode( root, rel, last );
	if ( !node )
		return;

	node->m_Data.erase( last );

	// Whatever we knew about it as a folder is stale
	auto child = node->m_Children.find( last );
	if ( child != node->m_Children.end() )
		node->m_Children.erase( child );

	if constexpr ( kExactFolders )
	{
		node->m_Names[std::string( last )] = flags;

		std::string folded;
		FoldName( last, folded );
		node->m_Folded[folded] |= flags;
	}
	else
	{
		std::string folded;
		FoldName( last, folded );
		auto it = node->m_Names.find( folded );
		if ( it != node->m_Names.end() )
			it->second |= flags | kFlagUnsure;
		else
			node->m_Names.emplace( std::move( folded ), flags );
	}
}

void FolderCache::Detail::DropRoot( Root &root )
{
	root.m_bActive = false;
	ResetTree( root );

	auto it = std::find( g_Roots.begin(), g_Roots.end(), &root );
	if ( it != g_Roots.end() )
		g_Roots.erase( it );
}

// Something at absPath changed, roots behind it might watch a stale link target now
void FolderCache::Detail::RetireRootsUnder( const std::string &absPath )
{
	for ( size_t i = 0; i < g_Roots.size(); )
	{
		Root *inner = g_Roots[i];
		const std::string &ip = inner->m_Path;
		if ( ip.size() > absPath.size() && ip.compare( 0, absPath.size(), absPath ) == 0 && ip[absPath.size()] == kSep )
		{
			inner->m_bActive = false;
			ResetTree( *inner );
			RetireRoot( *inner );
			g_Roots.erase( g_Roots.begin() + i );
			continue;
		}
		++i;
	}
}

//-----------------------------------------------------------------------------
// API
//-----------------------------------------------------------------------------
void FolderCache::AddRoot( const char *pAbsoluteDir )
{
	if ( !pAbsoluteDir || IsDisabled() || g_bShutdown )
		return;

	char buf[kMaxPath];
	int len = NormalizePath( pAbsoluteDir, buf, sizeof( buf ) - 1 );
	if ( len <= 0 )
		return;

	if ( buf[len - 1] != kSep )
	{
		buf[len++] = kSep;
		buf[len] = '\0';
	}

	std::string path( buf, len );
	if ( !IsWatchablePath( path ) )
		return;

	// An outer watch doesn't see into link targets, a path behind one gets its own
	{
		std::shared_lock<std::shared_mutex> lock( g_Lock );
		for ( Root *root : g_Roots )
		{
			if ( path.compare( 0, root->m_Path.size(), root->m_Path ) == 0 && !HasLinkBetween( root->m_Path, path ) )
				return;
		}
	}

	auto owned = std::make_unique<Root>();
	Root *root = owned.get();
	root->m_Path = path;
	if ( !PrepareRoot( *root ) )
		return;

	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );

		// A root that contains older ones replaces them, unless they're behind a link
		for ( size_t i = 0; i < g_Roots.size(); )
		{
			Root *old = g_Roots[i];
			if ( old->m_Path.compare( 0, path.size(), path ) == 0 && !HasLinkBetween( path, old->m_Path ) )
			{
				old->m_bActive = false;
				ResetTree( *old );
				RetireRoot( *old );
				g_Roots.erase( g_Roots.begin() + i );
				continue;
			}
			++i;
		}

		g_Roots.push_back( root );
		g_AllRoots.push_back( std::move( owned ) );
	}

	ActivateRoot( *root );
}

bool FolderCache::IsKnownMissing( const char *pAbsolutePath )
{
	if ( !pAbsolutePath || IsDisabled() )
		return false;

	char buf[kMaxPath];
	int len = NormalizePath( pAbsolutePath, buf, sizeof( buf ) );
	if ( len < 0 )
		return false;

	std::string_view path( buf, len );
	Result result = Result::Unknown;
	Root *root = nullptr;
	std::string_view rel;

	{
		std::shared_lock<std::shared_mutex> lock( g_Lock );
		root = FindRoot( path, false );
		if ( root && root->m_bActive && !HasUnprocessedChanges( *root ) )
		{
			rel = path.substr( root->m_Path.size() );
			if ( IsSimpleRelative( rel ) )
				result = Walk( *root, rel, false );
		}
	}

	if ( result == Result::NeedList )
	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );

		// Roots are never freed, but it might have been dropped meanwhile
		result = Result::Unknown;
		if ( FindRoot( path, false ) == root && root->m_bActive && !HasUnprocessedChanges( *root ) )
			result = Walk( *root, rel, true );
	}

	return result == Result::Missing;
}

bool FolderCache::GetFileData( const char *pAbsolutePath, FileData *pOut )
{
	if ( !pAbsolutePath || IsDisabled() || IsStatTurnedOff() )
		return false;

	char buf[kMaxPath];
	int len = NormalizePath( pAbsolutePath, buf, sizeof( buf ) );
	if ( len < 0 )
		return false;

	std::string_view path( buf, len );
	Root *root = nullptr;
	std::string_view rel;

	// Present, sure about it, a file, and we still have its data
	auto lookup = [&]( bool bAllowList ) -> Result {
		Result result = Walk( *root, rel, bAllowList );
		if ( result != Result::Present )
			return result;

		std::string_view last;
		Node *node = FindParentNode( *root, rel, last );
		if ( !node )
			return Result::Unknown;

		const unsigned char *pFlags = FindFinalEntry( *node, last );
		auto it = node->m_Data.find( last );
		if ( !pFlags || ( *pFlags & ( kFlagUnsure | kFlagDir ) ) || it == node->m_Data.end() )
			return Result::Unknown;

		*pOut = it->second;
		return Result::Present;
	};

	Result result = Result::Unknown;
	{
		std::shared_lock<std::shared_mutex> lock( g_Lock );
		root = FindRoot( path, false );
		if ( root && root->m_bActive && !HasUnprocessedChanges( *root ) && !HasUnprocessedDataChanges( *root ) )
		{
			rel = path.substr( root->m_Path.size() );
			if ( IsSimpleRelative( rel ) )
				result = lookup( false );
		}
	}

	if ( result == Result::NeedList )
	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );
		result = Result::Unknown;
		if ( FindRoot( path, false ) == root && root->m_bActive && !HasUnprocessedChanges( *root ) && !HasUnprocessedDataChanges( *root ) )
			result = lookup( true );
	}

	return result == Result::Present;
}

void FolderCache::OnPathChanged( const char *pAbsolutePath )
{
	if ( !pAbsolutePath )
		return;

	char buf[kMaxPath];
	int len = NormalizePath( pAbsolutePath, buf, sizeof( buf ) );
	if ( len < 0 )
	{
		InvalidateAll();
		return;
	}

	std::string_view path( buf, len );

	std::unique_lock<std::shared_mutex> lock( g_Lock );
	for ( Root *root : g_Roots )
	{
		const std::string &rp = root->m_Path;
		if ( path.size() >= rp.size() - 1 && memcmp( path.data(), rp.data(), rp.size() - 1 ) == 0 &&
			( path.size() == rp.size() - 1 || path[rp.size() - 1] == kSep ) )
		{
			std::string_view rel = path.size() > rp.size() ? path.substr( rp.size() ) : std::string_view();
			while ( !rel.empty() && rel.back() == kSep )
				rel.remove_suffix( 1 );

			// Things like .. we can't place in the tree
			if ( !rel.empty() && IsSimpleRelative( rel ) )
				MarkChanged( *root, rel );
			else
				ResetTree( *root );
		}
	}

	std::string changed( path );
	while ( !changed.empty() && changed.back() == kSep )
		changed.pop_back();
	RetireRootsUnder( changed );
}

void FolderCache::InvalidateAll()
{
	std::unique_lock<std::shared_mutex> lock( g_Lock );
	for ( Root *root : g_Roots )
		ResetTree( *root );
}

bool FolderCache::IsSimpleRelativePath( const char *pRel, size_t nLen )
{
	return IsSimpleRelative( std::string_view( pRel, nLen ) );
}

FolderCache::DirState FolderCache::ListDirectory( const char *pAbsoluteDir, DirEntryFn fn, void *pCtx, const void **ppRoot )
{
	*ppRoot = nullptr;
	if ( !pAbsoluteDir || IsDisabled() )
		return DirState::Unknown;

	char buf[kMaxPath];
	int len = NormalizePath( pAbsoluteDir, buf, sizeof( buf ) - 1 );
	if ( len <= 0 )
		return DirState::Unknown;

	if ( buf[len - 1] != kSep )
	{
		buf[len++] = kSep;
		buf[len] = '\0';
	}

	std::string_view path( buf, len );
	DirState state = DirState::Unknown;

	std::unique_lock<std::shared_mutex> lock( g_Lock );

	Root *root = FindRoot( path, true );
	if ( root && root->m_bActive && !HasUnprocessedChanges( *root ) )
	{
		std::string_view rel = path.substr( root->m_Path.size() );
		while ( !rel.empty() && rel.back() == kSep )
			rel.remove_suffix( 1 );

		if ( rel.empty() || IsSimpleRelative( rel ) )
		{
			Node *node = nullptr;
			state = WalkDir( *root, rel, &node );
			if ( state == DirState::Listed )
			{
				// Keys for the last part of a lookup, that's what callers match names against
				const NameMap &names = kExactFolders ? node->m_Folded : node->m_Names;
				for ( auto &entry : names )
					fn( pCtx, entry.first.data(), entry.first.size(), entry.second );
			}
			*ppRoot = root;
		}
	}

	return state;
}

bool FolderCache::IsRootSettled( const void *pRoot )
{
	const Root *root = (const Root *)pRoot;
	return root && *(volatile const bool *)&root->m_bActive && !HasUnprocessedChanges( *root );
}

struct FolderCache::FindState
{
	std::shared_ptr<const FindListing> m_pListing;
	std::vector<uint32_t> m_Order;
	size_t m_nNext = 0;
};

static bool NextFindMatch( FolderCache::FindState &state, const char **ppName, bool *pbDir )
{
	if ( state.m_nNext >= state.m_Order.size() )
		return false;

	const FindEntry &entry = state.m_pListing->m_Entries[state.m_Order[state.m_nNext++]];
	*ppName = entry.m_Name.c_str();
	*pbDir = entry.m_bDir;
	return true;
}

FolderCache::FindResult FolderCache::FindFirst( const char *pFindName, FindState **ppState, const char **ppName, bool *pbDir )
{
	*ppState = nullptr;
	if ( !pFindName || IsDisabled() || IsFindTurnedOff() )
		return FindResult::NotHandled;

	std::string_view full( pFindName );
	const size_t sep = full.find_last_of( "\\/" );
	if ( sep == std::string_view::npos )
		return FindResult::NotHandled;

	FindPattern pattern;
	if ( !ParseFindPattern( full.substr( sep + 1 ), pattern ) )
		return FindResult::NotHandled;

	char buf[kMaxPath];
	int len = NormalizePath( std::string( full.substr( 0, sep ) ).c_str(), buf, sizeof( buf ) - 1 );
	if ( len <= 0 )
		return FindResult::NotHandled;
	if ( buf[len - 1] != kSep )
	{
		buf[len++] = kSep;
		buf[len] = '\0';
	}
	std::string_view path( buf, len );

	Root *root = nullptr;
	std::string key;
	std::shared_ptr<const FindListing> listing;
	uint64_t seq = 0;
	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );
		root = FindRoot( path, true );
		if ( !root || !root->m_bActive || HasUnprocessedChanges( *root ) )
			return FindResult::NotHandled;

		std::string_view rel = path.substr( root->m_Path.size() );
		while ( !rel.empty() && rel.back() == kSep )
			rel.remove_suffix( 1 );
		if ( !rel.empty() && !IsSimpleRelative( rel ) )
			return FindResult::NotHandled;

		// Has to exist the way the tree knows it, no links on the way
		Node *node = nullptr;
		const DirState state = WalkDir( *root, rel, &node );
		if ( state == DirState::Missing )
			return FindResult::NoMatch;
		if ( state != DirState::Listed )
			return FindResult::NotHandled;

		key.assign( rel );
		auto it = root->m_FindListings.find( key );
		if ( it != root->m_FindListings.end() )
			listing = it->second;
		seq = root->m_nChangeSeq;
	}

	if ( !listing )
	{
		auto fresh = std::make_shared<FindListing>();
		if ( !ListForFind( std::string( full.substr( 0, sep + 1 ) ) + "*", *fresh ) )
			return FindResult::NotHandled;

		// Anything that changed while listing and we don't trust it
		std::unique_lock<std::shared_mutex> lock( g_Lock );
		if ( !root->m_bActive || root->m_nChangeSeq != seq || HasUnprocessedChanges( *root ) || FindRoot( path, true ) != root )
			return FindResult::NotHandled;

		root->m_FindListings[key] = fresh;
		listing = std::move( fresh );
	}

	if ( !listing->m_bUsable )
		return FindResult::NotHandled;

	auto *state = new FindState;
	if ( !BuildFindOrder( pattern, *listing, state->m_Order ) )
	{
		delete state;
		return FindResult::NotHandled;
	}

	state->m_pListing = std::move( listing );
	if ( !NextFindMatch( *state, ppName, pbDir ) )
	{
		delete state;
		return FindResult::NoMatch;
	}

	*ppState = state;
	return FindResult::Found;
}

bool FolderCache::FindNext( FindState *pState, const char **ppName, bool *pbDir )
{
	return pState && NextFindMatch( *pState, ppName, pbDir );
}

void FolderCache::FindClose( FindState *pState )
{
	delete pState;
}

void FolderCache::SetChangeCallback( ChangeFn fn )
{
	std::unique_lock<std::shared_mutex> lock( g_Lock );
	g_pChangeCallback = fn;
}

void FolderCache::Shutdown()
{
	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );
		if ( g_bShutdown )
			return;
		g_bShutdown = true;
	}

	ShutdownBackend();
}

//-----------------------------------------------------------------------------
// No backend for this platform: nothing gets watched, every lookup goes to the OS
//-----------------------------------------------------------------------------
#if !defined( FOLDERCACHE_WIN32 ) && !defined( FOLDERCACHE_LINUX )
int FolderCache::Detail::NormalizePath( const char *, char *, int ) { return -1; }
bool FolderCache::Detail::IsSimpleComponent( std::string_view ) { return false; }
void FolderCache::Detail::FoldName( std::string_view name, std::string &out ) { out.assign( name ); }
bool FolderCache::Detail::IsWatchablePath( const std::string & ) { return false; }
bool FolderCache::Detail::HasLinkBetween( const std::string &, const std::string & ) { return true; }
bool FolderCache::Detail::PrepareRoot( Root & ) { return false; }
void FolderCache::Detail::ActivateRoot( Root & ) {}
void FolderCache::Detail::RetireRoot( Root & ) {}
bool FolderCache::Detail::HasUnprocessedChanges( const Root & ) { return true; }
bool FolderCache::Detail::HasUnprocessedDataChanges( const Root & ) { return true; }
bool FolderCache::Detail::ListNode( Root &, Node &, std::string_view ) { return false; }
void FolderCache::Detail::ShutdownBackend() {}
bool FolderCache::Detail::ParseFindPattern( std::string_view, FindPattern & ) { return false; }
bool FolderCache::Detail::BuildFindOrder( const FindPattern &, const FindListing &, std::vector<uint32_t> & ) { return false; }
bool FolderCache::Detail::ListForFind( const std::string &, FindListing & ) { return false; }
#endif
