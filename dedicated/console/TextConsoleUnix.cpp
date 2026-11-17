//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Plain stdin/stdout console for POSIX
//
//=============================================================================//

#ifndef _WIN32

#include "TextConsoleUnix.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

bool CTextConsoleUnix::Init()
{
	setvbuf( stdout, NULL, _IONBF, 0 );
	m_nPending = 0;
	return CTextConsole::Init();
}

void CTextConsoleUnix::ShutDown()
{
}

void CTextConsoleUnix::Print( const char *pszMsg )
{
	fputs( pszMsg, stdout );
}

// Returns one finished line per call, NULL once nothing's left
char *CTextConsoleUnix::GetLine( int index, char *buf, int buflen )
{
	for ( ;; )
	{
		// A full line already sitting in the buffer
		char *pNewline = (char *)memchr( m_szPending, '\n', m_nPending );
		if ( pNewline )
		{
			int nLen = (int)( pNewline - m_szPending );
			int nCopy = nLen < buflen - 1 ? nLen : buflen - 1;
			memcpy( buf, m_szPending, nCopy );
			buf[nCopy] = 0;
			if ( nCopy > 0 && buf[nCopy - 1] == '\r' )
				buf[nCopy - 1] = 0;

			m_nPending -= nLen + 1;
			memmove( m_szPending, pNewline + 1, m_nPending );
			return buf;
		}

		fd_set fds;
		FD_ZERO( &fds );
		FD_SET( STDIN_FILENO, &fds );
		timeval tv = { 0, 0 };
		if ( select( STDIN_FILENO + 1, &fds, NULL, NULL, &tv ) <= 0 )
			return NULL;

		// No room and no newline, drop it
		if ( m_nPending >= (int)sizeof( m_szPending ) )
			m_nPending = 0;

		ssize_t nRead = read( STDIN_FILENO, m_szPending + m_nPending, sizeof( m_szPending ) - m_nPending );
		if ( nRead <= 0 )
			return NULL;

		m_nPending += (int)nRead;
	}
}

#endif // !_WIN32
