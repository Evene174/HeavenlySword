/* 
 * 
 * Confidential Information of Telekinesys Research Limited (t/a Havok).  Not for disclosure or distribution without Havok's
 * prior written consent.This software contains code, techniques and know-how which is confidential and proprietary to Havok.
 * Level 2 and Level 3 source code contains trade secrets of Havok. Havok Software (C) Copyright 1999-2006 Telekinesys Research Limited t/a Havok. All Rights Reserved. Use of this software is subject to the terms of an end user license agreement.
 * 
 */

#include <hkdynamics/hkDynamics.h>
#include <hkbase/thread/hkCriticalSection.h>
#include <hkbase/memory/hkMemory.h>
#include <hkutilities/thread/hkMultithreadingUtil.h>

#include <hkdynamics/world/hkWorld.h>
#include <hkdynamics/world/simulation/hkSimulation.h>

#include <hkbase/basesystem/hkBaseSystem.h>
#include <hkbase/monitor/hkMonitorStream.h>
#include <hkbase/monitor/hkMonitorStreamAnalyzer.h>
#include <hkutilities/visualdebugger/hkPhysicsContext.h>
#include <hkbase/stopwatch/hkStopwatch.h>

static void* HK_CALL WorkerThreadFunc(void* v);

hkMultithreadingUtilCinfo::hkMultithreadingUtilCinfo()
:	m_world(HK_NULL), 
	m_numThreads(1), 
	m_localStackSize( 4000000 ), 
	m_enableTimers(true),
	m_startSpuTasksetFunc(HK_NULL),
	m_startSpuTasksetFuncParam(HK_NULL),
	m_startSpuTaskFunc(HK_NULL),
	m_startSpuTaskFuncParam(HK_NULL),
	m_waitForSpuCompletionFunc(HK_NULL),
	m_waitForSpuCompletionFuncParam(HK_NULL)
{
}



hkMultithreadingUtil::State::State() 
:	m_workerThreadFinished( 0, hkMultithreadingUtil::MAX_NUM_PHYSICS_THREADS ), 
	m_stepInitSemaphore( 0, hkMultithreadingUtil::MAX_NUM_PHYSICS_THREADS ), 
	m_stepEndSemaphore( 0, hkMultithreadingUtil::MAX_NUM_PHYSICS_THREADS ), 
	m_asyncSetupSemaphore( 0, hkMultithreadingUtil::MAX_NUM_PHYSICS_THREADS ),
	m_asyncFinishSemaphore( 0, hkMultithreadingUtil::MAX_NUM_PHYSICS_THREADS ),
	m_workerThreads()
{
	m_numThreads = 0;
	m_physicsRunning = false;

	m_world = HK_NULL;
	m_deltaTime = 1.0f / 60.0f;

	// Asychronous stepping is disabled by default
	m_frameDeltaTime = -1;
}

hkMultithreadingUtil::WorkerThreadData::WorkerThreadData() : m_semaphore(0,1) 
{
	m_monitorStreamBegin = HK_NULL;
	m_monitorStreamEnd = HK_NULL;
	m_streamAnalyzer = HK_NULL;
	m_localMonitorStreamBegin = HK_NULL;
	m_localMonitorStreamEnd = HK_NULL;
	m_killThread = false;
	m_threadId = -1;
	m_state = HK_NULL;
}

	/// Initialize multi-threading state and create threads.
hkMultithreadingUtil::hkMultithreadingUtil(const hkMultithreadingUtilCinfo& ci)
{
	m_state.m_localStackSize = ci.m_localStackSize;
	m_state.m_enableTimers = ci.m_enableTimers;

	m_state.m_startSpuTasksetFunc = ci.m_startSpuTasksetFunc;
	m_state.m_startSpuTasksetFuncParam = ci.m_startSpuTasksetFuncParam;
	m_state.m_startSpuTaskFunc = ci.m_startSpuTaskFunc;
	m_state.m_startSpuTaskFuncParam = ci.m_startSpuTaskFuncParam;
	m_state.m_waitForSpuCompletionFunc = ci.m_waitForSpuCompletionFunc;
	m_state.m_waitForSpuCompletionFuncParam = ci.m_waitForSpuCompletionFuncParam;

	initMultithreading( ci.m_world, ci.m_numThreads);
}

