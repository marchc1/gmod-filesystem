#ifndef FOLDERCACHE_INTERNAL_H
#define FOLDERCACHE_INTERNAL_H

#pragma once

// Shared between the folder cache core (foldercache.cpp) and the platform backends
// (foldercache_win32.cpp, foldercache_linux.cpp). Nothing outside those includes this.

#include "foldercache.h"
#include "unordered_stuff.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined( _WIN32 )
#define FOLDERCACHE_WIN32 1
#elif defined( __linux__ )
#define FOLDERCACHE_LINUX 1
#endif

namespace FolderCache::Detail
{
#ifdef FOLDERCACHE_WIN32
	constexpr char kSep = '\\';
	// NTFS matches every part of a path without case
	constexpr bool kExactFolders = false;
	constexpr int kMaxPath = 260; // MAX_PATH, the A functions don't go past it
#else
	constexpr char kSep = '/';
	// Folders have to match exactly, only the last part gets the case-insensitive retry
	// the filesystem does with findFileInDirCaseInsensitive
	constexpr bool kExactFolders = true;
	constexpr int kMaxPath = 4096; // PATH_MAX
#endif

	constexpr unsigned char kFlagDir = FolderCache::kEntryDir;

	// Link we don't follow, name collision, or changed since we listed. Never trusted
	constexpr unsigned char kFlagUnsure = FolderCache::kEntryUnsure;

	constexpr int kMaxUnsurePerNode = 64;

	using NameMap = unordered_map<std::string, unsigned char, StringHash, StringEq>;

	struct Node
	{
		bool m_bListed = false;
		int m_nUnsure = 0;

		// Windows: folded names. Linux: exact names, what folder lookups compare against
		NameMap m_Names;

		// Linux: folded name -> everything that folds to it, for the last part of a lookup
		NameMap m_Folded;

		// Same keys as m_Names
		unordered_map<std::string, std::unique_ptr<Node>, StringHash, StringEq> m_Children;
	};

	// A folder the way FindFirstFile( folder\* ) returns it, in that order, . and .. included
	struct FindEntry
	{
		std::string m_Name;
		std::string m_ShortName; // empty when there's no separate 8.3 name
		bool m_bDir = false;
	};

	struct FindListing
	{
		std::vector<FindEntry> m_Entries;
		bool m_bUsable = true; // false: has names we can't hand out, ask the OS until it changes
	};

	// What the backend made of the pattern part
	struct FindPattern
	{
		int m_nKind = 0;
		std::string m_Text;
	};

	struct PlatformRoot;

	struct Root
	{
		std::string m_Path; // normalized, separator at the end
		bool m_bActive = false;
		Node m_Tree;
		PlatformRoot *m_pPlatform = nullptr;

		// Find listings by folder, relative like the tree. Dropped when anything in them changes
		std::map<std::string, std::shared_ptr<const FindListing>, std::less<>> m_FindListings;
		uint64_t m_nChangeSeq = 0;
	};

	//-------------------------------------------------------------------------
	// Core, foldercache.cpp
	//-------------------------------------------------------------------------
	extern std::shared_mutex g_Lock;
	extern std::vector<Root *> g_Roots; // active roots, none inside another unless a link is between

	bool IsDisabled();

	// Exclusive lock held for all of these
	void ResetTree( Root &root );
	void MarkChanged( Root &root, std::string_view rel ); // don't know what happened to it, the OS decides
	void MarkRemoved( Root &root, std::string_view rel ); // it's gone
	void MarkAdded( Root &root, std::string_view rel, unsigned char flags ); // it's there, flags = what it is now
	void DropRoot( Root &root ); // stops using it, the backend cleans up its handles
	void RetireRootsUnder( const std::string &absPath );
	void ClearNode( Node &node );
	void AddEntry( Node &node, std::string_view name, std::string_view folded, unsigned char flags );

	//-------------------------------------------------------------------------
	// Backend, foldercache_<platform>.cpp
	//-------------------------------------------------------------------------

	// Separators fixed, Windows also lowercases. -1 if it's not something we can reason about
	int NormalizePath( const char *pIn, char *pOut, int nOutSize );

	// Names the OS treats like any other. Anything it rewrites or special cases goes to the OS
	bool IsSimpleComponent( std::string_view comp );

	// Key for the case-insensitive part of a lookup
	void FoldName( std::string_view name, std::string &out );

	// Can we watch it reliably (local disk, not a network share)
	bool IsWatchablePath( const std::string &path );

	// Is any folder after outer, up to path, a link our watch won't see into
	bool HasLinkBetween( const std::string &outer, const std::string &path );

	// No lock held. Opens whatever the platform needs, false = can't watch it
	bool PrepareRoot( Root &root );

	// No lock held, root is in g_Roots already. Sets m_bActive once watching
	void ActivateRoot( Root &root );

	// Exclusive lock held. Root got replaced by an outer one, stop watching it
	void RetireRoot( Root &root );

	// The watcher has news for this root it didn't apply yet
	bool HasUnprocessedChanges( const Root &root );

	// Exclusive lock held. Fills node with AddEntry. false = can't trust the result
	bool ListNode( Root &root, Node &node, std::string_view relDir );

	void ShutdownBackend();

	// The pattern part of a FindFirstFile name. false = one we can't match exactly
	bool ParseFindPattern( std::string_view pattern, FindPattern &out );

	// Indexes into listing.m_Entries in the order FindFirstFile returns them. false = can't say, ask the OS
	bool BuildFindOrder( const FindPattern &pattern, const FindListing &listing, std::vector<uint32_t> &out );

	// No lock held. pattern is folder + separator + "*", the same folder FindFirstFile would get.
	// false = couldn't list it. Names the platform would rewrite make it !m_bUsable
	bool ListForFind( const std::string &pattern, FindListing &out );
}

#endif
