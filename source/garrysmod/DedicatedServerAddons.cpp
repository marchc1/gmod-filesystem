#include "garrysmod/DedicatedServerAddons.h"
#include "garrysmod/public/IGet.h"
#include "garrysmod/public/IAddonSystem.h"
#include "garrysmod/gmad/AddonFormat.h"
#include "filesystem.h"
#include "tier0/dbg.h"
#include "tier0/icommandline.h"
#include "Color.h"
#include "tier1/keyvalues.h"
#include "tier1/strtools.h"
#include "tier1/utlbuffer.h"
#include "tier2/tier2.h"
#include "steam/steam_api.h"
#include "steam/isteamgameserver.h"
#include "steam/isteamugc.h"
#include "steam/isteamutils.h"
#include "Bootil/Bootil.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// Reversed from GarrysMod::DedicatedServer in stock dedicated.dll. Output has to match it line for line

namespace
{
	const Color s_Cyan( 90, 255, 255, 0 );
	const Color s_Yellow( 255, 255, 90, 0 );
	const Color s_Red( 255, 90, 90, 0 );
	const Color s_Green( 90, 255, 90, 0 );

	// Where the GMA header keeps its timestamp. Legacy extracts get rtimeUpdated written here
	const int s_nGMATimestampOffset = 13;

	std::vector<PublishedFileId_t> s_CollectionQueue;
	std::vector<PublishedFileId_t> s_ProcessedCollections;
	std::vector<SteamUGCDetails_t> s_Addons;
	bool s_bLoadedFromCache = false;
	bool s_bAutoUpdate = true;

	PublishedFileId_t ParseID( const char *str )
	{
		unsigned long long id = 0;
		sscanf( str, "%llu", &id );
		return id;
	}

	std::string ResultString( int eResult )
	{
		switch ( eResult )
		{
		case k_EResultOK:								return "OK";
		case k_EResultFail:								return "Generic failure";
		case k_EResultNoConnection:						return "No internet connection";
		case k_EResultLoggedInElsewhere:				return "Logged in elsewhere";
		case k_EResultInvalidParam:						return "Invalid parameter (Weird symbols in name/descrption?)";
		case k_EResultFileNotFound:						return "File not found";
		case k_EResultBusy:								return "Method is busy";
		case k_EResultAccessDenied:						return "Access denied (Item hidden/banned?)";
		case k_EResultTimeout:							return "Timed out";
		case k_EResultBanned:							return "Item/user is banned";
		case k_EResultAccountNotFound:					return "Account not found";
		case k_EResultServiceUnavailable:				return "Service unavailable";
		case k_EResultNotLoggedOn:						return "Not logged on";
		case k_EResultInsufficientPrivilege:			return "Insufficient privilege";
		case k_EResultLimitExceeded:					return "Limit exceeded";
		case k_EResultLogonSessionReplaced:				return "Log on session replaced (Your GSLT is used elsewhere)";
		case k_EResultIOFailure:						return "Input/output failure";
		case k_EResultSuspended:						return "Operation suspended";
		case k_EResultCancelled:						return "Operation cancelled";
		case k_EResultDiskFull:							return "Disk drive full";
		case k_EResultAccountDeleted:					return "Item(GSLT?) deleted";
		case k_EResultTimeNotSynced:					return "Time is not synched";
		case k_EResultGSLTDenied:						return "GSL token banned";
		case k_EResultGSOwnerDenied:					return "GS owner denied";
		case k_EResultExpiredLoginAuthCode:				return "GSL token expired";
		case k_EResultLimitedUserAccount:				return "Limited user account";
		default:										return "Steam error code " + std::to_string( eResult );
		}
	}

