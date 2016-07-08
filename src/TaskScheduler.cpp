// Copyright (c) 2013 Doug Binks
// 
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
// 
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgement in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

#ifndef NO_STL_TYPE_CONSISTENCY
// Ensure we are compiling STL types consistently across DLL boundaries.
#undef _HAS_ITERATOR_DEBUGGING
#define _HAS_ITERATOR_DEBUGGING 0
#undef _SECURE_SCL
#define _SECURE_SCL 0
#undef _SECURE_SCL_THROWS
#define _SECURE_SCL_THROWS 0
#endif

#include "TaskScheduler.h"
#include "LockLessMultiReadPipe.h"

#include <thread>

using namespace enki;


static const uint32_t PIPESIZE_LOG2 = 8;
static const uint32_t SPIN_COUNT = 100;

#ifdef _MSC_VER
#if _MSC_VER <= 1800
#define thread_local __declspec(thread)
#endif
#elif defined(__APPLE__)
#define thread_local __thread
#endif

// each software thread gets it's own copy of gtl_threadNum, so this is safe to use as a static variable
static thread_local uint32_t                             gtl_threadNum       = 0;

namespace enki 
{
	struct TaskSetInfo
	{
		ITaskSet*           pTask;
		TaskSetPartition    partition;
	};

	// we derive class TaskPipe rather than typedef to get forward declaration working easily
	class TaskPipe : public LockLessMultiReadPipe<PIPESIZE_LOG2,enki::TaskSetInfo> {};

	struct ThreadArgs
	{
		uint32_t		threadNum;
		TaskScheduler*  pTaskScheduler;
	};
}

void TaskScheduler::TaskingThreadFunction( const ThreadArgs& args_ )
{
	uint32_t threadNum				= args_.threadNum;
	TaskScheduler*  pTS				= args_.pTaskScheduler;
    gtl_threadNum      = threadNum;
	pTS->m_NumThreadsActive.fetch_add(1, std::memory_order_relaxed );
    
    uint32_t spinCount = 0;
    while( pTS->m_bRunning.load( std::memory_order_relaxed ) )
    {
        if(!pTS->TryRunTask( threadNum ) )
        {
            // no tasks, will spin then wait
            ++spinCount;
            if( spinCount > SPIN_COUNT )
            {
				bool bHaveTasks = false;
				for( uint32_t thread = 0; thread < pTS->m_NumThreads; ++thread )
				{
					if( !pTS->m_pPipesPerThread[ thread ].IsPipeEmpty() )
					{
						bHaveTasks = true;
						break;
					}
				}
				if( bHaveTasks )
				{
					// keep trying
					spinCount = 0;
				}
				else
				{

					pTS->m_NumThreadsActive.fetch_sub( 1, std::memory_order_relaxed );
					std::unique_lock<std::mutex> lk( pTS->m_NewTaskEventMutex );
					pTS->m_NewTaskEvent.wait( lk );
					pTS->m_NumThreadsActive.fetch_add( 1, std::memory_order_relaxed );
					spinCount = 0;
				}
            }
        }
    }

    pTS->m_NumThreadsRunning.fetch_sub( 1, std::memory_order_relaxed );
    return;
}


void TaskScheduler::StartThreads()
{
    if( m_bHaveThreads )
    {
        return;
    }
    m_bRunning = 1;

    // we create one less thread than m_NumThreads as the main thread counts as one
    m_pThreadNumStore = new ThreadArgs[m_NumThreads];
    m_pThreads		  = new std::thread*[m_NumThreads];
	m_pThreadNumStore[0].threadNum      = 0;
	m_pThreadNumStore[0].pTaskScheduler = this;
    for( uint32_t thread = 1; thread < m_NumThreads; ++thread )
    {
		m_pThreadNumStore[thread].threadNum      = thread;
		m_pThreadNumStore[thread].pTaskScheduler = this;
        m_pThreads[thread] = new std::thread( TaskingThreadFunction, m_pThreadNumStore[thread] );
        ++m_NumThreadsRunning;
    }

    // ensure we have sufficient tasks to equally fill either all threads including main
    // or just the threads we've launched, this is outside the firstinit as we want to be able
    // to runtime change it
	if( 1 == m_NumThreads )
	{
		m_NumPartitions = 1;
	}
	else
	{
		m_NumPartitions = m_NumThreads * (m_NumThreads - 1);
	}

    m_bHaveThreads = true;
}

void TaskScheduler::StopThreads( bool bWait_ )
{
    if( m_bHaveThreads )
    {
        // wait for them threads quit before deleting data
        m_bRunning = 0;
        while( bWait_ && m_NumThreadsRunning )
        {
            // keep firing event to ensure all threads pick up state of m_bRunning
           m_NewTaskEvent.notify_all();
        }



		m_NumThreads = 0;
        delete[] m_pThreadNumStore;
        delete[] m_pThreads;
        m_pThreadNumStore = 0;
        m_pThreads = 0;

        m_bHaveThreads = false;
		m_NumThreadsActive = 0;
		m_NumThreadsRunning = 0;
    }
}