// Destroy threads and delete state.
hkMultithreadingUtil::~hkMultithreadingUtil()
{
	if (m_state.m_world)
	{
		m_state.m_world->removeReference();
	}

	quitMultithreading();
}


void hkMultithreadingUtil::initMultithreading(hkWorld* world, int numThreads)
{
	HK_ASSERT2(0xad67dd83, world && (numThreads <= 1 || world->m_simulationType >= hkWorldCinfo::SIMULATION_TYPE_MULTITHREADED), "To create more than one physics thread you must use the hkWorldCinfo::SIMULATION_TYPE_MULTITHREADED simulation type.");

	attachWorld(world);

	if (numThreads > MAX_NUM_PHYSICS_THREADS)
	{
		numThreads = MAX_NUM_PHYSICS_THREADS;
	}

	for (int i = 0; i < numThreads; i++ )
	{
		WorkerThreadData& data = m_state.m_workerThreads[m_state.m_numThreads++];
		data.m_state = &m_state;
		data.m_threadId = i;
		data.m_thread.startThread( &WorkerThreadFunc, &data );
	}

	reassignThreadTokens();
}

void hkMultithreadingUtil::attachWorld(hkWorld* world)
{
	HK_ASSERT2(0xaf33ede1, !m_state.m_world, "There is already a world attached to this hkMultithreadingUtil. Please detach the old world first.");
	m_state.m_world = world;
	m_state.m_world->addReference();
}

void hkMultithreadingUtil::detachWorld(hkWorld* world)
{
	HK_ASSERT2(0xaf33ede2, m_state.m_world, "No world attached yet.");
	m_state.m_world->removeReference();
	m_state.m_world = HK_NULL;
}

void hkMultithreadingUtil::setFrameTimeMarker( hkReal frameDeltaTime )
{	
	HK_ASSERT2(0xad56ddf7, m_state.m_physicsRunning == false, "Calling hkMultithreadingUtil::setFrameTimeMarker(), without having called hkMultithreadingUtil::waitForStepWorldFinished().");

	HK_WARN_ONCE(0x09afce65, "Multithreaded asynchrouous stepping with hkMultithreadingUtil is not recommended, as it will lead to poor performance \
							 as threads will often be waiting for the primary thread performing single threaded operations. Integrate the asychronous \
							 stepping code into your game to improve this performance, or switch to synchronous stepping" );

	m_state.m_frameDeltaTime = frameDeltaTime;
}


void hkMultithreadingUtil::startStepWorld( hkReal physicsDeltaTime )
{
	HK_ASSERT2(0xad56dd77, m_state.m_physicsRunning == false, "Calling hkMultithreadingUtil::startStepWorld() for the second time, without having called hkMultithreadingUtil::waitForStepWorldFinished().");
	m_state.m_physicsRunning = true;

	m_state.m_deltaTime = physicsDeltaTime;

	for (int i = m_state.m_numThreads - 1; i >=0; i--)
	{
		WorkerThreadData& data = m_state.m_workerThreads[i];
		data.m_semaphore.release();
	}
}