	std::string FormatBytes( uint64 nBytes )
	{
		char buf[64];
		const double gb = nBytes / 1024.0 / 1024.0 / 1024.0;
		const double mb = nBytes / 1024.0 / 1024.0;
		const double kb = nBytes / 1024.0;
		if ( gb >= 1.0 )
			V_snprintf( buf, sizeof( buf ), "%.1f GB", gb );
		else if ( mb >= 1.0 )
			V_snprintf( buf, sizeof( buf ), "%.1f MB", mb );
		else if ( kb >= 1.0 )
			V_snprintf( buf, sizeof( buf ), "%.1f KB", kb );
		else
			V_snprintf( buf, sizeof( buf ), "%llu B", (unsigned long long)nBytes );
		return buf;
	}

	// true = failed or timed out
	bool WaitForCall( SteamAPICall_t hCall )
	{
		if ( hCall == k_uAPICallInvalid )
			return true;

		bool bFailed = false;
		int nWaited = 0;
		while ( !get->SteamUtils()->IsAPICallCompleted( hCall, &bFailed ) )
		{
			ThreadSleep( 100 );
			nWaited += 100;
			if ( nWaited > 600000 )
			{
				Warning( "Steam call is taking longer than 10 minutes.. assuming timed out!\n" );
				return true;
			}
		}

		// Dispatches the call result
		get->RunSteamCallbacks();
		return bFailed;
	}

	template <typename T>
	int FindIndex( const std::vector<T> &vec, PublishedFileId_t id )
	{
		for ( size_t i = 0; i < vec.size(); ++i )
			if ( vec[i] == id )
				return (int)i;
		return -1;
	}

	int FindAddon( PublishedFileId_t id )
	{
		for ( size_t i = 0; i < s_Addons.size(); ++i )
			if ( s_Addons[i].m_nPublishedFileId == id )
				return (int)i;
		return -1;
	}

	struct CollectionInfoQuery
	{
		std::vector<PublishedFileId_t> m_Children;
		std::string m_strTitle;
		bool m_bFailed = false;
		CCallResult<CollectionInfoQuery, SteamUGCQueryCompleted_t> m_CallResult;

		void OnResult( SteamUGCQueryCompleted_t *pResult, bool bIOFailure )
		{
			ISteamUGC *pUGC = get->SteamUGC();
			if ( pResult->m_eResult != k_EResultOK || bIOFailure )
			{
				Warning( "   WS GetCollectionInfo failed with code %s\n", ResultString( pResult->m_eResult ).c_str() );
				pUGC->ReleaseQueryUGCRequest( pResult->m_handle );
				m_bFailed = true;
				return;
			}

			for ( uint32 i = 0; i < pResult->m_unNumResultsReturned; ++i )
			{
				SteamUGCDetails_t details;
				if ( !pUGC->GetQueryUGCResult( pResult->m_handle, i, &details ) )
				{
					m_bFailed = true;
					Warning( "   Failed to retrieve collection info!\n" );
					continue;
				}

				if ( details.m_eResult != k_EResultOK )
				{
					m_bFailed = true;
					Warning( "   Failed to get collection info! %s!\n", ResultString( details.m_eResult ).c_str() );
					continue;
				}

				m_strTitle = details.m_rgchTitle;
				if ( details.m_eFileType != k_EWorkshopFileTypeCollection )
				{
					Warning( "   Item is not a collection!\n" );
					continue;
				}

				if ( details.m_eVisibility != k_ERemoteStoragePublishedFileVisibilityPublic )
					Warning( "   Warning! Item is not public!\n" );

				std::vector<PublishedFileId_t> children( details.m_unNumChildren );
				pUGC->GetQueryUGCChildren( pResult->m_handle, i, children.data(), details.m_unNumChildren );
				m_Children.insert( m_Children.end(), children.begin(), children.end() );
			}

			pUGC->ReleaseQueryUGCRequest( pResult->m_handle );
		}
	};

	struct ItemInfoQuery
	{
		std::vector<SteamUGCDetails_t> m_Items;
		bool m_bFailed = false;
		CCallResult<ItemInfoQuery, SteamUGCQueryCompleted_t> m_CallResult;

