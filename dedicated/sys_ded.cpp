//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Entry point shared between Windows and Linux
//
//===========================================================================//

#include "isys.h"
#include "dedicated.h"
#include "engine_hlds_api.h"
#include "idedicatedexports.h"
#include "filesystem.h"
#include "filesystem_init.h"
#include "appframework/AppFramework.h"
#include "inputsystem/iinputsystem.h"
#include "tier0/vcrmode.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/strtools.h"
#include "tier2/tier2.h"
#include "vstdlib/cvar.h"

#ifdef _WIN32
#include "winlite.h"
#include <direct.h>
#include "console/TextConsoleWin32.h"
CTextConsoleWin32 console;
#else
#include <unistd.h>
#include <dlfcn.h>
#define _chdir chdir
#include "console/TextConsoleUnix.h"
CTextConsoleUnix console;
#endif

bool InitInstance();
int ProcessConsoleInput();
bool NET_Init();
void NET_Shutdown();
const char *UTIL_GetBaseDir();
SpewRetval_t DedicatedSpewOutputFunc( SpewType_t spewType, char const *pMsg );

// Always false, there's no vgui gui. Kept for the spew func
bool g_bVGui = false;

IDedicatedServerAPI *engine = NULL;

class CVCRHelpers : public IVCRHelpers
{
public:
	virtual void ErrorMessage( const char *pMsg )
	{
		printf( "ERROR: %s\n", pMsg );
	}

	virtual void *GetMainWindow()
	{
		return 0;
	}
};
static CVCRHelpers g_VCRHelpers;

// true = WM_QUIT
static bool HandleVCRHook()
{
#ifdef _WIN32
	MSG msg;
	while ( VCRHook_PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
	{
		if ( msg.message == WM_QUIT )
			return true;

		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}
#endif
	return false;
}

void RunServer()
{
	// Run 2 engine frames first to get the engine to load its resources.
	for ( int i = 0; i < 2; i++ )
	{
		if ( !engine->RunFrame() )
			return;
	}

	bool bDone = false;
	while ( !bDone )
	{
		if ( HandleVCRHook() )
			break;

		ProcessConsoleInput();

		if ( !engine->RunFrame() )
			bDone = true;

		sys->UpdateStatus( 0 );
	}
}

bool CDedicatedAppSystemGroup::Create()
{
#ifndef _WIN32
	if ( !console.Init() )
		return false;
#endif

	// Hook the debug output stuff (override the spew func in the appframework)
	SpewOutputFunc( DedicatedSpewOutputFunc );

	// The dedicated exports module for the engine to grab
	AppModule_t dedicatedModule = LoadModule( Sys_GetFactoryThis() );
	IAppSystem *pSystem = AddSystem( dedicatedModule, VENGINE_DEDICATEDEXPORTS_API_VERSION );
	if ( !pSystem )
		return false;

	return sys->LoadModules( this );
}

// Directory this module lives in, the filesystem sits next to it
static void GetThisModuleDir( char *pOut, int nMaxLen )
{
	pOut[0] = 0;
#ifdef _WIN32
	HMODULE hModule = NULL;
	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&GetThisModuleDir, &hModule ) )
		GetModuleFileNameA( hModule, pOut, nMaxLen );
#else
	Dl_info info;
	if ( dladdr( (void *)&GetThisModuleDir, &info ) && info.dli_fname )
		V_strncpy( pOut, info.dli_fname, nMaxLen );
#endif
	V_StripFilename( pOut );
}

// Stock's filesystem_init uses the platform bin folder for EXECUTABLE_PATH, the SDK one uses bin/
static void SetBasePaths( IFileSystem *pFileSystem )
{
	pFileSystem->RemoveSearchPaths( "EXECUTABLE_PATH" );

	char szPath[MAX_PATH];
	GetThisModuleDir( szPath, sizeof( szPath ) );
	pFileSystem->AddSearchPath( szPath, "EXECUTABLE_PATH" );
	pFileSystem->AddSearchPath( UTIL_GetBaseDir(), "BASE_PATH" );
}

bool CDedicatedAppSystemGroup::PreInit()
{
	// tier2 connects this up again from the factory
	g_pFullFileSystem = NULL;

	if ( !BaseClass::PreInit() )
		return false;

	CFSSteamSetupInfo steamInfo;
	steamInfo.m_pDirectoryName = NULL;
	steamInfo.m_bOnlyUseDirectoryName = false;
	steamInfo.m_bToolsMode = false;
	steamInfo.m_bSetSteamDLLPath = false;
	steamInfo.m_bSteam = g_pFullFileSystem->IsSteam();
	steamInfo.m_bNoGameInfo = steamInfo.m_bSteam;
	if ( FileSystem_SetupSteamEnvironment( steamInfo ) != FS_OK )
		return false;

	CFSMountContentInfo fsInfo;
	fsInfo.m_pFileSystem = g_pFullFileSystem;
	fsInfo.m_bToolsMode = false;
	fsInfo.m_pDirectoryName = steamInfo.m_GameInfoPath;
	if ( FileSystem_MountContent( fsInfo ) != FS_OK )
		return false;
	SetBasePaths( g_pFullFileSystem );

	if ( !NET_Init() )
		return false;

#ifdef _WIN32
	// Stock opens the admin gui here, we don't have one
	const bool bWantedGui = !CommandLine()->CheckParm( "-console" ) && !GetConsoleWindow();
#endif

	IInputSystem *inputsystem = (IInputSystem *)GetFactory()( INPUTSYSTEM_INTERFACE_VERSION, NULL );
	if ( inputsystem )
		inputsystem->SetConsoleTextMode( true );

	if ( !sys->CreateConsoleWindow() )
		return false;

#ifdef _WIN32
	if ( bWantedGui )
		Msg( "This dedicated.dll has no GUI, running in console mode. Start with -console to skip this message.\n" );
#endif

	return true;
}

