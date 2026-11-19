#include "dylibpatches.h"

#ifdef EXTERNAL_DYLIB_PATCHES

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <detouring/hook.hpp>

#include "networkstringtabledefs.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"

// protected_things.h, we don't do VCR
#undef CreateThread

namespace
{
	enum Bucket
	{
		kRead,
		kPrep,
		kCompile,
		kRun,
		kBucketCount,
	};

	enum Realm
	{
		kMenu,
		kClient,
		kServer,
		kUnknown,
		kRealmCount,
	};

	enum class FrameKind
	{
		Read,
		Compile,
		Script,
	};

	const char *const kRealmNames[kRealmCount] = { "menu", "client", "server", "unknown" };

	constexpr int kMaxDepth = 128;
	constexpr double kStartupGapSeconds = 10.0;

	struct Totals
	{
		int64_t m_Time[kBucketCount] = {};
		int64_t m_nScripts = 0;
		int64_t m_nBytes = 0;
	};

	struct FileStats
	{
		Totals m_Totals;
		int64_t m_nFirst = 0;
	};

	struct RealmStats
	{
		std::unordered_map<std::string, FileStats> m_Files;
		Totals m_Total;
		Totals m_Startup;
		int64_t m_nFirst = 0;
		int64_t m_nLast = 0;
		int64_t m_nStartupEnd = 0;
		bool m_bSettled = false;
	};

	struct State
	{
		std::mutex m_Lock;
		RealmStats m_Realms[kRealmCount];
		int64_t m_nFrequency = 1;
		int64_t m_nStart = 0;
		std::string m_OutputDir;
	};

	struct Frame
	{
		FrameKind m_Kind;
		int m_nRealm;
		const char *m_pName;
		const char *m_pPrefix;
		int64_t m_nStart;
		int64_t m_nChildren;
		int64_t m_nPrep;
		int64_t m_nCompile;
		size_t m_nBytes;
		bool m_bRun;
		bool m_bCompiled;
		bool m_bDirectCompile;
	};

	// Never freed, the game can still call in during shutdown
	State *g_pState = nullptr;

	thread_local Frame *t_pStack = nullptr;
	thread_local int t_nDepth = 0;
	thread_local int t_nLastRealm = kMenu;