		void OnResult( SteamUGCQueryCompleted_t *pResult, bool bIOFailure )
		{
			ISteamUGC *pUGC = get->SteamUGC();
			if ( pResult->m_eResult != k_EResultOK || bIOFailure )
			{
				Warning( "   WS GetItemInfo failed with code %s\n", ResultString( pResult->m_eResult ).c_str() );
				pUGC->ReleaseQueryUGCRequest( pResult->m_handle );
				m_bFailed = true;
				return;
			}

			for ( uint32 i = 0; i < pResult->m_unNumResultsReturned; ++i )
			{
				SteamUGCDetails_t details;
				if ( !pUGC->GetQueryUGCResult( pResult->m_handle, i, &details ) )
				{
					Warning( "   WS GetItemInfo failed to get item %i\n", i );
					m_bFailed = true;
					continue;
				}
				m_Items.push_back( details );
			}

			pUGC->ReleaseQueryUGCRequest( pResult->m_handle );
		}
	};

	struct DownloadWaiter
	{
		PublishedFileId_t m_nID = 0;
		bool m_bDone = false;
		bool m_bSuccess = false;
		int m_eResult = 0;

		STEAM_GAMESERVER_CALLBACK( DownloadWaiter, OnDownloaded, DownloadItemResult_t );
	};

	void DownloadWaiter::OnDownloaded( DownloadItemResult_t *pResult )
	{
		if ( pResult->m_nPublishedFileId != m_nID )
			return;

		m_bDone = true;
		m_bSuccess = pResult->m_eResult == k_EResultOK;
		m_eResult = pResult->m_eResult;
	}

	// true = this collection failed
	bool DownloadCollection()
	{
		const PublishedFileId_t nCollection = s_CollectionQueue[0];
		ConColorMsg( s_Yellow, "Processing collection %llu...\n", nCollection );

		bool bFailed = true;
		for ( int nAttempt = 0; nAttempt < 5; ++nAttempt )
		{
			CollectionInfoQuery info;
			PublishedFileId_t nID = nCollection;
			SteamAPICall_t hCall = k_uAPICallInvalid;
			UGCQueryHandle_t hQuery = get->SteamUGC()->CreateQueryUGCDetailsRequest( &nID, 1 );
			if ( hQuery != k_UGCQueryHandleInvalid )
			{
				get->SteamUGC()->SetReturnChildren( hQuery, true );
				hCall = get->SteamUGC()->SendQueryUGCRequest( hQuery );
				if ( hCall != k_uAPICallInvalid )
					info.m_CallResult.Set( hCall, &info, &CollectionInfoQuery::OnResult );
			}
			else
			{
				info.m_bFailed = true;
			}

			if ( WaitForCall( hCall ) || info.m_bFailed )
			{
				ConColorMsg( s_Red, "   Failed to get collection info, retrying...\n" );
				ThreadSleep( 500 );
				continue;
			}

			bFailed = false;
			ConColorMsg( s_Cyan, "   Collection '%s' (%i items)\n", info.m_strTitle.c_str(), (int)info.m_Children.size() );
			if ( info.m_Children.empty() )
			{
				ConColorMsg( s_Green, "   Reported 0 items, skipping\n" );
				break;
			}

			ConColorMsg( s_Cyan, "   Retrieving item details...\n" );

			ItemInfoQuery items;
			hQuery = get->SteamUGC()->CreateQueryUGCDetailsRequest( info.m_Children.data(), (uint32)info.m_Children.size() );
			hCall = get->SteamUGC()->SendQueryUGCRequest( hQuery );
			if ( hCall == k_uAPICallInvalid )
				items.m_bFailed = true;
			else
				items.m_CallResult.Set( hCall, &items, &ItemInfoQuery::OnResult );

			if ( WaitForCall( hCall ) || items.m_bFailed )
			{
				ConColorMsg( s_Red, "   Failed to get collection items, retrying...\n" );
				ThreadSleep( 500 );
				continue;
			}

			if ( items.m_Items.empty() )
			{
				ConColorMsg( s_Red, "   Failed to get info of collection items, retrying...\n" );
				ThreadSleep( 500 );
				continue;
			}

			int nNewAddons = 0;
			int nNewCollections = 0;
			for ( const SteamUGCDetails_t &details : items.m_Items )
			{
				const PublishedFileId_t id = details.m_nPublishedFileId;
				if ( details.m_eFileType == k_EWorkshopFileTypeCollection )
				{
					if ( FindIndex( s_ProcessedCollections, id ) != -1 || FindIndex( s_CollectionQueue, id ) != -1 || id == nCollection )
						continue;

					s_CollectionQueue.push_back( id );
					++nNewCollections;
				}
				else if ( details.m_eFileType == k_EWorkshopFileTypeCommunity )
				{
					if ( FindAddon( id ) != -1 )
						continue;

					s_Addons.push_back( details );
					++nNewAddons;
				}
				else
				{
					ConColorMsg( s_Red, "   Unhandled type %i for item %llu\n", details.m_eFileType, id );
				}
			}

			Msg( "  " );
			if ( nNewAddons > 0 )
				ConColorMsg( s_Green, " %i new addon(s)", nNewAddons );
			if ( nNewCollections > 0 )
				ConColorMsg( s_Green, " + %i new collection(s)", nNewCollections );
			if ( nNewAddons == 0 && nNewCollections == 0 )
				ConColorMsg( s_Green, " No new addons or collections" );
			Msg( "\n" );
			break;
		}

		s_ProcessedCollections.push_back( nCollection );
		int nIndex = FindIndex( s_CollectionQueue, nCollection );
		if ( nIndex != -1 )
			s_CollectionQueue.erase( s_CollectionQueue.begin() + nIndex );

		return bFailed;
	}

