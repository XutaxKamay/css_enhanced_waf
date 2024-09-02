//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#ifdef CSTRIKE_DLL
#include "cs_player.h"
#endif
#include "icvar.h"
#include "player.h"
#include "shareddefs.h"
#include "studio.h"
#include "usercmd.h"
#include "igamesystem.h"
#include "ilagcompensationmanager.h"
#include "inetchannelinfo.h"
#include "util.h"
#include "utllinkedlist.h"
#include "BaseAnimatingOverlay.h"
#include "tier0/vprof.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define LC_NONE				   0
#define LC_ALIVE			   ( 1 << 0 )

#define LC_ORIGIN_CHANGED	   ( 1 << 8 )
#define LC_ANGLES_CHANGED	   ( 1 << 9 )
#define LC_SIZE_CHANGED		   ( 1 << 10 )
#define LC_ANIMATION_CHANGED   ( 1 << 11 )
#define LC_POSE_PARAMS_CHANGED ( 1 << 12 )
#define LC_ENCD_CONS_CHANGED   ( 1 << 13 )

// Default to 1 second max.
#define MAX_TICKS_SAVED		   1000

ConVar sv_unlag( "sv_unlag", "1", FCVAR_DEVELOPMENTONLY, "Enables player lag compensation" );
ConVar sv_lagflushbonecache( "sv_lagflushbonecache", "0", 0, "Flushes entity bone cache on lag compensation" );

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

struct LayerRecord
{
	int m_sequence;
	float m_cycle;
	float m_weight;
	int m_order;
	int m_flags;

	LayerRecord()
	{
		m_sequence = 0;
		m_cycle	   = 0;
		m_weight   = 0;
		m_order	   = 0;
		m_flags	   = 0;
	}
};