	int64_t Now()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter( &value );
		return value.QuadPart;
	}

	std::string MakeKey( const char *pPrefix, const char *pName )
	{
		std::string key;
		if ( pPrefix )
			key += *pPrefix == '@' ? pPrefix + 1 : pPrefix;
		if ( pName )
			key += *pName == '@' ? pName + 1 : pName;

		std::replace( key.begin(), key.end(), '\\', '/' );
		return key;
	}

	void AddTotals( Totals &to, const Totals &from )
	{
		for ( int i = 0; i < kBucketCount; ++i )
			to.m_Time[i] += from.m_Time[i];

		to.m_nScripts += from.m_nScripts;
		to.m_nBytes += from.m_nBytes;
	}

	void Record( const Frame &frame, const Totals &delta )
	{
		std::string key = MakeKey( frame.m_pPrefix, frame.m_pName );

		std::lock_guard<std::mutex> lock( g_pState->m_Lock );
		RealmStats &realm = g_pState->m_Realms[frame.m_nRealm];
		FileStats &file = realm.m_Files[key];
		if ( !file.m_nFirst )
			file.m_nFirst = frame.m_nStart;

		AddTotals( file.m_Totals, delta );
		AddTotals( realm.m_Total, delta );
	}

	// Times one call, minus whatever nested calls took
	class Scope
	{
	public:
		Scope( FrameKind kind, int nRealm, const char *pName, const char *pPrefix, size_t nBytes, bool bRun )
		{
			if ( !t_pStack )
				t_pStack = new Frame[kMaxDepth];

			m_nDepth = t_nDepth++;
			if ( m_nDepth >= kMaxDepth )
				return;

			Frame *pParent = m_nDepth > 0 ? &t_pStack[m_nDepth - 1] : nullptr;
			Frame &frame = t_pStack[m_nDepth];
			m_pFrame = &frame;

			if ( nRealm < 0 )
				nRealm = pParent ? pParent->m_nRealm : t_nLastRealm;
			t_nLastRealm = nRealm;

			frame = {};
			frame.m_Kind = kind;
			frame.m_nRealm = nRealm;
			frame.m_pName = pName;
			frame.m_pPrefix = pPrefix;
			frame.m_nBytes = nBytes;
			frame.m_bRun = bRun;

			int64_t nNow = Now();
			if ( kind == FrameKind::Compile && pParent && pParent->m_Kind == FrameKind::Script && !pParent->m_bCompiled )
			{
				pParent->m_bCompiled = true;
				pParent->m_nPrep = nNow - pParent->m_nStart - pParent->m_nChildren;
				pParent->m_nBytes = nBytes;
				frame.m_bDirectCompile = true;
			}

			if ( m_nDepth == 0 )
				BeginOutermost( nRealm, nNow );

			frame.m_nStart = Now();
		}

		~Scope()
		{
			if ( m_pFrame )
				Finish();

			// Lua errors can skip the scopes above us
			t_nDepth = m_nDepth;
		}

	private:
		void BeginOutermost( int nRealm, int64_t nNow )
		{
			std::lock_guard<std::mutex> lock( g_pState->m_Lock );
			RealmStats &realm = g_pState->m_Realms[nRealm];
			if ( !realm.m_nFirst )
			{
				realm.m_nFirst = nNow;
			}
			else if ( !realm.m_bSettled && nNow - realm.m_nLast > int64_t( kStartupGapSeconds * g_pState->m_nFrequency ) )
			{
				realm.m_bSettled = true;
				realm.m_Startup = realm.m_Total;
				realm.m_nStartupEnd = realm.m_nLast;
			}
		}

		void Finish()
		{
			Frame &frame = *m_pFrame;
			int64_t nEnd = Now();
			int64_t nTotal = nEnd - frame.m_nStart;
			int64_t nOwn = nTotal - frame.m_nChildren;

			Frame *pParent = m_nDepth > 0 ? &t_pStack[m_nDepth - 1] : nullptr;
			if ( pParent )
				pParent->m_nChildren += nTotal;

			Totals delta;
			switch ( frame.m_Kind )
			{
			case FrameKind::Read:
				delta.m_Time[kRead] = nOwn;
				Record( frame, delta );
				break;

			case FrameKind::Compile:
				if ( frame.m_bDirectCompile )
				{
					pParent->m_nCompile += nOwn;
				}
				else
				{
					delta.m_Time[kCompile] = nOwn;
					Record( frame, delta );
				}
				break;

			case FrameKind::Script:
				nOwn -= frame.m_nPrep;
				delta.m_Time[kPrep] = frame.m_nPrep + ( frame.m_bRun ? 0 : nOwn );
				delta.m_Time[kCompile] = frame.m_nCompile;
				delta.m_Time[kRun] = frame.m_bRun ? nOwn : 0;
				delta.m_nScripts = 1;
				delta.m_nBytes = frame.m_nBytes;
				Record( frame, delta );
				break;
			}

			if ( m_nDepth == 0 )
			{
				std::lock_guard<std::mutex> lock( g_pState->m_Lock );
				g_pState->m_Realms[frame.m_nRealm].m_nLast = nEnd;
			}
		}

		Frame *m_pFrame = nullptr;
		int m_nDepth = 0;
	};

	// Hooks
	using LoadBufferXFn = int ( * )( void *L, const char *pBuffer, size_t nSize, const char *pName, const char *pMode );
	using RunStringExFn = bool ( * )( void *pThis, const char *pFile, const char *pPath, const char *pSource, bool bRun, bool bPrintErrors, bool bDontPushErrors, bool bNoReturns );
	using LoadFileFn = void *( * )( void *pThis, const std::string &path, const std::string &pathId, bool bFromDatatable, bool bFromFile );

	LoadBufferXFn g_pfnLoadBufferX = nullptr;
	RunStringExFn g_pfnRunStringEx = nullptr;
	LoadFileFn g_pfnLoadFile = nullptr;

	// ILuaInterface vtable, checked against RunStringEx when installing
	constexpr int kIsServerOffset = 0x258;
	constexpr int kIsMenuOffset = 0x268;

	int GetRealm( void *pLua )
	{
		using BoolFn = bool ( * )( void * );
		const uint8_t *pVTable = *reinterpret_cast<const uint8_t **>( pLua );

		if ( ( *reinterpret_cast<BoolFn const *>( pVTable + kIsServerOffset ) )( pLua ) )
			return kServer;

		if ( ( *reinterpret_cast<BoolFn const *>( pVTable + kIsMenuOffset ) )( pLua ) )
			return kMenu;

		return kClient;
	}

	int Hook_LoadBufferX( void *L, const char *pBuffer, size_t nSize, const char *pName, const char *pMode )
	{
		Scope scope( FrameKind::Compile, -1, pName, nullptr, nSize, false );
		return g_pfnLoadBufferX( L, pBuffer, nSize, pName, pMode );
	}

	bool Hook_RunStringEx( void *pThis, const char *pFile, const char *pPath, const char *pSource, bool bRun, bool bPrintErrors, bool bDontPushErrors, bool bNoReturns )
	{
		Scope scope( FrameKind::Script, GetRealm( pThis ), pFile, pPath, 0, bRun );
		return g_pfnRunStringEx( pThis, pFile, pPath, pSource, bRun, bPrintErrors, bDontPushErrors, bNoReturns );
	}

	// The header only lists 4 args but the function forwards a 5th
	void *Hook_LoadFile( void *pThis, const std::string &path, const std::string &pathId, bool bFromDatatable, bool bFromFile )
	{
		Scope scope( FrameKind::Read, -1, path.c_str(), nullptr, 0, false );
		return g_pfnLoadFile( pThis, path, pathId, bFromDatatable, bFromFile );
	}

	// Finding things in lua_shared
	struct Range
	{
		const uint8_t *m_pStart = nullptr;
		size_t m_nSize = 0;
	};

	Range FindSection( HMODULE hModule, const char *pName )
	{
		const uint8_t *pBase = reinterpret_cast<const uint8_t *>( hModule );
		auto *pDos = reinterpret_cast<const IMAGE_DOS_HEADER *>( pBase );
		auto *pNt = reinterpret_cast<const IMAGE_NT_HEADERS *>( pBase + pDos->e_lfanew );
		const IMAGE_SECTION_HEADER *pSection = IMAGE_FIRST_SECTION( pNt );

		for ( WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i )
		{
			if ( !strncmp( reinterpret_cast<const char *>( pSection[i].Name ), pName, IMAGE_SIZEOF_SHORT_NAME ) )
				return { pBase + pSection[i].VirtualAddress, pSection[i].Misc.VirtualSize };
		}

		return {};
	}

	const uint8_t *FindString( const Range &range, const char *pString )
	{
		size_t nLen = strlen( pString ) + 1;
		if ( range.m_nSize < nLen )
			return nullptr;

		for ( size_t i = 0; i <= range.m_nSize - nLen; ++i )
		{
			if ( ( i == 0 || range.m_pStart[i - 1] == 0 ) && !memcmp( range.m_pStart + i, pString, nLen ) )
				return range.m_pStart + i;
		}

		return nullptr;
	}

	const uint8_t *FunctionStart( const uint8_t *pAddress )
	{
		DWORD64 nBase = 0;
		PRUNTIME_FUNCTION pFunction = RtlLookupFunctionEntry( reinterpret_cast<DWORD64>( pAddress ), &nBase, nullptr );

		// Cold parts of a function chain back to the real start
		while ( pFunction )
		{
			if ( pFunction->UnwindData & 1 )
			{
				pFunction = reinterpret_cast<PRUNTIME_FUNCTION>( nBase + ( pFunction->UnwindData & ~DWORD( 1 ) ) );
				continue;
			}

			const uint8_t *pUnwind = reinterpret_cast<const uint8_t *>( nBase + pFunction->UnwindData );
			if ( !( ( pUnwind[0] >> 3 ) & UNW_FLAG_CHAININFO ) )
				return reinterpret_cast<const uint8_t *>( nBase + pFunction->BeginAddress );

			int nCodes = ( pUnwind[2] + 1 ) & ~1;
			pFunction = reinterpret_cast<PRUNTIME_FUNCTION>( const_cast<uint8_t *>( pUnwind + 4 + nCodes * 2 ) );
		}

		return nullptr;
	}

	// The one function with a rip relative reference to pTarget. nOpcode -1 = any instruction
	const uint8_t *FindReferencingFunction( const Range &text, const uint8_t *pTarget, int nOpcode )
	{
		if ( !pTarget || text.m_nSize < 5 )
			return nullptr;

		const uint8_t *pFound = nullptr;
		for ( size_t i = 1; i <= text.m_nSize - 4; ++i )
		{
			int32_t nRelative;
			memcpy( &nRelative, text.m_pStart + i, sizeof( nRelative ) );
			if ( text.m_pStart + i + 4 + nRelative != pTarget )
				continue;

			if ( nOpcode != -1 && text.m_pStart[i - 1] != nOpcode )
				continue;

			const uint8_t *pFunction = FunctionStart( text.m_pStart + i );
			if ( !pFunction )
				continue;

			if ( pFound && pFound != pFunction )
				return nullptr;

			pFound = pFunction;
		}

		return pFound;
	}

	bool HasVirtualCall( const uint8_t *pFunction, const Range &text, int nOffset )
	{
		// call qword [rax + nOffset]
		const uint8_t pattern[6] = { 0xFF, 0x90, uint8_t( nOffset ), uint8_t( nOffset >> 8 ), 0, 0 };

		size_t nOffsetInText = pFunction - text.m_pStart;
		size_t nWindow = std::min<size_t>( 0x600, text.m_nSize - nOffsetInText );
		for ( size_t i = 0; i + sizeof( pattern ) <= nWindow; ++i )
		{
			if ( !memcmp( pFunction + i, pattern, sizeof( pattern ) ) )
				return true;
		}

		return false;
	}

	bool InstallHook( void *pTarget, void *pDetour, void **ppTrampoline, std::vector<Detouring::Hook *> &hooks )
	{
		// Not deleted on purpose, lua_shared might be gone when destructors run
		auto *pHook = new Detouring::Hook;
		if ( !pHook->Create( Detouring::Hook::Target( pTarget ), pDetour ) )
			return false;

		*ppTrampoline = pHook->GetTrampoline();
		hooks.push_back( pHook );
		return true;
	}

	// Game code points into us from now on
	void PinSelf()
	{
		HMODULE hSelf;
		GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>( &PinSelf ), &hSelf );
	}

	// The garrysmod folder, from a dll in garrysmod/bin/win64
	std::string GameDirFromModule( HMODULE hModule )
	{
		char szPath[MAX_PATH];
		DWORD nLen = GetModuleFileNameA( hModule, szPath, sizeof( szPath ) );
		if ( !nLen || nLen >= sizeof( szPath ) )
			return {};

		std::string dir = szPath;
		for ( int i = 0; i < 3; ++i )
		{
			size_t nSlash = dir.find_last_of( "\\/" );
			if ( nSlash == std::string::npos )
				break;

			dir.resize( nSlash );
		}

		return dir + '\\';
	}

	void InstallLuaTiming( HMODULE hLuaShared )
	{
		Range text = FindSection( hLuaShared, ".text" );
		Range rdata = FindSection( hLuaShared, ".rdata" );

		void *pLoadBufferX = reinterpret_cast<void *>( GetProcAddress( hLuaShared, "luaL_loadbufferx" ) );

		const uint8_t *pRunStringEx = FindReferencingFunction( text, FindString( rdata, "DEFINE_BASECLASS" ), -1 );

		// LoadFile is the only caller of the function with this message
		const uint8_t *pDatatableLoad = FindReferencingFunction( text, FindString( rdata, "File in nosend - but we don't have it! (%s)\n" ), -1 );
		const uint8_t *pLoadFile = FindReferencingFunction( text, pDatatableLoad, 0xE8 );

		if ( !pLoadBufferX || !pRunStringEx || !pLoadFile )
		{
			Warning( "DylibPatches: Lua timing is off, couldn't find loadbufferx=%p RunStringEx=%p LoadFile=%p\n", pLoadBufferX, pRunStringEx, pLoadFile );
			return;
		}

		if ( !HasVirtualCall( pRunStringEx, text, kIsServerOffset ) || !HasVirtualCall( pRunStringEx, text, kIsMenuOffset ) )
		{
			Warning( "DylibPatches: Lua timing is off, ILuaInterface vtable changed\n" );
			return;
		}

		auto *pState = new State;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency( &frequency );
		pState->m_nFrequency = frequency.QuadPart;
		pState->m_nStart = Now();

		pState->m_OutputDir = GameDirFromModule( hLuaShared );

		g_pState = pState;

		std::vector<Detouring::Hook *> hooks;
		bool bOk = InstallHook( pLoadBufferX, reinterpret_cast<void *>( &Hook_LoadBufferX ), reinterpret_cast<void **>( &g_pfnLoadBufferX ), hooks )
			&& InstallHook( const_cast<uint8_t *>( pRunStringEx ), reinterpret_cast<void *>( &Hook_RunStringEx ), reinterpret_cast<void **>( &g_pfnRunStringEx ), hooks )
			&& InstallHook( const_cast<uint8_t *>( pLoadFile ), reinterpret_cast<void *>( &Hook_LoadFile ), reinterpret_cast<void **>( &g_pfnLoadFile ), hooks );

		for ( size_t i = 0; bOk && i < hooks.size(); ++i )
			bOk = hooks[i]->Enable();

		if ( !bOk )
		{
			for ( Detouring::Hook *pHook : hooks )
				pHook->Destroy();

			g_pState = nullptr;
			Warning( "DylibPatches: Lua timing is off, hooking failed\n" );
			return;
		}

		PinSelf();

		Msg( "DylibPatches: Lua timing on, use filesystem_luatiming\n" );
	}

	// Lua file lookup in client.dll
	// It walks the whole Lua string table with strcmp for every file it loads
	ConVar filesystem_lualookup( "filesystem_lualookup", "1", 0, "Find Lua files in the string table with its hash lookup instead of walking it" );

	constexpr int kLookupMissing = -1;
	constexpr int kLookupUseLoop = -2;

	std::atomic<int64_t> g_nLookupFound{ 0 };
	std::atomic<int64_t> g_nLookupMissing{ 0 };
	std::atomic<int64_t> g_nLookupLoop{ 0 };
	std::atomic<int64_t> g_nLookupVerified{ 0 };
	std::atomic<int64_t> g_nLookupMismatches{ 0 };
	bool g_bLookupInstalled = false;
	bool g_bLookupVerify = false;

	int FindLinear( INetworkStringTable *pTable, const char *pName )
	{
		int nCount = pTable->GetNumStrings();
		for ( int i = 0; i < nCount; ++i )
		{
			if ( !strcmp( pTable->GetString( i ), pName ) )
				return i;
		}

		return kLookupMissing;
	}

	int FindHashed( INetworkStringTable *pTable, const char *pName )
	{
		int nIndex = pTable->FindStringIndex( pName );
		if ( nIndex < 0 || nIndex == INVALID_STRING_INDEX )
			return kLookupMissing;

		if ( nIndex >= pTable->GetNumStrings() )
			return kLookupUseLoop;

		// The table's lookup might not be case sensitive, the original compare is
		const char *pFound = pTable->GetString( nIndex );
		if ( !pFound || strcmp( pFound, pName ) )
			return kLookupUseLoop;

		return nIndex;
	}

	int FindLuaFile( INetworkStringTable *pTable, const char *pName )
	{
		if ( !filesystem_lualookup.GetBool() )
			return kLookupUseLoop;

		int nResult = FindHashed( pTable, pName );

		if ( g_bLookupVerify && nResult != kLookupUseLoop )
		{
			int nExpected = FindLinear( pTable, pName );
			++g_nLookupVerified;
			if ( nExpected != nResult )
			{
				if ( ++g_nLookupMismatches <= 10 )
					Warning( "DylibPatches: lookup mismatch for %s, got %d, expected %d\n", pName, nResult, nExpected );

				nResult = nExpected;
			}
		}

		if ( nResult >= 0 )
			++g_nLookupFound;
		else if ( nResult == kLookupMissing )
			++g_nLookupMissing;
		else
			++g_nLookupLoop;

		return nResult;
	}

	void *AllocNear( const uint8_t *pNear, size_t nSize )
	{
		constexpr uintptr_t kStep = 0x10000;
		constexpr uintptr_t kRange = 0x70000000;

		uintptr_t nBase = reinterpret_cast<uintptr_t>( pNear ) & ~( kStep - 1 );
		for ( uintptr_t nOffset = kStep; nOffset < kRange; nOffset += kStep )
		{
			if ( void *p = VirtualAlloc( reinterpret_cast<void *>( nBase + nOffset ), nSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE ) )
				return p;

			if ( nOffset < nBase )
			{
				if ( void *p = VirtualAlloc( reinterpret_cast<void *>( nBase - nOffset ), nSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE ) )
					return p;
			}
		}

		return nullptr;
	}

	class StubWriter
	{
	public:
		explicit StubWriter( uint8_t *pStart ) : m_pStart( pStart ) {}

		void Bytes( std::initializer_list<uint8_t> bytes )
		{
			m_Code.insert( m_Code.end(), bytes );
		}

		void Pointer( const void *p )
		{
			uint64_t nValue = reinterpret_cast<uint64_t>( p );
			for ( int i = 0; i < 8; ++i )
				m_Code.push_back( uint8_t( nValue >> ( i * 8 ) ) );
		}

		void Relative( const uint8_t *pTarget )
		{
			int32_t nRelative = int32_t( pTarget - ( m_pStart + m_Code.size() + 4 ) );
			for ( int i = 0; i < 4; ++i )
				m_Code.push_back( uint8_t( uint32_t( nRelative ) >> ( i * 8 ) ) );
		}

		size_t Size() const { return m_Code.size(); }
		void PatchByte( size_t nOffset, uint8_t nValue ) { m_Code[nOffset] = nValue; }
		const uint8_t *Data() const { return m_Code.data(); }

	private:
		uint8_t *m_pStart;
		std::vector<uint8_t> m_Code;
	};

	// -1 in a pattern = any byte
	const uint8_t *FindUniquePattern( const Range &text, const int16_t *pPattern, size_t nSize )
	{
		const uint8_t *pFound = nullptr;
		for ( size_t i = 0; text.m_nSize >= nSize && i <= text.m_nSize - nSize; ++i )
		{
			if ( text.m_pStart[i] != pPattern[0] )
				continue;

			size_t j = 1;
			while ( j < nSize && ( pPattern[j] < 0 || text.m_pStart[i + j] == pPattern[j] ) )
				++j;

			if ( j != nSize )
				continue;

			if ( pFound )
				return nullptr;

			pFound = text.m_pStart + i;
		}

		return pFound;
	}

	void InstallLuaLookup( HMODULE hClient )
	{
		// -1 = rel32 of the je to the match path
		static const int16_t kPattern[] = {
			0x85, 0xC0, 0x7E, 0x56,
			0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00,
			0x49, 0x8B, 0x4D, 0x08, 0x8B, 0xD7, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x48,
			0x49, 0x83, 0xFF, 0x10, 0x4C, 0x8D, 0x45, 0x80, 0x4C, 0x8B, 0xF0, 0x48, 0x8B, 0xC8,
			0x4C, 0x0F, 0x43, 0xC6, 0x4C, 0x2B, 0xC0, 0x0F, 0xB6, 0x11, 0x42, 0x0F, 0xB6, 0x04, 0x01,
			0x2B, 0xD0, 0x75, 0x07, 0x48, 0xFF, 0xC1, 0x85, 0xC0, 0x75, 0xED, 0x85, 0xD2,
			0x0F, 0x84, -1, -1, -1, -1,
			0x49, 0x8B, 0x4D, 0x08, 0xFF, 0xC7, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18,
			0x3B, 0xF8, 0x7C, 0xB4, 0x45, 0x33, 0xF6,
		};
		constexpr size_t kPatternSize = sizeof( kPattern ) / sizeof( kPattern[0] );
		constexpr size_t kPaddingOffset = 4;
		constexpr size_t kPaddingSize = 7;
		constexpr size_t kLoopOffset = 11;
		constexpr size_t kMatchRelOffset = 67;
		constexpr size_t kMissOffset = 87;

		Range text = FindSection( hClient, ".text" );
		const uint8_t *pSite = FindUniquePattern( text, kPattern, kPatternSize );
		if ( !pSite )
		{
			Warning( "DylibPatches: Lua lookup is off, client.dll pattern not found or not unique\n" );
			return;
		}

		int32_t nMatchRelative;
		memcpy( &nMatchRelative, pSite + kMatchRelOffset, sizeof( nMatchRelative ) );

		const uint8_t *pLoop = pSite + kLoopOffset;
		const uint8_t *pMatch = pSite + kMatchRelOffset + 4 + nMatchRelative;
		const uint8_t *pMiss = pSite + kMissOffset;
		uint8_t *pPadding = const_cast<uint8_t *>( pSite + kPaddingOffset );

		constexpr size_t kStubSize = 0x1000;
		uint8_t *pStub = static_cast<uint8_t *>( AllocNear( pSite, kStubSize ) );
		if ( !pStub )
		{
			Warning( "DylibPatches: Lua lookup is off, no memory near client.dll\n" );
			return;
		}

		StubWriter stub( pStub );
		stub.Bytes( { 0x49, 0x8B, 0x4D, 0x08 } );				// mov rcx, [r13+8]
		stub.Bytes( { 0x48, 0x8D, 0x55, 0x80 } );				// lea rdx, [rbp-0x80]
		stub.Bytes( { 0x49, 0x83, 0xFF, 0x10 } );				// cmp r15, 0x10
		stub.Bytes( { 0x48, 0x0F, 0x43, 0xD6 } );				// cmovae rdx, rsi
		stub.Bytes( { 0x48, 0xB8 } );							// mov rax, FindLuaFile
		stub.Pointer( reinterpret_cast<const void *>( &FindLuaFile ) );
		stub.Bytes( { 0xFF, 0xD0 } );							// call rax
		stub.Bytes( { 0x85, 0xC0 } );							// test eax, eax
		stub.Bytes( { 0x78, 0x00 } );							// js notFound
		size_t nJumpEnd = stub.Size();

		stub.Bytes( { 0x8B, 0xF8 } );							// mov edi, eax
		stub.Bytes( { 0x49, 0x8B, 0x4D, 0x08 } );				// mov rcx, [r13+8]
		stub.Bytes( { 0x8B, 0xD7 } );							// mov edx, edi
		stub.Bytes( { 0x48, 0x8B, 0x01 } );						// mov rax, [rcx]
		stub.Bytes( { 0xFF, 0x50, 0x48 } );						// call [rax+0x48]
		stub.Bytes( { 0x4C, 0x8B, 0xF0 } );						// mov r14, rax
		stub.Bytes( { 0xE9 } );									// jmp match
		stub.Relative( pMatch );

		stub.PatchByte( nJumpEnd - 1, uint8_t( stub.Size() - nJumpEnd ) );
		stub.Bytes( { 0x83, 0xF8, 0xFF } );						// cmp eax, -1
		stub.Bytes( { 0x0F, 0x84 } );							// je miss
		stub.Relative( pMiss );
		stub.Bytes( { 0xE9 } );									// jmp original loop
		stub.Relative( pLoop );

		memcpy( pStub, stub.Data(), stub.Size() );

		DWORD nOldProtect;
		if ( !VirtualProtect( pStub, kStubSize, PAGE_EXECUTE_READ, &nOldProtect ) )
		{
			VirtualFree( pStub, 0, MEM_RELEASE );
			Warning( "DylibPatches: Lua lookup is off, couldn't protect the stub\n" );
			return;
		}
		FlushInstructionCache( GetCurrentProcess(), pStub, stub.Size() );

		PinSelf();
		g_bLookupVerify = CommandLine()->CheckParm( "-lualookupverify" ) != nullptr;

		// jmp stub, then a 2 byte nop over what's left of the padding
		uint8_t patch[kPaddingSize] = { 0xE9, 0, 0, 0, 0, 0x66, 0x90 };
		int32_t nStubRelative = int32_t( pStub - ( pPadding + 5 ) );
		memcpy( patch + 1, &nStubRelative, sizeof( nStubRelative ) );

		if ( !VirtualProtect( pPadding, kPaddingSize, PAGE_EXECUTE_READWRITE, &nOldProtect ) )
		{
			Warning( "DylibPatches: Lua lookup is off, couldn't patch client.dll\n" );
			return;
		}

		memcpy( pPadding, patch, kPaddingSize );
		VirtualProtect( pPadding, kPaddingSize, nOldProtect, &nOldProtect );
		FlushInstructionCache( GetCurrentProcess(), pPadding, kPaddingSize );

		g_bLookupInstalled = true;
		Msg( "DylibPatches: Lua lookup patched%s\n", g_bLookupVerify ? ", verifying every lookup" : "" );
	}

	// LoadFromDisk LZMA compresses every local Lua file just to put it in the string table,
	// and GetFromDatatable decompresses it again. We keep the file and leave a marker instead
	ConVar filesystem_luanocompress( "filesystem_luanocompress", "1", 0, "Keep Lua files found on disk in memory instead of LZMA compressing them" );

	constexpr size_t kLuaHashSize = 0x20;
	constexpr uint8_t kRawLuaMarker = 0xFF; // LZMA props can't be above 0xE0

	// Bootil::Buffer in the x64 client
	struct GameBuffer
	{
		void **m_pVTable;
		uint8_t *m_pData;
		uint64_t m_nSize;
		uint64_t m_nPos;
		uint64_t m_nWritten;

		bool EnsureCapacity( uint64_t nSize )
		{
			using Fn = bool ( * )( GameBuffer *, uint64_t );
			return reinterpret_cast<Fn>( m_pVTable[2] )( this, nSize );
		}

		uint8_t *Reserve( uint64_t nBytes )
		{
			if ( !EnsureCapacity( m_nPos + nBytes ) )
				return nullptr;

			uint8_t *pOut = m_pData + m_nPos;
			m_nPos += nBytes;
			m_nWritten = m_nPos;
			return pOut;
		}
	};

	using LzmaCompressFn = bool ( * )( const void *pData, uint32_t nSize, GameBuffer *pOut, int nLevel, uint32_t nDictSize );
	using LzmaExtractFn = bool ( * )( const uint8_t *pData, uint32_t nSize, GameBuffer *pOut, void *pProgress, uint64_t nMaxSize );

	LzmaCompressFn g_pfnLzmaCompress = nullptr;
	LzmaExtractFn g_pfnLzmaExtract = nullptr;

	// Keyed by the file's hash, nothing from the string table is trusted
	struct RawLuaFiles
	{
		std::mutex m_Lock;
		std::unordered_map<std::string, std::vector<uint8_t>> m_Files;
	};

	RawLuaFiles *g_pRawLua = nullptr;
	bool g_bRawLuaInstalled = false;
	std::atomic<int64_t> g_nRawStored{ 0 };
	std::atomic<int64_t> g_nRawBytes{ 0 };
	std::atomic<int64_t> g_nRawServed{ 0 };
	std::atomic<int64_t> g_nRawMissing{ 0 };

	bool Hook_LuaCompress( const void *pData, uint32_t nSize, GameBuffer *pOut, int nLevel, uint32_t nDictSize )
	{
		if ( !filesystem_luanocompress.GetBool() || pOut->m_nPos != kLuaHashSize )
			return g_pfnLzmaCompress( pData, nSize, pOut, nLevel, nDictSize );

		std::string hash( reinterpret_cast<const char *>( pOut->m_pData ), kLuaHashSize );
		{
			std::lock_guard<std::mutex> lock( g_pRawLua->m_Lock );
			std::vector<uint8_t> &file = g_pRawLua->m_Files[hash];
			if ( file.empty() )
			{
				const uint8_t *pBytes = static_cast<const uint8_t *>( pData );
				file.assign( pBytes, pBytes + nSize );
				++g_nRawStored;
				g_nRawBytes += nSize;
			}
		}

		uint8_t *pMarker = pOut->Reserve( 1 );
		if ( !pMarker )
			return false;

		*pMarker = kRawLuaMarker;
		return true;
	}

	// Only called where pData is right after the file's hash in the string table entry
	bool Hook_LuaExtract( const uint8_t *pData, uint32_t nSize, GameBuffer *pOut, void *pProgress, uint64_t nMaxSize )
	{
		if ( nSize != 1 || *pData != kRawLuaMarker )
			return g_pfnLzmaExtract( pData, nSize, pOut, pProgress, nMaxSize );

		std::string hash( reinterpret_cast<const char *>( pData - kLuaHashSize ), kLuaHashSize );

		std::lock_guard<std::mutex> lock( g_pRawLua->m_Lock );
		auto it = g_pRawLua->m_Files.find( hash );
		if ( it == g_pRawLua->m_Files.end() || ( nMaxSize && it->second.size() > nMaxSize ) )
		{
			++g_nRawMissing;
			return false;
		}

		uint8_t *pOutput = pOut->Reserve( it->second.size() );
		if ( !pOutput )
			return false;

		memcpy( pOutput, it->second.data(), it->second.size() );
		++g_nRawServed;
		return true;
	}

	struct CallSite
	{
		const int16_t *m_pPattern;
		size_t m_nPatternSize;
		size_t m_nCallOffset;
		const uint8_t *m_pCall;
		const uint8_t *m_pTarget;
	};

	const uint8_t *CallTarget( const uint8_t *pCall )
	{
		int32_t nRelative;
		memcpy( &nRelative, pCall + 1, sizeof( nRelative ) );
		return pCall + 5 + nRelative;
	}

	bool RedirectCall( const uint8_t *pCall, const uint8_t *pThunk )
	{
		uint8_t *pRelative = const_cast<uint8_t *>( pCall + 1 );
		int32_t nRelative = int32_t( pThunk - ( pCall + 5 ) );

		DWORD nOldProtect;
		if ( !VirtualProtect( pRelative, sizeof( nRelative ), PAGE_EXECUTE_READWRITE, &nOldProtect ) )
			return false;

		memcpy( pRelative, &nRelative, sizeof( nRelative ) );
		VirtualProtect( pRelative, sizeof( nRelative ), nOldProtect, &nOldProtect );
		FlushInstructionCache( GetCurrentProcess(), pRelative, sizeof( nRelative ) );
		return true;
	}

	void InstallLuaNoCompress( HMODULE hClient )
	{
		// LoadFromDisk
		static const int16_t kCompressSite[] = {
			0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x01, 0x00, 0x41, 0xB9, 0x05, 0x00, 0x00, 0x00,
			0x4C, 0x8D, 0x45, 0xD0, 0x49, 0x8B, 0xCE, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x30,
		};
		// GetFromDatatable
		static const int16_t kExtractSite[] = {
			0x48, 0x8B, 0xC8, 0x48, 0x89, 0x74, 0x24, 0x20, 0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x45, 0xDF,
			0x8B, 0xD3, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x75, 0x23,
		};
		// The datapack stats dump
		static const int16_t kStatsSite[] = {
			0x48, 0x8D, 0x4E, 0x20, 0x83, 0xC2, 0xE0, 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00,
			0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x45, 0xF7, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x0C,
		};

		CallSite sites[] = {
			{ kCompressSite, sizeof( kCompressSite ) / sizeof( kCompressSite[0] ), 21 },
			{ kExtractSite, sizeof( kExtractSite ) / sizeof( kExtractSite[0] ), 17 },
			{ kStatsSite, sizeof( kStatsSite ) / sizeof( kStatsSite[0] ), 23 },
		};

		Range text = FindSection( hClient, ".text" );
		for ( CallSite &site : sites )
		{
			const uint8_t *pFound = FindUniquePattern( text, site.m_pPattern, site.m_nPatternSize );
			if ( !pFound )
			{
				Warning( "DylibPatches: Lua compression patch is off, client.dll pattern not found or not unique\n" );
				return;
			}

			site.m_pCall = pFound + site.m_nCallOffset;
			site.m_pTarget = CallTarget( site.m_pCall );
		}

		if ( sites[1].m_pTarget != sites[2].m_pTarget )
		{
			Warning( "DylibPatches: Lua compression patch is off, the extract calls differ\n" );
			return;
		}

		constexpr size_t kThunkSize = 14;
		constexpr size_t kThunkPage = 0x1000;
		uint8_t *pThunks = static_cast<uint8_t *>( AllocNear( sites[0].m_pCall, kThunkPage ) );
		if ( !pThunks )
		{
			Warning( "DylibPatches: Lua compression patch is off, no memory near client.dll\n" );
			return;
		}

		// jmp [rip+0], then the address
		auto writeThunk = [&]( size_t nIndex, const void *pTarget ) {
			uint8_t *pThunk = pThunks + nIndex * kThunkSize;
			static const uint8_t kJump[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
			memcpy( pThunk, kJump, sizeof( kJump ) );
			memcpy( pThunk + sizeof( kJump ), &pTarget, sizeof( pTarget ) );
			return pThunk;
		};

		const uint8_t *pCompressThunk = writeThunk( 0, reinterpret_cast<const void *>( &Hook_LuaCompress ) );
		const uint8_t *pExtractThunk = writeThunk( 1, reinterpret_cast<const void *>( &Hook_LuaExtract ) );

		DWORD nOldProtect;
		if ( !VirtualProtect( pThunks, kThunkPage, PAGE_EXECUTE_READ, &nOldProtect ) )
		{
			VirtualFree( pThunks, 0, MEM_RELEASE );
			Warning( "DylibPatches: Lua compression patch is off, couldn't protect the thunks\n" );
			return;
		}
		FlushInstructionCache( GetCurrentProcess(), pThunks, kThunkSize * 2 );

		g_pRawLua = new RawLuaFiles;
		g_pfnLzmaCompress = reinterpret_cast<LzmaCompressFn>( const_cast<uint8_t *>( sites[0].m_pTarget ) );
		g_pfnLzmaExtract = reinterpret_cast<LzmaExtractFn>( const_cast<uint8_t *>( sites[1].m_pTarget ) );
		PinSelf();

		// Readers first, a marker must never reach the real extract
		if ( !RedirectCall( sites[1].m_pCall, pExtractThunk ) || !RedirectCall( sites[2].m_pCall, pExtractThunk ) || !RedirectCall( sites[0].m_pCall, pCompressThunk ) )
		{
			Warning( "DylibPatches: Lua compression patch failed to write to client.dll\n" );
			return;
		}

		g_bRawLuaInstalled = true;
		Msg( "DylibPatches: Lua compression patched\n" );
	}

	// Every CDmxAttribute name goes through a global symbol table, with a write lock and a tree search.
	// The table only grows and only holds a few hundred names
	ConVar filesystem_dmxnamecache( "filesystem_dmxnamecache", "1", 0, "Remember DMX attribute name symbols per thread" );

	struct DmxAttribute
	{
		uint32_t m_nType;
		uint16_t m_nName;
		void *m_pData;
	};

	struct DmxNameSlot
	{
		const char *m_pName;
		uint32_t m_nHash;
		uint16_t m_nSymbol;
	};

	using DmxAttributeCtorFn = DmxAttribute *( * )( DmxAttribute *pThis, const char *pName );

	constexpr uint32_t kDmxNameSlots = 1 << 13;
	constexpr uint16_t kInvalidSymbol = 0xFFFF;

	DmxAttributeCtorFn g_pfnDmxAttributeCtor = nullptr;
	bool g_bDmxNameCacheInstalled = false;
	bool g_bDmxNameVerify = false;
	std::atomic<int64_t> g_nDmxNameHits{ 0 };
	std::atomic<int64_t> g_nDmxNameMisses{ 0 };
	std::atomic<int64_t> g_nDmxNameVerified{ 0 };
	std::atomic<int64_t> g_nDmxNameMismatches{ 0 };

	thread_local DmxNameSlot *t_pDmxNames = nullptr;
	thread_local uint32_t t_nDmxNames = 0;

	// No lock prefix, these are only stats
	void BumpStat( std::atomic<int64_t> &stat )
	{
		stat.store( stat.load( std::memory_order_relaxed ) + 1, std::memory_order_relaxed );
	}

	DmxAttribute *Hook_DmxAttributeCtor( DmxAttribute *pThis, const char *pName )
	{
		if ( !pName || !filesystem_dmxnamecache.GetBool() )
			return g_pfnDmxAttributeCtor( pThis, pName );

		if ( !t_pDmxNames )
			t_pDmxNames = new DmxNameSlot[kDmxNameSlots]();

		uint32_t nHash = 2166136261u;
		size_t nLen = 0;
		for ( ; pName[nLen]; ++nLen )
			nHash = ( nHash ^ uint8_t( pName[nLen] ) ) * 16777619u;

		uint32_t nSlot = nHash & ( kDmxNameSlots - 1 );
		for ( ;; )
		{
			DmxNameSlot &slot = t_pDmxNames[nSlot];
			if ( !slot.m_pName )
				break;

			if ( slot.m_nHash == nHash && !strcmp( slot.m_pName, pName ) )
			{
				if ( g_bDmxNameVerify )
				{
					DmxAttribute check;
					g_pfnDmxAttributeCtor( &check, pName );
					g_nDmxNameVerified.fetch_add( 1, std::memory_order_relaxed );
					if ( check.m_nName != slot.m_nSymbol )
					{
						if ( g_nDmxNameMismatches.fetch_add( 1, std::memory_order_relaxed ) < 10 )
							Warning( "DylibPatches: DMX name mismatch for %s, cached %d, table %d\n", pName, slot.m_nSymbol, check.m_nName );

						return g_pfnDmxAttributeCtor( pThis, pName );
					}
				}

				pThis->m_nType = 0;
				pThis->m_nName = slot.m_nSymbol;
				pThis->m_pData = nullptr;
				BumpStat( g_nDmxNameHits );
				return pThis;
			}

			nSlot = ( nSlot + 1 ) & ( kDmxNameSlots - 1 );
		}

		g_pfnDmxAttributeCtor( pThis, pName );
		BumpStat( g_nDmxNameMisses );

		if ( pThis->m_nName != kInvalidSymbol && t_nDmxNames < kDmxNameSlots / 2 )
		{
			char *pCopy = new char[nLen + 1];
			memcpy( pCopy, pName, nLen + 1 );

			DmxNameSlot &slot = t_pDmxNames[nSlot];
			slot.m_nHash = nHash;
			slot.m_nSymbol = pThis->m_nName;
			slot.m_pName = pCopy;
			++t_nDmxNames;
		}

		return pThis;
	}

	void InstallDmxNameCache( HMODULE hClient )
	{
		static const int16_t kPattern[] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xB8, 0xFF, 0xFF, 0x00, 0x00,
			0x48, 0x8B, 0xF9, 0x66, 0x89, 0x41, 0x04, 0x48, 0x8B, 0xDA,
			0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xFF, 0x15, -1, -1, -1, -1,
			0x4C, 0x8B, 0xC3, 0x48, 0x8D, 0x54, 0x24, 0x30,
			0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xE8, -1, -1, -1, -1,
			0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0xFF, 0x15, -1, -1, -1, -1,
			0x0F, 0xB7, 0x44, 0x24, 0x30, 0x48, 0x8B, 0x5C, 0x24, 0x38, 0x66, 0x89, 0x47, 0x04,
			0x33, 0xC0, 0x89, 0x07, 0x48, 0x89, 0x47, 0x08, 0x48, 0x8B, 0xC7,
			0x48, 0x83, 0xC4, 0x20, 0x5F, 0xC3,
		};

		Range text = FindSection( hClient, ".text" );
		const uint8_t *pTarget = FindUniquePattern( text, kPattern, sizeof( kPattern ) / sizeof( kPattern[0] ) );
		if ( !pTarget )
		{
			Warning( "DylibPatches: DMX name cache is off, client.dll pattern not found or not unique\n" );
			return;
		}

		g_bDmxNameVerify = CommandLine()->CheckParm( "-dmxnameverify" ) != nullptr;

		std::vector<Detouring::Hook *> hooks;
		if ( !InstallHook( const_cast<uint8_t *>( pTarget ), reinterpret_cast<void *>( &Hook_DmxAttributeCtor ), reinterpret_cast<void **>( &g_pfnDmxAttributeCtor ), hooks ) || !hooks[0]->Enable() )
		{
			Warning( "DylibPatches: DMX name cache is off, hooking failed\n" );
			return;
		}

		PinSelf();
		g_bDmxNameCacheInstalled = true;
		Msg( "DylibPatches: DMX name cache on%s\n", g_bDmxNameVerify ? ", verifying every hit" : "" );
	}

	// FindInDatatable copies every name in the Lua file table into a std::string for every search path,
	// then throws away nearly all of them because they don't start with the path
	ConVar filesystem_luafindprefix( "filesystem_luafindprefix", "1", 0, "Check the path prefix before copying names in Lua file searches" );

	bool g_bLuaFindInstalled = false;
	bool g_bLuaFindVerify = false;
	std::atomic<int64_t> g_nLuaFindChecked{ 0 };
	std::atomic<int64_t> g_nLuaFindRejected{ 0 };
	std::atomic<int64_t> g_nLuaFindVerified{ 0 };
	std::atomic<int64_t> g_nLuaFindMismatches{ 0 };

	// false = skip this name
	bool CheckLuaFindPrefix( const char *pName, size_t nLen, const std::string *pPrefix )
	{
		if ( !filesystem_luafindprefix.GetBool() )
			return true;

		bool bMatch = nLen >= pPrefix->size() && !memcmp( pName, pPrefix->data(), pPrefix->size() );

		if ( g_bLuaFindVerify )
		{
			bool bExpected = std::string( pName, nLen ).find( *pPrefix, 0 ) == 0;
			g_nLuaFindVerified.fetch_add( 1, std::memory_order_relaxed );
			if ( bExpected != bMatch )
			{
				if ( g_nLuaFindMismatches.fetch_add( 1, std::memory_order_relaxed ) < 10 )
					Warning( "DylibPatches: Lua find prefix mismatch for %s, prefix %s\n", pName, pPrefix->c_str() );

				bMatch = bExpected;
			}
		}

		BumpStat( g_nLuaFindChecked );
		if ( !bMatch )
			BumpStat( g_nLuaFindRejected );

		return bMatch;
	}

	void InstallLuaFindPrefix( HMODULE hClient )
	{
		// From the top of the table loop to the StartsWith check
		static const int16_t kPattern[] = {
			0x49, 0x8B, 0x4E, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18, 0x85, 0xC0, 0x0F, 0x8E, -1, -1,
			-1, -1, 0x49, 0x8B, 0x4E, 0x08, 0x8B, 0xD3, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x48, 0x49, 0xC7,
			0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0x48, 0xC7, 0x44, 0x24, 0x70, 0x00, 0x00, 0x00, 0x00, 0x48, 0xC7,
			0x45, 0x80, 0x00, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x45, 0x88, 0x0F, 0x00, 0x00, 0x00, 0x66, 0x90,
			0x49, 0xFF, 0xC0, 0x42, 0x80, 0x3C, 0x00, 0x00, 0x75, 0xF6, 0x48, 0x8B, 0xD0, 0x48, 0x8D, 0x4C,
			0x24, 0x70, 0xE8, -1, -1, -1, -1, 0x48, 0x8B, 0x45, 0x10, 0x48, 0x39, 0x45, 0x80, 0x73,
			0x45, 0x48, 0x8B, 0x55, 0x88, 0x48, 0x83, 0xFA, 0x10, 0x0F, 0x82, -1, -1, -1, -1, 0x48,
			0x8B, 0x4C, 0x24, 0x70, 0x48, 0xFF, 0xC2, 0x48, 0x8B, 0xC1, 0x48, 0x81, 0xFA, 0x00, 0x10, 0x00,
			0x00, 0x72, 0x19, 0x48, 0x8B, 0x49, 0xF8, 0x48, 0x83, 0xC2, 0x27, 0x48, 0x2B, 0xC1, 0x48, 0x83,
			0xC0, 0xF8, 0x48, 0x83, 0xF8, 0x1F, 0x0F, 0x87, -1, -1, -1, -1, 0xE8, -1, -1, -1,
			-1, 0xE9, -1, -1, -1, -1, 0x48, 0x8D, 0x54, 0x24, 0x70, 0x48, 0x8D, 0x4D, 0x20, 0xE8,
			-1, -1, -1, -1, 0x84, 0xC0, 0x74, 0xA9, 0x48, 0x8D, 0x95, 0xA8, 0x00, 0x00, 0x00, 0x48,
			0x8D, 0x4C, 0x24, 0x70, 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74, 0x94,
		};
		constexpr size_t kSiteOffset = 0x4A;
		constexpr size_t kSiteSize = 8;
		constexpr size_t kResumeOffset = 0x52;
		constexpr size_t kRejectRelOffset = 0x6B;

		Range text = FindSection( hClient, ".text" );
		const uint8_t *pMatch = FindUniquePattern( text, kPattern, sizeof( kPattern ) / sizeof( kPattern[0] ) );
		if ( !pMatch )
		{
			Warning( "DylibPatches: Lua find patch is off, client.dll pattern not found or not unique\n" );
			return;
		}

		int32_t nRejectRelative;
		memcpy( &nRejectRelative, pMatch + kRejectRelOffset, sizeof( nRejectRelative ) );
		const uint8_t *pReject = pMatch + kRejectRelOffset + 4 + nRejectRelative;
		const uint8_t *pResume = pMatch + kResumeOffset;
		uint8_t *pSite = const_cast<uint8_t *>( pMatch + kSiteOffset );

		constexpr size_t kStubSize = 0x1000;
		uint8_t *pStub = static_cast<uint8_t *>( AllocNear( pSite, kStubSize ) );
		if ( !pStub )
		{
			Warning( "DylibPatches: Lua find patch is off, no memory near client.dll\n" );
			return;
		}

		// rax = name, r8 = length. The two stack slots are outgoing args, nothing reads them back
		StubWriter stub( pStub );
		stub.Bytes( { 0x48, 0x89, 0x44, 0x24, 0x20 } );					// mov [rsp+0x20], rax
		stub.Bytes( { 0x4C, 0x89, 0x44, 0x24, 0x28 } );					// mov [rsp+0x28], r8
		stub.Bytes( { 0x48, 0x8B, 0xC8 } );								// mov rcx, rax
		stub.Bytes( { 0x49, 0x8B, 0xD0 } );								// mov rdx, r8
		stub.Bytes( { 0x4C, 0x8D, 0x85, 0xA8, 0x00, 0x00, 0x00 } );		// lea r8, [rbp+0xA8]
		stub.Bytes( { 0x48, 0xB8 } );									// mov rax, CheckLuaFindPrefix
		stub.Pointer( reinterpret_cast<const void *>( &CheckLuaFindPrefix ) );
		stub.Bytes( { 0xFF, 0xD0 } );									// call rax
		stub.Bytes( { 0x4C, 0x8B, 0x44, 0x24, 0x28 } );					// mov r8, [rsp+0x28]
		stub.Bytes( { 0x84, 0xC0 } );									// test al, al
		stub.Bytes( { 0x48, 0x8B, 0x44, 0x24, 0x20 } );					// mov rax, [rsp+0x20]
		stub.Bytes( { 0x0F, 0x84 } );									// je next name
		stub.Relative( pReject );
		stub.Bytes( { 0x48, 0x8B, 0xD0 } );								// mov rdx, rax
		stub.Bytes( { 0x48, 0x8D, 0x4C, 0x24, 0x70 } );					// lea rcx, [rsp+0x70]
		stub.Bytes( { 0xE9 } );											// jmp back
		stub.Relative( pResume );

		memcpy( pStub, stub.Data(), stub.Size() );

		DWORD nOldProtect;
		if ( !VirtualProtect( pStub, kStubSize, PAGE_EXECUTE_READ, &nOldProtect ) )
		{
			VirtualFree( pStub, 0, MEM_RELEASE );
			Warning( "DylibPatches: Lua find patch is off, couldn't protect the stub\n" );
			return;
		}
		FlushInstructionCache( GetCurrentProcess(), pStub, stub.Size() );

		PinSelf();
		g_bLuaFindVerify = CommandLine()->CheckParm( "-luafindverify" ) != nullptr;

		uint8_t patch[kSiteSize] = { 0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x00 };
		int32_t nStubRelative = int32_t( pStub - ( pSite + 5 ) );
		memcpy( patch + 1, &nStubRelative, sizeof( nStubRelative ) );

		if ( !VirtualProtect( pSite, kSiteSize, PAGE_EXECUTE_READWRITE, &nOldProtect ) )
		{
			Warning( "DylibPatches: Lua find patch is off, couldn't patch client.dll\n" );
			return;
		}

		memcpy( pSite, patch, kSiteSize );
		VirtualProtect( pSite, kSiteSize, nOldProtect, &nOldProtect );
		FlushInstructionCache( GetCurrentProcess(), pSite, kSiteSize );

		g_bLuaFindInstalled = true;
		Msg( "DylibPatches: Lua find patched%s\n", g_bLuaFindVerify ? ", verifying every check" : "" );
	}

	// Lua refresh's folder watcher walks every folder just to find symlinks, Windows covers the rest.
	// The walk moves to a thread, the watcher gets restarted with the links on the next poll
	enum class LuaWatchMode
	{
		Normal,
		SkipWalk,
		Worker,
		ReplayLinks,
	};

	struct LuaWatchJob
	{
		void *m_pMonitor = nullptr;
		std::string m_Path;
		std::vector<std::string> m_Filter;
		bool m_bHasFilter = false;
		std::vector<std::string> m_Links;
		std::atomic<bool> m_bReady{ false };
	};

	using LuaWatchFolderFn = void ( * )( void *pThis, const std::string *pPath, bool bSubtree, const std::vector<std::string> *pFilter );
	using LuaWatchRecurseFn = void ( * )( std::vector<std::string> *pOut, const std::string *pPath, const std::vector<std::string> *pFilter );
	using LuaWatchHasChangesFn = bool ( * )( void *pThis );
	using FindFirstFileExAFn = HANDLE( WINAPI * )( LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD );
	using FindNextFileAFn = BOOL( WINAPI * )( HANDLE, LPWIN32_FIND_DATAA );

	LuaWatchFolderFn g_pfnLuaWatchFolder = nullptr;
	LuaWatchRecurseFn g_pfnLuaWatchRecurse = nullptr;
	LuaWatchHasChangesFn g_pfnLuaWatchHasChanges = nullptr;
	FindFirstFileExAFn g_pfnWatchFindFirst = nullptr;
	FindNextFileAFn g_pfnWatchFindNext = nullptr;

	std::atomic<LuaWatchJob *> g_pLuaWatchJob{ nullptr };
	bool g_bLuaWatchInstalled = false;
	std::atomic<int64_t> g_nLuaWatchDirs{ 0 };
	std::atomic<int64_t> g_nLuaWatchLinks{ 0 };
	std::atomic<int64_t> g_nLuaWatchWalkUs{ 0 };
	std::atomic<int64_t> g_nLuaWatchMergeUs{ 0 };
	std::atomic<int64_t> g_nLuaWatchMerges{ 0 };
	std::atomic<int64_t> g_nLuaWatchChangePolls{ 0 };
	std::atomic<LuaWatchJob *> g_pLuaWatchMerged{ nullptr };

	thread_local LuaWatchMode t_LuaWatchMode = LuaWatchMode::Normal;
	thread_local const std::vector<std::string> *t_pLuaWatchLinks = nullptr;
	thread_local std::unordered_map<std::string, DWORD> *t_pLuaWatchAttributes = nullptr;
	thread_local std::string *t_pLuaWatchPrefix = nullptr;

	int64_t TicksToUs( int64_t nTicks )
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency( &frequency );
		return nTicks * 1000000 / frequency.QuadPart;
	}

	void NoteWatchFindData( const WIN32_FIND_DATAA *pData )
	{
		if ( pData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
			( *t_pLuaWatchAttributes )[*t_pLuaWatchPrefix + pData->cFileName] = pData->dwFileAttributes;
	}

	HANDLE WINAPI Hook_WatchFindFirst( LPCSTR pName, FINDEX_INFO_LEVELS level, LPVOID pData, FINDEX_SEARCH_OPS op, LPVOID pFilter, DWORD nFlags )
	{
		if ( t_LuaWatchMode != LuaWatchMode::Worker )
			return g_pfnWatchFindFirst( pName, level, pData, op, pFilter, nFlags );

		HANDLE hFind = g_pfnWatchFindFirst( pName, FindExInfoBasic, pData, op, pFilter, nFlags | FIND_FIRST_EX_LARGE_FETCH );
		if ( hFind != INVALID_HANDLE_VALUE )
		{
			t_pLuaWatchPrefix->assign( pName );
			if ( !t_pLuaWatchPrefix->empty() && t_pLuaWatchPrefix->back() == '*' )
				t_pLuaWatchPrefix->pop_back();

			NoteWatchFindData( static_cast<const WIN32_FIND_DATAA *>( pData ) );
		}

		return hFind;
	}

	BOOL WINAPI Hook_WatchFindNext( HANDLE hFind, LPWIN32_FIND_DATAA pData )
	{
		BOOL bResult = g_pfnWatchFindNext( hFind, pData );
		if ( bResult && t_LuaWatchMode == LuaWatchMode::Worker )
			NoteWatchFindData( pData );

		return bResult;
	}

	DWORD WINAPI LuaWatchThread( LPVOID pParam )
	{
		auto *pJob = static_cast<LuaWatchJob *>( pParam );
		SetThreadPriority( GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN );

		int64_t nStart = Now();
		std::unordered_map<std::string, DWORD> attributes;
		std::string prefix;
		t_pLuaWatchAttributes = &attributes;
		t_pLuaWatchPrefix = &prefix;
		t_LuaWatchMode = LuaWatchMode::Worker;

		std::vector<std::string> dirs;
		g_pfnLuaWatchRecurse( &dirs, &pJob->m_Path, pJob->m_bHasFilter ? &pJob->m_Filter : nullptr );

		for ( const std::string &dir : dirs )
		{
			auto it = attributes.find( dir );
			DWORD nAttributes = it != attributes.end() ? it->second : GetFileAttributesA( dir.c_str() );
			if ( nAttributes & FILE_ATTRIBUTE_REPARSE_POINT )
				pJob->m_Links.push_back( dir );
		}

		g_nLuaWatchDirs = int64_t( dirs.size() );
		g_nLuaWatchLinks = int64_t( pJob->m_Links.size() );
		g_nLuaWatchWalkUs = TicksToUs( Now() - nStart );
		pJob->m_bReady.store( true, std::memory_order_release );
		return 0;
	}

	void Hook_LuaWatchRecurse( std::vector<std::string> *pOut, const std::string *pPath, const std::vector<std::string> *pFilter )
	{
		switch ( t_LuaWatchMode )
		{
		case LuaWatchMode::SkipWalk:
			return;

		case LuaWatchMode::ReplayLinks:
			for ( const std::string &link : *t_pLuaWatchLinks )
				pOut->push_back( link );
			return;

		default:
			g_pfnLuaWatchRecurse( pOut, pPath, pFilter );
		}
	}

	void Hook_LuaWatchFolder( void *pThis, const std::string *pPath, bool bSubtree, const std::vector<std::string> *pFilter )
	{
		if ( !bSubtree )
		{
			g_pLuaWatchJob.store( nullptr, std::memory_order_release );
			g_pfnLuaWatchFolder( pThis, pPath, bSubtree, pFilter );
			return;
		}

		auto *pJob = new LuaWatchJob;
		pJob->m_pMonitor = pThis;
		pJob->m_Path = *pPath;
		pJob->m_bHasFilter = pFilter != nullptr;
		if ( pFilter )
			pJob->m_Filter = *pFilter;

		HANDLE hThread = CreateThread( nullptr, 0, &LuaWatchThread, pJob, CREATE_SUSPENDED, nullptr );
		if ( !hThread )
		{
			delete pJob;
			g_pLuaWatchJob.store( nullptr, std::memory_order_release );
			g_pfnLuaWatchFolder( pThis, pPath, bSubtree, pFilter );
			return;
		}

		t_LuaWatchMode = LuaWatchMode::SkipWalk;
		g_pfnLuaWatchFolder( pThis, pPath, bSubtree, pFilter );
		t_LuaWatchMode = LuaWatchMode::Normal;

		// Older jobs are left alone, their thread might still be using them
		g_pLuaWatchJob.store( pJob, std::memory_order_release );
		ResumeThread( hThread );
		CloseHandle( hThread );
	}

	// Closing a watched folder with a read pending would complete it into freed memory
	void StopLuaWatchIO( void *pThis )
	{
		constexpr size_t kWatcherSize = 0x448;
		constexpr size_t kHandleOffset = 0x20;
		constexpr size_t kOverlappedOffset = 0x28;

		auto *pList = *static_cast<uint8_t ***>( pThis );
		if ( !pList )
			return;

		for ( uint8_t *pWatcher = pList[0]; pWatcher != pList[1]; pWatcher += kWatcherSize )
		{
			HANDLE hDir = *reinterpret_cast<HANDLE *>( pWatcher + kHandleOffset );
			auto *pOverlapped = reinterpret_cast<OVERLAPPED *>( pWatcher + kOverlappedOffset );
			if ( hDir && hDir != INVALID_HANDLE_VALUE && !HasOverlappedIoCompleted( pOverlapped ) )
			{
				DWORD nBytes;
				if ( CancelIoEx( hDir, pOverlapped ) || GetLastError() != ERROR_NOT_FOUND )
					GetOverlappedResult( hDir, pOverlapped, &nBytes, TRUE );
			}

			if ( pOverlapped->hEvent )
			{
				CloseHandle( pOverlapped->hEvent );
				pOverlapped->hEvent = nullptr;
			}
		}
	}

	bool Hook_LuaWatchHasChanges( void *pThis )
	{
		bool bChanges = g_pfnLuaWatchHasChanges( pThis );
		if ( bChanges )
			BumpStat( g_nLuaWatchChangePolls );

		LuaWatchJob *pJob = g_pLuaWatchJob.load( std::memory_order_acquire );
		if ( !pJob || pJob->m_pMonitor != pThis || !pJob->m_bReady.load( std::memory_order_acquire ) )
			return bChanges;

		g_pLuaWatchJob.store( nullptr, std::memory_order_release );

		int64_t nStart = Now();
		StopLuaWatchIO( pThis );

		t_pLuaWatchLinks = &pJob->m_Links;
		t_LuaWatchMode = LuaWatchMode::ReplayLinks;
		g_pfnLuaWatchFolder( pThis, &pJob->m_Path, true, pJob->m_bHasFilter ? &pJob->m_Filter : nullptr );
		t_LuaWatchMode = LuaWatchMode::Normal;
		t_pLuaWatchLinks = nullptr;

		g_nLuaWatchMergeUs = TicksToUs( Now() - nStart );
		++g_nLuaWatchMerges;
		g_pLuaWatchMerged.store( pJob, std::memory_order_release );

		return g_pfnLuaWatchHasChanges( pThis ) || bChanges;
	}

	void **FindImportSlot( HMODULE hModule, const char *pDll, const char *pName )
	{
		auto *pBase = reinterpret_cast<uint8_t *>( hModule );
		auto *pNt = reinterpret_cast<IMAGE_NT_HEADERS *>( pBase + reinterpret_cast<IMAGE_DOS_HEADER *>( pBase )->e_lfanew );
		const IMAGE_DATA_DIRECTORY &dir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

		auto *pImport = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>( pBase + dir.VirtualAddress );
		for ( ; pImport->Name; ++pImport )
		{
			if ( V_stricmp( reinterpret_cast<const char *>( pBase + pImport->Name ), pDll ) || !pImport->OriginalFirstThunk )
				continue;

			auto *pNames = reinterpret_cast<IMAGE_THUNK_DATA *>( pBase + pImport->OriginalFirstThunk );
			auto *pSlots = reinterpret_cast<IMAGE_THUNK_DATA *>( pBase + pImport->FirstThunk );
			for ( ; pNames->u1.AddressOfData; ++pNames, ++pSlots )
			{
				if ( IMAGE_SNAP_BY_ORDINAL( pNames->u1.Ordinal ) )
					continue;

				auto *pByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>( pBase + pNames->u1.AddressOfData );
				if ( !strcmp( reinterpret_cast<const char *>( pByName->Name ), pName ) )
					return reinterpret_cast<void **>( &pSlots->u1.Function );
			}
		}

		return nullptr;
	}

	bool PatchImportSlot( void **ppSlot, void *pHook, void **ppOriginal )
	{
		DWORD nOldProtect;
		if ( !VirtualProtect( ppSlot, sizeof( void * ), PAGE_READWRITE, &nOldProtect ) )
			return false;

		*ppOriginal = *ppSlot;
		*ppSlot = pHook;
		VirtualProtect( ppSlot, sizeof( void * ), nOldProtect, &nOldProtect );
		return true;
	}

	bool MatchesAt( const Range &text, const uint8_t *pAddress, const int16_t *pPattern, size_t nSize )
	{
		if ( pAddress < text.m_pStart || pAddress + nSize > text.m_pStart + text.m_nSize )
			return false;

		for ( size_t i = 0; i < nSize; ++i )
		{
			if ( pPattern[i] >= 0 && pAddress[i] != pPattern[i] )
				return false;
		}

		return true;
	}

	void InstallLuaWatch( HMODULE hServer )
	{
		if ( CommandLine()->CheckParm( "-noluawatchthread" ) )
			return;

		static const int16_t kWatchFolder[] = {
			0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xF0, 0xF7, 0xFF, 0xFF,
			0x48, 0x81, 0xEC, 0x10, 0x09, 0x00, 0x00, 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x85, 0x00,
		};
		static const int16_t kRecurseCall[] = {
			0x48, 0x89, 0x7C, 0x24, 0x50, 0x4D, 0x8B, 0xC7, 0x48, 0x8B, 0xD3, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0xE8,
		};
		static const int16_t kRecurse[] = {
			0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
		};
		static const int16_t kHasChanges[] = {
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, -1, -1, -1, -1,
			0x48, 0x83, 0x7B, 0x30, 0x00, 0x0F, 0x97, 0xC0, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3,
		};
		constexpr ptrdiff_t kRecurseCallOffset = 0x234;
		constexpr ptrdiff_t kHasChangesOffset = -0x760;

		Range text = FindSection( hServer, ".text" );
		const uint8_t *pWatchFolder = FindUniquePattern( text, kWatchFolder, sizeof( kWatchFolder ) / sizeof( kWatchFolder[0] ) );
		if ( !pWatchFolder )
		{
			Warning( "DylibPatches: Lua watch thread is off, WatchFolder not found\n" );
			return;
		}

		const uint8_t *pCallSite = pWatchFolder + kRecurseCallOffset;
		const uint8_t *pHasChanges = pWatchFolder + kHasChangesOffset;
		if ( !MatchesAt( text, pCallSite, kRecurseCall, sizeof( kRecurseCall ) / sizeof( kRecurseCall[0] ) )
			|| !MatchesAt( text, pHasChanges, kHasChanges, sizeof( kHasChanges ) / sizeof( kHasChanges[0] ) ) )
		{
			Warning( "DylibPatches: Lua watch thread is off, server.dll doesn't match\n" );
			return;
		}

		const uint8_t *pRecurse = CallTarget( pCallSite + sizeof( kRecurseCall ) / sizeof( kRecurseCall[0] ) - 1 );
		if ( !MatchesAt( text, pRecurse, kRecurse, sizeof( kRecurse ) / sizeof( kRecurse[0] ) ) )
		{
			Warning( "DylibPatches: Lua watch thread is off, the folder walk doesn't match\n" );
			return;
		}

		void **ppFindFirst = FindImportSlot( hServer, "KERNEL32.dll", "FindFirstFileExA" );
		void **ppFindNext = FindImportSlot( hServer, "KERNEL32.dll", "FindNextFileA" );
		if ( !ppFindFirst || !ppFindNext )
		{
			Warning( "DylibPatches: Lua watch thread is off, find imports not found\n" );
			return;
		}

		// Stays loaded for the worker thread
		HMODULE hPinned;
		GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>( pWatchFolder ), &hPinned );
		PinSelf();

		if ( !PatchImportSlot( ppFindFirst, reinterpret_cast<void *>( &Hook_WatchFindFirst ), reinterpret_cast<void **>( &g_pfnWatchFindFirst ) )
			|| !PatchImportSlot( ppFindNext, reinterpret_cast<void *>( &Hook_WatchFindNext ), reinterpret_cast<void **>( &g_pfnWatchFindNext ) ) )
		{
			Warning( "DylibPatches: Lua watch thread is off, couldn't patch imports\n" );
			return;
		}

		std::vector<Detouring::Hook *> hooks;
		bool bOk = InstallHook( const_cast<uint8_t *>( pRecurse ), reinterpret_cast<void *>( &Hook_LuaWatchRecurse ), reinterpret_cast<void **>( &g_pfnLuaWatchRecurse ), hooks )
			&& InstallHook( const_cast<uint8_t *>( pHasChanges ), reinterpret_cast<void *>( &Hook_LuaWatchHasChanges ), reinterpret_cast<void **>( &g_pfnLuaWatchHasChanges ), hooks )
			&& InstallHook( const_cast<uint8_t *>( pWatchFolder ), reinterpret_cast<void *>( &Hook_LuaWatchFolder ), reinterpret_cast<void **>( &g_pfnLuaWatchFolder ), hooks );

		for ( size_t i = 0; bOk && i < hooks.size(); ++i )
			bOk = hooks[i]->Enable();

		if ( !bOk )
		{
			for ( Detouring::Hook *pHook : hooks )
				pHook->Destroy();

			Warning( "DylibPatches: Lua watch thread is off, hooking failed\n" );
			return;
		}

		g_bLuaWatchInstalled = true;
		Msg( "DylibPatches: Lua watch walk moved to a thread\n" );
	}

	// Timing for CParticleSystemMgr::ReadParticleConfigFile
	struct ParticleLoad
	{
		std::string m_File;
		std::string m_Script;
		int64_t m_nStart;
		int64_t m_nTicks;
		bool m_bResult;
	};

	struct ParticleTiming
	{
		std::mutex m_Lock;
		std::vector<ParticleLoad> m_Loads;
		std::string m_OutputDir;
		int64_t m_nFrequency = 1;
		int64_t m_nStart = 0;
	};

	using ReadParticleConfigFn = bool ( * )( void *pThis, const char *pFileName, bool bPrecache, bool bDecommitTempMemory );

	ParticleTiming *g_pParticles = nullptr;
	ReadParticleConfigFn g_pfnReadParticleConfig = nullptr;

	std::string CurrentScript()
	{
		if ( !t_pStack )
			return {};

		for ( int i = ( t_nDepth < kMaxDepth ? t_nDepth : kMaxDepth ) - 1; i >= 0; --i )
		{
			if ( t_pStack[i].m_Kind == FrameKind::Script )
				return MakeKey( t_pStack[i].m_pPrefix, t_pStack[i].m_pName );
		}

		return {};
	}

	bool Hook_ReadParticleConfig( void *pThis, const char *pFileName, bool bPrecache, bool bDecommitTempMemory )
	{
		ParticleLoad load;
		load.m_File = pFileName ? pFileName : "";
		load.m_Script = CurrentScript();
		load.m_nStart = Now();
		load.m_bResult = g_pfnReadParticleConfig( pThis, pFileName, bPrecache, bDecommitTempMemory );
		load.m_nTicks = Now() - load.m_nStart;

		bool bResult = load.m_bResult;
		std::lock_guard<std::mutex> lock( g_pParticles->m_Lock );
		g_pParticles->m_Loads.push_back( std::move( load ) );
		return bResult;
	}

	void InstallParticleTiming( HMODULE hClient )
	{
		Range text = FindSection( hClient, ".text" );
		Range rdata = FindSection( hClient, ".rdata" );

		const uint8_t *pTarget = FindReferencingFunction( text, FindString( rdata, "%s_dx80.%s" ), -1 );
		if ( !pTarget )
		{
			Warning( "DylibPatches: particle timing is off, ReadParticleConfigFile not found\n" );
			return;
		}

		auto *pParticles = new ParticleTiming;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency( &frequency );
		pParticles->m_nFrequency = frequency.QuadPart;
		pParticles->m_nStart = Now();
		pParticles->m_OutputDir = GameDirFromModule( hClient );
		g_pParticles = pParticles;

		std::vector<Detouring::Hook *> hooks;
		if ( !InstallHook( const_cast<uint8_t *>( pTarget ), reinterpret_cast<void *>( &Hook_ReadParticleConfig ), reinterpret_cast<void **>( &g_pfnReadParticleConfig ), hooks ) || !hooks[0]->Enable() )
		{
			g_pParticles = nullptr;
			Warning( "DylibPatches: particle timing is off, hooking failed\n" );
			return;
		}

		PinSelf();
	}

	void WriteParticleReport()
	{
		if ( !g_pParticles )
			return;

		std::lock_guard<std::mutex> lock( g_pParticles->m_Lock );
		double flToMs = 1000.0 / g_pParticles->m_nFrequency;

		int64_t nTotal = 0;
		for ( const ParticleLoad &load : g_pParticles->m_Loads )
			nTotal += load.m_nTicks;

		Msg( "Particle files: %zu loads, %.1f ms\n", g_pParticles->m_Loads.size(), nTotal * flToMs );

		if ( g_pParticles->m_OutputDir.empty() )
			return;

		std::string path = g_pParticles->m_OutputDir + "particle_timing.csv";
		FILE *pFile = fopen( path.c_str(), "wb" );
		if ( !pFile )
		{
			Warning( "DylibPatches: couldn't write %s\n", path.c_str() );
			return;
		}

		fputs( "order,file,start_ms,ms,result,script\n", pFile );
		int nOrder = 0;
		for ( const ParticleLoad &load : g_pParticles->m_Loads )
		{
			fprintf( pFile, "%d,\"%s\",%.3f,%.3f,%d,\"%s\"\n", ++nOrder, load.m_File.c_str(),
				( load.m_nStart - g_pParticles->m_nStart ) * flToMs, load.m_nTicks * flToMs, load.m_bResult ? 1 : 0, load.m_Script.c_str() );
		}

		fclose( pFile );
		Msg( "Wrote %s\n", path.c_str() );
	}

	// Output
	double ToMs( int64_t nTicks )
	{
		return nTicks * 1000.0 / g_pState->m_nFrequency;
	}

	void WriteReport()
	{
		std::lock_guard<std::mutex> lock( g_pState->m_Lock );

		std::string summary;
		char szLine[512];

		V_snprintf( szLine, sizeof( szLine ), "Lua timing in ms. Startup ends at the first %.0f s gap in a realm\n", kStartupGapSeconds );
		summary += szLine;
		V_snprintf( szLine, sizeof( szLine ), "%-8s %8s %8s %10s %10s %10s %10s %10s %10s %10s %10s\n", "realm", "scripts", "MB", "read", "prep", "compile", "run", "lua", "span", "outside", "late" );
		summary += szLine;

		for ( int i = 0; i < kRealmCount; ++i )
		{
			const RealmStats &realm = g_pState->m_Realms[i];
			if ( !realm.m_nFirst )
				continue;

			const Totals &window = realm.m_bSettled ? realm.m_Startup : realm.m_Total;
			int64_t nEnd = realm.m_bSettled ? realm.m_nStartupEnd : realm.m_nLast;

			int64_t nLua = 0;
			int64_t nAll = 0;
			for ( int nBucket = 0; nBucket < kBucketCount; ++nBucket )
			{
				nLua += window.m_Time[nBucket];
				nAll += realm.m_Total.m_Time[nBucket];
			}

			int64_t nSpan = nEnd > realm.m_nFirst ? nEnd - realm.m_nFirst : 0;
			V_snprintf( szLine, sizeof( szLine ), "%-8s %8lld %8.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
				kRealmNames[i], window.m_nScripts, window.m_nBytes / 1048576.0,
				ToMs( window.m_Time[kRead] ), ToMs( window.m_Time[kPrep] ), ToMs( window.m_Time[kCompile] ), ToMs( window.m_Time[kRun] ),
				ToMs( nLua ), ToMs( nSpan ), ToMs( nSpan - nLua ), ToMs( nAll - nLua ) );
			summary += szLine;
		}

		Msg( "%s", summary.c_str() );

		if ( g_pState->m_OutputDir.empty() )
			return;

		std::string summaryPath = g_pState->m_OutputDir + "lua_timing.txt";
		if ( FILE *pFile = fopen( summaryPath.c_str(), "wb" ) )
		{
			fwrite( summary.data(), 1, summary.size(), pFile );
			fclose( pFile );
		}

		std::string csvPath = g_pState->m_OutputDir + "lua_timing.csv";
		FILE *pFile = fopen( csvPath.c_str(), "wb" );
		if ( !pFile )
		{
			Warning( "DylibPatches: couldn't write %s\n", csvPath.c_str() );
			return;
		}

		fputs( "realm,file,bytes,runs,first_ms,read_ms,prep_ms,compile_ms,run_ms,total_ms\n", pFile );

		using Entry = std::pair<const std::string *, const FileStats *>;
		auto total = []( const FileStats &file ) {
			int64_t nTotal = 0;
			for ( int nBucket = 0; nBucket < kBucketCount; ++nBucket )
				nTotal += file.m_Totals.m_Time[nBucket];
			return nTotal;
		};

		for ( int i = 0; i < kRealmCount; ++i )
		{
			std::vector<Entry> entries;
			for ( const auto &file : g_pState->m_Realms[i].m_Files )
				entries.emplace_back( &file.first, &file.second );

			std::sort( entries.begin(), entries.end(), [&]( const Entry &a, const Entry &b ) { return total( *a.second ) > total( *b.second ); } );

			for ( const Entry &entry : entries )
			{
				const Totals &stats = entry.second->m_Totals;
				fprintf( pFile, "%s,\"%s\",%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
					kRealmNames[i], entry.first->c_str(), stats.m_nBytes, stats.m_nScripts,
					ToMs( entry.second->m_nFirst - g_pState->m_nStart ),
					ToMs( stats.m_Time[kRead] ), ToMs( stats.m_Time[kPrep] ), ToMs( stats.m_Time[kCompile] ), ToMs( stats.m_Time[kRun] ),
					ToMs( total( *entry.second ) ) );
			}
		}

		fclose( pFile );
		Msg( "Wrote %s\n", csvPath.c_str() );
	}

	void ResetStats()
	{
		std::lock_guard<std::mutex> lock( g_pState->m_Lock );
		for ( RealmStats &realm : g_pState->m_Realms )
			realm = RealmStats();

		g_pState->m_nStart = Now();
	}
}

