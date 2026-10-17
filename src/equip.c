/*
 * Оборудование кухни. Счётный семафор ограничивает число поваров,
 * одновременно работающих на оборудовании типа, а массив занятости под
 * мьютексом говорит, какая именно единица свободна (номер печатаем в лог).
 */
#include "kitchen.h"
#include "log.h"

int pool_init(pool_t *p, int type, int count)
{
    int i;

    p->type = type;
    p->count = count;
    p->busy_total = 0.0;
    p->in_use = 0;
    for (i = 0; i < MAX_UNITS; i++) {
        p->busy_by[i] = 0;
        p->since[i] = 0.0;
    }
    p->free = ksem_create((unsigned)count);
    if (p->free == NULL)
        return -1;
    if (mx_init(&p->mx) != 0) {
        ksem_destroy(p->free);
        p->free = NULL;
        return -1;
    }
    return 0;
}

void pool_destroy(pool_t *p)
{
    if (p->free == NULL)
        return;
    ksem_destroy(p->free);
    mx_destroy(&p->mx);
    p->free = NULL;
}

int pool_acquire(pool_t *p, int cook, const char *who)
{
    int i, unit = -1;

    CHECK(p->count > 0, "запрошено оборудование, которого на кухне нет");
    if (ksem_trywait(p->free) != 0) {
        if (sync_aborted())
            return -1;
        log_event(who, "ждёт %s: свободных нет (всего %d)", EQ_ACC[p->type], p->count);
        if (ksem_wait(p->free) != 0)
            return -1;
    }
    mx_lock(&p->mx);
    for (i = 0; i < p->count; i++) {
        if (p->busy_by[i] == 0) {
            unit = i;
            break;
        }
    }
    CHECK(unit >= 0, "семафор оборудования пропустил, а свободной единицы нет");
    if (unit >= 0) {
        p->busy_by[unit] = cook;
        p->since[unit] = model_now();
        p->in_use++;
        CHECK(p->in_use <= p->count, "занято больше единиц оборудования, чем есть");
    }
    mx_unlock(&p->mx);
    return unit >= 0 ? unit + 1 : -1;
}

void pool_release(pool_t *p, int unit, int cook)
{
    int i = unit - 1;

    mx_lock(&p->mx);
    CHECK(p->busy_by[i] == cook, "оборудование освобождает не тот повар, что занял");
    p->busy_by[i] = 0;
    p->in_use--;
    p->busy_total += model_now() - p->since[i];
    mx_unlock(&p->mx);
    ksem_post(p->free);
}

int pool_in_use(pool_t *p)
{
    int n;

    mx_lock(&p->mx);
    n = p->in_use;
    mx_unlock(&p->mx);
    return n;
}

double pool_busy_time(pool_t *p)
{
    double t;

    mx_lock(&p->mx);
    t = p->busy_total;
    mx_unlock(&p->mx);
    return t;
}
