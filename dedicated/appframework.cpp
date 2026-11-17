//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Defines a group of app systems that all have the same lifetime
// that need to be connected/initialized, etc. in a well-defined order
//
// The engine has its own copy of this code and reads our group's members
// directly (FindSystem walks up to the parent), so the layout has to match
// what GMod's dedicated.dll was built with.
//
//=============================================================================//

#include "appframework/IAppSystemGroup.h"
#include "appframework/AppFramework.h"
#include "appframework/iappsystem.h"
#include "tier1/interface.h"
#include "tier1/strtools.h"
#include "filesystem.h"
#include "filesystem_init.h"
#include "tier0/dbg.h"
#include "vstdlib/cvar.h"
#include "icvar.h"

#include <cstdlib>

#include "tier0/memdbgon.h"

#ifdef PLATFORM_64BITS
static_assert( sizeof( CAppSystemGroup ) == 0x80, "CAppSystemGroup doesn't match the engine" );
static_assert( sizeof( CSteamAppSystemGroup ) == 0x88 + MAX_PATH + 4, "CSteamAppSystemGroup doesn't match the engine" );
#endif

CAppSystemGroup::CAppSystemGroup( CAppSystemGroup *pAppSystemParent ) : m_SystemDict( false, 0, 16 )
{
	m_pParentAppSystem = pAppSystemParent;
	m_nErrorStage = NONE;
}

CSysModule *CAppSystemGroup::LoadModuleDLL( const char *pDLLName )
{
	return Sys_LoadModule( pDLLName );
}

AppModule_t CAppSystemGroup::LoadModule( const char *pDLLName )
{
	// Remove the extension when creating the name.
	int nLen = V_strlen( pDLLName ) + 1;
	char *pModuleName = (char *)stackalloc( nLen );
	Q_StripExtension( pDLLName, pModuleName, nLen );

	// See if we already loaded it...
	for ( int i = m_Modules.Count(); --i >= 0; )
	{
		if ( m_Modules[i].m_pModuleName && !Q_stricmp( pModuleName, m_Modules[i].m_pModuleName ) )
			return i;
	}

	CSysModule *pSysModule = LoadModuleDLL( pDLLName );
	if ( !pSysModule )
	{
		Warning( "AppFramework : Unable to load module %s!\n", pDLLName );
		return APP_MODULE_INVALID;
	}

	int nIndex = m_Modules.AddToTail();
	m_Modules[nIndex].m_pModule = pSysModule;
	m_Modules[nIndex].m_Factory = 0;
	m_Modules[nIndex].m_pModuleName = (char *)malloc( nLen );
	Q_strncpy( m_Modules[nIndex].m_pModuleName, pModuleName, nLen );

	return nIndex;
}

AppModule_t CAppSystemGroup::LoadModule( CreateInterfaceFn factory )
{
	if ( !factory )
	{
		Warning( "AppFramework : Unable to load module %p!\n", factory );
		return APP_MODULE_INVALID;
	}

	// See if we already loaded it...
	for ( int i = m_Modules.Count(); --i >= 0; )
	{
		if ( m_Modules[i].m_Factory && m_Modules[i].m_Factory == factory )
			return i;
	}

	int nIndex = m_Modules.AddToTail();
	m_Modules[nIndex].m_pModule = NULL;
	m_Modules[nIndex].m_Factory = factory;
	m_Modules[nIndex].m_pModuleName = NULL;
	return nIndex;
}

void CAppSystemGroup::UnloadAllModules()
{
	// NOTE: Iterate in reverse order so they are unloaded in opposite order
	// from loading
	for ( int i = m_Modules.Count(); --i >= 0; )
	{
		if ( m_Modules[i].m_pModule )
			Sys_UnloadModule( m_Modules[i].m_pModule );
		if ( m_Modules[i].m_pModuleName )
			free( m_Modules[i].m_pModuleName );
	}
	m_Modules.RemoveAll();
}