int CDedicatedAppSystemGroup::Main()
{
#ifdef _WIN32
	if ( !console.Init() )
		return -1;
#endif

	if ( !CommandLine()->CheckParm( "-game" ) )
		CommandLine()->AppendParm( "-game", "garrysmod" );

	ModInfo_t info;
	info.m_pInstance = GetAppInstance();
	info.m_pBaseDirectory = UTIL_GetBaseDir();
	info.m_pInitialMod = CommandLine()->ParmValue( "-game", "hl2" );
	info.m_pInitialGame = CommandLine()->ParmValue( "-defaultgamedir", "hl2" );
	info.m_pParentAppSystemGroup = this;
	info.m_bTextMode = CommandLine()->CheckParm( "-textmode" ) != NULL;

	if ( engine->ModInit( info ) )
		engine->ModShutdown();

	return 0;
}

void CDedicatedAppSystemGroup::PostShutdown()
{
	sys->DestroyConsoleWindow();
	console.ShutDown();
	NET_Shutdown();
	BaseClass::PostShutdown();
}

void CDedicatedAppSystemGroup::Destroy()
{
}

#ifdef POSIX
char g_szEXEName[256];
#endif

static bool GetExecutableName( char *out, int nMaxLen )
{
#ifdef _WIN32
	return ::GetModuleFileName( (HINSTANCE)GetModuleHandle( NULL ), out, nMaxLen ) != 0;
#else
	V_strncpy( out, g_szEXEName, nMaxLen );
	return true;
#endif
}

// Where the exe runs from
static void UTIL_ComputeBaseDir( char *pBaseDir, int nMaxLen )
{
	pBaseDir[0] = 0;

	if ( GetExecutableName( pBaseDir, nMaxLen ) )
	{
		char *pBuffer = strrchr( pBaseDir, CORRECT_PATH_SEPARATOR );
		if ( pBuffer && *pBuffer )
			*( pBuffer + 1 ) = '\0';

		int j = V_strlen( pBaseDir );
		if ( j > 0 && ( pBaseDir[j - 1] == '\\' || pBaseDir[j - 1] == '/' ) )
			pBaseDir[j - 1] = 0;
	}

	char const *pOverrideDir = CommandLine()->CheckParm( "-basedir" );
	if ( pOverrideDir )
		V_strncpy( pBaseDir, pOverrideDir, nMaxLen );

	Q_strlower( pBaseDir );
	Q_FixSlashes( pBaseDir );
}

// Stock compiles the filesystem in. We load ours from filesystem_stdio next to us
class CDedicatedSteamApplication : public CSteamApplication
{
public:
	CDedicatedSteamApplication( CSteamAppSystemGroup *pAppSystemGroup ) : CSteamApplication( pAppSystemGroup ) {}

	virtual bool Create()
	{
		AppModule_t cvarModule = LoadModule( VStdLib_GetICVarFactory() );
		AddSystem( cvarModule, CVAR_INTERFACE_VERSION );

		char szPath[MAX_PATH];
		GetThisModuleDir( szPath, sizeof( szPath ) );
		V_AppendSlash( szPath, sizeof( szPath ) );
		V_strncat( szPath, "filesystem_stdio" MODULE_EXT, sizeof( szPath ) );

		AppModule_t fileSystemModule = LoadModule( szPath );
		m_pFileSystem = (IFileSystem *)AddSystem( fileSystemModule, FILESYSTEM_INTERFACE_VERSION );
		if ( !m_pFileSystem )
		{
			Warning( "Unable to load the file system! (%s)\n", szPath );
			return false;
		}

		return true;
	}

	// Same as CSteamApplication::Main, except EXECUTABLE_PATH is the platform bin folder we sit in like stock
	virtual int Main()
	{
		SetBasePaths( m_pFileSystem );

		m_pChildAppSystemGroup->Setup( m_pFileSystem, this );
		return m_pChildAppSystemGroup->Run();
	}
};

int main( int argc, char **argv )
{
#ifdef POSIX
	V_strncpy( g_szEXEName, *argv, sizeof( g_szEXEName ) );
	CommandLine()->CreateCmdLine( argc, argv );
#else
	CommandLine()->CreateCmdLine( VCRHook_GetCommandLine() );
#endif

	const char *filename;
	if ( CommandLine()->CheckParm( "-vcrrecord", &filename ) )
	{
		if ( !VCRStart( filename, true, &g_VCRHelpers ) )
		{
			Error( "-vcrrecord: can't open '%s' for writing.\n", filename );
			return -1;
		}
	}
	else if ( CommandLine()->CheckParm( "-vcrplayback", &filename ) )
	{
		if ( !VCRStart( filename, false, &g_VCRHelpers ) )
		{
			Error( "-vcrplayback: can't open '%s' for reading.\n", filename );
			return -1;
		}
	}

	// Figure out the directory the executable is running from
	// and make that be the current working directory
	char pBasedir[MAX_PATH];
	UTIL_ComputeBaseDir( pBasedir, MAX_PATH );
	_chdir( pBasedir );

	// Rehook the command line through VCR mode.
#ifndef POSIX
	CommandLine()->CreateCmdLine( VCRHook_GetCommandLine() );
#endif

	// GMod's dedicated always runs garrysmod
	CommandLine()->RemoveParm( "-game" );
	CommandLine()->AppendParm( "-game", "garrysmod" );

	if ( !InitInstance() )
		return -1;

	CDedicatedAppSystemGroup dedicatedSystems;
	CDedicatedSteamApplication steamApplication( &dedicatedSystems );
	return steamApplication.Run();
}
