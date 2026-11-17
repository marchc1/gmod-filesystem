#include "pathresolver.h"
#include "foldercache.h"
#include "packfile.h"
#include "unordered_stuff.h"

#include "tier1/convar.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string_view>

static ConVar filesystem_resolver( "filesystem_resolver", "1", 0, "Look files up in an index instead of trying every search path" );

namespace
{
	using SourceId = unsigned short;
	constexpr SourceId kNoSource = 0xFFFF;

	constexpr char kSep = CORRECT_PATH_SEPARATOR;

	// The case-insensitive part of a name, what FolderCache listings are keyed by
	void FoldAscii( std::string_view in, char *out )
	{
		for ( size_t i = 0; i < in.size(); ++i )
		{
			char c = in[i];
			out[i] = ( c >= 'A' && c <= 'Z' ) ? (char)( c + 'a' - 'A' ) : c;
		}
	}

	uint64 HashPath( const char *p, size_t n )
	{
		uint64 h = 14695981039346656037ull;
		for ( size_t i = 0; i < n; ++i )
			h = ( h ^ (unsigned char)p[i] ) * 1099511628211ull;
		return h;
	}

	enum class SourceKind : unsigned char
	{
		Loose,
		VPK,
		Probe, // zip/bsp, always asked
	};

	struct Source
	{
		SourceKind m_Kind = SourceKind::Loose;
		bool m_bLive = false;
		const void *m_pKey = nullptr;
		uint64 m_nSerial = 0;
		std::string m_Path; // loose: lowercase, backslash at the end
		std::vector<uint64> m_Hashes; // VPK: what went into the file index
		std::vector<uint64> m_DirHashes; // VPK: and the folder index
	};

	// hash -> sources. Most hashes have one, high bit set = index into m_Multi
	struct SourceIndex
	{
		unordered_map<uint64, uint32> m_Map;
		std::vector<std::vector<SourceId>> m_Multi;

		void Add( uint64 h, SourceId id )
		{
			auto it = m_Map.find( h );
			if ( it == m_Map.end() )
			{
				m_Map.emplace( h, (uint32)id );
				return;
			}

			uint32 &v = it->second;
			if ( v & 0x80000000u )
			{
				auto &list = m_Multi[v & 0x7FFFFFFFu];
				if ( std::find( list.begin(), list.end(), id ) == list.end() )
					list.push_back( id );
			}
			else if ( v != id )
			{
				m_Multi.push_back( { (SourceId)v, id } );
				v = 0x80000000u | (uint32)( m_Multi.size() - 1 );
			}
		}

		void Remove( uint64 h, SourceId id )
		{
			auto it = m_Map.find( h );
			if ( it == m_Map.end() )
				return;

			uint32 v = it->second;
			if ( v & 0x80000000u )
			{
				auto &list = m_Multi[v & 0x7FFFFFFFu];
				list.erase( std::remove( list.begin(), list.end(), id ), list.end() );
			}
			else if ( v == id )
			{
				m_Map.erase( it );
			}
		}

		template <class F>
		void ForEach( uint64 h, F &&fn ) const
		{
			auto it = m_Map.find( h );
			if ( it == m_Map.end() )
				return;

			uint32 v = it->second;
			if ( v & 0x80000000u )
			{
				for ( SourceId id : m_Multi[v & 0x7FFFFFFFu] )
					fn( id );
			}
			else
			{
				fn( (SourceId)v );
			}
		}
	};

	struct MergedDir
	{
		bool m_bBuilt = false;
		// name -> ( source << 8 ) | FolderCache flags
		unordered_map<std::string, std::vector<uint32>, StringHash, StringEq> m_Names;
		std::vector<SourceId> m_Unknown;
		std::vector<const void *> m_Roots;
		unordered_map<std::string, std::unique_ptr<MergedDir>, StringHash, StringEq> m_Children;

		void Reset()
		{
			m_bBuilt = false;
			m_Names.clear();
			m_Unknown.clear();
			m_Roots.clear();
		}
	};

	enum class Status
	{
		Ok,
		NeedExclusive,
		Fail,
	};
}

struct CPathResolver::View
{
	struct Entry
	{
		unsigned short m_Index; // into m_SearchPaths
		int m_StoreId;
		SourceId m_Source;
	};

