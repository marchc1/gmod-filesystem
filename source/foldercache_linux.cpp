#include "foldercache_internal.h"

#ifdef FOLDERCACHE_LINUX

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

using namespace FolderCache::Detail;

// Nothing per root, inotify watches are per folder
struct FolderCache::Detail::PlatformRoot
{
};

namespace
{
	// inotify isn't recursive, every folder we list gets a watch. It follows symlinks
	constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

	int g_nInotify = -1;
	int g_nWake = -1; // eventfd, stops the thread
	std::thread g_Thread;
	std::mutex g_InitMutex;
	PlatformRoot g_PlatformRoot;

	// Set before the thread reads events, cleared once they're applied under the lock
	std::atomic<int> g_nApplying{ 0 };

	struct WatchTarget
	{
		Root *m_pRoot;
		std::string m_RelDir;
	};

	// wd -> every folder it stands for. The same folder through two paths (a symlink) gets one wd
	unordered_map<int, std::vector<WatchTarget>> g_Watches;
}

//-----------------------------------------------------------------------------
// Names
//-----------------------------------------------------------------------------
int FolderCache::Detail::NormalizePath( const char *pIn, char *pOut, int nOutSize )
{
	int i = 0;
	for ( ; pIn[i]; ++i )
	{
		if ( i >= nOutSize - 1 || i >= kMaxPath - 1 )
			return -1;

		char c = pIn[i];
		pOut[i] = c == '\\' ? '/' : c;
	}
	pOut[i] = '\0';
	return i;
}

bool FolderCache::Detail::IsSimpleComponent( std::string_view comp )
{
	return !comp.empty() && comp != "." && comp != "..";
}

// What V_strieq in findFileInDirCaseInsensitive treats as equal, ASCII only
void FolderCache::Detail::FoldName( std::string_view name, std::string &out )
{
	out.assign( name );
	for ( char &c : out )
	{
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
	}
}

//-----------------------------------------------------------------------------
// Paths
//-----------------------------------------------------------------------------
bool FolderCache::Detail::IsWatchablePath( const std::string &path )
{
	if ( path.empty() || path[0] != '/' )
		return false;

	struct statfs info;
	if ( statfs( path.c_str(), &info ) != 0 )
		return false;

	// Changes made on another machine never show up in inotify
	switch ( (unsigned long)info.f_type )
	{
	case 0x6969: // NFS
	case 0x517B: // SMB
	case 0xFF534D42: // CIFS
	case 0xFE534D42: // SMB2
	case 0x65735546: // FUSE
	case 0x01021997: // 9P (WSL's /mnt/c too)
	case 0x00C36400: // Ceph
	case 0x5346414F: // AFS
	case 0x73757245: // Coda
	case 0x564C: // NCP
		return false;
	}
	return true;
}

// Every listed folder has its own watch, through links too
bool FolderCache::Detail::HasLinkBetween( const std::string &, const std::string & )
{
	return false;
}

//-----------------------------------------------------------------------------
// Listing
//-----------------------------------------------------------------------------
static int PendingEventBytes()
{
	int n = 0;
	if ( g_nInotify < 0 || ioctl( g_nInotify, FIONREAD, &n ) != 0 )
		return 1;
	return n;
}

bool FolderCache::Detail::HasUnprocessedChanges( const Root & )
{
	return g_nApplying.load( std::memory_order_acquire ) != 0 || PendingEventBytes() > 0;
}

// No stored file data here, stat always goes to the OS
bool FolderCache::Detail::HasUnprocessedDataChanges( const Root & )
{
	return true;
}

static void AddWatchTarget( int wd, Root &root, std::string_view relDir )
{
	auto &targets = g_Watches[wd];
	for ( const WatchTarget &t : targets )
	{
		if ( t.m_pRoot == &root && t.m_RelDir == relDir )
			return;
	}
	targets.push_back( { &root, std::string( relDir ) } );
}

