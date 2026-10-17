/* Участники модели ресторана и общие для них данные. */
#ifndef KITCHEN_H
#define KITCHEN_H

#include <pthread.h>
#include <stddef.h>
#include "bqueue.h"
#include "config.h"
#include "sync.h"

enum { OP_WAIT, OP_QUEUED, OP_COOKING, OP_DONE };     /* состояние операции */

enum {                                                /* состояние заказа */
    ORD_NEW,          /* посетитель ждёт официанта */
    ORD_KITCHEN,      /* передан на кухню */
    ORD_READY,        /* все блюда готовы, ждёт выдачи */
    ORD_CANCELLED,    /* отменён координатором, ждёт официанта */
    ORD_SERVED,       /* выдан посетителю */
    ORD_REJECTED      /* посетителю сообщили об отмене */
};

enum { OUT_NONE, OUT_LEFT, OUT_SERVED, OUT_CANCELLED, OUT_ABORTED };   /* итог посетителя */

struct order;
struct customer;
struct restaurant;

/* Блюдо в заказе. Им владеет тот, у кого оно сейчас в руках:
 * координатор, очередь или повар, поэтому отдельный мьютекс не нужен. */
typedef struct dish {
    const dish_def_t *def;
    struct order *order;
    int cur;                         /* текущая операция цепочки */
    int op_state[MAX_OPS];
} dish_t;

typedef struct order {
    int id;                          /* номер заказа = номер посетителя */
    int vip;
    int ndishes;
    dish_t dishes[MAX_ORDER_DISHES];
    int dishes_ready;                /* собрано готовых блюд */
    int status;
    struct customer *owner;
} order_t;

typedef struct customer {
    int id;
    struct restaurant *r;
    pthread_t tid;
    ksem_t *served;                  /* личный семафор "заказ принесли" */
    order_t order;
    double t_arrive;
    unsigned seed;                   /* свой seed для rand_r */
    int finished, outcome;           /* под r->mx */
    struct customer *next;           /* список посетителей в main */
} customer_t;

/* Оборудование одного типа: счётный семафор + массив занятости. */
typedef struct pool {
    int type, count;
    ksem_t *free;                    /* свободные единицы */
    pthread_mutex_t mx;
    int busy_by[MAX_UNITS];          /* номер повара или 0, под mx */
    double since[MAX_UNITS];         /* когда заняли, под mx */
    double busy_total;               /* суммарное время занятости, под mx */
    int in_use;                      /* под mx */
} pool_t;

typedef struct stats {
    int arrived, left, seated;       /* пришли, ушли без места, сели */
    int accepted, served, cancelled; /* заказы */
    int waited;                      /* сколько ожиданий учтено */
    double wait_sum, wait_max;
    int ops_done, dishes_done;
    int hall_used, slots_used, cooks_busy;   /* текущая занятость для проверок */
} stats_t;

/*
 * Всё общее для потоков. Передаётся в потоки указателем через аргумент.
 * waiter_q - задачи официантов: посетители из зала и заказы на выдачу.
 */
typedef struct restaurant {
    bqueue_t waiter_q;               /* зал + выдача, выдача приоритетнее */
    bqueue_t orders;                 /* очередь заказов на кухню */
    bqueue_t ops;                    /* очередь операций для поваров */
    bqueue_t reports;                /* отчёты поваров координатору */
    ksem_t *hall_seats;              /* свободные места в зале */
    ksem_t *ready_slots;             /* свободные слоты зоны готовых блюд */
    ksem_t *bell;                    /* "у координатора есть работа" */
    pool_t eq[EQ_COUNT];
    pthread_mutex_t mx;              /* защищает st и finished/outcome посетителей */
    stats_t st;
} restaurant_t;

typedef struct worker_arg {
    int id;
    restaurant_t *r;
} worker_arg_t;

void *customer_thread(void *arg);
void *waiter_thread(void *arg);
void *coordinator_thread(void *arg);
void *cook_thread(void *arg);

int    pool_init(pool_t *p, int type, int count);
void   pool_destroy(pool_t *p);
int    pool_acquire(pool_t *p, int cook, const char *who);  /* номер 1.. или -1 */
void   pool_release(pool_t *p, int unit, int cook);
int    pool_in_use(pool_t *p);
double pool_busy_time(pool_t *p);

long order_key(const order_t *o);    /* меньше - раньше */
/* "steak, pizza" */
void dishes_str(const order_t *o, char *buf, size_t size);
/* "нарезка (стол, 1) → жарка (гриль, 4)" */
void dish_chain_str(const dish_def_t *d, char *buf, size_t size);

#endif
