//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "isys.h"
#include "dedicated.h"
#include "engine_hlds_api.h"
#include "tier0/vcrmode.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "tier1/strtools.h"
#include "materialsystem/imaterialsystem.h"
#include "istudiorender.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "datacache/idatacache.h"
#include "datacache/imdlcache.h"
#include "vphysics_interface.h"
#include "icvar.h"
#include "filesystem/IQueuedLoader.h"
#include "console/TextConsoleUnix.h"

extern CTextConsoleUnix console;

class CSys : public ISys
{
public:
	virtual ~CSys();

	virtual bool LoadModules( CDedicatedAppSystemGroup *pAppSystemGroup );

	void Sleep( int msec );
	bool GetExecutableName( char *out );
	void ErrorMessage( int level, const char *msg );

	void WriteStatusText( char *szText ) {}
	void UpdateStatus( int force ) {}

	intp LoadLibrary( char *lib );
	void FreeLibrary( intp library );

	bool CreateConsoleWindow() { return true; }
	void DestroyConsoleWindow() {}

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
	usleep( msec * 1000 );
}

intp CSys::LoadLibrary( char *lib )
{
	char cwd[1024];
	if ( !getcwd( cwd, sizeof( cwd ) ) )
		ErrorMessage( 1, "Sys_LoadLibrary: Couldn't determine current directory." );

	if ( cwd[strlen( cwd ) - 1] == '/' )
		cwd[strlen( cwd ) - 1] = 0;

	char absolute_lib[1024];
	Q_snprintf( absolute_lib, sizeof( absolute_lib ), "%s/%s", cwd, lib );

	void *hDll = dlopen( absolute_lib, RTLD_NOW );
	if ( !hDll )
		ErrorMessage( 1, dlerror() );
	return (intp)hDll;
}

void CSys::FreeLibrary( intp library )
{
	if ( library )
		dlclose( (void *)library );
}

bool CSys::GetExecutableName( char *out )
{
	char *name = strrchr( g_szEXEName, '/' );
	if ( !name )
		return false;

	strcpy( out, name + 1 );
	return true;
}

void CSys::ErrorMessage( int level, const char *msg )
{
	Error( "%s\n", msg );
	exit( -1 );
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

static std::string s_strBinDir;

// Server bins are <name>_srv.so on some branches and <name>.so on others
static const char *ModuleName( const char *pBase )
{
	static std::string s_Names[16];
	static int s_nNext = 0;

	std::string &out = s_Names[s_nNext++ % 16];
	out = std::string( pBase ) + "_srv.so";
	if ( access( ( s_strBinDir + out ).c_str(), F_OK ) != 0 )
		out = std::string( pBase ) + ".so";
	return out.c_str();
}

bool CSys::LoadModules( CDedicatedAppSystemGroup *pAppSystemGroup )
{
	Dl_info info;
	if ( dladdr( (void *)&ModuleName, &info ) && info.dli_fname )
	{
		s_strBinDir = info.dli_fname;
		s_strBinDir.erase( s_strBinDir.find_last_of( '/' ) + 1 );
	}

	AppSystemInfo_t appSystems[] =
	{
		{ ModuleName( "engine" ),				CVAR_QUERY_INTERFACE_VERSION },
		{ ModuleName( "soundemittersystem" ),	SOUNDEMITTERSYSTEM_INTERFACE_VERSION },
		{ ModuleName( "materialsystem" ),		MATERIAL_SYSTEM_INTERFACE_VERSION },
		{ ModuleName( "studiorender" ),		STUDIO_RENDER_INTERFACE_VERSION },
		{ ModuleName( "vphysics" ),			VPHYSICS_INTERFACE_VERSION },
		{ ModuleName( "datacache" ),			DATACACHE_INTERFACE_VERSION },
		{ ModuleName( "datacache" ),			MDLCACHE_INTERFACE_VERSION },
		{ ModuleName( "datacache" ),			STUDIO_DATA_CACHE_INTERFACE_VERSION },
		// Stock has the queued loader in dedicated_srv.so, ours lives in the filesystem
		{ "filesystem_stdio.so",				QUEUEDLOADER_INTERFACE_VERSION },
		{ ModuleName( "engine" ),				VENGINE_HLDS_API_VERSION },
		{ "", "" }
	};

	if ( !pAppSystemGroup->AddSystems( appSystems ) )
		return false;

	engine = (IDedicatedServerAPI *)pAppSystemGroup->FindSystem( VENGINE_HLDS_API_VERSION );

	IMaterialSystem *pMaterialSystem = (IMaterialSystem *)pAppSystemGroup->FindSystem( MATERIAL_SYSTEM_INTERFACE_VERSION );
	pMaterialSystem->SetShaderAPI( ModuleName( "shaderapiempty" ) );
	return true;
}

bool NET_Init()
{
	return true;
}

void NET_Shutdown()
{
}

int main( int argc, char **argv );

extern "C" __attribute__( ( visibility( "default" ) ) ) int DedicatedMain( int argc, char **argv )
{
	return main( argc, argv );
}