IAppSystem *CAppSystemGroup::AddSystem( AppModule_t module, const char *pInterfaceName )
{
	if ( module == APP_MODULE_INVALID )
		return NULL;

	Assert( ( module >= 0 ) && ( module < m_Modules.Count() ) );
	CreateInterfaceFn pFactory = m_Modules[module].m_pModule ? Sys_GetFactory( m_Modules[module].m_pModule ) : m_Modules[module].m_Factory;

	int retval;
	void *pSystem = pFactory( pInterfaceName, &retval );
	if ( ( retval != IFACE_OK ) || ( !pSystem ) )
	{
		Warning( "AppFramework : Unable to create system %s!\n", pInterfaceName );
		return NULL;
	}

	IAppSystem *pAppSystem = static_cast<IAppSystem *>( pSystem );
	int sysIndex = m_Systems.AddToTail( pAppSystem );

	// Inserting into the dict will help us do named lookup later
	MEM_ALLOC_CREDIT();
	m_SystemDict.Insert( pInterfaceName, sysIndex );
	return pAppSystem;
}

static char const *g_StageLookup[] =
{
	"CREATION",
	"CONNECTION",
	"PREINITIALIZATION",
	"INITIALIZATION",
	"SHUTDOWN",
	"POSTSHUTDOWN",
	"DISCONNECTION",
	"DESTRUCTION",
	"NONE",
};

void CAppSystemGroup::ReportStartupFailure( int nErrorStage, int nSysIndex )
{
	char const *pszStageDesc = "Unknown";
	if ( nErrorStage >= 0 && nErrorStage < ARRAYSIZE( g_StageLookup ) )
		pszStageDesc = g_StageLookup[nErrorStage];

	char const *pszSystemName = "(Unknown)";
	for ( unsigned short i = m_SystemDict.First(); i != m_SystemDict.InvalidIndex(); i = m_SystemDict.Next( i ) )
	{
		if ( m_SystemDict[i] != nSysIndex )
			continue;

		pszSystemName = m_SystemDict.GetElementName( i );
		break;
	}

	Warning( "System (%s) failed during stage %s\n", pszSystemName, pszStageDesc );
}

void CAppSystemGroup::AddSystem( IAppSystem *pAppSystem, const char *pInterfaceName )
{
	if ( !pAppSystem )
		return;

	int sysIndex = m_Systems.AddToTail( pAppSystem );

	MEM_ALLOC_CREDIT();
	m_SystemDict.Insert( pInterfaceName, sysIndex );
}

void CAppSystemGroup::RemoveAllSystems()
{
	m_Systems.RemoveAll();
	m_SystemDict.RemoveAll();
}

bool CAppSystemGroup::AddSystems( AppSystemInfo_t *pSystemList )
{
	while ( pSystemList->m_pModuleName[0] )
	{
		AppModule_t module = LoadModule( pSystemList->m_pModuleName );
		IAppSystem *pSystem = AddSystem( module, pSystemList->m_pInterfaceName );
		if ( !pSystem )
		{
			Warning( "Unable to load interface %s from %s\n", pSystemList->m_pInterfaceName, pSystemList->m_pModuleName );
			return false;
		}
		++pSystemList;
	}

	return true;
}

void *CAppSystemGroup::FindSystem( const char *pSystemName )
{
	unsigned short i = m_SystemDict.Find( pSystemName );
	if ( i != m_SystemDict.InvalidIndex() )
		return m_Systems[m_SystemDict[i]];

	// If it's not an interface we know about, it could be an older
	// version of an interface, or maybe something implemented by
	// one of the instantiated interfaces...
	for ( i = 0; i < m_Systems.Count(); ++i )
	{
		void *pInterface = m_Systems[i]->QueryInterface( pSystemName );
		if ( pInterface )
			return pInterface;
	}

	if ( m_pParentAppSystem )
	{
		void *pInterface = m_pParentAppSystem->FindSystem( pSystemName );
		if ( pInterface )
			return pInterface;
	}

	return NULL;
}

CAppSystemGroup *CAppSystemGroup::GetParent()
{
	return m_pParentAppSystem;
}

bool CAppSystemGroup::ConnectSystems()
{
	for ( int i = 0; i < m_Systems.Count(); ++i )
	{
		if ( !m_Systems[i]->Connect( GetFactory() ) )
		{
			ReportStartupFailure( CONNECTION, i );
			return false;
		}
	}
	return true;
}

