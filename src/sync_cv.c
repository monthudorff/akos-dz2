/*
 * Реализация sync.h на мьютексах и условных переменных.
 * Каждый семафор модели - счётчик под своим мьютексом и условная переменная,
 * ожидание всегда в цикле while (защита от ложных пробуждений).
 * Для аварийной остановки все объекты лежат в общем списке, sync_abort
 * делает broadcast каждому.
 */
#include "sync.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

struct ksem {
    pthread_mutex_t mx;
    pthread_cond_t cv;
    unsigned value;              /* счётчик, под mx */
    struct ksem *prev, *next;    /* список всех объектов (под reg_mx) */
};

static atomic_int aborted;                          /* флаг аварийной остановки */
static pthread_mutex_t reg_mx = PTHREAD_MUTEX_INITIALIZER;
static struct ksem *reg_head;                       /* список объектов, под reg_mx */
static ksem_t *sleeper;                             /* на нём спим с тайм-аутом */

const char *sync_name(void)
{
    return "мьютексы и условные переменные";
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
    pthread_condattr_t ca;
    ksem_t *s = malloc(sizeof *s);
    int rc, rc2;

    if (s == NULL) {
        sys_fail("malloc", errno);
        return NULL;
    }
    if ((rc = pthread_mutex_init(&s->mx, NULL)) != 0) {
        sys_fail("pthread_mutex_init", rc);
        free(s);
        return NULL;
    }
    /* тайм-ауты считаем по монотонным часам */
    if ((rc = pthread_condattr_init(&ca)) == 0) {
        if ((rc = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC)) == 0)
            rc = pthread_cond_init(&s->cv, &ca);
        if ((rc2 = pthread_condattr_destroy(&ca)) != 0)
            sys_fail("pthread_condattr_destroy", rc2);
    }
    if (rc != 0) {
        sys_fail("pthread_cond_init", rc);
        pthread_mutex_destroy(&s->mx);
        free(s);
        return NULL;
    }
    s->value = value;
    reg_add(s);
    return s;
}

void ksem_destroy(ksem_t *s)
{
    int rc;

    if (s == NULL)
        return;
    reg_del(s);
    if ((rc = pthread_cond_destroy(&s->cv)) != 0)
        sys_fail("pthread_cond_destroy", rc);
    if ((rc = pthread_mutex_destroy(&s->mx)) != 0)
        sys_fail("pthread_mutex_destroy", rc);
    free(s);
}

int ksem_wait(ksem_t *s)
{
    int rc, err = 0, ok;

    if ((rc = pthread_mutex_lock(&s->mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return -1;
    }
    while (s->value == 0 && !atomic_load(&aborted)) {
        if ((rc = pthread_cond_wait(&s->cv, &s->mx)) != 0) {
            err = rc;
            break;
        }
    }
    ok = !err && !atomic_load(&aborted) && s->value > 0;
    if (ok)
        s->value--;
    if ((rc = pthread_mutex_unlock(&s->mx)) != 0)
        sys_fail("pthread_mutex_unlock", rc);
    /* об ошибке сообщаем вне мьютекса: sys_fail вызывает sync_abort */
    if (err)
        sys_fail("pthread_cond_wait", err);
    return ok ? 0 : -1;
}

int ksem_trywait(ksem_t *s)
{
    int rc, ok;

    if ((rc = pthread_mutex_lock(&s->mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return -1;
    }
    ok = !atomic_load(&aborted) && s->value > 0;
    if (ok)
        s->value--;
    if ((rc = pthread_mutex_unlock(&s->mx)) != 0)
        sys_fail("pthread_mutex_unlock", rc);
    return ok ? 0 : -1;
}

int ksem_post(ksem_t *s)
{
    int rc, err;

    if ((rc = pthread_mutex_lock(&s->mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return -1;
    }
    s->value++;
    err = pthread_cond_signal(&s->cv);
    if ((rc = pthread_mutex_unlock(&s->mx)) != 0)
        sys_fail("pthread_mutex_unlock", rc);
    if (err) {
        sys_fail("pthread_cond_signal", err);
        return -1;
    }
    return 0;
}

int sync_sleep(long ms)
{
    struct timespec ts;
    int rc, err = 0, res;

    if (ms <= 0)
        return atomic_load(&aborted) ? -1 : 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        sys_fail("clock_gettime", errno);
        return -1;
    }
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    if ((rc = pthread_mutex_lock(&sleeper->mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return -1;
    }
    /* ждём до тайм-аута; ложные пробуждения пропускаем, будит только остановка */
    while (!atomic_load(&aborted)) {
        rc = pthread_cond_timedwait(&sleeper->cv, &sleeper->mx, &ts);
        if (rc == ETIMEDOUT)
            break;
        if (rc != 0) {
            err = rc;
            break;
        }
    }
    res = (err || atomic_load(&aborted)) ? -1 : 0;
    if ((rc = pthread_mutex_unlock(&sleeper->mx)) != 0)
        sys_fail("pthread_mutex_unlock", rc);
    if (err)
        sys_fail("pthread_cond_timedwait", err);
    return res;
}

void sync_abort(void)
{
    ksem_t *s;
    int rc;

    if (atomic_exchange(&aborted, 1))
        return;                         /* уже остановлены */
    if ((rc = pthread_mutex_lock(&reg_mx)) != 0) {
        sys_fail("pthread_mutex_lock", rc);
        return;
    }
    /* флаг выставлен до захвата мьютекса объекта, поэтому ждущий либо
     * увидит флаг до cond_wait, либо получит broadcast */
    for (s = reg_head; s != NULL; s = s->next) {
        if ((rc = pthread_mutex_lock(&s->mx)) != 0) {
            sys_fail("pthread_mutex_lock", rc);
            continue;
        }
        if ((rc = pthread_cond_broadcast(&s->cv)) != 0)
            sys_fail("pthread_cond_broadcast", rc);
        if ((rc = pthread_mutex_unlock(&s->mx)) != 0)
            sys_fail("pthread_mutex_unlock", rc);
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