bool FolderCache::Detail::ListNode( Root &root, Node &node, std::string_view relDir )
{
	std::string path = root.m_Path;
	path.append( relDir );

	// Watch first, so nothing slips in between listing and watching
	int wd = inotify_add_watch( g_nInotify, path.c_str(), kWatchMask );
	if ( wd < 0 )
		return false;
	AddWatchTarget( wd, root, relDir );

	DIR *pDir = opendir( path.c_str() );
	if ( !pDir )
		return false;

	ClearNode( node );

	if ( path.back() != '/' )
		path += '/';
	const size_t nBase = path.size();

	std::string folded;
	while ( dirent *pEntry = readdir( pDir ) )
	{
		const char *pName = pEntry->d_name;
		if ( pName[0] == '.' && ( pName[1] == 0 || ( pName[1] == '.' && pName[2] == 0 ) ) )
			continue;

		unsigned char flags = 0;
		if ( pEntry->d_type == DT_DIR )
		{
			flags = kFlagDir;
		}
		else if ( pEntry->d_type == DT_LNK || pEntry->d_type == DT_UNKNOWN )
		{
			// Links count as what they point at, their folder gets its own watch when listed
			path.resize( nBase );
			path += pName;
			struct stat st;
			if ( stat( path.c_str(), &st ) == 0 )
				flags = S_ISDIR( st.st_mode ) ? kFlagDir : 0;
			else
				flags = kFlagUnsure; // broken link, let the OS say
		}

		std::string_view name( pName );
		FoldName( name, folded );
		AddEntry( node, name, folded, flags );
	}
	closedir( pDir );

	// Something changed while we were listing, don't trust it
	if ( HasUnprocessedChanges( root ) )
	{
		ClearNode( node );
		return false;
	}

	node.m_bListed = true;
	return true;
}

//-----------------------------------------------------------------------------
// Watcher thread
//-----------------------------------------------------------------------------

// A watch is a folder, the path we know it by stops being true once something at that path
// gets removed or replaced. Events after that would land on whatever lives there now
static void DropTargetsUnder( const Root &root, std::string_view rel )
{
	for ( auto &entry : g_Watches )
	{
		auto &targets = entry.second;
		targets.erase( std::remove_if( targets.begin(), targets.end(), [&]( const WatchTarget &t ) {
			if ( t.m_pRoot != &root )
				return false;
			const std::string &d = t.m_RelDir;
			return d.size() >= rel.size() && d.compare( 0, rel.size(), rel ) == 0 && ( d.size() == rel.size() || d[rel.size()] == '/' );
		} ), targets.end() );
	}
}

static void ApplyEvents( const char *pBuf, ssize_t nBytes )
{
	std::string rel;
	for ( const char *p = pBuf; p < pBuf + nBytes; )
	{
		auto *ev = (const inotify_event *)p;
		p += sizeof( inotify_event ) + ev->len;

		if ( ev->mask & IN_Q_OVERFLOW )
		{
			// Lost events, start over. Listing again registers the watches again
			for ( Root *root : g_Roots )
				ResetTree( *root );
			for ( auto &entry : g_Watches )
				entry.second.clear();
			continue;
		}

		auto it = g_Watches.find( ev->wd );
		if ( it == g_Watches.end() )
			continue;

		if ( ev->mask & IN_IGNORED )
		{
			g_Watches.erase( it );
			continue;
		}

		if ( ev->mask & ( IN_DELETE_SELF | IN_MOVE_SELF ) )
		{
			// The parent's watch reports it as a name going away, only roots have no parent watched
			for ( const WatchTarget &t : it->second )
			{
				if ( t.m_pRoot->m_bActive && t.m_RelDir.empty() )
					ResetTree( *t.m_pRoot );
			}
			it->second.clear();
			continue;
		}

		// Copy, dropping targets below can change this list
		const std::vector<WatchTarget> targets = it->second;
		for ( const WatchTarget &t : targets )
		{
			if ( !t.m_pRoot->m_bActive )
				continue;

			if ( !ev->len )
				continue;

			rel = t.m_RelDir;
			if ( !rel.empty() )
				rel += '/';
			rel += ev->name;

			// A folder (or a link to one) going away or arriving at this path
			DropTargetsUnder( *t.m_pRoot, rel );

			// Events come in order, anything that happens after this gets its own event
			if ( ev->mask & ( IN_DELETE | IN_MOVED_FROM ) )
			{
				MarkRemoved( *t.m_pRoot, rel );
			}
			else
			{
				struct stat st;
				if ( stat( ( t.m_pRoot->m_Path + rel ).c_str(), &st ) == 0 )
					MarkAdded( *t.m_pRoot, rel, S_ISDIR( st.st_mode ) ? kFlagDir : 0 );
				else
					MarkChanged( *t.m_pRoot, rel ); // gone again or a broken link
			}
		}
	}
}