void CAppSystemGroup::DisconnectSystems()
{
	// Disconnect in reverse order of connection
	for ( int i = m_Systems.Count(); --i >= 0; )
		m_Systems[i]->Disconnect();
}

InitReturnVal_t CAppSystemGroup::InitSystems()
{
	for ( int i = 0; i < m_Systems.Count(); ++i )
	{
		InitReturnVal_t nRetVal = m_Systems[i]->Init();
		if ( nRetVal != INIT_OK )
		{
			ReportStartupFailure( INITIALIZATION, i );
			return nRetVal;
		}
	}
	return INIT_OK;
}

void CAppSystemGroup::ShutdownSystems()
{
	// Shutdown in reverse order of initialization
	for ( int i = m_Systems.Count(); --i >= 0; )
		m_Systems[i]->Shutdown();
}

CAppSystemGroup::AppSystemGroupStage_t CAppSystemGroup::GetErrorStage() const
{
	return m_nErrorStage;
}

static CAppSystemGroup *s_pCurrentAppSystem;
void *AppSystemCreateInterfaceFn( const char *pName, int *pReturnCode )
{
	void *pInterface = s_pCurrentAppSystem->FindSystem( pName );
	if ( pReturnCode )
		*pReturnCode = pInterface ? IFACE_OK : IFACE_FAILED;
	return pInterface;
}

CreateInterfaceFn CAppSystemGroup::GetFactory()
{
	return AppSystemCreateInterfaceFn;
}

int CAppSystemGroup::Run()
{
	// The factory now uses this app system group
	s_pCurrentAppSystem = this;

	// Load, connect, init
	int nRetVal = OnStartup();
	if ( m_nErrorStage != NONE )
		return nRetVal;

	// Main loop implemented by the application
	nRetVal = Main();

	// Shutdown, disconnect, unload
	OnShutdown();

	// The factory now uses the parent's app system group
	s_pCurrentAppSystem = GetParent();

	return nRetVal;
}

int CAppSystemGroup::Startup()
{
	return OnStartup();
}

void CAppSystemGroup::Shutdown()
{
	return OnShutdown();
}

int CAppSystemGroup::OnStartup()
{
	// The factory now uses this app system group
	s_pCurrentAppSystem = this;

	m_nErrorStage = NONE;

	if ( !Create() )
	{
		m_nErrorStage = CREATION;
		return -1;
	}

	if ( !ConnectSystems() )
	{
		m_nErrorStage = CONNECTION;
		return -1;
	}

	if ( !PreInit() )
	{
		m_nErrorStage = PREINITIALIZATION;
		return -1;
	}

	int nRetVal = InitSystems();
	if ( nRetVal != INIT_OK )
	{
		m_nErrorStage = INITIALIZATION;
		return -1;
	}

	return nRetVal;
}

void CAppSystemGroup::OnShutdown()
{
	// The factory now uses this app system group
	s_pCurrentAppSystem = this;

	switch ( m_nErrorStage )
	{
	case NONE:
		break;

	case PREINITIALIZATION:
	case INITIALIZATION:
		goto disconnect;

	case CREATION:
	case CONNECTION:
		goto destroy;

	default:
		break;
	}

	ShutdownSystems();
	PostShutdown();

disconnect:
	DisconnectSystems();

destroy:
	RemoveAllSystems();
	UnloadAllModules();
	Destroy();
}

CSteamAppSystemGroup::CSteamAppSystemGroup( IFileSystem *pFileSystem, CAppSystemGroup *pAppSystemParent ) : CAppSystemGroup( pAppSystemParent )
{
	m_pFileSystem = pFileSystem;
	m_pGameInfoPath[0] = '\0';
}

void CSteamAppSystemGroup::Setup( IFileSystem *pFileSystem, CAppSystemGroup *pParentAppSystem )
{
	m_pFileSystem = pFileSystem;
	m_pParentAppSystem = pParentAppSystem;
}

CSysModule *CSteamAppSystemGroup::LoadModuleDLL( const char *pDLLName )
{
	return m_pFileSystem->LoadModule( pDLLName );
}

