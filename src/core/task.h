/* Background work for the UI thread.
 *
 * LVGL is single-threaded, so anything slow (HTTP, ssh-keygen, image
 * downloads) runs `work` on a small worker pool and `done` back on the UI
 * thread, from an LVGL timer. Jobs belong to an `owner` (usually the app's
 * state); cancelling the owner drops pending `done` calls, so an app that
 * closes mid-request never gets called back into freed memory. */
#ifndef ZOS_TASK_H
#define ZOS_TASK_H

typedef void (*zos_task_fn)(void *arg);

void zos_task_init(void);
/* `done` and `release` may be NULL. `release` always runs (on the UI thread),
 * whether or not the job was cancelled; use it to free `arg`. */
void zos_task_run(const void *owner, zos_task_fn work, zos_task_fn done, zos_task_fn release, void *arg);
void zos_task_cancel(const void *owner);

#endif
