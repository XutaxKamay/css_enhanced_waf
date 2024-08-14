#ifndef C_EVENTQUEUE_H
#define C_EVENTQUEUE_H

#ifdef WIN32
#pragma once
#endif

#include "css_enhanced/variant_t.h"
#include "cbase.h"
#include "vprof.h"
#include "isaverestore.h"
#include "datacache/imdlcache.h"
#include "mempool.h"

#define CEventQueue C_EventQueue

class variant_t;

struct EventQueuePrioritizedEvent_t
{
	int m_iFireTick;
	string_t m_iTarget;
	string_t m_iTargetInput;
	EHANDLE m_pActivator;
	EHANDLE m_pCaller;
	int m_iOutputID;
	EHANDLE m_pEntTarget;  // a pointer to the entity to target; overrides m_iTarget

	variant_t m_VariantValue;	// variable-type parameter

	EventQueuePrioritizedEvent_t *m_pNext;
	EventQueuePrioritizedEvent_t *m_pPrev;

	DECLARE_SIMPLE_DATADESC();
	DECLARE_FIXEDSIZE_ALLOCATOR( PrioritizedEvent_t );
};

class CEventQueue
{
public:
	// pushes an event into the queue, targeting a string name (m_iName), or directly by a pointer
	void AddEvent( const char *target, const char *action, variant_t Value, float fireDelay, CBaseEntity *pActivator, CBaseEntity *pCaller, int outputID = 0 );
	void AddEvent( CBaseEntity *target, const char *action, float fireDelay, CBaseEntity *pActivator, CBaseEntity *pCaller, int outputID = 0 );
	void AddEvent( CBaseEntity *target, const char *action, variant_t Value, float fireDelay, CBaseEntity *pActivator, CBaseEntity *pCaller, int outputID = 0 );

	void CancelEvents( CBaseEntity *pCaller );
	void CancelEventOn( CBaseEntity *pTarget, const char *sInputName );
	bool HasEventPending( CBaseEntity *pTarget, const char *sInputName );

	// services the queue, firing off any events who's time hath come
	void ServiceEvents( void );

	// debugging
	void ValidateQueue( void );

	// serialization
	int Save( ISave &save );
	int Restore( IRestore &restore );

	CEventQueue();
	~CEventQueue();

	void Init( void );
	void Clear( void ); // resets the list

	void Dump( void );

private:

	void AddEvent( EventQueuePrioritizedEvent_t *event );
	void RemoveEvent( EventQueuePrioritizedEvent_t *pe );

	DECLARE_SIMPLE_DATADESC();
	EventQueuePrioritizedEvent_t m_Events;
	int m_iListCount;
};

// XYZ_TODO call this in client prediction
void ServiceEventQueue( void );

extern CEventQueue g_EventQueue;



#endif