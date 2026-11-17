#pragma once

#include <cstdint>

// The +host_workshop_collection downloader. Stock has this in dedicated.dll / dedicated_srv.so
namespace GarrysMod
{
namespace DedicatedServer
{
	// Fetches the collection, downloads what's missing and hands every addon to Addons()->AddAddon
	// Called from Addon::FileSystem::ScanForSubscriptions
	void RunAddonProcess( const char *pszCollection, bool bAutoUpdate );
}
}