	uint32 m_nSeq = 0;
	std::vector<Entry> m_Entries; // in the order CSearchPathsIterator visits them
	std::vector<std::vector<unsigned short>> m_RanksBySource;
	std::vector<unsigned short> m_ProbeRanks;
	std::vector<SourceId> m_GmaSources; // loose sources a GMA lookup could match through
	std::string m_GmaModPath; // what m_GmaSources was worked out for
};

struct CPathResolver::Impl
{
	CBaseFileSystem *m_pFS;
	std::shared_mutex m_Lock;

	std::vector<Source> m_Sources;
	std::vector<SourceId> m_LooseSources;
	unordered_map<std::string, SourceId, StringHash, StringEq> m_LooseByPath;

	// relative path hash -> VPK sources having that file / that folder
	SourceIndex m_VPKFiles;
	SourceIndex m_VPKDirs;

	// VPK and pack sources by pointer
	unordered_map<const void *, SourceId> m_ByKey;

	unordered_map<UtlSymId_t, std::shared_ptr<View>> m_Views;
	MergedDir m_Merged;

	// Filled from the FolderCache callback, which holds its own lock
	std::mutex m_ChangeMutex;
	std::vector<std::string> m_Changes;
	std::atomic<bool> m_bHasChanges{ false };

	static Impl *s_pInstance;

	static void OnFolderChanged( const char *pPath, size_t nLen )
	{
		Impl *self = s_pInstance;
		if ( !self )
			return;
		std::lock_guard<std::mutex> lock( self->m_ChangeMutex );
		self->m_Changes.emplace_back( pPath, nLen );
		self->m_bHasChanges.store( true, std::memory_order_release );
	}

	SourceId NewSource()
	{
		for ( size_t i = 0; i < m_Sources.size(); ++i )
		{
			if ( !m_Sources[i].m_bLive && m_Sources[i].m_pKey == nullptr && m_Sources[i].m_Path.empty() )
				return (SourceId)i;
		}
		if ( m_Sources.size() >= kNoSource )
			return kNoSource;
		m_Sources.emplace_back();
		return (SourceId)( m_Sources.size() - 1 );
	}

	struct VPKIndexCtx
	{
		Impl *m_pSelf;
		SourceId m_Id;
		Source *m_pSource;
		unordered_set<uint64> m_SeenDirs;
	};

	// Folder and all its parents. Lowercase, VPK FindFirst compares folders without case
	static void IndexVPKDir( VPKIndexCtx &ctx, const char *pDir )
	{
		if ( !strcmp( pDir, " " ) )
			return;

		char buf[MAX_PATH * 2];
		size_t n = 0;
		for ( const char *s = pDir; *s && n < sizeof( buf ) - 1; ++s )
		{
			char c = *s;
			if ( c == '/' )
				c = kSep;
			else if ( c >= 'A' && c <= 'Z' )
				c += 'a' - 'A';
			buf[n++] = c;
		}

		while ( n )
		{
			uint64 h = HashPath( buf, n );
			if ( !ctx.m_SeenDirs.insert( h ).second )
				return; // parents are in already too

			ctx.m_pSource->m_DirHashes.push_back( h );
			ctx.m_pSelf->m_VPKDirs.Add( h, ctx.m_Id );

			while ( n && buf[n - 1] != kSep )
				--n;
			if ( n )
				--n;
		}
	}

	// The relative name FindFileInSearchPath would ask the VPK for, see SplitFileComponents
	static void IndexVPKFile( void *pCtx, const char *pDir, const char *pBase, const char *pExt, bool bReachable )
	{
		auto *ctx = (VPKIndexCtx *)pCtx;

		// Folders come from everything, the FindFirst cache and DirectoryEntryExists see shadowed ones too
		IndexVPKDir( *ctx, pDir );
		if ( !bReachable )
			return;

		char buf[MAX_PATH * 3];
		size_t n = 0;
		auto append = [&]( const char *s, bool bFixSlashes ) {
			for ( ; *s && n < sizeof( buf ) - 1; ++s )
				buf[n++] = ( bFixSlashes && *s == '/' ) ? kSep : *s;
		};

		if ( strcmp( pDir, " " ) != 0 )
		{
			append( pDir, true );
			buf[n++] = kSep;
		}
		append( pBase, false );
		if ( strcmp( pExt, " " ) != 0 && n < sizeof( buf ) - 2 )
		{
			buf[n++] = '.';
			append( pExt, false );
		}

		uint64 h = HashPath( buf, n );
		ctx->m_pSource->m_Hashes.push_back( h );
		ctx->m_pSelf->m_VPKFiles.Add( h, ctx->m_Id );
	}