CON_COMMAND( filesystem_luatiming, "Prints where Lua startup time went and writes lua_timing.csv. Pass reset to clear" )
{
	if ( args.ArgC() > 1 && !V_stricmp( args[1], "reset" ) )
	{
		if ( g_pState )
			ResetStats();

		g_nLookupFound = 0;
		g_nLookupMissing = 0;
		g_nLookupLoop = 0;
		g_nLookupVerified = 0;
		g_nLookupMismatches = 0;
		g_nRawServed = 0;
		g_nRawMissing = 0;
		g_nDmxNameHits = 0;
		g_nDmxNameMisses = 0;
		g_nDmxNameVerified = 0;
		g_nDmxNameMismatches = 0;
		g_nLuaFindChecked = 0;
		g_nLuaFindRejected = 0;
		g_nLuaFindVerified = 0;
		g_nLuaFindMismatches = 0;
		if ( g_pParticles )
		{
			std::lock_guard<std::mutex> lock( g_pParticles->m_Lock );
			g_pParticles->m_Loads.clear();
		}
		Msg( "Lua timing reset\n" );
		return;
	}

	Msg( "Lua lookup: %s, %lld found, %lld missing, %lld used the old loop, %lld verified, %lld mismatches\n",
		g_bLookupInstalled ? "patched" : "not patched",
		g_nLookupFound.load(), g_nLookupMissing.load(), g_nLookupLoop.load(), g_nLookupVerified.load(), g_nLookupMismatches.load() );

	Msg( "Lua compression: %s, %lld files kept (%.1f MB), %lld served, %lld not found\n",
		g_bRawLuaInstalled ? "patched" : "not patched",
		g_nRawStored.load(), g_nRawBytes.load() / 1048576.0, g_nRawServed.load(), g_nRawMissing.load() );

	Msg( "DMX name cache: %s, %lld hits, %lld misses, %lld verified, %lld mismatches\n",
		g_bDmxNameCacheInstalled ? "on" : "off",
		g_nDmxNameHits.load(), g_nDmxNameMisses.load(), g_nDmxNameVerified.load(), g_nDmxNameMismatches.load() );

	Msg( "Lua find: %s, %lld names checked, %lld skipped, %lld verified, %lld mismatches\n",
		g_bLuaFindInstalled ? "patched" : "not patched", g_nLuaFindChecked.load(), g_nLuaFindRejected.load(), g_nLuaFindVerified.load(), g_nLuaFindMismatches.load() );

	Msg( "Lua watch thread: %s, %lld folders, %lld links, walk %.1f ms, %lld merges, last merge %.1f ms, %lld polls saw changes\n",
		g_bLuaWatchInstalled ? "on" : "off", g_nLuaWatchDirs.load(), g_nLuaWatchLinks.load(),
		g_nLuaWatchWalkUs.load() / 1000.0, g_nLuaWatchMerges.load(), g_nLuaWatchMergeUs.load() / 1000.0, g_nLuaWatchChangePolls.load() );

	if ( LuaWatchJob *pMerged = g_pLuaWatchMerged.load( std::memory_order_acquire ) )
	{
		for ( const std::string &link : pMerged->m_Links )
			Msg( "  watching link %s\n", link.c_str() );
	}

	WriteParticleReport();

	if ( !g_pState )
	{
		Msg( "Lua timing isn't running\n" );
		return;
	}

	WriteReport();
}

