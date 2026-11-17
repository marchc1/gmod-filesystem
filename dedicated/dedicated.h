//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Defines a group of app systems that all have the same lifetime
// that need to be connected/initialized, etc. in a well-defined order
//
//===========================================================================//

#ifndef DEDICATED_H
#define DEDICATED_H

#ifdef _WIN32
#pragma once
#endif

#include "appframework/tier2app.h"

class IDedicatedServerAPI;

extern IDedicatedServerAPI *engine;

#ifdef POSIX
extern char g_szEXEName[ 256 ];
#endif

// Stock uses CVguiSteamApp on Windows, we skip the vgui gui so tier2 is all we need
class CDedicatedAppSystemGroup : public CTier2SteamApp
{
	typedef CTier2SteamApp BaseClass;

public:
	virtual bool Create();
	virtual bool PreInit();
	virtual int Main();
	virtual void PostShutdown();
	virtual void Destroy();

	AppModule_t LoadModule( CreateInterfaceFn factory )
	{
		return CSteamAppSystemGroup::LoadModule( factory );
	}

	bool AddSystems( AppSystemInfo_t *pSystems )
	{
		return CSteamAppSystemGroup::AddSystems( pSystems );
	}

	void *FindSystem( const char *pInterfaceName )
	{
		return CSteamAppSystemGroup::FindSystem( pInterfaceName );
	}
};

#endif // DEDICATED_H
