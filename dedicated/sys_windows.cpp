//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//
#include "winlite.h"
#include <winsock.h>
#include <shellapi.h>
#include <eh.h>

#include "isys.h"
#include "console/conproc.h"
#include "console/TextConsoleWin32.h"
#include "dedicated.h"
#include "engine_hlds_api.h"
#include "tier0/vcrmode.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/strtools.h"
#include "inputsystem/iinputsystem.h"
#include "appframework/AppFramework.h"
#include "materialsystem/imaterialsystem.h"
#include "istudiorender.h"
#include "vgui/IVGui.h"
#include "icvar.h"
#include "datacache/idatacache.h"
#include "datacache/imdlcache.h"
#include "vphysics_interface.h"
#include "filesystem.h"
#include "filesystem/IQueuedLoader.h"
#include "steam/steam_api.h"

extern CTextConsoleWin32 console;

class CSys : public ISys
{
public:
	virtual ~CSys();

	virtual bool LoadModules( CDedicatedAppSystemGroup *pAppSystemGroup );

	void Sleep( int msec );
	bool GetExecutableName( char *out );
	void ErrorMessage( int level, const char *msg );

	void WriteStatusText( char *szText );
	void UpdateStatus( int force );

	intp LoadLibrary( char *lib );
	void FreeLibrary( intp library );

	bool CreateConsoleWindow();
	void DestroyConsoleWindow();

	void ConsoleOutput( char *string );
	char *ConsoleInput( int index, char *buf, int buflen );
	void Printf( PRINTF_FORMAT_STRING const char *fmt, ... );
};

static CSys g_Sys;
ISys *sys = &g_Sys;

CSys::~CSys()
{
	sys = NULL;
}

void CSys::Sleep( int msec )
{
	ThreadSleep( msec );
}

intp CSys::LoadLibrary( char *lib )
{
	return (intp)::LoadLibraryA( lib );
}

void CSys::FreeLibrary( intp library )
{
	if ( library )
		::FreeLibrary( (HMODULE)library );
}

bool CSys::GetExecutableName( char *out )
{
	return ::GetModuleFileName( (HINSTANCE)GetModuleHandle( NULL ), out, 256 ) != 0;
}

void CSys::ErrorMessage( int level, const char *msg )
{
	MessageBox( NULL, msg, "Half-Life", MB_OK );
	PostQuitMessage( 0 );
}

void CSys::UpdateStatus( int force )
{
	static double tLast = 0.0;

	if ( !engine )
		return;

	double tCurrent = Plat_FloatTime();
	if ( !force && ( tCurrent - tLast ) < 0.5 )
		return;

	tLast = tCurrent;

	float fps;
	int n, nMax;
	char szMap[64];
	char szHostname[128];
	engine->UpdateStatus( &fps, &n, &nMax, szMap, sizeof( szMap ) );
	engine->UpdateHostname( szHostname, sizeof( szHostname ) );

	console.SetTitle( szHostname );

	if ( !szMap[0] )
		V_strncpy( szMap, "<NO MAP>", sizeof( szMap ) );

	char szPrompt[256];
	Q_snprintf( szPrompt, sizeof( szPrompt ), "%.1f fps %2i/%2i on map %16s", (float)fps, n, nMax, szMap );

	console.SetStatusLine( szPrompt );
	console.UpdateStatus();
}

void CSys::ConsoleOutput( char *string )
{
	console.Print( string );
}

void CSys::Printf( PRINTF_FORMAT_STRING const char *fmt, ... )
{
	va_list argptr;
	char szText[1024];

	va_start( argptr, fmt );
	Q_vsnprintf( szText, sizeof( szText ), fmt, argptr );
	va_end( argptr );

	ConsoleOutput( szText );
}

char *CSys::ConsoleInput( int index, char *buf, int buflen )
{
	return console.GetLine( index, buf, buflen );
}

void CSys::WriteStatusText( char *szText )
{
	SetConsoleTitle( szText );
}