	void KillSource( SourceId id )
	{
		Source &s = m_Sources[id];
		for ( uint64 h : s.m_Hashes )
			m_VPKFiles.Remove( h, id );
		for ( uint64 h : s.m_DirHashes )
			m_VPKDirs.Remove( h, id );

		if ( s.m_Kind == SourceKind::Loose )
			m_LooseByPath.erase( s.m_Path );
		else
			m_ByKey.erase( s.m_pKey );

		s = Source{};
	}

	void Rebuild()
	{
		for ( Source &s : m_Sources )
			s.m_bLive = false;

		bool bLooseChanged = false;
		auto &paths = m_pFS->m_SearchPaths;
		for ( auto i = paths.Head(); i != paths.InvalidIndex(); i = paths.Next( i ) )
		{
			const CBaseFileSystem::CSearchPath &sp = paths[i];
			SourceId id = FindOrAddSource( sp, bLooseChanged );
			if ( id != kNoSource )
				m_Sources[id].m_bLive = true;
		}

		for ( size_t i = 0; i < m_Sources.size(); ++i )
		{
			Source &s = m_Sources[i];
			if ( s.m_bLive || ( !s.m_pKey && s.m_Path.empty() ) )
				continue;
			if ( s.m_Kind == SourceKind::Loose )
				bLooseChanged = true;
			KillSource( (SourceId)i );
		}

		m_LooseSources.clear();
		for ( size_t i = 0; i < m_Sources.size(); ++i )
		{
			if ( m_Sources[i].m_bLive && m_Sources[i].m_Kind == SourceKind::Loose )
				m_LooseSources.push_back( (SourceId)i );
		}

		// Merged listings only know about the loose sources they were built with
		if ( bLooseChanged )
			ResetMerged();

		m_Views.clear();
	}

	SourceId FindOrAddSource( const CBaseFileSystem::CSearchPath &sp, bool &bLooseChanged )
	{
		if ( CPackedStoreRefCount *pVPK = sp.GetPackedStore() )
		{
			auto it = m_ByKey.find( pVPK );
			if ( it != m_ByKey.end() )
			{
				const Source &s = m_Sources[it->second];
				if ( s.m_Kind == SourceKind::VPK && s.m_nSerial == pVPK->m_nInstanceSerial )
					return it->second;

				// Freed and a new one landed at the same address
				KillSource( it->second );
			}

			SourceId id = NewSource();
			if ( id == kNoSource )
				return kNoSource;

			Source &s = m_Sources[id];
			s.m_Kind = SourceKind::VPK;
			s.m_pKey = pVPK;
			s.m_nSerial = pVPK->m_nInstanceSerial;
			m_ByKey[pVPK] = id;

			VPKIndexCtx ctx{ this, id, &s, {} };
			pVPK->ForEachFile( IndexVPKFile, &ctx );
			return id;
		}

		if ( CPackFile *pPack = sp.GetPackFile() )
		{
			auto it = m_ByKey.find( pPack );
			if ( it != m_ByKey.end() && m_Sources[it->second].m_Kind == SourceKind::Probe )
				return it->second;
			if ( it != m_ByKey.end() )
				KillSource( it->second );

			SourceId id = NewSource();
			if ( id == kNoSource )
				return kNoSource;
			m_Sources[id].m_Kind = SourceKind::Probe;
			m_Sources[id].m_pKey = pPack;
			m_ByKey[pPack] = id;
			return id;
		}

		const char *pPath = sp.GetPathString();
		auto it = m_LooseByPath.find( std::string_view( pPath ) );
		if ( it != m_LooseByPath.end() )
			return it->second;

		SourceId id = NewSource();
		if ( id == kNoSource )
			return kNoSource;
		m_Sources[id].m_Kind = SourceKind::Loose;
		m_Sources[id].m_Path = pPath;
		m_LooseByPath.emplace( m_Sources[id].m_Path, id );
		bLooseChanged = true;
		return id;
	}

	void ResetMerged()
	{
		m_Merged.Reset();
		m_Merged.m_Children.clear();
	}

