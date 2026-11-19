#include "Tasks.h"

void Addon::Task::OnSubscribed::Start()
{
	SteamAPICall_t hCall = SteamUGC()->RequestUGCDetails( m_WSID, 0 );
	if ( hCall == k_uAPICallInvalid )
	{
		m_bFinished = true;
		return;
	}

	m_Query.Set( hCall, this, &Addon::Task::OnSubscribed::OnDetails );
}

void Addon::Task::OnSubscribed::OnDetails( SteamUGCRequestUGCDetailsResult_t *pResult, bool bIOFailure )
{
	Msg( "Got Subscription details!\n" );
	m_bFinished = true;

	if ( bIOFailure || pResult->m_details.m_eResult != k_EResultOK )
	{
		Warning( "Error getting new subscription: %i.\n", bIOFailure ? (int)k_EResultIOFailure : (int)pResult->m_details.m_eResult );
		return;
	}

	m_pAddonSystem->OnAddonSubscribed( pResult->m_details );
}

void Addon::Task::OnSubscribed::Cycle()
{
}

bool Addon::Task::OnSubscribed::Finished()
{
	return m_bFinished;
}

Addon::Task::OnSubscribed::OnSubscribed( uint64_t wsid ) : m_WSID( wsid )
{
}