	bool LoadCachedAddonList()
	{
		if ( !g_pFullFileSystem->FileExists( "cfg/srcds_addons.txt", "DEFAULT_WRITE_PATH" ) )
		{
			ConColorMsg( s_Red, "WS: Would load workshop content from cache file, but it doesn't exist!\n" );
			return false;
		}

		ConColorMsg( s_Red, "WS: Loading workshop content from cache file, disabling updates..\n" );
		s_Addons.clear();

		KeyValues *pKV = new KeyValues( "srcds_addons" );
		bool bLoaded = false;
		if ( pKV->LoadFromFile( g_pFullFileSystem, "cfg/srcds_addons.txt", "DEFAULT_WRITE_PATH" ) )
		{
			for ( KeyValues *pAddon = pKV->GetFirstSubKey(); pAddon; pAddon = pAddon->GetNextKey() )
			{
				const PublishedFileId_t id = ParseID( pAddon->GetName() );
				const char *pszPath = pAddon->GetString( "path", "" );
				const char *pszName = pAddon->GetString( "name", "" );

				SteamUGCDetails_t details;
				memset( &details, 0, sizeof( details ) );
				details.m_nPublishedFileId = id;
				details.m_eResult = k_EResultOK;
				V_strncpy( details.m_rgchTags, "", sizeof( details.m_rgchTags ) );
				V_strncpy( details.m_rgchTitle, pszName, sizeof( details.m_rgchTitle ) );
				V_strncpy( details.m_pchFileName, pszPath, sizeof( details.m_pchFileName ) );
				s_Addons.push_back( details );
			}

			s_bLoadedFromCache = true;
			s_bAutoUpdate = false;
			bLoaded = true;
		}

		pKV->deleteThis();
		return bLoaded;
	}

