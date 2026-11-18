#include "GameDepotSystem.h"
#include <stdlib.h>
#include <basetypes.h>
#include "tier0/dbg.h"
#include "tier1/keyvalues.h"
#include "filesystem.h"
#include <sdk_backports.h>
#include "public/IGet.h"
#include <Bootil/Bootil.h>
#include <Bootil/Types/String.h>
#include "steam/isteamapps.h"
#include "basefilesystem.h"

static const std::vector<IGameDepotSystem::Information>& MountableGames()
{
	static const std::vector<IGameDepotSystem::Information> mountableGames =
	{
		// mounted, enabled, owned, installed, retail, bundled
		{ 220,     "Half-Life 2 & Episodes",            "hl2",               true,  true,  false, false, true,  false, { "episodic", "ep2", "lostcoast" } },
		{ 240,     "Counter-Strike: Source",            "cstrike",           true,  true,  false, false, true,  false },
		{ 440,     "Team Fortress 2",                   "tf",                true,  true,  false, false, false, false },
		{ 300,     "Day of Defeat: Source",             "dod",               false, true,  false, false, false, false },
		{ 320,     "Half-Life 2: Deathmatch",           "hl2mp",             false, true,  false, false, false, false },
		{ 280,     "Half-Life: Source",                 "hl1",               false, true,  false, false, false, false },
		{ 360,     "Half-Life Deathmatch: Source",      "hl1mp",             false, true,  false, false, false, false, { "hl1" } },
		{ 22208,   "Zeno Clash (Model Pack)",           "zeno_clash",        false, false, false, false, false, false },
		{ 400,     "Portal",                            "portal",            false, true,  false, false, false, false },
		{ 17530,   "D.I.P.R.I.P.",                      "diprip",            false, false, false, false, false, false },
		{ 17500,   "Zombie Panic! Source",              "zps",               false, false, false, false, false, false, { "vpks" } },
		{ 17570,   "Pirates, Vikings and Knights II",   "pvkii",             false, false, false, false, false, false },
		{ 17580,   "Dystopia",                          "dystopia",          false, false, false, false, false, false },
		{ 17700,   "Insurgency (Source Mod)",           "insurgency",        false, false, false, false, false, false },
		{ 222880,  "Insurgency (Standalone)",           "insurgency",        false, false, false, false, false, false },
		{ 17510,   "Age of Chivalry",                   "ageofchivalry",     false, false, false, false, false, false },
		{ 550,     "Left 4 Dead 2",                     "left4dead2",        false, true,  false, false, false, false, { "left4dead2_dlc1", "left4dead2_dlc2", "left4dead2_dlc3", "update" } },
		{ 500,     "Left 4 Dead",                       "left4dead",         false, true,  false, false, false, false, { "left4dead_dlc3" } },
		{ 620,     "Portal 2",                          "portal2",           false, true,  false, false, false, false, { "portal2_dlc1", "portal2_dlc2" } },
		{ 630,     "Alien Swarm",                       "swarm",             false, true,  false, false, false, false },
		{ 17710,   "Nuclear Dawn",                      "nucleardawn",       false, true,  false, false, false, false },
		{ 70000,   "Dino D-Day",                        "dinodday",          false, true,  false, false, false, false },
		{ 4465480, "Counter-Strike: GO",                "csgo",              false, true,  false, false, false, false },
		{ 225600,  "Blade Symphony",                    "berimbau",          false, true,  false, false, false, false },
		{ 251110,  "INFRA",                             "infra",             false, true,  false, false, false, false },
		{ 265630,  "Fistful of Frags",                  "fof",               false, true,  false, false, false, false },
		{ 221910,  "The Stanley Parable",               "thestanleyparable", false, false, false, false, false, false },
		{ 1224600, "G String Game",                     "gstringv2",         false, true,  false, false, false, false },
		{ 1786950, "Klaus Veen's Treason",              "treason",           false, true,  false, false, false, false },
		{ 1012110, "Military Conflict: Vietnam",        "vietnam",           false, true,  false, false, false, false, { "gmod" } },
		{ 362890,  "Black Mesa",                        "bms",               false, true,  false, false, false, false },
	};

	return mountableGames;
}

void GameDepot::System::Refresh()
{
	FS_TRACE( "GameDepot::System::Refresh()\n" );
	m_RefreshCount++;
	Clear();
	Load();

	for ( auto &game : m_MountedGames )
	{
#ifdef DEDICATED
		// Dedicated only has the sourceengine/content_*.vpk copies, retail games always use those
		if ( game.owned && game.installed && game.mounted && !game.retail )
			Mount( game, false );
		else if ( game.retail )
			MountAsFallback( game );
#else
		if ( game.owned && game.installed && game.mounted )
			Mount( game, false );
		else if ( game.retail )
			MountAsFallback( game );
#endif
	}
}

