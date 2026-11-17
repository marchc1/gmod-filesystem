#include "foldercache_internal.h"

#ifdef FOLDERCACHE_WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

// protected_things.h, we don't do VCR
#undef CreateThread
#undef WaitForSingleObject

using namespace FolderCache::Detail;

namespace
{
	constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES;

	HANDLE g_hThread = nullptr;
	bool g_bBackendShutdown = false;
}

// One recursive ReadDirectoryChangesW per root. Every call for it happens on the watcher thread
struct FolderCache::Detail::PlatformRoot
{
	OVERLAPPED m_Overlapped = {};
	Root *m_pRoot = nullptr;
	std::wstring m_WidePath;
	HANDLE m_hDir = INVALID_HANDLE_VALUE;
	alignas( DWORD ) BYTE m_Buffer[64 * 1024];
};

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

		unsigned char c = (unsigned char)pIn[i];
		if ( c >= 0x80 )
			return -1;
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
		else if ( c == '/' )
			c = '\\';
		pOut[i] = (char)c;
	}
	pOut[i] = '\0';
	return i;
}

static bool IsReservedDeviceName( std::string_view comp )
{
	size_t dot = comp.find( '.' );
	std::string_view base = comp.substr( 0, dot );
	while ( !base.empty() && base.back() == ' ' )
		base.remove_suffix( 1 );

	if ( base.size() == 3 )
		return base == "con" || base == "prn" || base == "aux" || base == "nul";

	if ( base.size() == 4 && ( base.substr( 0, 3 ) == "com" || base.substr( 0, 3 ) == "lpt" ) )
		return base[3] >= '0' && base[3] <= '9';

	return base == "conin$" || base == "conout$";
}

// Only plain names. Anything Win32 would rewrite or treat specially goes to the OS
bool FolderCache::Detail::IsSimpleComponent( std::string_view comp )
{
	if ( comp.empty() || comp == "." || comp == ".." )
		return false;

	if ( comp.back() == '.' || comp.back() == ' ' )
		return false;

	for ( size_t i = 0; i < comp.size(); ++i )
	{
		unsigned char c = (unsigned char)comp[i];
		if ( c < 0x20 || c >= 0x80 )
			return false;

		switch ( c )
		{
		case '<': case '>': case ':': case '"': case '|': case '?': case '*':
			return false;
		case '~':
			// 8.3 short name
			if ( i + 1 < comp.size() && comp[i + 1] >= '0' && comp[i + 1] <= '9' )
				return false;
			break;
		}
	}

	// con prn aux nul com# lpt# conin$ conout$
	const char c0 = comp[0];
	if ( c0 != 'c' && c0 != 'p' && c0 != 'a' && c0 != 'n' && c0 != 'l' )
		return true;

	return !IsReservedDeviceName( comp );
}

// NTFS compares names with its upcase table. These two are the only non-ASCII chars that upcase into ASCII
static unsigned char FoldWideChar( wchar_t wc )
{
	if ( wc < 0x80 )
	{
		unsigned char c = (unsigned char)wc;
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
		return c;
	}

	if ( wc == 0x0131 )
		return 'i';
	if ( wc == 0x017F )
		return 's';

	// Can't match any ASCII lookup
	return 0x80;
}

static void FoldWideName( const wchar_t *pName, size_t nLen, std::string &out )
{
	out.clear();
	out.reserve( nLen );
	for ( size_t i = 0; i < nLen; ++i )
		out += (char)FoldWideChar( pName[i] );
}

void FolderCache::Detail::FoldName( std::string_view name, std::string &out )
{
	out.assign( name );
	for ( char &c : out )
	{
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
	}
}