	void SaveCachedAddonList( const std::vector<SteamUGCDetails_t> &addons )
	{
		if ( s_bLoadedFromCache )
			return;

		KeyValues *pKV = new KeyValues( "srcds_addons" );
		for ( const SteamUGCDetails_t &details : addons )
		{
			char szFolder[MAX_PATH] = { 0 };
			uint64 nSize = 0;
			uint32 nTimestamp = 0;
			if ( !get->SteamUGC()->GetItemInstallInfo( details.m_nPublishedFileId, &nSize, szFolder, sizeof( szFolder ), &nTimestamp ) )
			{
				// Stock gives up on the whole file here
				Warning( "Failed to cache addon location of %llu\n", details.m_nPublishedFileId );
				pKV->deleteThis();
				return;
			}

			KeyValues *pAddon = pKV->CreateNewKey();
			pAddon->SetName( std::to_string( details.m_nPublishedFileId ).c_str() );
			pAddon->SetString( "name", details.m_rgchTitle );
			pAddon->SetString( "path", szFolder );
		}

		pKV->SaveToFile( g_pFullFileSystem, "cfg/srcds_addons.txt", "DEFAULT_WRITE_PATH" );
		pKV->deleteThis();
	}

	// Gets mounted by Addons()->Refresh later
	void MountAddon( const SteamUGCDetails_t &details, const std::string &strPath, bool bLegacy )
	{
		IAddonSystem::Information info;
		info.title = details.m_rgchTitle;
		info.file = strPath;
		info.tags = details.m_rgchTags;
		info.time_updated = details.m_rtimeUpdated;
		info.model_count = 0;
		info.wsid = details.m_nPublishedFileId;
		info.creator = details.m_ulSteamIDOwner;
		info.hcontent_file = details.m_hFile;
		info.size = (uint32)details.m_nFileSize;
		info.hcontent_preview = details.m_hPreviewFile;
		info.timeadded = 0;
		info.canUpdate = false;
		info.downloaded = true;
		info.unknown_b6 = false;
		info.legacy = bLegacy || s_bLoadedFromCache;
		info.unknown_b8 = false;

		g_pFullFileSystem->Addons()->AddAddon( info );
	}

	// The first .gma in Steam's install folder
	void MountSteamUGCAddon( const SteamUGCDetails_t &details, const char *pszFolder )
	{
		char szWildcard[MAX_PATH * 2];
		V_snprintf( szWildcard, sizeof( szWildcard ), "%s/*.gma", pszFolder );

		FileFindHandle_t hFind;
		const char *pszFile = g_pFullFileSystem->FindFirstEx( szWildcard, "", &hFind );
		if ( pszFile )
		{
			char szPath[MAX_PATH * 2];
			V_snprintf( szPath, sizeof( szPath ), "%s/%s", pszFolder, pszFile );
			MountAddon( details, szPath, false );
		}
		g_pFullFileSystem->FindClose( hFind );
	}

	// Steam's legacy .bin is an lzma'd gma. Extracted to cache/srcds/<id>.gma with rtimeUpdated in the header
	bool ExtractLegacy( const SteamUGCDetails_t &details, const char *pszFolder, uint64 nSize, const std::string &strGMAPath )
	{
		CUtlBuffer buf;
		if ( !nSize || !g_pFullFileSystem->ReadFile( pszFolder, "GAME", buf, (int)nSize, 0 ) )
		{
			ConColorMsg( s_Red, "   Error! Failed to read the downloaded file\n" );
			return false;
		}

		ConColorMsg( s_Yellow, "   Extracting...\n" );

		Bootil::Buffer compressed( buf.Base(), (unsigned int)nSize );
		unsigned int nCompressed = (unsigned int)nSize;
		unsigned int nExpected = nCompressed;
		compressed.SetPos( nCompressed - 4 );
		if ( compressed.ReadType<unsigned int>() == Addon::CompressionSignature )
		{
			compressed.SetPos( nCompressed - 8 );
			nExpected = compressed.ReadType<unsigned int>();
			nCompressed -= 8;
		}
		else
		{
			ConColorMsg( s_Red, "   Warning! Addon was created with a 3rd party tool, which might cause install/load issues.\n" );
		}

		Bootil::AutoBuffer out( 32 );
		out.EnsureCapacity( nExpected );
		Bootil::Compression::LZMA::Extract( buf.Base(), nCompressed, out );

		out.SetPos( s_nGMATimestampOffset );
		uint32 nUpdated = details.m_rtimeUpdated;
		out.Write( &nUpdated, sizeof( nUpdated ) );

		FileHandle_t hFile = g_pFullFileSystem->Open( strGMAPath.c_str(), "wb", "MOD" );
		g_pFullFileSystem->Write( out.GetBase(), out.GetWritten(), hFile );
		g_pFullFileSystem->Close( hFile );

		ConColorMsg( s_Yellow, "   Mounting...\n" );
		MountAddon( details, strGMAPath, true );
		ConColorMsg( s_Green, "   Mounted!\n" );
		return true;
	}