void GameDepot::System::Clear()
{
	FS_TRACE( "GameDepot::System::Clear()\n" );
	Setup();

	g_pFullFileSystem->RemoveSearchPathsByGroup( PRIORITY_GROUP_HEAD( GN_CURRENTGAME ) );
	g_pFullFileSystem->RemoveSearchPathsByGroup( PRIORITY_GROUP_HEAD( GN_GAMECONTENT ) );
}

void GameDepot::System::SetMount( uint32_t nAppID, bool bMounted )
{
	FS_TRACE( "GameDepot::System::SetMount()\n" );
	for ( auto &v : m_MountedGames )
	{
		if (v.appid != nAppID)
			continue;

		if (v.mounted != bMounted)
			m_RefreshCount++;

		v.mounted = bMounted;
		break;
	}
}

void GameDepot::System::MarkGameAsMounted( const std::string strGameFolder )
{
	FS_TRACE( "GameDepot::System::MarkGameAsMounted()\n" );
	for ( auto& v : m_MountedGames )
	{
		if ( v.folder == strGameFolder )
		{
			v.mounted = true;
			break;
		}
	}
}

const std::list<IGameDepotSystem::Information> &GameDepot::System::GetList()
{
	FS_TRACE( "GameDepot::System::GetList()\n" );
	return m_MountedGames;
}

int GameDepot::System::GetRefreshCount()
{
	return m_RefreshCount;
}

GameDepot::System::System()
{
	m_RefreshCount = 0;
}

void GameDepot::System::FindGame( std::string &strGameName )
{
	FS_TRACE( "GameDepot::System::FindGame()\n" );
}

bool GameDepot::System::MountAsSteampipe( Information &info, bool bHead )
{
#ifdef DEDICATED
	return true;
#else
    std::string installDir = GetAppInstallDir_FixedCase( info.appid );
    if ( installDir.empty() )
        return false;

    for ( const std::string& sub : info.mountSubDirs )
        DoMountDir( info, installDir + "\\" + sub, bHead );

    DoMountDir( info, installDir + "\\" + info.folder, bHead );
    return true;
#endif
}

void GameDepot::System::Mount( Information &info, bool bMount )
{
	DevMsg( "Mounting game '%s' (%s, %i)...\n",
		info.title.c_str(), info.folder.c_str(), info.appid );

	if ( MountAsSteampipe( info, bMount ) )  
		return;

	if ( info.folder == "hl2" )
		return;

	if ( bMount )
	{
		g_pFullFileSystem->AddSearchPath( info.folder.c_str(), "GAME", PRIORITY_GROUP_TAIL( GN_CURRENTGAME ) );
		g_pFullFileSystem->AddSearchPath( info.folder.c_str(), info.folder.c_str(), PRIORITY_GROUP_HEAD( GN_CURRENTGAME ) );
	}
	else
	{
		g_pFullFileSystem->AddSearchPath( info.folder.c_str(), "GAME", PRIORITY_GROUP_TAIL( GN_GAMECONTENT ) );
		g_pFullFileSystem->AddSearchPath( info.folder.c_str(), info.folder.c_str(), PRIORITY_GROUP_HEAD( GN_GAMECONTENT ) );
	}

	g_pFullFileSystem->MarkPathIDByRequestOnly( info.folder.c_str(), true );

	if ( info.enabled )
		g_pFullFileSystem->AddSearchPath("pak01", info.folder.c_str(), 21);
}

void GameDepot::System::MountAsFallback( Information &info )
{
	DevMsg( "Mounting game '%s' as fallback (%s, %i)...\n", info.title.c_str(), info.folder.c_str(), info.appid );

	// Stock uses the root, sourceengine/ isn't under garrysmod/
	std::string path = BaseFileSystem()->GMOD_GamePath();
	path += "\\sourceengine\\";
	path += "content_";
	path += info.folder.c_str();
	path += ".vpk";

	g_pFullFileSystem->AddSearchPath( path.c_str(), "GAME", PRIORITY_GROUP_TAIL( GN_GAMECONTENT ) );
	g_pFullFileSystem->AddSearchPath( path.c_str(), info.folder.c_str(), PRIORITY_GROUP_TAIL( GN_GAMECONTENT ) );
	g_pFullFileSystem->MarkPathIDByRequestOnly( info.folder.c_str(), true );
}

#define GAMEDEPOTSYSTEM "gamedepotsystem"
void GameDepot::System::Load()
{
	KeyValues* kv = new KeyValues( GAMEDEPOTSYSTEM );
	RunCodeAtScopeExit( kv->deleteThis(); );

	if ( !g_pFullFileSystem->FileExists( "cfg/mountdepots.txt", "DEFAULT_WRITE_PATH" ) )
	{
		Save();
		return;
	}

	kv->LoadFromFile( g_pFullFileSystem, "cfg/mountdepots.txt", "DEFAULT_WRITE_PATH" );

	for ( auto& v : m_MountedGames )
		v.mounted = false;

	for ( KeyValues* pKv = kv->GetFirstSubKey(); pKv; pKv = pKv->GetNextKey() )
	{
		for ( auto& v : m_MountedGames)
		{
			if ( !strcmp( v.folder.c_str(), pKv->GetName() ) )
			{
				if ( pKv->GetInt() )
					v.mounted = true;
			}
		}
	}
}

