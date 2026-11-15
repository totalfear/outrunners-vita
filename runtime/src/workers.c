/**
 *    Name:        workers.c
 *    Description: threads that take the rows of a frame's work, and the tasks queued for them (workers.h):
 *                 Windows' threads, POSIX threads, or on the Switch libnx's, each on a core of its own
 *    Version:     0.2.0
 *    Created:     23-10-2026
 *    Modified:    14-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    One lock keeps it all. A job is rows shared out: who shares them and every thread with nothing else to
 *    do take a few at a time until none is left, and who shared them waits for the last ones to be done. So
 *    the threads there are do the rows, however many they are at the moment: a thread that comes late takes
 *    what is left, and none waits beside rows not taken.
 *
 *    A task is work one thread does by itself, the tasks one after another in the order they were queued,
 *    while who queued them goes on; a task shares rows out as anyone does. s32_workers_finish is how the
 *    caller waits for one: until it is done the caller is one more thread, for rows and for the tasks before
 *    it. With no threads the tasks wait in the queue, and s32_workers_finish does them.
 */
#include "s32/workers.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
typedef SRWLOCK pool_lock;
typedef CONDITION_VARIABLE pool_signal;
#define LOCK(lock) AcquireSRWLockExclusive(lock)
#define UNLOCK(lock) ReleaseSRWLockExclusive(lock)
#define WAIT(signal, lock) SleepConditionVariableSRW(signal, lock, INFINITE, 0)
#define WAKE_ALL(signal) WakeAllConditionVariable(signal)
#elif defined(__SWITCH__)
#include <switch.h>
typedef Mutex pool_lock;
typedef CondVar pool_signal;
#define LOCK(lock) mutexLock(lock)
#define UNLOCK(lock) mutexUnlock(lock)
#define WAIT(signal, lock) condvarWait(signal, lock)
#define WAKE_ALL(signal) condvarWakeAll(signal)
#else
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
typedef pthread_mutex_t pool_lock;
typedef pthread_cond_t pool_signal;
#define LOCK(lock) pthread_mutex_lock(lock)
#define UNLOCK(lock) pthread_mutex_unlock(lock)
#define WAKE_ALL(signal) pthread_cond_broadcast(signal)
#define WAITS_TIMED

/* A wait that looks again after 2 ms however it went, true if it was the time that ended it. Every wait here
   is in a loop that checks what it waits for, so a wake-up the system loses costs 2 ms and not the game: the
   Vita's game froze at random, at 60 frames a second and with nothing in its log, once the frame's video
   became the workers' tasks and three threads waited on the one signal */