void hkMultithreadingUtil::waitForStepWorldFinished()
{
	// This function does nothing if waitForStepWorldFinished() is called before startStepWorld()
	if (m_state.m_physicsRunning)
	{
		for (int i = 0; i < m_state.m_numThreads; ++i)
		{
			m_state.m_workerThreadFinished.acquire();
		}
		m_state.m_physicsRunning = false;
	}

	// copy all monitor streams to the internal buffer
	{
		for (int i = 0; i < m_state.m_numThreads; ++i)
		{
			WorkerThreadData& data = m_state.m_workerThreads[i];
			if ( data.m_localMonitorStreamBegin )
			{
				int size = int(data.m_localMonitorStreamEnd - data.m_localMonitorStreamBegin);
				hkString::memCpy( data.m_monitorStreamBegin, data.m_localMonitorStreamBegin, size );
				data.m_monitorStreamEnd = data.m_monitorStreamBegin + size;
			}
			else
			{
				data.m_monitorStreamEnd = data.m_monitorStreamBegin;
			}
		}
	}
}

void hkMultithreadingUtil::stepWorld( hkReal physicsDeltaTime )
{
	HK_ASSERT2( 0xf0213454, m_state.m_world->isLocked() == false, "You cannot lock the world when calling this function." );
	startStepWorld( physicsDeltaTime );
	waitForStepWorldFinished();
}

void hkMultithreadingUtil::quitMultithreading()
{
	waitForStepWorldFinished();
	
	{
		for (int i = 0; i < m_state.m_numThreads; i++)
		{
			WorkerThreadData& data = m_state.m_workerThreads[i];
			data.m_killThread = true;
			data.m_semaphore.release(); // sets the thread off to enable it to finish
		}
	}
	{
		for (int i = 0; i < m_state.m_numThreads; i++)
		{
			m_state.m_workerThreadFinished.acquire(); // wait for the N threads to actually end
		}
	}
}

// Get number of threads used for physics
int hkMultithreadingUtil::getNumThreads()
{
	return m_state.m_numThreads;
}

// Set the number of threads to be used for physics. Returns the number of threads actually set.
int hkMultithreadingUtil::setNumThreads(int numThreads)
{
	while( m_state.m_numThreads < numThreads && HK_SUCCESS == addThread()    ) { }
	while( m_state.m_numThreads > numThreads && HK_SUCCESS == removeThread() ) { }
	return m_state.m_numThreads;
}

void hkMultithreadingUtil::reassignThreadTokens()
{

	//
	// Get the thread "Token". This will be either HK_THREAD_TOKEN_PRIMARY for the first thread or HK_THREAD_TOKEN_SECONDARY 
	// (for all subsequent threads). 
	//

	m_state.m_world->resetThreadTokens();
	for (int i = 0; i < m_state.m_numThreads; ++i )
	{
		WorkerThreadData& data = m_state.m_workerThreads[i];
		data.m_threadToken = m_state.m_world->getThreadToken();
	}
}

hkResult hkMultithreadingUtil::addThread()
{
	HK_ASSERT2(0xad67bd88, ! m_state.m_physicsRunning, "You can only add or remove working threads via calls from the master thread and not between startStepWorld() and waitForStepWorldFinished() calls. ");

	if (m_state.m_world->m_simulationType < hkWorldCinfo::SIMULATION_TYPE_MULTITHREADED)
	{
		HK_WARN_ONCE(0xad67dd83, "To create more than one physics thread you must use the hkWorldCinfo::SIMULATION_TYPE_MULTITHREADED simulation type.");
	}

	int maxNumThreads = m_state.m_world->m_simulationType >= hkWorldCinfo::SIMULATION_TYPE_MULTITHREADED ? MAX_NUM_PHYSICS_THREADS : 1;
	if (m_state.m_numThreads < maxNumThreads)
	{
		WorkerThreadData& data = m_state.m_workerThreads[m_state.m_numThreads];
		data.m_state = &m_state;
		data.m_threadId = m_state.m_numThreads;
		data.m_monitorStreamBegin = HK_NULL;
		data.m_monitorStreamEnd = HK_NULL;
		data.m_killThread = false;
		data.m_thread.startThread( &WorkerThreadFunc, &data );

		m_state.m_numThreads++;

		reassignThreadTokens();

		return HK_SUCCESS;
	}

	return HK_FAILURE;
}


