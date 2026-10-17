/*
 * Ограниченная очередь (кольцевой буфер) для обмена между потоками.
 * Буфер под мьютексом, ожидание места и элементов - на семафорах free/items.
 * У элемента есть ключ приоритета: get отдаёт элемент с наименьшим ключом,
 * при равных ключах - самый ранний (FIFO). NULL в очереди - сигнал "стоп".
 */
#ifndef BQUEUE_H
#define BQUEUE_H

#include <pthread.h>
#include "sync.h"

typedef struct {
    void *item;
    long key;
} bq_slot_t;

typedef struct {
    const char *name;
    pthread_mutex_t mx;
    ksem_t *free_sem;            /* свободные места */
    ksem_t *items_sem;           /* занятые места */
    bq_slot_t *buf;              /* кольцевой буфер, под mx */
    int cap, head, count;        /* под mx */
} bqueue_t;

int   bqueue_init(bqueue_t *q, const char *name, int cap);   /* 0 / -1 */
void  bqueue_destroy(bqueue_t *q);
int   bqueue_put(bqueue_t *q, void *item, long key);    /* 0, -1 - остановка */
int   bqueue_tryput(bqueue_t *q, void *item, long key); /* 0, -1 - полна или остановка */
void *bqueue_get(bqueue_t *q);                          /* NULL - стоп или остановка */
int   bqueue_tryget(bqueue_t *q, void **item);          /* 0, -1 - пусто или остановка */
int   bqueue_count(bqueue_t *q);

#endif