struct LagRecord
{
  public:
	LagRecord()
	{
		m_fFlags = LC_NONE;
		m_vecOrigin.Init();
		m_vecAngles.Init();
		m_vecMinsPreScaled.Init();
		m_vecMaxsPreScaled.Init();
		m_flSimulationTime = -1;
		m_flAnimTime	   = -1;
		m_masterSequence   = 0;
		m_masterCycle	   = 0;

		for ( int layerIndex = 0; layerIndex < MAX_LAYER_RECORDS; ++layerIndex )
		{
			m_layerRecords[layerIndex] = {};
		}

		for ( int i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
		{
			m_poseParameters[i] = 0;
		}

		for ( int i = 0; i < MAXSTUDIOBONECTRLS; i++ )
		{
			m_encodedControllers[i] = 0;
		}
#ifdef CSTRIKE_DLL
		m_angRenderAngles.Init();
#endif
	}

	LagRecord( const LagRecord& src )
	{
		m_fFlags		   = src.m_fFlags;
		m_vecOrigin		   = src.m_vecOrigin;
		m_vecAngles		   = src.m_vecAngles;
		m_vecMinsPreScaled = src.m_vecMinsPreScaled;
		m_vecMaxsPreScaled = src.m_vecMaxsPreScaled;
		m_flSimulationTime = src.m_flSimulationTime;
		m_flAnimTime	   = src.m_flAnimTime;

		for ( int layerIndex = 0; layerIndex < MAX_LAYER_RECORDS; ++layerIndex )
		{
			m_layerRecords[layerIndex] = src.m_layerRecords[layerIndex];
		}

		m_masterSequence = src.m_masterSequence;
		m_masterCycle	 = src.m_masterCycle;

		for ( int i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
		{
			m_poseParameters[i] = src.m_poseParameters[i];
		}

		for ( int i = 0; i < MAXSTUDIOBONECTRLS; i++ )
		{
			m_encodedControllers[i] = src.m_encodedControllers[i];
		}

#ifdef CSTRIKE_DLL
		m_angRenderAngles = src.m_angRenderAngles;
#endif
	}

	// Did player die this frame
	int m_fFlags;

	// Player position, orientation and bbox
	Vector m_vecOrigin;
	QAngle m_vecAngles;
	Vector m_vecMinsPreScaled;
	Vector m_vecMaxsPreScaled;

	float m_flSimulationTime;
	float m_flAnimTime;

	// Player animation details, so we can get the legs in the right spot.
	LayerRecord m_layerRecords[MAX_LAYER_RECORDS];
	int m_masterSequence;
	float m_masterCycle;
	float m_poseParameters[MAXSTUDIOPOSEPARAM];
	float m_encodedControllers[MAXSTUDIOBONECTRLS];
#ifdef CSTRIKE_DLL
	QAngle m_angRenderAngles;
#endif
};

//
// Try to take the player from his current origin to vWantedPos.
// If it can't get there, leave the player where he is.
//

ConVar sv_unlag_debug( "sv_unlag_debug", "0", FCVAR_GAMEDLL | FCVAR_DEVELOPMENTONLY );

float g_flFractionScale = 0.95;

static void RestorePlayerTo( CBasePlayer* pPlayer, const Vector& vWantedPos )
{
	// Try to move to the wanted position from our current position.
	trace_t tr;
	VPROF_BUDGET( "RestorePlayerTo", "CLagCompensationManager" );
	UTIL_TraceEntity( pPlayer, vWantedPos, vWantedPos, MASK_PLAYERSOLID, pPlayer, COLLISION_GROUP_PLAYER_MOVEMENT, &tr );
	if ( tr.startsolid || tr.allsolid )
	{
		if ( sv_unlag_debug.GetBool() )
		{
			DevMsg( "RestorePlayerTo() could not restore player position for client \"%s\" ( %.1f %.1f %.1f )\n",
					pPlayer->GetPlayerName(),
					vWantedPos.x,
					vWantedPos.y,
					vWantedPos.z );
		}

		UTIL_TraceEntity( pPlayer,
						  pPlayer->GetAbsOrigin(),
						  vWantedPos,
						  MASK_PLAYERSOLID,
						  pPlayer,
						  COLLISION_GROUP_PLAYER_MOVEMENT,
						  &tr );
		if ( tr.startsolid || tr.allsolid )
		{
			// In this case, the guy got stuck back wherever we lag compensated him to. Nasty.

			if ( sv_unlag_debug.GetBool() )
			{
				DevMsg( " restore failed entirely\n" );
			}
		}
		else
		{
			// We can get to a valid place, but not all the way back to where we were.
			Vector vPos;
			VectorLerp( pPlayer->GetAbsOrigin(), vWantedPos, tr.fraction * g_flFractionScale, vPos );
			UTIL_SetOrigin( pPlayer, vPos, true );

			if ( sv_unlag_debug.GetBool() )
			{
				DevMsg( " restore got most of the way\n" );
			}
		}
	}
	else
	{
		// Cool, the player can go back to whence he came.
		UTIL_SetOrigin( pPlayer, tr.endpos, true );
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CLagCompensationManager : public CAutoGameSystemPerFrame,
								public ILagCompensationManager
{
  public:
	CLagCompensationManager( const char* name )
	{
	}

	// IServerSystem stuff
	virtual void Shutdown()
	{
		ClearHistory();
	}

	virtual void LevelShutdownPostEntity()
	{
		ClearHistory();
	}

	// ILagCompensationManager stuff

	// Called during player movement to set up/restore after lag compensation
	void StartLagCompensation( CBasePlayer* player, CUserCmd* cmd );
	void FinishLagCompensation( CBasePlayer* player );
	virtual void TrackPlayerData( CBasePlayer* pPlayer );

  private:
	void BacktrackPlayer( CBasePlayer* player, CUserCmd* cmd );

	void ClearHistory()
	{
		for ( int i = 0; i < MAX_EDICTS; i++ )
		{
			m_EntityTrack[i].Clear();
		}
	}

	// keep a list of lag records for each entities
	CUtlCircularBuffer< LagRecord, MAX_TICKS_SAVED > m_EntityTrack[MAX_EDICTS];

	// Scratchpad for determining what needs to be restored
	CBitVec< MAX_EDICTS > m_RestorePlayer;
	bool m_bNeedToRestore;

	LagRecord m_RestoreData[MAX_EDICTS]; // entities data before we moved him back
	LagRecord m_ChangeData[MAX_EDICTS];	 // entities data where we moved him back
};

static CLagCompensationManager g_LagCompensationManager( "CLagCompensationManager" );
ILagCompensationManager* lagcompensation = &g_LagCompensationManager;

//-----------------------------------------------------------------------------
// Purpose: Called once per frame after all entities have had a chance to think
//-----------------------------------------------------------------------------
void CLagCompensationManager::TrackPlayerData( CBasePlayer* pPlayer )
{
	if ( ( gpGlobals->maxClients <= 1 ) || !sv_unlag.GetBool() )
	{
		ClearHistory();
		return;
	}

	VPROF_BUDGET( "TrackPlayerData", "CLagCompensationManager" );

	// remove all records before that time:
	auto track = &m_EntityTrack[pPlayer->entindex()];

	// add new record to player track
	LagRecord record;

	record.m_fFlags = LC_NONE;
	if ( pPlayer->IsAlive() )
	{
		record.m_fFlags |= LC_ALIVE;
	}

	record.m_flSimulationTime = pPlayer->GetSimulationTime();
	record.m_flAnimTime		  = pPlayer->GetAnimTime();
	record.m_vecAngles		  = pPlayer->GetAbsAngles();
	record.m_vecOrigin		  = pPlayer->GetAbsOrigin();
	record.m_vecMinsPreScaled = pPlayer->CollisionProp()->OBBMinsPreScaled();
	record.m_vecMaxsPreScaled = pPlayer->CollisionProp()->OBBMaxsPreScaled();

	int layerCount = pPlayer->GetNumAnimOverlays();

	for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
	{
		CAnimationLayer* currentLayer = pPlayer->GetAnimOverlay( layerIndex );
		if ( currentLayer )
		{
			record.m_layerRecords[layerIndex].m_cycle	 = currentLayer->m_flCycle;
			record.m_layerRecords[layerIndex].m_order	 = currentLayer->m_nOrder;
			record.m_layerRecords[layerIndex].m_sequence = currentLayer->m_nSequence;
			record.m_layerRecords[layerIndex].m_weight	 = currentLayer->m_flWeight;
			record.m_layerRecords[layerIndex].m_flags	 = currentLayer->m_fFlags;
		}
	}

	record.m_masterSequence = pPlayer->GetSequence();
	record.m_masterCycle	= pPlayer->GetCycle();

	CStudioHdr* hdr = pPlayer->GetModelPtr();

	if ( hdr )
	{
		for ( int paramIndex = 0; paramIndex < hdr->GetNumPoseParameters(); paramIndex++ )
		{
			record.m_poseParameters[paramIndex] = pPlayer->GetPoseParameterArray()[paramIndex];
		}
	}

	if ( hdr )
	{
		for ( int boneIndex = 0; boneIndex < hdr->GetNumBoneControllers(); boneIndex++ )
		{
			record.m_encodedControllers[boneIndex] = pPlayer->GetBoneControllerArray()[boneIndex];
		}
	}

#ifdef CSTRIKE_DLL
	const auto csPlayer = ToCSPlayer( pPlayer );

	if ( csPlayer )
	{
		record.m_angRenderAngles = csPlayer->m_angRenderAngles;
	}
#endif

	track->Push( record );
}

// Called during player movement to set up/restore after lag compensation
void CLagCompensationManager::StartLagCompensation( CBasePlayer* player, CUserCmd* cmd )
{
	// Assume no players need to be restored
	m_RestorePlayer.ClearAll();
	m_bNeedToRestore = false;

	if ( !player->m_bLagCompensation	   // Player not wanting lag compensation
		 || ( gpGlobals->maxClients <= 1 ) // no lag compensation in single player
		 || !sv_unlag.GetBool()			   // disabled by server admin
		 || player->IsBot()				   // not for bots
		 || player->IsObserver()		   // not for spectators
	)
	{
		return;
	}

	// NOTE: Put this here so that it won't show up in single player mode.
	VPROF_BUDGET( "StartLagCompensation", VPROF_BUDGETGROUP_OTHER_NETWORKING );
	Q_memset( m_RestoreData, 0, sizeof( m_RestoreData ) );
	Q_memset( m_ChangeData, 0, sizeof( m_ChangeData ) );

	// Iterate all active players
	const CBitVec< MAX_EDICTS >* pEntityTransmitBits = engine->GetEntityTransmitBitsForClient( player->entindex() - 1 );
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer* pPlayer = UTIL_PlayerByIndex( i );

		if ( !pPlayer )
		{
			continue;
		}

		// Don't lag compensate yourself you loser...
		if ( player == pPlayer )
		{
			continue;
		}

		// Custom checks for if things should lag compensate (based on things like what team the player is on).
		if ( !player->WantsLagCompensationOnEntity( pPlayer, cmd, pEntityTransmitBits ) )
		{
			continue;
		}

		// Move other player back in time
		BacktrackPlayer( pPlayer, cmd );
	}
}

void CLagCompensationManager::BacktrackPlayer( CBasePlayer* pPlayer, CUserCmd* cmd )
{
	VPROF_BUDGET( "BacktrackPlayer", "CLagCompensationManager" );

	Vector org;
	Vector minsPreScaled;
	Vector maxsPreScaled;
	QAngle ang;
#ifdef CSTRIKE_DLL
	QAngle renderAngles;
#endif
	LagRecord* prevRecordSim;
	LagRecord* recordSim;
	LagRecord* recordAnim;

#ifdef CSTRIKE_DLL
	auto csPlayer = ToCSPlayer( pPlayer );
#endif

	int pl_index = pPlayer->entindex();

	float flTargetLerpSimTime			 = cmd->simulationdata[pl_index].lerp_time;
	float flTargetAnimatedSimulationTime = cmd->simulationdata[pl_index].animated_sim_time;

	// get track history of this player
	auto track = &m_EntityTrack[pl_index];

	for ( int i = 0; i < MAX_TICKS_SAVED; i++ )
	{
		recordSim = track->Get( i );

		if ( !recordSim )
		{
			break;
		}

		if ( !( recordSim->m_fFlags & LC_ALIVE ) )
		{
			break;
		}

		if ( flTargetLerpSimTime == recordSim->m_flSimulationTime )
		{
			break;
		}

		if ( recordSim->m_flSimulationTime < flTargetLerpSimTime )
		{
			prevRecordSim = track->Get( i - 1 );
			break;
		}
	}

	for ( int i = 0; i < MAX_TICKS_SAVED; i++ )
	{
		recordAnim = track->Get( i );

		if ( !recordAnim )
		{
			break;
		}

		if ( recordAnim->m_flAnimTime == flTargetAnimatedSimulationTime )
		{
			break;
		}
	}

	Assert( recordAnim );
	Assert( recordSim );

	if ( !recordSim || !recordAnim )
	{
		if ( sv_unlag_debug.GetBool() )
		{
			DevMsg( "No valid positions in history for BacktrackPlayer client ( %s )\n", pPlayer->GetPlayerName() );
		}

		return; // that should never happen
	}

	float fracSim = 0.0f;
	if ( prevRecordSim && ( recordSim->m_flSimulationTime < flTargetLerpSimTime )
		 && ( recordSim->m_flSimulationTime < prevRecordSim->m_flSimulationTime ) )
	{
		// we didn't find the exact time but have a valid previous record
		// so interpolate between these two records;

		Assert( prevRecordSim->m_flSimulationTime > recordSim->m_flSimulationTime );
		Assert( flTargetLerpSimTime < prevRecordSim->m_flSimulationTime );

		// calc fraction between both records
		fracSim = float( ( double( flTargetLerpSimTime ) - double( recordSim->m_flSimulationTime ) )
						 / ( double( prevRecordSim->m_flSimulationTime ) - double( recordSim->m_flSimulationTime ) ) );

		Assert( fracSim > 0 && fracSim < 1 ); // should never extrapolate

		ang			  = Lerp( fracSim, recordSim->m_vecAngles, prevRecordSim->m_vecAngles );
		org			  = Lerp( fracSim, recordSim->m_vecOrigin, prevRecordSim->m_vecOrigin );
		minsPreScaled = Lerp( fracSim, recordSim->m_vecMinsPreScaled, prevRecordSim->m_vecMinsPreScaled );
		maxsPreScaled = Lerp( fracSim, recordSim->m_vecMaxsPreScaled, prevRecordSim->m_vecMaxsPreScaled );
#ifdef CSTRIKE_DLL
		if ( csPlayer )
		{
			renderAngles = Lerp( fracSim, recordSim->m_angRenderAngles, prevRecordSim->m_angRenderAngles );
		}
#endif
	}
	else
	{
		// we found the exact record or no other record to interpolate with
		// just copy these values since they are the best we have
		org			  = recordSim->m_vecOrigin;
		ang			  = recordSim->m_vecAngles;
		minsPreScaled = recordSim->m_vecMinsPreScaled;
		maxsPreScaled = recordSim->m_vecMaxsPreScaled;
#ifdef CSTRIKE_DLL
		renderAngles = recordSim->m_angRenderAngles;
#endif
	}

	// See if this represents a change for the player
	int flags		   = 0;
	LagRecord* restore = &m_RestoreData[pl_index];
	LagRecord* change  = &m_ChangeData[pl_index];

	QAngle angdiff = pPlayer->GetAbsAngles() - ang;
	Vector orgdiff = pPlayer->GetAbsOrigin() - org;

	// Always remember the pristine simulation time in case we need to restore it.
	restore->m_flSimulationTime = pPlayer->GetSimulationTime();
	restore->m_flAnimTime		= pPlayer->GetAnimTime();

#ifdef CSTRIKE_DLL
	if ( csPlayer )
	{
		restore->m_angRenderAngles = csPlayer->m_angRenderAngles;
		csPlayer->m_angRenderAngles = renderAngles;
	}
#endif

	if ( angdiff.LengthSqr() > 0.0f )
	{
		flags				 |= LC_ANGLES_CHANGED;
		restore->m_vecAngles  = pPlayer->GetAbsAngles();
		pPlayer->SetAbsAngles( ang );
		change->m_vecAngles = ang;
	}

	// Use absolute equality here
	if ( minsPreScaled != pPlayer->CollisionProp()->OBBMinsPreScaled()
		 || maxsPreScaled != pPlayer->CollisionProp()->OBBMaxsPreScaled() )
	{
		flags |= LC_SIZE_CHANGED;

		restore->m_vecMinsPreScaled = pPlayer->CollisionProp()->OBBMinsPreScaled();
		restore->m_vecMaxsPreScaled = pPlayer->CollisionProp()->OBBMaxsPreScaled();

		pPlayer->SetSize( minsPreScaled, maxsPreScaled );

		change->m_vecMinsPreScaled = minsPreScaled;
		change->m_vecMaxsPreScaled = maxsPreScaled;
	}

	// Note, do origin at end since it causes a relink into the k/d tree
	if ( orgdiff.LengthSqr() > 0.0f )
	{
		flags				 |= LC_ORIGIN_CHANGED;
		restore->m_vecOrigin  = pPlayer->GetAbsOrigin();
		pPlayer->SetAbsOrigin( org );
		change->m_vecOrigin = org;
	}

	// Sorry for the loss of the optimization for the case of people
	// standing still, but you breathe even on the server.
	// This is quicker than actually comparing all bazillion floats.
	flags					  |= LC_ANIMATION_CHANGED;
	restore->m_masterSequence  = pPlayer->GetSequence();
	restore->m_masterCycle	   = pPlayer->GetCycle();

	pPlayer->SetSequence( recordAnim->m_masterSequence );
	pPlayer->SetCycle( recordAnim->m_masterCycle );

	////////////////////////
	// Now do all the layers
	int layerCount = pPlayer->GetNumAnimOverlays();
	for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
	{
		CAnimationLayer* currentLayer = pPlayer->GetAnimOverlay( layerIndex );
		if ( currentLayer )
		{
			restore->m_layerRecords[layerIndex].m_cycle	   = currentLayer->m_flCycle;
			restore->m_layerRecords[layerIndex].m_order	   = currentLayer->m_nOrder;
			restore->m_layerRecords[layerIndex].m_sequence = currentLayer->m_nSequence;
			restore->m_layerRecords[layerIndex].m_weight   = currentLayer->m_flWeight;
			restore->m_layerRecords[layerIndex].m_flags	   = currentLayer->m_fFlags;

			currentLayer->m_flCycle	  = recordAnim->m_layerRecords[layerIndex].m_cycle;
			currentLayer->m_nOrder	  = recordAnim->m_layerRecords[layerIndex].m_order;
			currentLayer->m_nSequence = recordAnim->m_layerRecords[layerIndex].m_sequence;
			currentLayer->m_flWeight  = recordAnim->m_layerRecords[layerIndex].m_weight;
			currentLayer->m_fFlags	  = recordAnim->m_layerRecords[layerIndex].m_flags;
		}
	}

	flags |= LC_POSE_PARAMS_CHANGED;

	// Now do pose parameters
	CStudioHdr* hdr = pPlayer->GetModelPtr();
	if ( hdr )
	{
		for ( int paramIndex = 0; paramIndex < hdr->GetNumPoseParameters(); paramIndex++ )
		{
			restore->m_poseParameters[paramIndex] = pPlayer->GetPoseParameterArray()[paramIndex];
			float poseParameter					  = recordAnim->m_poseParameters[paramIndex];

			pPlayer->SetPoseParameterRaw( paramIndex, poseParameter );
		}
	}

	flags |= LC_ENCD_CONS_CHANGED;

	if ( hdr )
	{
		for ( int encIndex = 0; encIndex < hdr->GetNumBoneControllers(); encIndex++ )
		{
			restore->m_encodedControllers[encIndex] = pPlayer->GetBoneControllerArray()[encIndex];
			float encodedController					= recordAnim->m_encodedControllers[encIndex];

			pPlayer->SetBoneControllerRaw( encIndex, encodedController );
		}
	}

	if ( !flags )
	{
		return; // we didn't change anything
	}

	// Set lag compensated player's times
	pPlayer->SetSimulationTime( flTargetLerpSimTime );
	// pPlayer->SetAnimTime(animationData->m_flAnimTime);

	if ( sv_lagflushbonecache.GetBool() )
	{
		pPlayer->InvalidateBoneCache();
	}

	m_RestorePlayer.Set( pl_index ); // remember that we changed this player
	m_bNeedToRestore  = true;		 // we changed at least one player
	restore->m_fFlags = flags;		 // we need to restore these flags
	change->m_fFlags  = flags;		 // we have changed these flags
}

void CLagCompensationManager::FinishLagCompensation( CBasePlayer* player )
{
	VPROF_BUDGET_FLAGS( "FinishLagCompensation",
						VPROF_BUDGETGROUP_OTHER_NETWORKING,
						BUDGETFLAG_CLIENT | BUDGETFLAG_SERVER );

	if ( !m_bNeedToRestore )
	{
		return; // no player was changed at all
	}

	// Iterate all active players
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		if ( !m_RestorePlayer.Get( i ) )
		{
			// player wasn't changed by lag compensation
			continue;
		}

		CBasePlayer* pPlayer = UTIL_PlayerByIndex( i );
		if ( !pPlayer )
		{
			continue;
		}

		LagRecord* restore = &m_RestoreData[i];
		LagRecord* change  = &m_ChangeData[i];

#ifdef CSTRIKE_DLL
		auto csPlayer = ToCSPlayer( pPlayer );

		if ( csPlayer )
		{
			csPlayer->m_angRenderAngles = restore->m_angRenderAngles;
		}
#endif

		if ( restore->m_fFlags & LC_SIZE_CHANGED )
		{
			// see if simulation made any changes, if no, then do the restore, otherwise,
			//  leave new values in
			if ( pPlayer->CollisionProp()->OBBMinsPreScaled() == change->m_vecMinsPreScaled
				 && pPlayer->CollisionProp()->OBBMaxsPreScaled() == change->m_vecMaxsPreScaled )
			{
				// Restore it
				pPlayer->SetSize( restore->m_vecMinsPreScaled, restore->m_vecMaxsPreScaled );
			}
#ifdef STAGING_ONLY
			else
			{
				Warning( "Should we really not restore the size?\n" );
			}
#endif
		}

		if ( restore->m_fFlags & LC_ANGLES_CHANGED )
		{
			if ( pPlayer->GetAbsAngles() == change->m_vecAngles )
			{
				pPlayer->SetAbsAngles( restore->m_vecAngles );
			}
		}

		if ( restore->m_fFlags & LC_ORIGIN_CHANGED )
		{
			// Okay, let's see if we can do something reasonable with the change
			Vector delta = pPlayer->GetAbsOrigin() - change->m_vecOrigin;

			RestorePlayerTo( pPlayer, restore->m_vecOrigin + delta );
		}

		if ( restore->m_fFlags & LC_ANIMATION_CHANGED )
		{
			pPlayer->SetSequence( restore->m_masterSequence );
			pPlayer->SetCycle( restore->m_masterCycle );

			int layerCount = pPlayer->GetNumAnimOverlays();
			for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
			{
				CAnimationLayer* currentLayer = pPlayer->GetAnimOverlay( layerIndex );
				if ( currentLayer )
				{
					currentLayer->m_flCycle	  = restore->m_layerRecords[layerIndex].m_cycle;
					currentLayer->m_nOrder	  = restore->m_layerRecords[layerIndex].m_order;
					currentLayer->m_nSequence = restore->m_layerRecords[layerIndex].m_sequence;
					currentLayer->m_flWeight  = restore->m_layerRecords[layerIndex].m_weight;
					currentLayer->m_fFlags	  = restore->m_layerRecords[layerIndex].m_flags;
				}
			}
		}

		CStudioHdr* hdr = pPlayer->GetModelPtr();

		if ( restore->m_fFlags & LC_POSE_PARAMS_CHANGED )
		{
			if ( hdr )
			{
				for ( int paramIndex = 0; paramIndex < hdr->GetNumPoseParameters(); paramIndex++ )
				{
					pPlayer->SetPoseParameterRaw( paramIndex, restore->m_poseParameters[paramIndex] );
				}
			}
		}

		if ( restore->m_fFlags & LC_ENCD_CONS_CHANGED )
		{
			if ( hdr )
			{
				for ( int encIndex = 0; encIndex < hdr->GetNumBoneControllers(); encIndex++ )
				{
					pPlayer->SetBoneControllerRaw( encIndex, restore->m_encodedControllers[encIndex] );
				}
			}
		}

		pPlayer->SetSimulationTime( restore->m_flSimulationTime );
		pPlayer->SetAnimTime( restore->m_flAnimTime );
	}
}
