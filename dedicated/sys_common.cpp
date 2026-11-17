//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//===========================================================================//
#ifdef _WIN32
#include "winlite.h"
#else
#include <unistd.h>
#include <stdlib.h>
#endif
#include <stdio.h>
#include "isys.h"
#include "dedicated.h"
#include "engine_hlds_api.h"
#include "idedicatedexports.h"
#include "filesystem.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/strtools.h"
#include "tier2/tier2.h"

static intp s_hDLLThirdParty = 0;

// Third party .dlls ( gamehost )
static void Load3rdParty()
{
	// Only do this if the server operator wants the support.
	if ( CommandLine()->CheckParm( "-usegh" ) )
		s_hDLLThirdParty = sys->LoadLibrary( (char *)"ghostinj" MODULE_EXT );
}

bool InitInstance()
{
	Load3rdParty();
	return true;
}

int ProcessConsoleInput()
{
	int count = 0;
	if ( !engine )
		return count;

	char *s;
	do
	{
		char szBuf[256];
		s = sys->ConsoleInput( count++, szBuf, sizeof( szBuf ) );
		if ( s && s[0] )
		{
			V_strcat_safe( szBuf, "\n" );
			engine->AddConsoleText( szBuf );
		}
	} while ( s );

	return count;
}

void RunServer();

class CDedicatedExports : public CBaseAppSystem<IDedicatedExports>
{
public:
	virtual void Sys_Printf( char *text )
	{
		if ( sys )
			sys->Printf( "%s", text );
	}

	virtual void RunServer()
	{
		::RunServer();
	}
};

EXPOSE_SINGLE_INTERFACE( CDedicatedExports, IDedicatedExports, VENGINE_DEDICATEDEXPORTS_API_VERSION );

static const char *GetConsoleLogFilename()
{
	static bool s_bInited = false;
	static char s_consolelog[MAX_PATH];

	if ( !s_bInited )
	{
		s_bInited = true;

		// Don't do the -consolelog thing if -consoledebug is present.
		const char *filename = NULL;
		if ( !CommandLine()->FindParm( "-consoledebug" ) && CommandLine()->CheckParm( "-consolelog", &filename ) && filename )
			V_strncpy( s_consolelog, filename, sizeof( s_consolelog ) );
	}

	return s_consolelog;
}

#ifdef _WIN32
extern bool g_bVGui;
#endif

SpewRetval_t DedicatedSpewOutputFunc( SpewType_t spewType, char const *pMsg )
{
	if ( sys )
	{
		sys->Printf( "%s", pMsg );

		// -consolelog gets the early messages that come before Sys_InitGame sets up its own logging
		const char *filename = GetConsoleLogFilename();
		if ( filename[0] && pMsg[0] && g_pFullFileSystem )
		{
			FileHandle_t fh = g_pFullFileSystem->Open( filename, "a" );
			if ( fh != FILESYSTEM_INVALID_HANDLE )
			{
				g_pFullFileSystem->Write( pMsg, V_strlen( pMsg ), fh );
				g_pFullFileSystem->Close( fh );
			}
		}
	}

#ifdef _WIN32
	Plat_DebugString( pMsg );
#endif

	if ( spewType == SPEW_ERROR )
	{
#ifdef _WIN32
		if ( g_bVGui )
			MessageBox( NULL, pMsg, "Error", MB_OK | MB_TASKMODAL );
		TerminateProcess( GetCurrentProcess(), 1 );
#else
		fflush( stdout );
		_exit( 1 );
#endif
		return SPEW_ABORT;
	}

	if ( spewType == SPEW_ASSERT )
		return CommandLine()->FindParm( "-noassert" ) == 0 ? SPEW_DEBUGGER : SPEW_CONTINUE;

	return SPEW_CONTINUE;
}

static int Sys_GetExecutableName( char *out, int nMaxLen )
{
#ifdef _WIN32
	if ( !::GetModuleFileName( (HINSTANCE)GetModuleHandle( NULL ), out, nMaxLen ) )
		return 0;
#else
	V_strncpy( out, g_szEXEName, nMaxLen );
#endif
	return 1;
}

// The bin directory, even if the exe isn't in it
const char *UTIL_GetExecutableDir()
{
	static char exedir[MAX_PATH];

	exedir[0] = 0;
	if ( !Sys_GetExecutableName( exedir, sizeof( exedir ) ) )
		return NULL;

	char *pSlash = strrchr( exedir, '\\' );
	char *pSlash2 = strrchr( exedir, '/' );
	if ( pSlash2 > pSlash )
		pSlash = pSlash2;
	if ( pSlash )
		*pSlash = 0;

	int exeLen = V_strlen( exedir );
	if ( exeLen < 4 || exedir[exeLen - 4] != CORRECT_PATH_SEPARATOR || exedir[exeLen - 3] != 'b' || exedir[exeLen - 2] != 'i' || exedir[exeLen - 1] != 'n' )
	{
		Q_strncat( exedir, "\\bin", sizeof( exedir ), COPY_ALL_CHARACTERS );
		Q_FixSlashes( exedir );
	}

	return exedir;
}

const char *UTIL_GetBaseDir()
{
	static char basedir[MAX_PATH];

	char const *pOverrideDir = CommandLine()->CheckParm( "-basedir" );
	if ( pOverrideDir )
		return pOverrideDir;

	basedir[0] = 0;
	const char *pExeDir = UTIL_GetExecutableDir();
	if ( pExeDir )
	{
		V_strncpy( basedir, pExeDir, sizeof( basedir ) );
		int dirlen = V_strlen( basedir );
		if ( dirlen >= 4 && basedir[dirlen - 3] == 'b' && basedir[dirlen - 2] == 'i' && basedir[dirlen - 1] == 'n' )
			basedir[dirlen - 4] = 0;
	}

	return basedir;
}