bool CSys::CreateConsoleWindow()
{
	// srcds started from a terminal already has one
	if ( !GetConsoleWindow() && !AllocConsole() )
		return false;

	InitConProc();
	return true;
}

void CSys::DestroyConsoleWindow()
{
	FreeConsole();

	// shut down QHOST hooks if necessary
	DeinitConProc();
}

bool CSys::LoadModules( CDedicatedAppSystemGroup *pAppSystemGroup )
{
	AppSystemInfo_t appSystems[] =
	{
		{ "engine.dll",				CVAR_QUERY_INTERFACE_VERSION },	// NOTE: This one must be first!!
		{ "inputsystem.dll",		INPUTSYSTEM_INTERFACE_VERSION },
		{ "materialsystem.dll",		MATERIAL_SYSTEM_INTERFACE_VERSION },
		{ "studiorender.dll",		STUDIO_RENDER_INTERFACE_VERSION },
		{ "vphysics.dll",			VPHYSICS_INTERFACE_VERSION },
		{ "datacache.dll",			DATACACHE_INTERFACE_VERSION },
		{ "datacache.dll",			MDLCACHE_INTERFACE_VERSION },
		{ "datacache.dll",			STUDIO_DATA_CACHE_INTERFACE_VERSION },
		{ "vgui2.dll",				VGUI_IVGUI_INTERFACE_VERSION },
		{ "engine.dll",				VENGINE_HLDS_API_VERSION },
		// Stock has the queued loader in dedicated.dll, ours lives in the filesystem
		{ "filesystem_stdio.dll",	QUEUEDLOADER_INTERFACE_VERSION },
		{ "", "" }
	};

	if ( !pAppSystemGroup->AddSystems( appSystems ) )
		return false;

	engine = (IDedicatedServerAPI *)pAppSystemGroup->FindSystem( VENGINE_HLDS_API_VERSION );

	IMaterialSystem *pMaterialSystem = (IMaterialSystem *)pAppSystemGroup->FindSystem( MATERIAL_SYSTEM_INTERFACE_VERSION );
	pMaterialSystem->SetShaderAPI( "shaderapiempty.dll" );
	return true;
}

bool NET_Init()
{
	WSADATA wsaData;
	if ( WSAStartup( MAKEWORD( 1, 1 ), &wsaData ) != 0 )
	{
		char msg[256];
		Q_snprintf( msg, sizeof( msg ), "Winsock 1.1 unavailable...\n" );
		sys->Printf( "%s", msg );
		Plat_DebugString( msg );
		return false;
	}
	return true;
}

void NET_Shutdown()
{
	WSACleanup();
}

int main( int argc, char **argv );

static void MiniDumpFunction( unsigned int nExceptionCode, EXCEPTION_POINTERS *pException )
{
	SteamAPI_WriteMiniDump( nExceptionCode, pException, 0 );
}

extern "C" __declspec( dllexport ) int DedicatedMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow )
{
	SetAppInstance( hInstance );

	// main reads the command line from tier0, argv is only here for its signature
	int argc = 0;
	LPWSTR *wargv = CommandLineToArgvW( GetCommandLineW(), &argc );
	CommandLine()->CreateCmdLine( VCRHook_GetCommandLine() );

	int ret;
	if ( !Plat_IsInDebugSession() && !CommandLine()->FindParm( "-nominidumps" ) )
	{
		_se_translator_function old = _set_se_translator( MiniDumpFunction );
		try // this try block allows the SE translator to work
		{
			ret = main( argc, NULL );
		}
		catch ( ... )
		{
			ret = -1;
		}
		_set_se_translator( old );
	}
	else
	{
		ret = main( argc, NULL );
	}

	GlobalFree( wargv );
	return ret;
}

// Stock exports tier1's cvar
#pragma comment( linker, "/EXPORT:cvar=?cvar@@3PEAVICvar@@EA,DATA" )