const char *CSteamAppSystemGroup::GetGameInfoPath() const
{
	return m_pGameInfoPath;
}

bool CSteamAppSystemGroup::SetupSearchPaths( const char *pStartingDir, bool bOnlyUseStartingDir, bool bIsTool )
{
	CFSSteamSetupInfo steamInfo;
	steamInfo.m_pDirectoryName = pStartingDir;
	steamInfo.m_bOnlyUseDirectoryName = bOnlyUseStartingDir;
	steamInfo.m_bToolsMode = bIsTool;
	steamInfo.m_bSetSteamDLLPath = true;
	steamInfo.m_bSteam = m_pFileSystem->IsSteam();
	if ( FileSystem_SetupSteamEnvironment( steamInfo ) != FS_OK )
		return false;

	CFSMountContentInfo fsInfo;
	fsInfo.m_pFileSystem = m_pFileSystem;
	fsInfo.m_bToolsMode = bIsTool;
	fsInfo.m_pDirectoryName = steamInfo.m_GameInfoPath;
	if ( FileSystem_MountContent( fsInfo ) != FS_OK )
		return false;

	CFSSearchPathsInit searchPathsInit;
	searchPathsInit.m_pDirectoryName = steamInfo.m_GameInfoPath;
	searchPathsInit.m_pFileSystem = fsInfo.m_pFileSystem;
	if ( FileSystem_LoadSearchPaths( searchPathsInit ) != FS_OK )
		return false;

	FileSystem_AddSearchPath_Platform( fsInfo.m_pFileSystem, steamInfo.m_GameInfoPath );
	Q_strncpy( m_pGameInfoPath, steamInfo.m_GameInfoPath, sizeof( m_pGameInfoPath ) );
	return true;
}

CSteamApplication::CSteamApplication( CSteamAppSystemGroup *pAppSystemGroup )
{
	m_pChildAppSystemGroup = pAppSystemGroup;
	m_pFileSystem = NULL;
	m_bSteam = false;
}

bool CSteamApplication::Create()
{
	FileSystem_SetErrorMode( FS_ERRORMODE_AUTO );

	char pFileSystemDLL[MAX_PATH];
	if ( FileSystem_GetFileSystemDLLName( pFileSystemDLL, MAX_PATH, m_bSteam ) != FS_OK )
		return false;

	AppModule_t cvarModule = LoadModule( VStdLib_GetICVarFactory() );
	AddSystem( cvarModule, CVAR_INTERFACE_VERSION );

	AppModule_t fileSystemModule = LoadModule( pFileSystemDLL );
	m_pFileSystem = (IFileSystem *)AddSystem( fileSystemModule, FILESYSTEM_INTERFACE_VERSION );
	if ( !m_pFileSystem )
	{
		Error( "Unable to load %s", pFileSystemDLL );
		return false;
	}

	return true;
}

void CSteamApplication::Destroy()
{
	m_pFileSystem = NULL;
}

bool CSteamApplication::PreInit()
{
	return true;
}

void CSteamApplication::PostShutdown()
{
}

int CSteamApplication::Main()
{
	// Now that Steam is loaded, we can load up main libraries through steam
	if ( FileSystem_SetBasePaths( m_pFileSystem ) != FS_OK )
		return 0;

	m_pChildAppSystemGroup->Setup( m_pFileSystem, this );
	return m_pChildAppSystemGroup->Run();
}

int CSteamApplication::Startup()
{
	int nRetVal = BaseClass::Startup();
	if ( GetErrorStage() != NONE )
		return nRetVal;

	if ( FileSystem_SetBasePaths( m_pFileSystem ) != FS_OK )
		return 0;

	m_pChildAppSystemGroup->Setup( m_pFileSystem, this );
	return m_pChildAppSystemGroup->Startup();
}

void CSteamApplication::Shutdown()
{
	m_pChildAppSystemGroup->Shutdown();
	BaseClass::Shutdown();
}

static void *s_HInstance;

void *GetAppInstance()
{
	return s_HInstance;
}

void SetAppInstance( void *hInstance )
{
	s_HInstance = hInstance;
}
