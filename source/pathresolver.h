#ifndef PATHRESOLVER_H
#define PATHRESOLVER_H

#pragma once

#include "basefilesystem.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Answers "which search paths could have this file" without walking them all.
// VPKs get indexed when mounted, loose folders get merged per directory from FolderCache listings,
// GMAs get asked directly. Anything it can't index is always a candidate, so the caller still opens
// candidates in normal search path order and the first one that works wins, same as the full walk.
class CPathResolver
{
public:
	struct View;

	struct Candidates
	{
		std::shared_ptr<const View> m_pView;
		uint32 m_nSeq = 0;
		int m_nCount = 0;
		unsigned short m_Ranks[64];
	};

	explicit CPathResolver( CBaseFileSystem *pFileSystem );
	~CPathResolver();

	// Search paths were added, removed or changed. Main thread
	void OnSearchPathsChanged();

	enum class Kind
	{
		File, // anything that can be opened or stat'd, including folders on disk
		Directory, // IsDirectory
	};

	// pFileName is relative. false = can't help, walk the search paths normally.
	// true with 0 candidates = it isn't anywhere
	bool GetCandidates( const char *pFileName, const char *pPathID, Candidates &out, Kind kind = Kind::File );

	// Which m_SearchPaths entries a FindFirst for this wildcard can get anything from
	using FindFilter = CResolverFindFilter;
	static constexpr unsigned char kFindMatches = 1; // could have files or folders matching
	static constexpr unsigned char kFindGma = 2; // GMA FindFirst could match through it

	// pWildCard is FixUpPath'd and relative. false = no filtering
	bool GetFindFilter( const char *pWildCard, const char *pPathID, FindFilter &out );

	// true if index has the flag, or the search paths changed since and we can't say
	bool FindFilterAllows( const FindFilter &filter, unsigned short index, unsigned char flag ) const
	{
		if ( m_nSeq.load( std::memory_order_acquire ) != filter.m_nSeq )
			return true;
		return index < filter.m_Flags.size() && ( filter.m_Flags[index] & flag );
	}

	// Copies the search path for a candidate into storage, AddRef'd like CSearchPathsIterator does.
	// nullptr if the search paths changed since GetCandidates, then walk normally
	CBaseFileSystem::CSearchPath *CopySearchPath( const Candidates &c, int i, void *pStorage );
	static void ReleaseSearchPath( CBaseFileSystem::CSearchPath *pSearchPath );

	// fn( pSearchPath ) on each candidate in order, with the same pack filtering CSearchPathsIterator does,
	// until it returns true. false = search paths changed partway, walk normally
	template <class F>
	bool Visit( const Candidates &c, PathTypeFilter_t filter, F &&fn )
	{
		for ( int i = 0; i < c.m_nCount; ++i )
		{
			alignas( CBaseFileSystem::CSearchPath ) unsigned char storage[sizeof( CBaseFileSystem::CSearchPath )];
			CBaseFileSystem::CSearchPath *pSearchPath = CopySearchPath( c, i, storage );
			if ( !pSearchPath )
				return false;

			const bool bSkip = ( filter == FILTER_CULLPACK && pSearchPath->GetPackFile() ) ||
				( filter == FILTER_CULLNONPACK && !pSearchPath->GetPackFile() );
			const bool bStop = !bSkip && fn( pSearchPath );
			ReleaseSearchPath( pSearchPath );
			if ( bStop )
				return true;
		}
		return true;
	}

	// Bumped around every search path change
	std::atomic<uint32> m_nSeq{ 0 };
	int m_nChangeDepth = 0;

private:
	struct Impl;
	std::unique_ptr<Impl> m_pImpl;
	CBaseFileSystem *m_pFileSystem;
};

// Put on the stack in anything that changes m_SearchPaths
class CSearchPathChangeScope
{
public:
	explicit CSearchPathChangeScope( CPathResolver &resolver );
	~CSearchPathChangeScope();

private:
	CPathResolver &m_Resolver;
};

#endif