bool TaskScheduler::TryRunTask( uint32_t threadNum )
{
    // check for tasks
    TaskSetInfo info;
    bool bHaveTask = m_pPipesPerThread[ threadNum ].WriterTryReadFront( &info );

    if( m_NumThreads )
    {
        uint32_t checkOtherThread = 0;
        while( !bHaveTask && checkOtherThread < m_NumThreads )
        {
			if( checkOtherThread != threadNum )
			{
				bHaveTask = m_pPipesPerThread[ checkOtherThread ].ReaderTryReadBack( &info );
			}
            ++checkOtherThread;
        }
    }
        
    if( bHaveTask )
    {
        // the task has already been divided up by AddTaskSetToPipe, so just run it
        info.pTask->ExecuteRange( info.partition, threadNum );
        info.pTask->m_CompletionCount.fetch_sub(1,std::memory_order_relaxed );
    }

    return bHaveTask;

}


void    TaskScheduler::AddTaskSetToPipe( ITaskSet* pTaskSet )
{
    TaskSetInfo info;
    info.pTask = pTaskSet;
    info.partition.start = 0;
    info.partition.end = pTaskSet->m_SetSize;

    // no one owns the task as yet, so just add to count
    pTaskSet->m_CompletionCount.store( 0, std::memory_order_relaxed );

    // divide task up and add to pipe
    uint32_t numToRun = info.pTask->m_SetSize / m_NumPartitions;
    if( numToRun == 0 ) { numToRun = 1; }
    uint32_t rangeLeft = info.partition.end - info.partition.start ;
    while( rangeLeft )
    {
        if( numToRun > rangeLeft )
        {
            numToRun = rangeLeft;
        }
        info.partition.start = pTaskSet->m_SetSize - rangeLeft;
        info.partition.end = info.partition.start + numToRun;
        rangeLeft -= numToRun;

        // add the partition to the pipe
        info.pTask->m_CompletionCount.fetch_add( 1, std::memory_order_relaxed );
        if( !m_pPipesPerThread[ gtl_threadNum ].WriterTryWriteFront( info ) )
        {
			if( m_NumThreadsActive.load( std::memory_order_relaxed ) < m_NumThreadsRunning.load( std::memory_order_relaxed ) )
			{
				m_NewTaskEvent.notify_all();
			}
            info.pTask->ExecuteRange( info.partition, gtl_threadNum );
            pTaskSet->m_CompletionCount.fetch_sub(1,std::memory_order_relaxed );
        }
    }

	if( m_NumThreadsActive.load( std::memory_order_relaxed ) < m_NumThreadsRunning.load( std::memory_order_relaxed ) )
	{
		m_NewTaskEvent.notify_all();
	}

}

void    TaskScheduler::WaitforTaskSet( const ITaskSet* pTaskSet )
{
	if( pTaskSet )
	{
		while( !pTaskSet->GetIsComplete() )
		{
			TryRunTask( gtl_threadNum );
			// should add a spin then wait for task completion event.
		}
	}
	else
	{
			TryRunTask( gtl_threadNum );
	}
}

void    TaskScheduler::WaitforAll()
{
    bool bHaveTasks = true;
    while( bHaveTasks || m_NumThreadsActive.load( std::memory_order_relaxed ) )
    {
        TryRunTask( gtl_threadNum );
        bHaveTasks = false;
        for( uint32_t thread = 0; thread < m_NumThreads; ++thread )
        {
            if( !m_pPipesPerThread[ thread ].IsPipeEmpty() )
            {
                bHaveTasks = true;
                break;
            }
        }
     }
}

void    TaskScheduler::WaitforAllAndShutdown()
{
    WaitforAll();
    StopThreads(true);
	delete[] m_pPipesPerThread;
    m_pPipesPerThread = 0;
}

uint32_t        TaskScheduler::GetNumTaskThreads() const
{
    return m_NumThreads;
}

TaskScheduler::TaskScheduler()
		: m_pPipesPerThread(NULL)
		, m_NumThreads(0)
		, m_pThreadNumStore(NULL)
		, m_pThreads(NULL)
		, m_bRunning(0)
		, m_NumThreadsRunning(0)
		, m_NumThreadsActive(0)
		, m_NumPartitions(0)
		, m_bHaveThreads(false)
{
}

TaskScheduler::~TaskScheduler()
{
    StopThreads( true ); // Stops threads, waiting for them.

    delete[] m_pPipesPerThread;
    m_pPipesPerThread = 0;
}

void    TaskScheduler::Initialize( uint32_t numThreads_ )
{
	assert( numThreads_ );
    StopThreads( true ); // Stops threads, waiting for them.
    delete[] m_pPipesPerThread;

	m_NumThreads = numThreads_;

    m_pPipesPerThread = new TaskPipe[ m_NumThreads ];

    StartThreads();
}

void   TaskScheduler::Initialize()
{
	Initialize( std::thread::hardware_concurrency() );
}