	// Exclusive lock held
	void DrainChanges()
	{
		std::vector<std::string> changes;
		{
			std::lock_guard<std::mutex> lock( m_ChangeMutex );
			changes.swap( m_Changes );
			m_bHasChanges.store( false, std::memory_order_release );
		}

		for ( const std::string &changed : changes )
		{
			std::string_view x( changed );
			while ( !x.empty() && x.back() == kSep )
				x.remove_suffix( 1 );

			for ( SourceId id : m_LooseSources )
			{
				std::string_view sp( m_Sources[id].m_Path );
				std::string_view spNoSlash = sp.substr( 0, sp.size() - 1 );

				// Something at or above the source's folder
				if ( spNoSlash.size() >= x.size() && spNoSlash.compare( 0, x.size(), x ) == 0 &&
					( spNoSlash.size() == x.size() || spNoSlash[x.size()] == kSep ) )
				{
					ResetMerged();
					return;
				}

				if ( x.size() > sp.size() && x.compare( 0, sp.size(), sp ) == 0 )
					DropMerged( x.substr( sp.size() ) );
			}
		}
	}

	// rel changed: its folder's listing and everything under it are stale
	void DropMerged( std::string_view rel )
	{
		MergedDir *node = &m_Merged;
		size_t pos = 0;
		while ( true )
		{
			size_t slash = rel.find( kSep, pos );
			std::string_view comp = rel.substr( pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos );
			if ( slash == std::string_view::npos )
			{
				node->Reset();
				auto child = node->m_Children.find( comp );
				if ( child != node->m_Children.end() )
					node->m_Children.erase( child );
				return;
			}

			auto child = node->m_Children.find( comp );
			if ( child == node->m_Children.end() )
				return;
			node = child->second.get();
			pos = slash + 1;
		}
	}

	struct ListCtx
	{
		MergedDir *m_pDir;
		SourceId m_Id;
	};

	static void AddListedName( void *pCtx, const char *pName, size_t nLen, unsigned char flags )
	{
		auto *ctx = (ListCtx *)pCtx;
		auto &names = ctx->m_pDir->m_Names;
		auto it = names.find( std::string_view( pName, nLen ) );
		if ( it == names.end() )
			it = names.emplace( std::string( pName, nLen ), std::vector<uint32>() ).first;
		it->second.push_back( ( (uint32)ctx->m_Id << 8 ) | flags );
	}

	// dir has no trailing backslash, empty = top level
	MergedDir *FindMerged( std::string_view dir, bool bCreate )
	{
		MergedDir *node = &m_Merged;
		size_t pos = 0;
		while ( pos < dir.size() )
		{
			size_t slash = dir.find( kSep, pos );
			std::string_view comp = dir.substr( pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos );
			auto child = node->m_Children.find( comp );
			if ( child == node->m_Children.end() )
			{
				if ( !bCreate )
					return nullptr;
				child = node->m_Children.emplace( std::string( comp ), std::make_unique<MergedDir>() ).first;
			}
			node = child->second.get();
			pos = slash == std::string_view::npos ? dir.size() : slash + 1;
		}
		return node;
	}

	// Exclusive lock held
	void BuildMerged( MergedDir &node, std::string_view dir )
	{
		node.Reset();

		std::string abs;
		for ( SourceId id : m_LooseSources )
		{
			abs.assign( m_Sources[id].m_Path );
			abs.append( dir );

			ListCtx ctx{ &node, id };
			const void *pRoot = nullptr;
			FolderCache::DirState state = FolderCache::ListDirectory( abs.c_str(), AddListedName, &ctx, &pRoot );
			if ( state == FolderCache::DirState::Unknown )
				node.m_Unknown.push_back( id );

			if ( pRoot && std::find( node.m_Roots.begin(), node.m_Roots.end(), pRoot ) == node.m_Roots.end() )
				node.m_Roots.push_back( pRoot );
		}

		node.m_bBuilt = true;
	}

	static void NormalizeForGma( std::string_view in, std::string &out )
	{
		out.assign( in );
		for ( char &c : out )
		{
			if ( c == '\\' )
				c = '/';
			else if ( c >= 'A' && c <= 'Z' )
				c += 'a' - 'A';
		}
	}