static bool HasShortNameMarker( std::string_view s )
{
	for ( size_t i = 0; i + 1 < s.size(); ++i )
	{
		if ( s[i] == '~' && s[i + 1] >= '0' && s[i + 1] <= '9' )
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Paths
//-----------------------------------------------------------------------------
bool FolderCache::Detail::IsWatchablePath( const std::string &path )
{
	if ( path.size() < 3 || path[1] != ':' || path[2] != '\\' )
		return false;

	// Network drives don't reliably send change notifications
	char drive[4] = { path[0], ':', '\\', '\0' };
	return GetDriveTypeA( drive ) == DRIVE_FIXED;
}

bool FolderCache::Detail::HasLinkBetween( const std::string &outer, const std::string &path )
{
	for ( size_t i = outer.size(); i < path.size(); ++i )
	{
		if ( path[i] != '\\' )
			continue;

		DWORD attr = GetFileAttributesA( path.substr( 0, i ).c_str() );
		if ( attr == INVALID_FILE_ATTRIBUTES )
			return false; // doesn't exist (yet), the outer watch sees it get created
		if ( attr & FILE_ATTRIBUTE_REPARSE_POINT )
			return true;
	}
	return false;
}

// Where it really is once links are followed, has to be a local drive too
static bool IsOnFixedDrive( HANDLE hDir )
{
	wchar_t final[MAX_PATH + 8];
	DWORD n = GetFinalPathNameByHandleW( hDir, final, MAX_PATH + 8, VOLUME_NAME_DOS );
	if ( !n || n >= MAX_PATH + 8 )
		return false;

	// \\?\C:\... or \\?\UNC\server\...
	if ( wcsncmp( final, L"\\\\?\\", 4 ) != 0 || final[5] != L':' )
		return false;

	wchar_t drive[4] = { final[4], L':', L'\\', 0 };
	return GetDriveTypeW( drive ) == DRIVE_FIXED;
}

//-----------------------------------------------------------------------------
// Listing
//-----------------------------------------------------------------------------

// Kernel wrote a result into the OVERLAPPED that the watcher thread hasn't handled yet
bool FolderCache::Detail::HasUnprocessedChanges( const Root &root )
{
	const PlatformRoot *p = root.m_pPlatform;
	return !p || *(volatile ULONG_PTR *)&p->m_Overlapped.Internal != STATUS_PENDING;
}

bool FolderCache::Detail::ListNode( Root &root, Node &node, std::string_view relDir )
{
	std::wstring pattern = root.m_pPlatform->m_WidePath;
	for ( char c : relDir )
		pattern += (wchar_t)(unsigned char)c;
	if ( !relDir.empty() )
		pattern += L'\\';
	pattern += L'*';

	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileExW( pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH );
	if ( hFind == INVALID_HANDLE_VALUE )
		return false;

	ClearNode( node );

	std::string name;
	do
	{
		const wchar_t *pName = fd.cFileName;
		if ( pName[0] == L'.' && ( pName[1] == 0 || ( pName[1] == L'.' && pName[2] == 0 ) ) )
			continue;

		FoldWideName( pName, wcslen( pName ), name );

		unsigned char flags = ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ? kFlagDir : 0;
		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT )
			flags |= kFlagUnsure;

		AddEntry( node, name, name, flags );
	} while ( FindNextFileW( hFind, &fd ) );

	DWORD dwError = GetLastError();
	::FindClose( hFind );

	// Something changed while we were listing, don't trust it
	if ( dwError != ERROR_NO_MORE_FILES || HasUnprocessedChanges( root ) )
	{
		ClearNode( node );
		return false;
	}

	node.m_bListed = true;
	return true;
}

//-----------------------------------------------------------------------------
// Find
//-----------------------------------------------------------------------------
namespace
{
	constexpr int kFindAll = 1; // "*"
	constexpr int kFindExt = 2; // "*.ext"
}

// kernel32 turns "*.ext" into DOS_STAR + ".ext", which matches a name ending in ".ext".
// Anything else has more DOS wildcard rules than we want to copy
bool FolderCache::Detail::ParseFindPattern( std::string_view pattern, FindPattern &out )
{
	if ( pattern == "*" )
	{
		out.m_nKind = kFindAll;
		return true;
	}

	if ( pattern.size() < 3 || pattern[0] != '*' || pattern[1] != '.' )
		return false;

	out.m_Text.assign( "." );
	for ( char c : pattern.substr( 2 ) )
	{
		const bool bOk = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if ( !bOk )
			return false;
		out.m_Text += ( c >= 'A' && c <= 'Z' ) ? (char)( c + 'a' - 'A' ) : c;
	}

	out.m_nKind = kFindExt;
	return true;
}

static bool EndsWithNoCase( const std::string &name, const std::string &lowerSuffix )
{
	if ( name.size() < lowerSuffix.size() )
		return false;

	const char *p = name.data() + name.size() - lowerSuffix.size();
	for ( size_t i = 0; i < lowerSuffix.size(); ++i )
	{
		char c = p[i];
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
		if ( c != lowerSuffix[i] )
			return false;
	}
	return true;
}

// How NTFS orders a folder's index: upcased, then by code unit. ASCII only here
static int Collate( const std::string &a, const std::string &b )
{
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for ( size_t i = 0; i < n; ++i )
	{
		unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if ( ca >= 'a' && ca <= 'z' )
			ca -= 'a' - 'A';
		if ( cb >= 'a' && cb <= 'z' )
			cb -= 'a' - 'A';
		if ( ca != cb )
			return ca < cb ? -1 : 1;
	}
	return a.size() == b.size() ? 0 : ( a.size() < b.size() ? -1 : 1 );
}

static bool IsDotEntry( const std::string &name )
{
	return name == "." || name == "..";
}

// The 8.3 name gets matched too, that's how "*.lua" finds "x.luac". Every 8.3 name has its own
// entry in the index, so a file only its short name matches comes back where that name sorts
bool FolderCache::Detail::BuildFindOrder( const FindPattern &pattern, const FindListing &listing, std::vector<uint32_t> &out )
{
	const std::vector<FindEntry> &entries = listing.m_Entries;
	out.clear();

	if ( pattern.m_nKind == kFindAll )
	{
		out.reserve( entries.size() );
		for ( uint32_t i = 0; i < (uint32_t)entries.size(); ++i )
			out.push_back( i );
		return true;
	}

	if ( pattern.m_nKind != kFindExt )
		return false;

	std::vector<bool> byShort;
	bool bAnyShort = false;
	for ( uint32_t i = 0; i < (uint32_t)entries.size(); ++i )
	{
		const FindEntry &entry = entries[i];
		const bool bLong = EndsWithNoCase( entry.m_Name, pattern.m_Text );
		const bool bShort = !entry.m_ShortName.empty() && EndsWithNoCase( entry.m_ShortName, pattern.m_Text );
		if ( !bLong && !bShort )
			continue;

		// Both match and the short one sorts first, don't know which spot NTFS picks
		if ( bLong && bShort && Collate( entry.m_ShortName, entry.m_Name ) < 0 )
			return false;

		out.push_back( i );
		byShort.push_back( !bLong );
		bAnyShort |= !bLong;
	}

	if ( bAnyShort )
	{
		std::vector<std::pair<const std::string *, uint32_t>> keyed;
		keyed.reserve( out.size() );
		for ( size_t k = 0; k < out.size(); ++k )
		{
			const FindEntry &entry = entries[out[k]];
			keyed.emplace_back( byShort[k] ? &entry.m_ShortName : &entry.m_Name, out[k] );
		}
		std::stable_sort( keyed.begin(), keyed.end(), []( const auto &a, const auto &b ) { return Collate( *a.first, *b.first ) < 0; } );
		for ( size_t k = 0; k < keyed.size(); ++k )
			out[k] = keyed[k].second;
	}

	return true;
}

static bool IsPlainAnsiName( const char *p )
{
	for ( ; *p; ++p )
	{
		// non-ASCII depends on the code page, '?' is what unmappable characters turn into
		if ( (unsigned char)*p >= 0x80 || *p == '?' )
			return false;
	}
	return true;
}

// Same call FindFirstFile makes, so the same folder, names and order
bool FolderCache::Detail::ListForFind( const std::string &pattern, FindListing &out )
{
	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileExA( pattern.c_str(), FindExInfoStandard, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH );
	if ( hFind == INVALID_HANDLE_VALUE )
		return false;

	bool bOk = true;
	do
	{
		if ( !IsPlainAnsiName( fd.cFileName ) || !IsPlainAnsiName( fd.cAlternateFileName ) )
		{
			out.m_Entries.clear();
			out.m_bUsable = false;
			break;
		}

		FindEntry &entry = out.m_Entries.emplace_back();
		entry.m_Name = fd.cFileName;
		entry.m_ShortName = fd.cAlternateFileName;
		entry.m_bDir = ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
	} while ( FindNextFileA( hFind, &fd ) );

	if ( out.m_bUsable && GetLastError() != ERROR_NO_MORE_FILES )
		bOk = false;

	// . and .. come first no matter what. The rest has to be in the order Collate says,
	// not true for FAT and friends, which keep the order things were made in
	if ( bOk && out.m_bUsable )
	{
		const std::string *pPrev = nullptr;
		for ( const FindEntry &entry : out.m_Entries )
		{
			if ( IsDotEntry( entry.m_Name ) )
				continue;
			if ( pPrev && Collate( *pPrev, entry.m_Name ) >= 0 )
			{
				out.m_Entries.clear();
				out.m_bUsable = false;
				break;
			}
			pPrev = &entry.m_Name;
		}
	}

	::FindClose( hFind );
	return bOk;
}

//-----------------------------------------------------------------------------
// Watcher thread. All ReadDirectoryChangesW calls happen on it
//-----------------------------------------------------------------------------
static void CALLBACK OnChanges( DWORD dwError, DWORD dwBytes, LPOVERLAPPED pOverlapped );

static bool Arm( PlatformRoot &p )
{
	p.m_Overlapped = {};
	p.m_Overlapped.Internal = STATUS_PENDING;
	return ReadDirectoryChangesW( p.m_hDir, p.m_Buffer, sizeof( p.m_Buffer ), TRUE, kNotifyFilter, nullptr, &p.m_Overlapped, OnChanges ) != FALSE;
}

static void CloseWatch( Root &root )
{
	DropRoot( root );

	PlatformRoot &p = *root.m_pPlatform;
	if ( p.m_hDir != INVALID_HANDLE_VALUE )
	{
		CloseHandle( p.m_hDir );
		p.m_hDir = INVALID_HANDLE_VALUE;
	}
}

static void ProcessChanges( Root &root, DWORD dwBytes )
{
	std::string rel;
	const BYTE *p = root.m_pPlatform->m_Buffer;
	const BYTE *end = p + dwBytes;
	while ( p + sizeof( FILE_NOTIFY_INFORMATION ) <= end )
	{
		auto *info = (const FILE_NOTIFY_INFORMATION *)p;
		FoldWideName( info->FileName, info->FileNameLength / sizeof( wchar_t ), rel );

		// It's allowed to report the short name, which we can't map back
		if ( HasShortNameMarker( rel ) )
		{
			ResetTree( root );
			return;
		}

		if ( info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME )
			MarkRemoved( root, rel );
		else
			MarkChanged( root, rel );

		if ( g_Roots.size() > 1 )
			RetireRootsUnder( root.m_Path + rel );

		if ( !info->NextEntryOffset )
			break;
		p += info->NextEntryOffset;
	}
}

static void CALLBACK OnChanges( DWORD dwError, DWORD dwBytes, LPOVERLAPPED pOverlapped )
{
	PlatformRoot *p = CONTAINING_RECORD( pOverlapped, PlatformRoot, m_Overlapped );
	Root &root = *p->m_pRoot;

	std::unique_lock<std::shared_mutex> lock( g_Lock );

	if ( g_bBackendShutdown || !root.m_bActive || dwError == ERROR_OPERATION_ABORTED )
	{
		CloseWatch( root );
	}
	else if ( dwError == ERROR_SUCCESS || dwError == ERROR_NOTIFY_ENUM_DIR )
	{
		// 0 bytes = the change buffer overflowed
		if ( dwError == ERROR_SUCCESS && dwBytes > 0 )
			ProcessChanges( root, dwBytes );
		else
			ResetTree( root );

		if ( !Arm( *p ) )
			CloseWatch( root );
	}
	else
	{
		// Folder got deleted or the volume went away
		CloseWatch( root );
	}
}

struct ArmRequest
{
	Root *m_pRoot;
	HANDLE m_hDone;
};

static void CALLBACK ArmApc( ULONG_PTR param )
{
	auto *req = (ArmRequest *)param;
	Root &root = *req->m_pRoot;

	{
		std::unique_lock<std::shared_mutex> lock( g_Lock );
		if ( Arm( *root.m_pPlatform ) )
			root.m_bActive = true;
		else
			CloseWatch( root );
	}

	SetEvent( req->m_hDone );
}

static void CALLBACK ShutdownApc( ULONG_PTR )
{
	std::unique_lock<std::shared_mutex> lock( g_Lock );
	g_bBackendShutdown = true;
	for ( Root *root : g_Roots )
		CancelIoEx( root->m_pPlatform->m_hDir, &root->m_pPlatform->m_Overlapped );
}

static DWORD WINAPI WatcherThread( void * )
{
	while ( true )
	{
		SleepEx( INFINITE, TRUE );

		std::shared_lock<std::shared_mutex> lock( g_Lock );
		if ( g_bBackendShutdown && g_Roots.empty() )
			return 0;
	}
}

//-----------------------------------------------------------------------------
// Roots
//-----------------------------------------------------------------------------
bool FolderCache::Detail::PrepareRoot( Root &root )
{
	auto p = std::make_unique<PlatformRoot>();
	p->m_pRoot = &root;

	int nWide = MultiByteToWideChar( CP_ACP, 0, root.m_Path.c_str(), (int)root.m_Path.size(), nullptr, 0 );
	p->m_WidePath.resize( nWide );
	MultiByteToWideChar( CP_ACP, 0, root.m_Path.c_str(), (int)root.m_Path.size(), p->m_WidePath.data(), nWide );

	p->m_hDir = CreateFileW( p->m_WidePath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr );
	if ( p->m_hDir == INVALID_HANDLE_VALUE )
		return false;

	if ( !IsOnFixedDrive( p->m_hDir ) )
	{
		CloseHandle( p->m_hDir );
		return false;
	}

	if ( !g_hThread )
	{
		g_hThread = CreateThread( nullptr, 64 * 1024, WatcherThread, nullptr, 0, nullptr );
		if ( !g_hThread )
		{
			CloseHandle( p->m_hDir );
			return false;
		}
		SetThreadPriority( g_hThread, THREAD_PRIORITY_HIGHEST );
	}

	// Lives as long as the root, roots are never freed
	root.m_pPlatform = p.release();
	return true;
}

void FolderCache::Detail::ActivateRoot( Root &root )
{
	ArmRequest req = { &root, CreateEventW( nullptr, TRUE, FALSE, nullptr ) };
	if ( req.m_hDone && QueueUserAPC( ArmApc, g_hThread, (ULONG_PTR)&req ) )
		WaitForSingleObject( req.m_hDone, INFINITE );

	if ( req.m_hDone )
		CloseHandle( req.m_hDone );
}

void FolderCache::Detail::RetireRoot( Root &root )
{
	// The aborted completion closes the handle on the watcher thread
	if ( root.m_pPlatform )
		CancelIoEx( root.m_pPlatform->m_hDir, &root.m_pPlatform->m_Overlapped );
}

void FolderCache::Detail::ShutdownBackend()
{
	if ( !g_hThread )
		return;

	QueueUserAPC( ShutdownApc, g_hThread, 0 );
	WaitForSingleObject( g_hThread, 2000 );
	CloseHandle( g_hThread );
	g_hThread = nullptr;
}

#endif
