/*
 * Реализация sync.h на семафорах POSIX (sem_t).
 * Для аварийной остановки все семафоры хранятся в общем списке, и у каждого
 * считается число потоков внутри ожидания: при остановке каждый семафор
 * поднимается столько раз, сколько на нём ждут.
 */
#include "sync.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct ksem {
    sem_t sem;
    atomic_int waiting;          /* сколько потоков сейчас ждут на семафоре */
    struct ksem *prev, *next;    /* список всех семафоров (под reg_mx) */
};

static atomic_int aborted;                          /* флаг аварийной остановки */
static pthread_mutex_t reg_mx = PTHREAD_MUTEX_INITIALIZER;
static struct ksem *reg_head;                       /* список семафоров, под reg_mx */
static ksem_t *sleeper;                             /* на нём спим с тайм-аутом */

const char *sync_name(void)
{
    return "мьютексы и семафоры";
}

static void reg_add(ksem_t *s)
{
    mx_lock(&reg_mx);
    s->prev = NULL;
    s->next = reg_head;
    if (reg_head)
        reg_head->prev = s;
    reg_head = s;
    mx_unlock(&reg_mx);
}

static void reg_del(ksem_t *s)
{
    mx_lock(&reg_mx);
    if (s->prev)
        s->prev->next = s->next;
    else
        reg_head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    mx_unlock(&reg_mx);
}

ksem_t *ksem_create(unsigned value)
{
    ksem_t *s = malloc(sizeof *s);

    if (s == NULL) {
        sys_fail("malloc", errno);
        return NULL;
    }
    if (sem_init(&s->sem, 0, value) != 0) {
        sys_fail("sem_init", errno);
        free(s);
        return NULL;
    }
    atomic_init(&s->waiting, 0);
    reg_add(s);
    return s;
}

void ksem_destroy(ksem_t *s)
{
    if (s == NULL)
        return;
    reg_del(s);
    if (sem_destroy(&s->sem) != 0)
        sys_fail("sem_destroy", errno);
    free(s);
}

int ksem_wait(ksem_t *s)
{
    int rc = 0;

    /* сначала отмечаемся как ждущий, потом смотрим флаг: так sync_abort
     * либо увидит нас в счётчике, либо мы увидим флаг */
    atomic_fetch_add(&s->waiting, 1);
    if (atomic_load(&aborted)) {
        rc = -1;
    } else {
        while (sem_wait(&s->sem) != 0) {
            if (errno != EINTR) {
                sys_fail("sem_wait", errno);
                rc = -1;
                break;
            }
        }
        if (atomic_load(&aborted))
            rc = -1;
    }
    atomic_fetch_sub(&s->waiting, 1);
    return rc;
}

int ksem_trywait(ksem_t *s)
{
    if (atomic_load(&aborted))
        return -1;
    while (sem_trywait(&s->sem) != 0) {
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            sys_fail("sem_trywait", errno);
        return -1;
    }
    return 0;
}

int ksem_post(ksem_t *s)
{
    if (sem_post(&s->sem) != 0) {
        sys_fail("sem_post", errno);
        return -1;
    }
    return 0;
}

int sync_sleep(long ms)
{
    struct timespec ts;
    int rc = 0;

    if (ms <= 0)
        return atomic_load(&aborted) ? -1 : 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        sys_fail("clock_gettime", errno);
        return -1;
    }
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    /* семафор sleeper поднимает только sync_abort, обычно ждём до тайм-аута */
    atomic_fetch_add(&sleeper->waiting, 1);
    if (atomic_load(&aborted)) {
        rc = -1;
    } else {
        while (sem_timedwait(&sleeper->sem, &ts) != 0) {
            if (errno == EINTR)
                continue;
            if (errno != ETIMEDOUT) {
                sys_fail("sem_timedwait", errno);
                rc = -1;
            }
            break;
        }
        if (atomic_load(&aborted))
            rc = -1;
    }
    atomic_fetch_sub(&sleeper->waiting, 1);
    return rc;
}

void sync_abort(void)
{
    ksem_t *s;
    int i, n, rc;

    if (atomic_exchange(&aborted, 1))
        return;                         /* уже остановлены */
    if ((rc = pthread_mutex_lock(&reg_mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return;
    }
    for (s = reg_head; s != NULL; s = s->next) {
        n = atomic_load(&s->waiting);
        for (i = 0; i < n; i++) {
            if (sem_post(&s->sem) != 0) {
                sys_fail("sem_post", errno);
                break;
            }
        }
    }
    if ((rc = pthread_mutex_unlock(&reg_mx)) != 0)
        sys_fail("pthread_mutex_unlock", rc);
}

int sync_aborted(void)
{
    return atomic_load(&aborted);
}

int sync_init(void)
{
    sleeper = ksem_create(0);
    return sleeper ? 0 : -1;
}

void sync_done(void)
{
    ksem_destroy(sleeper);
    sleeper = NULL;
}