	void Download( const SteamUGCDetails_t &details, const std::string &strGMAPath )
	{
		const PublishedFileId_t id = details.m_nPublishedFileId;

		DownloadWaiter waiter;
		waiter.m_nID = id;
		if ( get->SteamUGC()->DownloadItem( id, true ) )
		{
			while ( !waiter.m_bDone )
			{
				ThreadSleep( 1000 );

				uint64 nDownloaded = 0, nTotal = 0;
				if ( !get->SteamUGC()->GetItemDownloadInfo( id, &nDownloaded, &nTotal ) )
					Warning( "WS: GetItemDownloadInfo failed for %llu!\n", id );

				get->RunSteamCallbacks();

				if ( nTotal )
					ConColorMsg( s_Yellow, "   Downloading [ %llu%% of %s ]\n", nDownloaded * 100 / nTotal, FormatBytes( nTotal ).c_str() );
			}
		}
		else
		{
			Warning( "WS: Failed to start download for %llu!\n", id );
			waiter.m_bDone = true;
		}

		if ( !waiter.m_bSuccess )
		{
			ConColorMsg( s_Red, "   Download Failed! %s!\n", ResultString( waiter.m_eResult ).c_str() );
			return;
		}

		ConColorMsg( s_Green, "   Downloaded!\n" );

		char szFolder[MAX_PATH] = { 0 };
		uint64 nSize = 0;
		uint32 nTimestamp = 0;
		if ( !get->SteamUGC()->GetItemInstallInfo( id, &nSize, szFolder, sizeof( szFolder ), &nTimestamp ) )
		{
			if ( !s_bLoadedFromCache )
			{
				ConColorMsg( s_Red, "   Error! Could not retrieve intall info\n" );
				return;
			}
			V_strncpy( szFolder, details.m_pchFileName, sizeof( szFolder ) );
		}

		if ( !nSize )
			nSize = g_pFullFileSystem->Size( szFolder, "GAME" );

		if ( !( get->SteamUGC()->GetItemState( id ) & k_EItemStateLegacyItem ) )
		{
			MountSteamUGCAddon( details, szFolder );
			ConColorMsg( s_Green, "   Mounted!\n" );
			return;
		}

		ExtractLegacy( details, szFolder, nSize, strGMAPath );
	}