void GameDepot::System::Save()
{
	FS_TRACE( "GameDepot::System::Save()\n" );
	KeyValues* kv = new KeyValues( GAMEDEPOTSYSTEM );
	RunCodeAtScopeExit( kv->deleteThis(); );

	for ( auto& v : m_MountedGames )
	{
		if ( v.mounted )
			kv->SetInt( v.folder.c_str(), 1 );
	}

	kv->SaveToFile( g_pFullFileSystem, "cfg/mountdepots.txt", "DEFAULT_WRITE_PATH" );
}

#undef GAMEDEPOTSYSTEM

static void FillDepotList( std::list<IGameDepotSystem::Information> &MountedGames )
{
	MountedGames.clear();
	const std::vector<IGameDepotSystem::Information> &mountableGames = MountableGames();
	for ( size_t i = 0; i < mountableGames.size(); ++i ) 
		MountedGames.push_back( mountableGames[i] );
}

bool GameDepot::bInitialized = false;
void GameDepot::System::Setup()
{
	if ( GameDepot::bInitialized )
		return;

	GameDepot::bInitialized = true;
	FillDepotList( m_MountedGames );

#ifdef DEDICATED
	for ( auto& game : m_MountedGames )
	{
		game.owned = true;
		game.installed = true;
	}
#else
	for ( auto& game : m_MountedGames )
	{
		game.owned = SteamApps()->BIsSubscribedApp( game.appid );
		game.installed = SteamApps()->BIsAppInstalled( game.appid );
		// RaphaelIT7: Apparently I was wrong with this
#if 0
		if ( game.owned && game.installed )
		{
			// RaphaelIT7: GetAppInstallDir_FixedCase differs in some way- idk what it is yet.
			std::string strGamePath = GetAppInstallDir_FixedCase( game.appid );
			if ( strGamePath.empty() )
				continue;

			game.folder = strGamePath;
		}
#endif
	}
#endif

	Load();
}

#ifndef DEDICATED
const char* GameDepot::DoMountDir( IGameDepotSystem::Information& info, const std::string& dir, bool bToHead )
{
	const unsigned int addType = bToHead ? PRIORITY_GROUP_HEAD( GN_GAMECONTENT ) : PRIORITY_GROUP_TAIL( GN_GAMECONTENT );
	std::string findPattern = dir + "/*.vpk";

	// TODO: developer convar test I think
	Msg( "MountableGame looking in '%s'...\n", findPattern.c_str() );

	FileFindHandle_t findHandle = 0;
	const char* pFileName = g_pFullFileSystem->FindFirst( findPattern.c_str(), &findHandle );

	while (pFileName)
	{
		if (pFileName[0] == '.')
		{
			pFileName = g_pFullFileSystem->FindNext(findHandle);
			continue;
		}

		if (!Bootil::String::Test::EndsWith( std::string( pFileName ), "_000.vpk" ) )
		{
			pFileName = g_pFullFileSystem->FindNext( findHandle );
			continue;
		}

		std::string vpkPath = dir + "/" + pFileName;
		Bootil::String::Util::FindAndReplace( vpkPath, "_000.vpk", "" );

		// TODO: developer convar test I think
		Msg( "MountableGame adding '%s'\n", vpkPath.c_str() );

		g_pFullFileSystem->AddVPKFileFromPath( vpkPath.c_str(), "GAME", addType );
		g_pFullFileSystem->AddVPKFileFromPath( vpkPath.c_str(), info.folder.c_str(), addType );

		pFileName = g_pFullFileSystem->FindNext( findHandle );
	}

	g_pFullFileSystem->FindClose(findHandle);

	// TODO: developer convar test I think
	Msg("MountableGame adding path '%s'\n", dir.c_str());

	g_pFullFileSystem->AddSearchPath( dir.c_str(), "GAME", addType );
	g_pFullFileSystem->AddSearchPath( dir.c_str(), info.folder.c_str(), addType );
	g_pFullFileSystem->MarkPathIDByRequestOnly( info.folder.c_str(), true );

	return NULL;
}

std::string GameDepot::GetAppInstallDir_FixedCase( int nAppID )
{
	static std::string empty = ""; // Just in case to avoid issues.

	char szPath[260];
	uint32_t len = SteamApps()->GetAppInstallDir( nAppID, szPath, sizeof( szPath ) );
	if ( !len )
		return empty;

	return szPath;
}
#endif