static void WatcherLoop()
{
	alignas( inotify_event ) char buf[64 * 1024];
	pollfd fds[2] = { { g_nInotify, POLLIN, 0 }, { g_nWake, POLLIN, 0 } };

	while ( true )
	{
		if ( poll( fds, 2, -1 ) < 0 )
		{
			if ( errno == EINTR )
				continue;
			return;
		}

		if ( fds[1].revents )
			return;

		if ( !( fds[0].revents & POLLIN ) )
			continue;

		g_nApplying.store( 1, std::memory_order_release );
		ssize_t n = read( g_nInotify, buf, sizeof( buf ) );

		std::unique_lock<std::shared_mutex> lock( g_Lock );
		if ( n > 0 )
			ApplyEvents( buf, n );
		g_nApplying.store( 0, std::memory_order_release );
	}
}

//-----------------------------------------------------------------------------
// Find, not from memory here yet. todo: scandir + FileSelect + alphasort on the cached names
//-----------------------------------------------------------------------------
bool FolderCache::Detail::ParseFindPattern( std::string_view, FindPattern & )
{
	return false;
}

bool FolderCache::Detail::BuildFindOrder( const FindPattern &, const FindListing &, std::vector<uint32_t> & )
{
	return false;
}

bool FolderCache::Detail::ListForFind( const std::string &, FindListing & )
{
	return false;
}

//-----------------------------------------------------------------------------
// Roots
//-----------------------------------------------------------------------------
bool FolderCache::Detail::PrepareRoot( Root &root )
{
	std::lock_guard<std::mutex> lock( g_InitMutex );
	if ( g_nInotify < 0 )
	{
		g_nInotify = inotify_init1( IN_NONBLOCK | IN_CLOEXEC );
		if ( g_nInotify < 0 )
			return false;

		g_nWake = eventfd( 0, EFD_CLOEXEC );
		if ( g_nWake < 0 )
		{
			close( g_nInotify );
			g_nInotify = -1;
			return false;
		}

		g_Thread = std::thread( WatcherLoop );
	}

	root.m_pPlatform = &g_PlatformRoot;
	return true;
}

void FolderCache::Detail::ActivateRoot( Root &root )
{
	std::unique_lock<std::shared_mutex> lock( g_Lock );
	root.m_bActive = true;
}

void FolderCache::Detail::RetireRoot( Root & )
{
	// Its watches stay, events for it land on a tree nobody looks at
}

void FolderCache::Detail::ShutdownBackend()
{
	std::lock_guard<std::mutex> lock( g_InitMutex );
	if ( g_nInotify < 0 )
		return;

	uint64_t one = 1;
	const bool bWoken = write( g_nWake, &one, sizeof( one ) ) == sizeof( one );
	if ( g_Thread.joinable() )
	{
		// Can't wake it, don't hang shutdown on it
		if ( bWoken )
			g_Thread.join();
		else
			g_Thread.detach();
	}

	close( g_nWake );
	close( g_nInotify );
	g_nWake = -1;
	g_nInotify = -1;
}

#endif
