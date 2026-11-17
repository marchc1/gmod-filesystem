//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Plain stdin/stdout console for POSIX
//
//=============================================================================//

#if !defined TEXTCONSOLE_UNIX_H
#define TEXTCONSOLE_UNIX_H
#pragma once

#ifndef _WIN32

#include "textconsole.h"

class CTextConsoleUnix : public CTextConsole
{
public:
	virtual ~CTextConsoleUnix() {}

	bool Init();
	void ShutDown();
	void Print( const char *pszMsg );

	void SetTitle( const char *pszTitle ) {}
	void SetStatusLine( const char *pszStatus ) {}
	void UpdateStatus() {}

	char *GetLine( int index, char *buf, int buflen );
	int GetWidth() { return 80; }

private:
	char m_szPending[1024];
	int m_nPending = 0;
};

#endif // !_WIN32

#endif // !defined TEXTCONSOLE_UNIX_H