void DylibPatches::OnModuleLoaded( const char *pFileName, CSysModule *pModule )
{
	if ( !pModule )
		return;

	HMODULE hModule = reinterpret_cast<HMODULE>( pModule );
	char szPath[MAX_PATH];
	DWORD nLen = GetModuleFileNameA( hModule, szPath, sizeof( szPath ) );
	if ( !nLen || nLen >= sizeof( szPath ) )
		return;

	const char *pName = V_UnqualifiedFileName( szPath );

	static bool s_bLuaShared = false;
	if ( !s_bLuaShared && !V_stricmp( pName, "lua_shared.dll" ) )
	{
		s_bLuaShared = true;
		InstallLuaTiming( hModule );
	}

	static bool s_bClient = false;
	if ( !s_bClient && !V_stricmp( pName, "client.dll" ) )
	{
		s_bClient = true;
		InstallLuaLookup( hModule );
		InstallLuaNoCompress( hModule );
		InstallParticleTiming( hModule );
		InstallDmxNameCache( hModule );
		InstallLuaFindPrefix( hModule );
	}

	static bool s_bServer = false;
	if ( !s_bServer && !V_stricmp( pName, "server.dll" ) )
	{
		s_bServer = true;
		InstallLuaWatch( hModule );
	}
}

#endif
