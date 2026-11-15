/**
 *    Name:        workers.h
 *    Description: the frame's rows shared among threads: the video draws each layer, and mixes them, a band
 *                 of rows on each, the same pixels one thread draws
 *    Version:     0.2.0
 *    Created:     23-10-2026
 *    Modified:    14-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    A port starts the threads its machine has room for (a handheld's cores); with none, the default, the
 *    caller draws every row, as the checks do. The rows of a frame do not depend on each other: each band
 *    reads what the layers and the board hold and writes its own rows.
 *
 *    The threads also take tasks: work queued for one of them to do while the caller goes on with its own
 *    (the video draws a frame so while the game runs the next one). One caller at a time uses all this: the
 *    game's thread, or the task being done, which shares rows out as the caller does.
 */
#ifndef S32_WORKERS_H
#define S32_WORKERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The work for rows 'first' to 'last' (inclusive) */
typedef void (*s32_rows_work)(void *context, int first, int last);

/* Starts 'count' threads beside the caller's (at most 7; 0 or less: none) */
void s32_workers_start(int count);

/* The threads started */
int s32_workers_count(void);

/* 'work' over rows 'first' to 'last', in bands (two for each thread and for the caller, as even as they
   come: a thread takes the next band as it ends its own). The caller returns when every band is done */
void s32_workers_rows(s32_rows_work work, void *context, int first, int last);

/* The same, 'step' rows at a time: each thread takes the next ones as it ends its own, so rows that take
   longer, or a thread that comes late or is kept busy, hold nobody up. For work that costs nothing to start */
void s32_workers_rows_by(s32_rows_work work, void *context, int first, int last, int step);

/* A task: work for one thread */
typedef void (*s32_task_work)(void *context);

/* Queues a task and returns at once with its number. The tasks are done one at a time in the order they were
   queued, each by a thread with nothing else to do; with no threads they wait for s32_workers_finish. While
   tasks are queued the caller shares no rows out itself: it finishes them first */
unsigned s32_workers_queue(s32_task_work work, void *context);

/* Returns when the task of that number is done, and those queued before it. Until then the caller is one more
   thread: it takes rows of the work being done, and the tasks no thread has taken */
void s32_workers_finish(unsigned number);

/* Returns when every task queued is done */
void s32_workers_finish_all(void);

#ifdef __cplusplus
}
#endif

#endif