	// Exclusive lock held
	void BuildGmaSources( View &view, const std::string &modPath )
	{
		view.m_GmaSources.clear();
		view.m_GmaModPath = modPath;
		if ( modPath.empty() )
			return;

		std::string norm;
		for ( const View::Entry &e : view.m_Entries )
		{
			const Source &s = m_Sources[e.m_Source];
			if ( s.m_Kind != SourceKind::Loose )
				continue;

			if ( IsGmaCapable( s.m_Path, modPath, norm ) && std::find( view.m_GmaSources.begin(), view.m_GmaSources.end(), e.m_Source ) == view.m_GmaSources.end() )
				view.m_GmaSources.push_back( e.m_Source );
		}
	}

	// Exclusive lock held. Same filtering and order as CSearchPathsIterator with FILTER_NONE
	std::shared_ptr<View> BuildView( const char *pPathID, uint32 nSeq )
	{
		auto view = std::make_shared<View>();
		view->m_nSeq = nSeq;
		view->m_RanksBySource.resize( m_Sources.size() );

		CUtlSymbol pathID = pPathID ? g_PathIDTable.AddString( pPathID ) : CUtlSymbol( UTL_INVAL_SYMBOL );

		std::vector<int> visited;
		auto &paths = m_pFS->m_SearchPaths;
		for ( auto i = paths.Head(); i != paths.InvalidIndex(); i = paths.Next( i ) )
		{
			const CBaseFileSystem::CSearchPath &sp = paths[i];
			if ( m_pFS->FilterByPathID( &sp, pathID ) )
				continue;
			if ( std::find( visited.begin(), visited.end(), sp.m_storeId ) != visited.end() )
				continue;
			visited.push_back( sp.m_storeId );

			bool bUnused = false;
			SourceId id = FindOrAddSource( sp, bUnused );
			if ( id == kNoSource || view->m_Entries.size() >= 0xFFFF )
				return nullptr;
			m_Sources[id].m_bLive = true;
			if ( view->m_RanksBySource.size() < m_Sources.size() )
				view->m_RanksBySource.resize( m_Sources.size() );

			unsigned short rank = (unsigned short)view->m_Entries.size();
			view->m_Entries.push_back( { (unsigned short)i, sp.m_storeId, id } );
			view->m_RanksBySource[id].push_back( rank );
			if ( m_Sources[id].m_Kind == SourceKind::Probe )
				view->m_ProbeRanks.push_back( rank );
		}

		BuildGmaSources( *view, m_pFS->m_AddonFileSystem.ModPathRef() );
		return view;
	}

	// GetFile/FindFirst strip the mod path off the absolute name, only folders on its line can match
	static bool IsGmaCapable( const std::string &loosePath, const std::string &modPath, std::string &scratch )
	{
		if ( modPath.empty() )
			return false;
		NormalizeForGma( loosePath, scratch );
		return modPath.compare( 0, scratch.size(), scratch ) == 0 || scratch.compare( 0, modPath.size(), modPath ) == 0;
	}