hkResult hkMultithreadingUtil::removeThread()
{
	HK_ASSERT2(0xad67bd89, !m_state.m_physicsRunning, "You can only add or remove working threads via calls from the master thread and not between startStepWorld() and waitForStepWorldFinished() calls. ");

	if (m_state.m_numThreads > 0)
	{
		--m_state.m_numThreads;
		
		WorkerThreadData& data = m_state.m_workerThreads[m_state.m_numThreads];

		// Signal the thread to be killed, and release the thread
		data.m_killThread = true;
		data.m_semaphore.release();

		// Wait until the thread actually finishes
		m_state.m_workerThreadFinished.acquire();

		reassignThreadTokens();

		return HK_SUCCESS;
	}
	return HK_FAILURE;
}




static void* HK_CALL WorkerThreadFunc(void *v)
{
	hkMultithreadingUtil::WorkerThreadData& data = *static_cast<hkMultithreadingUtil::WorkerThreadData*>(v);
	hkMultithreadingUtil::State& state = *data.m_state;

#ifdef HK_PLATFORM_XBOX360
	// If the platform is xbox 360 then take one thread per cpu for the 1st 3 threads, then start hyperthreading.
	int tid = (data.m_threadId % 3) * 2;
	if (data.m_threadId > 2)
		tid++; // take other slot on this cpu
	XSetThreadProcessor(GetCurrentThread(), tid);
#endif

	// Initialize the thread memory for this thread, referencing the global memory instance
	hkThreadMemory threadMemory(&hkMemory::getInstance(), 16);
	{ 
		hkBaseSystem::initThread( &threadMemory );
	}

	// Allocate stack memory for this thread 
	char* stackBuffer;
	{
		int stackSize = state.m_localStackSize; 
		stackBuffer = hkAllocate<char>( stackSize, HK_MEMORY_CLASS_BASE);
		threadMemory.setStackArea( stackBuffer, stackSize);
	}

	if (state.m_enableTimers)
	{
		// Allocate a monitor stream for this thread - this  enables timers.
		int monitorStreamSize = 2000000;
		hkMonitorStream::getInstance().resize(monitorStreamSize);
		data.m_monitorStreamBegin = hkAllocate<char>(monitorStreamSize, HK_MEMORY_CLASS_BASE);
		data.m_monitorStreamEnd = data.m_monitorStreamBegin;

		// Allocate a large buffer for this thread - this is only needed if we wish to gather
		// detailed stats for the simulation.
		{
			data.m_streamAnalyzer = new hkMonitorStreamAnalyzer( 10000000 ); // 10megs
		}
	}


	// Wait for the main thread to release the physics thread 
	data.m_semaphore.acquire();

	hkThreadToken token = data.m_threadToken;

	// The thread "main loop"
	while (data.m_killThread == false)
	{
		hkMonitorStream::getInstance().reset();

		// Enable timers for critical sections just during the step call
		hkCriticalSection::setTimersEnabled();

		if ( state.m_frameDeltaTime == -1.f )
		{
			//
			// Note: This code section is the same as the code in hkWorld::stepDeltaTime (multithreaded version)
			// It is explicitly added here, so the fine grained multithreaded interface can be seen in operation.
			// The fine grained interface allows you to separate out the single threaded step begin and step end 
			// from the multithreaded process call.
			//	

			{
				HK_TIMER_BEGIN("Simulate", HK_NULL);

				if ( token == HK_THREAD_TOKEN_PRIMARY )
				{
					//
					// The primary thread only calls this section.  This can currently take a significant amount of time.
					// You can utilize secondary thread time by polling the m_stepInitSemaphore and doing other work if you
					// like. It is expected that you do not make other calls to hkWorld during stepBeginSt, or between 
					// stepBeginSt() and stepProcessMt().
					//
					state.m_world->stepBeginSt( state.m_deltaTime );

					if (state.m_startSpuTasksetFunc)
					{
						state.m_startSpuTasksetFunc(state.m_startSpuTasksetFuncParam);
						state.m_startSpuTasksetFunc = HK_NULL;
					}

					if (state.m_startSpuTaskFunc)
					{
						state.m_startSpuTaskFunc(state.m_startSpuTaskFuncParam);
					}
					state.m_stepInitSemaphore.release( state.m_numThreads );
				}
				else
				{
					// Secondary threads could do additional work in this section, and could poll the m_stepInitSemaphore
					// in order to check whether stepBeginSt is finished.
				}

				// All secondary threads must wait here until stepBeginSt is finished.  If they do not, they will return 
				// immediately from stepProcessMt(), because no work will be ready for them.
				state.m_stepInitSemaphore.acquire();

				// All threads process this in parallel. Note that no thread returns from this function until
				// all jobs are complete.
				state.m_world->stepProcessMt( token );


				if ( token == HK_THREAD_TOKEN_PRIMARY )
				{
					//
					// stepEndSt must be called single threaded.  This actually doesn't have to be the same thread that
					// called stepBeginSt, but for convenience it is here.  This can take some time, as all the continuous
					// simulation is processed here.  All other threads simply exit this code block.  It is necessary
					// to wait until stepEndSt is complete before any thread re-enters this code block and starts to 
					// process a world step.  In this utility, we use a semaphore, data.m_semaphore, which is released by
					// a separate ("main") thread each step.
					//
					state.m_world->stepEndSt( );
				}


				HK_TIMER_NAMED_END("Simulate");
			}
		}
		else
		{

			//
			// Asynchronous multithreaded stepping.  Please also refer to the comments in the above code section, as they
			// apply to this section too.  This is an amalgamation of the code hkAsynchronousTimestepper::stepAsynchronously (single
			// threaded asynchronous stepping) and the code in the above section.
			//

			HK_TIMER_BEGIN("Simulate", HK_NULL);

			if ( token == HK_THREAD_TOKEN_PRIMARY )
			{
				// The primary thread sets the frame time, and advances the simulation time.
				state.m_world->setFrameTimeMarker( state.m_frameDeltaTime );
				state.m_world->advanceTime();

				state.m_asyncSetupSemaphore.release( state.m_numThreads );
			}

			// All secondary threads must wait until the primary thread has set the frame time, and advanced the simulation.
			state.m_asyncSetupSemaphore.acquire();

			// Keep iterating until we have reached the simulation marker
			while ( !state.m_world->isSimulationAtMarker() ) 
			{


				if ( token == HK_THREAD_TOKEN_PRIMARY )
				{

					if (state.m_startSpuTasksetFunc)
					{
						state.m_startSpuTasksetFunc(state.m_startSpuTasksetFuncParam);
						state.m_startSpuTasksetFunc = HK_NULL;
					}

					HK_ASSERT( 0x11179564, state.m_world->isSimulationAtPsi() );
					state.m_world->stepBeginSt( state.m_deltaTime );

					if (state.m_startSpuTaskFunc)
					{
						state.m_startSpuTaskFunc(state.m_startSpuTaskFuncParam);
					}
					state.m_stepInitSemaphore.release( state.m_numThreads );
				}
				else
				{
					// Secondary threads can do something else while waiting for primary thread
				}

				state.m_stepInitSemaphore.acquire();

				// Make also the PRIMARY thread WAIT for all SECONDARY threads at the start of this loop here.
				// As not doing so causes problems with the logic below. 
				//
				// Together with m_stepEndSemaphore, this is analogical to what is done by the main thread when ( state.m_frameDeltaTime == -1.f )
				// and when we don't use this loop:
				//
				//   The main thread waits until each worker thread calls "state.m_workerThreadFinished.release()".
				//	 and only then all threads are resumed from the line "data.m_semaphore.acquire()".
				if (token == HK_THREAD_TOKEN_PRIMARY)
				{
					// Acquire a semaphore once for each NON-PRIMARY thread
					for (int t = 0; t < state.m_numThreads-1; t++)
					{
						state.m_asyncFinishSemaphore.acquire();
					}
				}
				else
				{
					state.m_asyncFinishSemaphore.release();
				}

				// All threads process the multithreaded step
				state.m_world->stepProcessMt( token );

				if ( token == HK_THREAD_TOKEN_PRIMARY )
				{
					// This function cleans up any data structures necessary from the multithreaded stepping and then
					// calls world::advanceTime().
					state.m_world->stepEndSt();

					if ( state.m_world->isSimulationAtPsi() )
					{
						// This is the correct place to interact with the physics.  If the code is here it would be
						// performed single threaded by the primary thread.
					}
					if (state.m_waitForSpuCompletionFunc)
					{
						state.m_waitForSpuCompletionFunc(state.m_waitForSpuCompletionFuncParam);
					}
					state.m_stepEndSemaphore.release ( state.m_numThreads );
				}
				else
				{
					// Do something else while waiting for the primary thread
					// Poll m_stepEndSemaphore
				}

				// All secondary threads must wait until stepEndSt() is complete before continuing.
				state.m_stepEndSemaphore.acquire();

				// Make also the PRIMARY thread WAIT for all SECONDARY threads at the end of this loop here.
				// As not doing so causes problems with the logic above. 
				//
				// Together with m_stepEndSemaphore, this is analogical to what is done by the main thread when ( state.m_frameDeltaTime == -1.f )
				// and when we don't use this loop:
				//
				//   The main thread waits until each worker thread calls "state.m_workerThreadFinished.release()".
				//	 and only then all threads are resumed from the line "data.m_semaphore.acquire()".
				if (token == HK_THREAD_TOKEN_PRIMARY)
				{
					// Acquire a semaphore once for each NON-PRIMARY thread
					for (int t = 0; t < state.m_numThreads-1; t++)
					{
						state.m_asyncFinishSemaphore.acquire();
					}
				}
				else
				{
					state.m_asyncFinishSemaphore.release();
				}
			}

			HK_TIMER_NAMED_END("Simulate");
		}

		// Disable timers for critical sections just during the step call
		hkCriticalSection::setTimersDisabled();

		// Note collected timer data
		hkMonitorStream& stream = hkMonitorStream::getInstance();
		data.m_localMonitorStreamBegin = stream.getStart();
		data.m_localMonitorStreamEnd   = stream.getEnd();

		// Release any thread (usually the main thread) which may be waiting for all worker threads to finish.
		state.m_workerThreadFinished.release();

		// Immediately wait until the main thread releases the physics thread again 
		// - we do nothing but physics in this thread
		data.m_semaphore.acquire();
	}
	

	// Perform cleanup operations

	hkDeallocate(data.m_monitorStreamBegin);
	data.m_monitorStreamBegin = HK_NULL;
	data.m_monitorStreamEnd  = HK_NULL;
	data.m_localMonitorStreamBegin = HK_NULL;
	data.m_localMonitorStreamEnd = HK_NULL;
	delete data.m_streamAnalyzer;
	data.m_streamAnalyzer = HK_NULL;

	threadMemory.setStackArea(0, 0);
	hkDeallocate(stackBuffer);

	hkBaseSystem::clearThreadResources();

	state.m_workerThreadFinished.release();

	return 0;
}


/*
* Havok SDK - PUBLIC RELEASE, BUILD(#20061017)
*
* Confidential Information of Havok.  (C) Copyright 1999-2006 
* Telekinesys Research Limited t/a Havok. All Rights Reserved. The Havok
* Logo, and the Havok buzzsaw logo are trademarks of Havok.  Title, ownership
* rights, and intellectual property rights in the Havok software remain in
* Havok and/or its suppliers.
*
* Use of this software for evaluation purposes is subject to and indicates 
* acceptance of the End User licence Agreement for this product. A copy of 
* the license is included with this software and is also available from salesteam@havok.com.
*
*/