static bool wait_timed(pool_signal *signal, pool_lock *lock)
{
	struct timeval now;
	struct timespec until;

	gettimeofday(&now, NULL);
	until.tv_sec = now.tv_sec;
	until.tv_nsec = (long)now.tv_usec * 1000 + 2000000;
	if (until.tv_nsec >= 1000000000)
	{
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	return pthread_cond_timedwait(signal, lock, &until) == ETIMEDOUT;
}
#endif
#ifndef WAITS_TIMED
static bool wait_timed(pool_signal *signal, pool_lock *lock)
{
	WAIT(signal, lock);
	return false;
}
#endif
#include <stdio.h>
#include <time.h>

/* (for a port's log: the waits that ended on their time, and of those the ones that found what they waited
   for already there, a wake-up lost; read and put back to 0 by whoever reads them) */
unsigned long s32_workers_waits_timed_out, s32_workers_wakeups_lost;

#define WORKERS_MOST 7
#define TASKS_MOST   8    /* queued and not done: one more waits for the oldest */
#define BANDS_A_THREAD 2  /* s32_workers_rows: the bands the rows are shared out in, for each thread there is */

static struct
{
	int count;
	pool_lock lock;
	pool_signal changed;     /* there are rows to take or a task, or a job's last rows or a task are done */

	/* the job: the rows being shared out, one job at a time */
	s32_rows_work work;
	void *context;
	int next;                /* the first row not taken yet */
	int last;
	int step;                /* the rows a thread takes at a time */
	int taking;              /* threads at work on rows they took */

	/* the tasks, by their numbers: task n is in tasks[n % TASKS_MOST] until it is done */
	struct
	{
		s32_task_work work;
		void *context;
	} tasks[TASKS_MOST];
	unsigned queued;         /* the number of the last one queued */
	unsigned started;        /* of the last one a thread took */
	unsigned done;           /* of the last one done: the next is taken when started == done */
#ifdef __SWITCH__
	Thread threads[WORKERS_MOST];
#endif
} pool = { .last = -1 };

/* The next rows of the job, if any are left, done by the calling thread. The lock is held when it is called
   and when it returns, and not while the rows are worked */
static bool take_rows(void)
{
	const s32_rows_work work = pool.work;
	void *const context = pool.context;
	const int from = pool.next;
	const int to = pool.last - from < pool.step ? pool.last : from + pool.step - 1;

	if (from > pool.last)
		return false;
	pool.next = to + 1;
	pool.taking++;
	UNLOCK(&pool.lock);
	work(context, from, to);
	LOCK(&pool.lock);
	if (--pool.taking == 0 && pool.next > pool.last)
		WAKE_ALL(&pool.changed);    /* (the job's last rows: who shared them out waits for these) */
	return true;
}

/* The next task, if one waits and none is running, done by the calling thread; the lock as in take_rows */
static bool take_task(void)
{
	s32_task_work work;
	void *context;

	if (pool.started == pool.queued || pool.started != pool.done)
		return false;
	pool.started++;
	work = pool.tasks[pool.started % TASKS_MOST].work;
	context = pool.tasks[pool.started % TASKS_MOST].context;
	UNLOCK(&pool.lock);
	work(context);
	LOCK(&pool.lock);
	pool.done++;
	WAKE_ALL(&pool.changed);    /* (who waits for it; and the next task can be taken) */
	return true;
}

/* There are rows to take, or a task that can be taken. The lock is held */
static bool work_waiting(void)
{
	return pool.next <= pool.last || (pool.started != pool.queued && pool.started == pool.done);
}

/* A wait on the pool's signal for a thread that waits while 'still' holds, the lock held: a wait the time
   ended that finds it no longer holds, or work to take, missed a wake-up */
#define WAIT_WHILE(still) \
	do \
	{ \
		if (wait_timed(&pool.changed, &pool.lock)) \
		{ \
			s32_workers_waits_timed_out++; \
			if (!(still) || work_waiting()) \
				s32_workers_wakeups_lost++; \
		} \
	} while (0)

static void worker_loop(int number)
{
	(void)number;
	LOCK(&pool.lock);
	for (;;)
		if (!take_rows() && !take_task())
			WAIT_WHILE(true);
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID number)
{
	worker_loop((int)(intptr_t)number);
	return 0;
}
#elif defined(__SWITCH__)
static void worker_entry(void *number)
{
	worker_loop((int)(intptr_t)number);
}
#else
static void *worker_entry(void *number)
{
	worker_loop((int)(intptr_t)number);
	return NULL;
}
#endif

void s32_workers_start(int count)
{
	if (pool.count > 0 || count <= 0)
		return;
	s32_workers_finish_all();    /* (tasks queued while there were no threads: done as they would have been) */
	if (count > WORKERS_MOST)
		count = WORKERS_MOST;
#if defined(_WIN32)
	InitializeSRWLock(&pool.lock);
	InitializeConditionVariable(&pool.changed);
#elif defined(__SWITCH__)
	mutexInit(&pool.lock);
	condvarInit(&pool.changed);
#else
	pthread_mutex_init(&pool.lock, NULL);
	pthread_cond_init(&pool.changed, NULL);
#endif
	for (int number = 0; number < count; number++)
	{
#if defined(_WIN32)
		HANDLE thread = CreateThread(NULL, 0, worker_entry, (LPVOID)(intptr_t)number, 0, NULL);

		if (!thread)
			break;
		CloseHandle(thread);
#elif defined(__SWITCH__)
		/* (an application has cores 0 to 2 and runs on 0: the workers on 1 and 2) */
		if (R_FAILED(threadCreate(&pool.threads[number], worker_entry, (void *)(intptr_t)number, NULL, 0x10000,
			0x2C, 1 + number % 2)) || R_FAILED(threadStart(&pool.threads[number])))
			break;
#else
		pthread_t thread;

		if (pthread_create(&thread, NULL, worker_entry, (void *)(intptr_t)number) != 0)
			break;
		pthread_detach(thread);
#endif
		pool.count++;
	}
}

int s32_workers_count(void)
{
	return pool.count;
}

void s32_workers_rows_by(s32_rows_work work, void *context, int first, int last, int step)
{
	if (pool.count == 0)
	{
		work(context, first, last);
		return;
	}
	LOCK(&pool.lock);
	while (pool.next <= pool.last || pool.taking > 0)
		WAIT_WHILE(pool.next <= pool.last || pool.taking > 0);    /* (one job at a time: another thread's is let finish first) */
	pool.work = work;
	pool.context = context;
	pool.next = first;
	pool.last = last;
	pool.step = step > 0 ? step : 1;
	WAKE_ALL(&pool.changed);
	while (take_rows())
		;
	while (pool.taking > 0)
		WAIT_WHILE(pool.taking > 0 && !work_waiting());
	UNLOCK(&pool.lock);
}

void s32_workers_rows(s32_rows_work work, void *context, int first, int last)
{
	/* two bands for each thread, as even as they come: the rows do not all cost the same (the sprites are
	   where the road is), and a thread that ends a light band takes another instead of waiting beside a
	   heavy one */
	const int bands = BANDS_A_THREAD * (pool.count + 1);

	s32_workers_rows_by(work, context, first, last, (last - first + bands) / bands);
}

unsigned s32_workers_queue(s32_task_work work, void *context)
{
	unsigned number;

	if (pool.count == 0)
	{
		if (pool.queued - pool.done == TASKS_MOST)
			s32_workers_finish(pool.done + 1);
		number = ++pool.queued;
		pool.tasks[number % TASKS_MOST].work = work;
		pool.tasks[number % TASKS_MOST].context = context;
		return number;
	}
	LOCK(&pool.lock);
	while (pool.queued - pool.done == TASKS_MOST)
		if (!take_rows() && !take_task())
			WAIT_WHILE(pool.queued - pool.done == TASKS_MOST);
	number = ++pool.queued;
	pool.tasks[number % TASKS_MOST].work = work;
	pool.tasks[number % TASKS_MOST].context = context;
	WAKE_ALL(&pool.changed);
	UNLOCK(&pool.lock);
	return number;
}

void s32_workers_finish(unsigned number)
{
	if (pool.count == 0)
	{
		/* (no threads: the caller does them, in their order) */
		while ((int)(pool.done - number) < 0 && pool.done != pool.queued)
		{
			const unsigned next = pool.done + 1;

			pool.started = next;
			pool.tasks[next % TASKS_MOST].work(pool.tasks[next % TASKS_MOST].context);
			pool.done = next;
		}
		return;
	}
	LOCK(&pool.lock);
	{
		const time_t since = time(NULL);
		bool told = false;

		while ((int)(pool.done - number) < 0 && pool.done != pool.queued)
			if (!take_rows() && !take_task())
			{
				WAIT_WHILE((int)(pool.done - number) < 0 && pool.done != pool.queued);
				/* (seconds of this are a game stuck: what the pool holds goes to the log, once) */
				if (!told && time(NULL) - since >= 3)
				{
					told = true;
					fprintf(stderr, "workers: waited seconds for task %u: queued %u, started %u, done %u, rows %d to %d"
						" (step %d), %d threads at rows, %d threads\n", number, pool.queued, pool.started, pool.done,
						pool.next, pool.last, pool.step, pool.taking, pool.count);
					fflush(stderr);
				}
			}
	}
	UNLOCK(&pool.lock);
}

void s32_workers_finish_all(void)
{
	s32_workers_finish(pool.queued);    /* (only the caller queues: the last one queued is the last there is) */
}