	Status BuildFindFilter( const char *pWildCard, size_t nLen, uint32 nSeq, bool bExclusive, CPathResolver::FindFilter &out )
	{
		if ( m_bHasChanges.load( std::memory_order_acquire ) )
		{
			if ( !bExclusive )
				return Status::NeedExclusive;
			DrainChanges();
		}

		// Wildcards only work in the last part, the folder before it has to exist
		std::string_view wild( pWildCard, nLen );
		size_t slash = wild.find_last_of( kSep );
		std::string_view dir = slash == std::string_view::npos ? std::string_view() : wild.substr( 0, slash );
		if ( !dir.empty() && !FolderCache::IsSimpleRelativePath( dir.data(), dir.size() ) )
			return Status::Fail;

		std::vector<unsigned char> sourceOk( m_Sources.size(), 0 );
		if ( dir.empty() )
		{
			for ( size_t i = 0; i < m_Sources.size(); ++i )
				sourceOk[i] = m_Sources[i].m_bLive;
		}
		else
		{
			size_t parentSlash = dir.find_last_of( kSep );
			std::string_view parent = parentSlash == std::string_view::npos ? std::string_view() : dir.substr( 0, parentSlash );
			std::string_view name = parentSlash == std::string_view::npos ? dir : dir.substr( parentSlash + 1 );

			MergedDir *merged = FindMerged( parent, bExclusive );
			if ( !merged || !merged->m_bBuilt )
			{
				if ( !bExclusive )
					return Status::NeedExclusive;
				BuildMerged( *merged, parent );
			}

			for ( const void *pRoot : merged->m_Roots )
			{
				if ( !FolderCache::IsRootSettled( pRoot ) )
					return Status::Fail;
			}

			char folded[MAX_PATH];
			FoldAscii( name, folded );
			auto loose = merged->m_Names.find( std::string_view( folded, name.size() ) );
			if ( loose != merged->m_Names.end() )
			{
				for ( uint32 entry : loose->second )
					sourceOk[entry >> 8] = 1;
			}
			for ( SourceId id : merged->m_Unknown )
				sourceOk[id] = 1;

			char lowerDir[MAX_PATH];
			if ( dir.size() >= sizeof( lowerDir ) )
				return Status::Fail;
			FoldAscii( dir, lowerDir );
			m_VPKDirs.ForEach( HashPath( lowerDir, dir.size() ), [&]( SourceId id ) { sourceOk[id] = 1; } );
		}

		const bool bGma = m_pFS->m_AddonFileSystem.HasFiles();
		const std::string &modPath = m_pFS->m_AddonFileSystem.ModPathRef();
		std::string scratch;

		auto &paths = m_pFS->m_SearchPaths;
		out.m_Flags.clear();
		for ( auto i = paths.Head(); i != paths.InvalidIndex(); i = paths.Next( i ) )
		{
			const CBaseFileSystem::CSearchPath &sp = paths[i];

			SourceId id = kNoSource;
			if ( CPackedStoreRefCount *pVPK = sp.GetPackedStore() )
			{
				auto it = m_ByKey.find( pVPK );
				if ( it != m_ByKey.end() && m_Sources[it->second].m_nSerial == pVPK->m_nInstanceSerial )
					id = it->second;
			}
			else if ( CPackFile *pPack = sp.GetPackFile() )
			{
				auto it = m_ByKey.find( pPack );
				if ( it != m_ByKey.end() )
					id = it->second;
			}
			else
			{
				auto it = m_LooseByPath.find( std::string_view( sp.GetPathString() ) );
				if ( it != m_LooseByPath.end() )
					id = it->second;
			}

			unsigned char flags = 0;
			if ( id == kNoSource || id >= sourceOk.size() || m_Sources[id].m_Kind == SourceKind::Probe || sourceOk[id] )
				flags |= CPathResolver::kFindMatches;

			if ( bGma && ( id == kNoSource || ( m_Sources[id].m_Kind == SourceKind::Loose && IsGmaCapable( m_Sources[id].m_Path, modPath, scratch ) ) ) )
				flags |= CPathResolver::kFindGma | CPathResolver::kFindMatches;

			if ( out.m_Flags.size() <= i )
				out.m_Flags.resize( (size_t)i + 1, 0 );
			out.m_Flags[i] = flags;
		}

		out.m_nSeq = nSeq;
		return Status::Ok;
	}

