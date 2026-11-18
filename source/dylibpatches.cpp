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

		char szPath[MAX_PATH];
		DWORD nLen = GetModuleFileNameA( hLuaShared, szPath, sizeof( szPath ) );
		if ( nLen && nLen < sizeof( szPath ) )
		{
			// garrysmod\bin\win64\lua_shared.dll
			pState->m_OutputDir = szPath;
			for ( int i = 0; i < 3; ++i )
			{
				size_t nSlash = pState->m_OutputDir.find_last_of( "\\/" );
				if ( nSlash == std::string::npos )
					break;

				pState->m_OutputDir.resize( nSlash );
			}
			pState->m_OutputDir += '\\';
		}

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
		Msg( "Lua timing reset\n" );
		return;
	}

	Msg( "Lua lookup: %s, %lld found, %lld missing, %lld used the old loop, %lld verified, %lld mismatches\n",
		g_bLookupInstalled ? "patched" : "not patched",
		g_nLookupFound.load(), g_nLookupMissing.load(), g_nLookupLoop.load(), g_nLookupVerified.load(), g_nLookupMismatches.load() );

	Msg( "Lua compression: %s, %lld files kept (%.1f MB), %lld served, %lld not found\n",
		g_bRawLuaInstalled ? "patched" : "not patched",
		g_nRawStored.load(), g_nRawBytes.load() / 1048576.0, g_nRawServed.load(), g_nRawMissing.load() );

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
	}
}

#endif
