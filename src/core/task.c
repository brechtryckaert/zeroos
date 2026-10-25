#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "lvgl.h"
#include "task.h"

#define WORKERS 2

typedef struct job {
    const void *owner;
    zos_task_fn work, done, release;
    void *arg;
    bool cancelled;
    struct job *next;
} job_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static job_t *todo_head, *todo_tail, *done_head, *done_tail, *running[WORKERS];

static void push(job_t **h, job_t **t, job_t *j)
{
    j->next = NULL;
    if (*t) (*t)->next = j;
    else *h = j;
    *t = j;
}

static void *worker(void *idx_p)
{
    int idx = (int)(intptr_t)idx_p;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!todo_head) pthread_cond_wait(&cv, &mu);
        job_t *j = todo_head;
        todo_head = j->next;
        if (!todo_head) todo_tail = NULL;
        running[idx] = j;
        bool skip = j->cancelled;
        pthread_mutex_unlock(&mu);

        if (!skip && j->work) j->work(j->arg);

        pthread_mutex_lock(&mu);
        running[idx] = NULL;
        push(&done_head, &done_tail, j);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void drain(lv_timer_t *t)
{
    LV_UNUSED(t);
    pthread_mutex_lock(&mu);
    job_t *list = done_head;
    done_head = done_tail = NULL;
    pthread_mutex_unlock(&mu);
    while (list) {
        job_t *j = list;
        list = j->next;
        if (!j->cancelled && j->done) j->done(j->arg);
        if (j->release) j->release(j->arg);
        free(j);
    }
}

void zos_task_init(void)
{
    static bool started;
    if (started) return;
    started = true;
    for (int i = 0; i < WORKERS; i++) {
        pthread_t th;
        pthread_create(&th, NULL, worker, (void *)(intptr_t)i);
        pthread_detach(th);
    }
    lv_timer_create(drain, 15, NULL);
}

void zos_task_run(const void *owner, zos_task_fn work, zos_task_fn done, zos_task_fn release, void *arg)
{
    job_t *j = calloc(1, sizeof *j);
    j->owner = owner;
    j->work = work;
    j->done = done;
    j->release = release;
    j->arg = arg;
    pthread_mutex_lock(&mu);
    push(&todo_head, &todo_tail, j);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void zos_task_cancel(const void *owner)
{
    pthread_mutex_lock(&mu);
    for (job_t *j = todo_head; j; j = j->next)
        if (j->owner == owner) j->cancelled = true;
    for (job_t *j = done_head; j; j = j->next)
        if (j->owner == owner) j->cancelled = true;
    for (int i = 0; i < WORKERS; i++)
        if (running[i] && running[i]->owner == owner) running[i]->cancelled = true;
    pthread_mutex_unlock(&mu);
}
