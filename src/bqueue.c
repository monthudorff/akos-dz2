/* Ограниченная очередь с приоритетом (см. bqueue.h). */
#include "bqueue.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>

int bqueue_init(bqueue_t *q, const char *name, int cap)
{
    q->name = name;
    q->cap = cap;
    q->head = 0;
    q->count = 0;
    q->buf = malloc(sizeof *q->buf * (size_t)cap);
    if (q->buf == NULL) {
        sys_fail("malloc", errno);
        return -1;
    }
    q->free_sem = ksem_create((unsigned)cap);
    q->items_sem = ksem_create(0);
    if (q->free_sem == NULL || q->items_sem == NULL || mx_init(&q->mx) != 0) {
        ksem_destroy(q->free_sem);
        ksem_destroy(q->items_sem);
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    return 0;
}

void bqueue_destroy(bqueue_t *q)
{
    if (q->buf == NULL)
        return;
    ksem_destroy(q->free_sem);
    ksem_destroy(q->items_sem);
    mx_destroy(&q->mx);
    free(q->buf);
    q->buf = NULL;
}

/* Положить в хвост; место уже занято через free_sem. */
static void push(bqueue_t *q, void *item, long key)
{
    int i;

    mx_lock(&q->mx);
    CHECK(q->count < q->cap, "в очередь кладут больше элементов, чем её ёмкость");
    if (q->count < q->cap) {
        i = (q->head + q->count) % q->cap;
        q->buf[i].item = item;
        q->buf[i].key = key;
        q->count++;
    }
    mx_unlock(&q->mx);
}

/* Забрать элемент с наименьшим ключом; элемент уже занят через items_sem. */
static void *pop(bqueue_t *q)
{
    void *item = NULL;
    int i, best = 0;

    mx_lock(&q->mx);
    CHECK(q->count > 0, "из пустой очереди забирают элемент");
    if (q->count > 0) {
        for (i = 1; i < q->count; i++)
            if (q->buf[(q->head + i) % q->cap].key
                < q->buf[(q->head + best) % q->cap].key)
                best = i;
        item = q->buf[(q->head + best) % q->cap].item;
        /* элементы перед найденным сдвигаем на место, порядок сохраняется */
        for (i = best; i > 0; i--)
            q->buf[(q->head + i) % q->cap] = q->buf[(q->head + i - 1) % q->cap];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    mx_unlock(&q->mx);
    return item;
}

int bqueue_put(bqueue_t *q, void *item, long key)
{
    if (ksem_wait(q->free_sem) < 0)
        return -1;
    push(q, item, key);
    return ksem_post(q->items_sem);
}

int bqueue_tryput(bqueue_t *q, void *item, long key)
{
    if (ksem_trywait(q->free_sem) < 0)
        return -1;
    push(q, item, key);
    return ksem_post(q->items_sem);
}

void *bqueue_get(bqueue_t *q)
{
    void *item;

    if (ksem_wait(q->items_sem) < 0)
        return NULL;
    item = pop(q);
    ksem_post(q->free_sem);
    return item;
}

int bqueue_tryget(bqueue_t *q, void **item)
{
    if (ksem_trywait(q->items_sem) < 0)
        return -1;
    *item = pop(q);
    return ksem_post(q->free_sem);
}

int bqueue_count(bqueue_t *q)
{
    int n;

    mx_lock(&q->mx);
    n = q->count;
    mx_unlock(&q->mx);
    return n;
}