	// Handles s_Addons[0] and takes it off the list
	void DownloadAddon()
	{
		const SteamUGCDetails_t details = s_Addons[0];
		const PublishedFileId_t id = details.m_nPublishedFileId;

		if ( details.m_rgchTitle[0] )
			ConColorMsg( s_Yellow, "Processing addon %i: %s (%llu)...\n", (int)s_Addons.size(), details.m_rgchTitle, id );
		else
			ConColorMsg( s_Yellow, "Processing addon %i: %llu...\n", (int)s_Addons.size(), id );

		[&]()
		{
			if ( details.m_bBanned )
			{
				ConColorMsg( s_Red, "   Addon banned!\n" );
				return;
			}

			if ( details.m_eResult != k_EResultOK )
			{
				ConColorMsg( s_Red, "   Failed to get addon info! %s!\n", ResultString( details.m_eResult ).c_str() );
				return;
			}

			char szGMAPath[64];
			V_snprintf( szGMAPath, sizeof( szGMAPath ), "cache/srcds/%llu.gma", id );
			const std::string strGMAPath = szGMAPath;

			char szFolder[MAX_PATH] = { 0 };
			uint64 nSize = 0;
			uint32 nTimestamp = 0;
			if ( !get->SteamUGC()->GetItemInstallInfo( id, &nSize, szFolder, sizeof( szFolder ), &nTimestamp ) )
			{
				if ( !s_bLoadedFromCache )
				{
					ConColorMsg( s_Cyan, "   Addon needs downloading...\n" );
					Download( details, strGMAPath );
					return;
				}
				V_strncpy( szFolder, details.m_pchFileName, sizeof( szFolder ) );
			}

			uint32 nState = get->SteamUGC()->GetItemState( id );
			if ( s_bLoadedFromCache )
				nState = V_stristr( szFolder, ".bin" ) ? k_EItemStateLegacyItem : k_EItemStateInstalled;
			else if ( ( nState & ( k_EItemStateInstalled | k_EItemStateNeedsUpdate ) ) == k_EItemStateInstalled && nTimestamp != details.m_rtimeUpdated )
				nState |= k_EItemStateNeedsUpdate;

			const bool bLegacy = ( nState & k_EItemStateLegacyItem ) != 0;
			if ( bLegacy && !g_pFullFileSystem->FileExists( szFolder, "GAME" ) )
			{
				ConColorMsg( s_Cyan, "   Addon file cache missing, redownloading...\n" );
				Download( details, strGMAPath );
				return;
			}

			if ( nState & k_EItemStateNeedsUpdate )
			{
				if ( s_bAutoUpdate )
				{
					ConColorMsg( s_Cyan, "   Addon downloaded, needs updating...\n" );
					Download( details, strGMAPath );
					return;
				}
				ConColorMsg( s_Cyan, "   Addon downloaded, needs updating, but autoupdates are disabled\n" );
			}

			if ( !bLegacy )
			{
				ConColorMsg( s_Green, "   Mounted!\n" );
				MountSteamUGCAddon( details, szFolder );
				return;
			}

			FileHandle_t hFile = g_pFullFileSystem->Open( strGMAPath.c_str(), "rb", "MOD" );
			if ( !hFile )
			{
				ConColorMsg( s_Yellow, "   Failed to open .gma, redownloading...\n" );
				Download( details, strGMAPath );
				return;
			}

			uint32 nGMATime = 0;
			g_pFullFileSystem->Seek( hFile, s_nGMATimestampOffset, FILESYSTEM_SEEK_HEAD );
			g_pFullFileSystem->Read( &nGMATime, sizeof( nGMATime ), hFile );
			g_pFullFileSystem->Close( hFile );

			if ( nGMATime != details.m_rtimeUpdated && s_bAutoUpdate )
			{
				ConColorMsg( s_Yellow, "   Addon needs updating...\n" );
				Download( details, strGMAPath );
				return;
			}

			MountAddon( details, strGMAPath, true );
			ConColorMsg( s_Green, "   Mounted!\n" );
		}();

		int nIndex = FindAddon( id );
		if ( nIndex != -1 )
			s_Addons.erase( s_Addons.begin() + nIndex );
	}

	void CleanupWorkshopCache()
	{
		ConColorMsg( s_Cyan, "WS: Cleaning up unused files in /cache/workshop/..\n" );

		FileFindHandle_t hFind;
		for ( const char *pszFile = g_pFullFileSystem->FindFirstEx( "cache/workshop/*.gma", "MOD", &hFind ); pszFile; pszFile = g_pFullFileSystem->FindNext( hFind ) )
		{
			char szName[MAX_PATH];
			V_StripExtension( pszFile, szName, sizeof( szName ) );

			const std::string strPath = std::string( "cache/workshop/" ) + pszFile;
			const PublishedFileId_t id = ParseID( szName );
			if ( !id )
			{
				ConColorMsg( s_Red, "   Removing '%s' - Invalid filename, must be Workshop ID\n", strPath.c_str() );
				g_pFullFileSystem->RemoveFile( strPath.c_str(), "MOD" );
				continue;
			}

			if ( !( get->SteamUGC()->GetItemState( id ) & k_EItemStateInstalled ) )
			{
				ConColorMsg( s_Yellow, "   Removing '%s' - Workshop item is no longer installed\n", strPath.c_str() );
				g_pFullFileSystem->RemoveFile( strPath.c_str(), "MOD" );
			}
		}
		g_pFullFileSystem->FindClose( hFind );

		ConColorMsg( s_Cyan, "WS: Finished cleanup!\n" );
	}