	// pFileName: key for loose folders. pVPKName: same name lowercased, for the VPK indexes
	Status Resolve( const char *pFileName, const char *pVPKName, size_t nLen, const char *pPathID, uint32 nSeq, bool bExclusive, Kind kind, Candidates &out )
	{
		if ( m_bHasChanges.load( std::memory_order_acquire ) )
		{
			if ( !bExclusive )
				return Status::NeedExclusive;
			DrainChanges();
		}

		UtlSymId_t key = pPathID ? (UtlSymId_t)g_PathIDTable.Find( pPathID ) : UTL_INVAL_SYMBOL;
		std::shared_ptr<View> view;
		if ( !pPathID || key != UTL_INVAL_SYMBOL )
		{
			auto it = m_Views.find( key );
			if ( it != m_Views.end() && it->second->m_nSeq == nSeq )
				view = it->second;
		}

		if ( !view )
		{
			if ( !bExclusive )
				return Status::NeedExclusive;
			view = BuildView( pPathID, nSeq );
			if ( !view )
				return Status::Fail;
			key = pPathID ? (UtlSymId_t)g_PathIDTable.Find( pPathID ) : UTL_INVAL_SYMBOL;
			m_Views[key] = view;
		}

		const bool bHasGma = m_pFS->m_AddonFileSystem.HasFiles();
		if ( bHasGma && view->m_GmaModPath != m_pFS->m_AddonFileSystem.ModPathRef() )
		{
			if ( !bExclusive )
				return Status::NeedExclusive;
			BuildGmaSources( *view, m_pFS->m_AddonFileSystem.ModPathRef() );
		}

		std::string_view path( pFileName, nLen );
		size_t slash = path.find_last_of( kSep );
		std::string_view dir = slash == std::string_view::npos ? std::string_view() : path.substr( 0, slash );
		std::string_view name = slash == std::string_view::npos ? path : path.substr( slash + 1 );

		MergedDir *merged = FindMerged( dir, bExclusive );
		if ( !merged || !merged->m_bBuilt )
		{
			if ( !bExclusive )
				return Status::NeedExclusive;
			BuildMerged( *merged, dir );
		}

		// The watcher has news it didn't apply yet, don't trust anything from those roots
		for ( const void *pRoot : merged->m_Roots )
		{
			if ( !FolderCache::IsRootSettled( pRoot ) )
				return Status::Fail;
		}

		out.m_pView = view;
		out.m_nSeq = nSeq;
		out.m_nCount = 0;

		bool bOverflow = false;
		auto addSource = [&]( SourceId id ) {
			if ( id >= view->m_RanksBySource.size() )
				return;
			for ( unsigned short rank : view->m_RanksBySource[id] )
			{
				if ( out.m_nCount >= (int)std::size( out.m_Ranks ) )
				{
					bOverflow = true;
					return;
				}
				out.m_Ranks[out.m_nCount++] = rank;
			}
		};

		if ( kind == Kind::Directory )
			m_VPKDirs.ForEach( HashPath( pVPKName, nLen ), addSource );
		else
			m_VPKFiles.ForEach( HashPath( pVPKName, nLen ), addSource );

		// Listings are keyed by the folded name
		char folded[MAX_PATH];
		FoldAscii( name, folded );
		auto loose = merged->m_Names.find( std::string_view( folded, name.size() ) );
		if ( loose != merged->m_Names.end() )
		{
			for ( uint32 entry : loose->second )
				addSource( (SourceId)( entry >> 8 ) );
		}

		for ( SourceId id : merged->m_Unknown )
			addSource( id );

		if ( bHasGma )
		{
			std::string abs;
			for ( SourceId id : view->m_GmaSources )
			{
				// GMA folders: just let the caller ask, there's only a couple of these
				if ( kind == Kind::Directory )
				{
					addSource( id );
					continue;
				}

				abs.assign( m_Sources[id].m_Path );
				abs.append( path );
				if ( m_pFS->m_AddonFileSystem.GetFile( abs ) )
					addSource( id );
			}
		}

		for ( unsigned short rank : view->m_ProbeRanks )
		{
			if ( out.m_nCount >= (int)std::size( out.m_Ranks ) )
			{
				bOverflow = true;
				break;
			}
			out.m_Ranks[out.m_nCount++] = rank;
		}

		if ( bOverflow )
			return Status::Fail;

		std::sort( out.m_Ranks, out.m_Ranks + out.m_nCount );
		out.m_nCount = (int)( std::unique( out.m_Ranks, out.m_Ranks + out.m_nCount ) - out.m_Ranks );
		return Status::Ok;
	}
};

CPathResolver::Impl *CPathResolver::Impl::s_pInstance = nullptr;

CPathResolver::CPathResolver( CBaseFileSystem *pFileSystem )
	: m_pImpl( std::make_unique<Impl>() ), m_pFileSystem( pFileSystem )
{
	m_pImpl->m_pFS = pFileSystem;
	Impl::s_pInstance = m_pImpl.get();
	FolderCache::SetChangeCallback( Impl::OnFolderChanged );
}

CPathResolver::~CPathResolver()
{
	FolderCache::SetChangeCallback( nullptr );
	Impl::s_pInstance = nullptr;
}

void CPathResolver::OnSearchPathsChanged()
{
	std::unique_lock<std::shared_mutex> lock( m_pImpl->m_Lock );
	m_pImpl->Rebuild();
}

