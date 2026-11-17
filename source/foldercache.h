#ifndef FOLDERCACHE_H
#define FOLDERCACHE_H

#pragma once

#include <cstddef>

// In-memory listings of the loose search path folders, kept current with ReadDirectoryChangesW.
// It only ever answers "this definitely doesn't exist", anything else still goes to the OS.
// NOTE: Changes made outside the filesystem show up once the watcher thread got the notification.
namespace FolderCache
{
	void AddRoot( const char *pAbsoluteDir );
	void Shutdown();

	// pAbsolutePath can be any case, any slashes
	bool IsKnownMissing( const char *pAbsolutePath );

	// Call after the filesystem itself created, removed or renamed something
	void OnPathChanged( const char *pAbsolutePath );

	void InvalidateAll();

	// Names we can reason about without asking the OS. Backslashes, lowercase
	bool IsSimpleRelativePath( const char *pRel, size_t nLen );

	enum class DirState
	{
		Missing,
		Listed,
		Unknown,
	};

	constexpr unsigned char kEntryDir = 1;
	constexpr unsigned char kEntryUnsure = 2;

	using DirEntryFn = void ( * )( void *pCtx, const char *pName, size_t nLen, unsigned char flags );

	// Lists a folder, from memory if we have it. *ppRoot is for IsRootSettled
	DirState ListDirectory( const char *pAbsoluteDir, DirEntryFn fn, void *pCtx, const void **ppRoot );

	// false if the watcher has changes for this root it hasn't handled yet
	bool IsRootSettled( const void *pRoot );

	// FindFirstFile from memory, for the patterns the platform can match exactly. pFindName is what
	// would go to the OS, folder + pattern. NotHandled = ask the OS
	enum class FindResult
	{
		NotHandled,
		NoMatch,
		Found,
	};

	struct FindState;
	FindResult FindFirst( const char *pFindName, FindState **ppState, const char **ppName, bool *pbDir );
	bool FindNext( FindState *pState, const char **ppName, bool *pbDir );
	void FindClose( FindState *pState );

	// Called with the folder lock held, after a path changed or a whole root got dropped.
	// The path is lowercase, backslashes. Keep it short, don't call back into FolderCache
	using ChangeFn = void ( * )( const char *pAbsolutePath, size_t nLen );
	void SetChangeCallback( ChangeFn fn );
}

#endif