	// Waits for the gameserver to log on and sets up workshop. false = nothing to do
	bool InitWorkshop( const char *pszCollection )
	{
		if ( CommandLine()->FindParm( "-authkey" ) )
			Warning( "-authkey is no longer required by Garry's Mod. You can safely remove it from your server's launch options.\n" );

		if ( !pszCollection || V_strlen( pszCollection ) < 5 )
		{
			ConColorMsg( s_Red, "WS: No +host_workshop_collection or it is invalid!\n" );
			return false;
		}

		if ( !get->SteamUGC() )
		{
			ConColorMsg( s_Red, "WS: Couldn't get SteamUGC! (maybe missing Steam binaries?)\n" );
			return false;
		}

		int nWaited = 0;
		while ( !SteamGameServer()->BLoggedOn() )
		{
			if ( nWaited == 0 )
				ConColorMsg( s_Cyan, "WS: Waiting for Steam to log us in" );

			ThreadSleep( 250 );
			nWaited += 250;
			ConColorMsg( s_Cyan, "." );
			get->RunSteamCallbacks();

			if ( nWaited > 10000 )
			{
				ConColorMsg( s_Red, "\nWS: Steam failed to log on within 10 seconds! Loading cached list...\n" );
				return LoadCachedAddonList();
			}
		}

		Msg( "\n" );
		get->RunSteamCallbacks();

		char szCurrentDir[MAX_PATH] = { 0 };
		g_pFullFileSystem->GetCurrentDirectory( szCurrentDir, sizeof( szCurrentDir ) );

		char szCacheDir[MAX_PATH] = { 0 };
		V_snprintf( szCacheDir, sizeof( szCacheDir ), "%s/steam_cache", szCurrentDir );
		V_FixSlashes( szCacheDir, '\\' );

		if ( !get->SteamUGC()->BInitWorkshopForGameServer( 4000, szCacheDir ) )
			Warning( "WS: BInitWorkshopForGameServer failed!\n" );

		return true;
	}
}

void GarrysMod::DedicatedServer::RunAddonProcess( const char *pszCollection, bool bAutoUpdate )
{
	if ( !InitWorkshop( pszCollection ) )
		return;

	if ( !s_bLoadedFromCache )
	{
		s_CollectionQueue.push_back( ParseID( pszCollection ) );
		s_bAutoUpdate = bAutoUpdate;
		ConColorMsg( s_Cyan, "WS: Fetching collection info...\n" );

		bool bAnyFailed = false;
		while ( !s_CollectionQueue.empty() )
		{
			if ( DownloadCollection() )
				bAnyFailed = true;
		}

		ConColorMsg( s_Cyan, "WS: Finished!\n\n" );
		s_ProcessedCollections.clear();

		if ( bAnyFailed )
		{
			ConColorMsg( s_Red, "WS: Detected failed collections!\n" );
			LoadCachedAddonList();
		}
	}

	g_pFullFileSystem->CreateDirHierarchy( "cache/srcds", "DEFAULT_WRITE_PATH" );

	const std::vector<SteamUGCDetails_t> addons = s_Addons;
	ConColorMsg( s_Cyan, "WS: Processing %i addons...%s\n", (int)s_Addons.size(), s_bAutoUpdate ? "" : " (Updates disabled)" );
	while ( !s_Addons.empty() )
		DownloadAddon();
	ConColorMsg( s_Cyan, "WS: Finished!\n" );

	SaveCachedAddonList( addons );
	CleanupWorkshopCache();
}
