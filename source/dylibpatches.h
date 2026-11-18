#ifndef DYLIBPATCHES_H
#define DYLIBPATCHES_H

#pragma once

#ifdef EXTERNAL_DYLIB_PATCHES

class CSysModule;

// Patches for other game dlls. Windows x64 client only
namespace DylibPatches
{
	void OnModuleLoaded( const char *pFileName, CSysModule *pModule );
}

#endif

#endif
