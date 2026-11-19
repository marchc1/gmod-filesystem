#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "AddonFileHandle.h"

namespace
{
	struct OpenHandles
	{
		std::shared_mutex m_Mutex;
		std::vector<Addon::FileHandle *> m_Handles;
	};

	// Never freed, CFileHandles can outlive static destruction
	OpenHandles &GetOpenHandles()
	{
		static OpenHandles *pHandles = new OpenHandles;
		return *pHandles;
	}
}

Addon::FileHandle::FileHandle( Addon::FileInfo *pInfo )
{
	m_hFileHandle = pInfo->m_hFileHandle;
	m_nOffset = pInfo->m_nOffset;
	m_nSize = pInfo->m_nSize;
	m_nPosition = 0;

	OpenHandles &open = GetOpenHandles();
	std::unique_lock<std::shared_mutex> lock( open.m_Mutex );
	open.m_Handles.push_back( this );
}

Addon::FileHandle::~FileHandle()
{
	OpenHandles &open = GetOpenHandles();
	std::unique_lock<std::shared_mutex> lock( open.m_Mutex );
	auto it = std::find( open.m_Handles.begin(), open.m_Handles.end(), this );
	if ( it != open.m_Handles.end() )
	{
		*it = open.m_Handles.back();
		open.m_Handles.pop_back();
	}
}

void Addon::FileHandle::OnPackFileUnmounted( FileHandle_t hFileHandle )
{
	OpenHandles &open = GetOpenHandles();
	std::unique_lock<std::shared_mutex> lock( open.m_Mutex );
	for ( Addon::FileHandle *pHandle : open.m_Handles )
	{
		if ( pHandle->m_hFileHandle == hFileHandle )
			pHandle->m_hFileHandle = nullptr;
	}
}

void Addon::FileHandle::CheckPackFileUnmounted(void *pUnknown)
{
}

int Addon::FileHandle::Read( void *pBuffer, int nSize, int nLength )
{
	int iRemaining = static_cast<int>( m_nSize - m_nPosition );
	if ( nLength < iRemaining )
		iRemaining = nLength;

	if ( nSize < iRemaining )
		iRemaining = nSize;

	if ( nSize < 0 )
	{
		Warning( "Trying to read Workshop file into a negatively sized buffer! This should never happen!\n" );
		return 0;
	}

	if ( iRemaining <= 0 )
		return 0;

	{
		// Held over the read, the pack file gets closed right after OnPackFileUnmounted
		std::shared_lock<std::shared_mutex> lock( GetOpenHandles().m_Mutex );
		if ( !m_hFileHandle )
			return 0;

		g_pFullFileSystem->Seek( m_hFileHandle, m_nOffset + m_nPosition, FILESYSTEM_SEEK_HEAD );
		g_pFullFileSystem->Read( pBuffer, iRemaining, m_hFileHandle );
	}

	m_nPosition += iRemaining;
	return iRemaining;
}

unsigned int Addon::FileHandle::Seek( int64 iOffset, int iSeekOrigin )
{
	int64_t nNewPosition;
	switch ( iSeekOrigin )
	{
	case SEEK_SET:
		nNewPosition = iOffset;
		break;

	case SEEK_CUR:
		nNewPosition = m_nPosition + iOffset;
		break;

	case SEEK_END:
		nNewPosition = static_cast<int64_t>( m_nSize ) + iOffset;
		break;

	default:
		nNewPosition = m_nOffset;
		break;
	}

	if ( nNewPosition < 0 )
		nNewPosition = 0;

	if ( nNewPosition > static_cast<int64_t>( m_nSize ) )
		nNewPosition = m_nSize;

	m_nPosition = nNewPosition;
	return (unsigned int)m_nPosition;
}

unsigned int Addon::FileHandle::Tell()
{
	return m_nPosition;
}

unsigned int Addon::FileHandle::Size()
{
	return m_nSize;
}