bool CPathResolver::GetCandidates( const char *pFileNameIn, const char *pPathID, Candidates &out, Kind kind )
{
	if ( !filesystem_resolver.GetBool() || !pFileNameIn )
		return false;

	const uint32 nSeq = m_nSeq.load( std::memory_order_acquire );
	if ( nSeq & 1 )
		return false;

	// Callers don't all hand names over the same way. Loose folders: the platform's separator, and on
	// Windows no case. Linux folders match exactly, only the name gets a case-insensitive retry.
	// VPKs lowercase everything on every platform
	char pLoose[MAX_PATH];
	char pVPK[MAX_PATH];
	size_t nLen = 0;
	for ( ; pFileNameIn[nLen]; ++nLen )
	{
		if ( nLen >= MAX_PATH - 1 )
			return false;

		char c = pFileNameIn[nLen];
		if ( c == '/' || c == '\\' )
			c = kSep;
		const char lower = ( c >= 'A' && c <= 'Z' ) ? (char)( c + 'a' - 'A' ) : c;

		pVPK[nLen] = lower;
#ifdef _WIN32
		pLoose[nLen] = lower;
#else
		pLoose[nLen] = c;
#endif
	}
	pLoose[nLen] = '\0';
	pVPK[nLen] = '\0';

	// Anything the OS would treat specially (8.3 names, .., devices): the normal walk handles those
	if ( !nLen || !FolderCache::IsSimpleRelativePath( pLoose, nLen ) )
		return false;

	Status status;
	{
		std::shared_lock<std::shared_mutex> lock( m_pImpl->m_Lock );
		status = m_pImpl->Resolve( pLoose, pVPK, nLen, pPathID, nSeq, false, kind, out );
	}

	if ( status == Status::NeedExclusive )
	{
		std::unique_lock<std::shared_mutex> lock( m_pImpl->m_Lock );
		status = m_pImpl->Resolve( pLoose, pVPK, nLen, pPathID, nSeq, true, kind, out );
	}

	return status == Status::Ok && m_nSeq.load( std::memory_order_acquire ) == nSeq;
}

bool CPathResolver::GetFindFilter( const char *pWildCard, const char *pPathID, FindFilter &out )
{
	if ( !filesystem_resolver.GetBool() || !pWildCard )
		return false;

	const uint32 nSeq = m_nSeq.load( std::memory_order_acquire );
	if ( nSeq & 1 )
		return false;

	size_t nLen = strlen( pWildCard );
	if ( !nLen || nLen >= MAX_PATH )
		return false;

	Status status;
	{
		std::shared_lock<std::shared_mutex> lock( m_pImpl->m_Lock );
		status = m_pImpl->BuildFindFilter( pWildCard, nLen, nSeq, false, out );
	}

	if ( status == Status::NeedExclusive )
	{
		std::unique_lock<std::shared_mutex> lock( m_pImpl->m_Lock );
		status = m_pImpl->BuildFindFilter( pWildCard, nLen, nSeq, true, out );
	}

	return status == Status::Ok && m_nSeq.load( std::memory_order_acquire ) == nSeq;
}

CBaseFileSystem::CSearchPath *CPathResolver::CopySearchPath( const Candidates &c, int i, void *pStorage )
{
	if ( m_nSeq.load( std::memory_order_acquire ) != c.m_nSeq )
		return nullptr;

	const View::Entry &entry = c.m_pView->m_Entries[c.m_Ranks[i]];
	auto &paths = m_pFileSystem->m_SearchPaths;
	if ( !paths.IsValidIndex( entry.m_Index ) || paths[entry.m_Index].m_storeId != entry.m_StoreId )
		return nullptr;

	auto *pCopy = new ( pStorage ) CBaseFileSystem::CSearchPath( paths[entry.m_Index] );
	if ( pCopy->GetPackFile() )
		pCopy->GetPackFile()->AddRef();
	else if ( pCopy->GetPackedStore() )
		pCopy->GetPackedStore()->AddRef();

	if ( m_nSeq.load( std::memory_order_acquire ) != c.m_nSeq )
	{
		ReleaseSearchPath( pCopy );
		return nullptr;
	}

	return pCopy;
}

void CPathResolver::ReleaseSearchPath( CBaseFileSystem::CSearchPath *pSearchPath )
{
	if ( pSearchPath )
		pSearchPath->~CSearchPath();
}

CSearchPathChangeScope::CSearchPathChangeScope( CPathResolver &resolver )
	: m_Resolver( resolver )
{
	if ( m_Resolver.m_nChangeDepth++ == 0 )
		m_Resolver.m_nSeq.fetch_add( 1, std::memory_order_acq_rel );
}

CSearchPathChangeScope::~CSearchPathChangeScope()
{
	if ( --m_Resolver.m_nChangeDepth == 0 )
	{
		m_Resolver.OnSearchPathsChanged();
		m_Resolver.m_nSeq.fetch_add( 1, std::memory_order_acq_rel );
	}
}
