/*
 * Потоки-участники модели: посетитель, официант, координатор кухни, повар.
 * Путь заказа: посетитель -> очередь официантов -> официант -> очередь
 * заказов -> координатор -> очередь операций -> повар -> очередь отчётов ->
 * координатор -> (все блюда готовы) очередь официантов -> официант ->
 * личный семафор посетителя.
 */
#include "kitchen.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>

#define WAITER_TIME 1               /* единиц на приём заказа и на вынос */

static const char COORD[] = "Координатор";

/* Имитация длительности в единицах модельного времени. */
static int nap(int units)
{
    return sync_sleep((long)units * g_cfg.unit_ms);
}

long order_key(const order_t *o)
{
    switch (g_cfg.priority) {
    case PRIO_VIP:
        return o->vip ? 0 : 1;
    case PRIO_SHORT:
        return o->ndishes;          /* меньший заказ раньше */
    default:
        return 0;                   /* fifo: равные ключи, порядок поступления */
    }
}

void dishes_str(const order_t *o, char *buf, size_t size)
{
    size_t len = 0;
    int i, n;

    buf[0] = '\0';
    for (i = 0; i < o->ndishes; i++) {
        n = snprintf(buf + len, size - len, "%s%s", i ? ", " : "",
                     o->dishes[i].def->name);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
    }
}

void dish_chain_str(const dish_def_t *d, char *buf, size_t size)
{
    size_t len = 0;
    int i, n;

    buf[0] = '\0';
    for (i = 0; i < d->nops; i++) {
        n = snprintf(buf + len, size - len, "%s%s (%s, %d)", i ? " → " : "",
                     OP_NAME[d->ops[i].eq], EQ_NAME[d->ops[i].eq], d->ops[i].dur);
        if (n < 0 || (size_t)n >= size - len)
            break;
        len += (size_t)n;
    }
}

/* ---------------------------------------------------------------- посетитель */

void *customer_thread(void *arg)
{
    customer_t *c = arg;
    restaurant_t *r = c->r;
    order_t *o = &c->order;
    char who[32];
    int outcome, n, eat;
    double wait;

    snprintf(who, sizeof who, "Посетитель %d", c->id);
    mx_lock(&r->mx);
    r->st.arrived++;
    mx_unlock(&r->mx);

    /* место в зале: если всё занято, посетитель уходит */
    if (ksem_trywait(r->hall_seats) != 0) {
        mx_lock(&r->mx);
        r->st.left++;
        mx_unlock(&r->mx);
        log_event(who, "свободных мест в зале нет, ушёл");
        outcome = OUT_LEFT;
        goto out;
    }
    mx_lock(&r->mx);
    n = ++r->st.hall_used;
    r->st.seated++;
    CHECK(n <= g_cfg.hall_queue, "в зале больше посетителей, чем мест");
    mx_unlock(&r->mx);
    log_event(who, "сел в зал (занято мест %d из %d), ждёт официанта", n, g_cfg.hall_queue);

    /* в очереди официантов у каждого сидящего не больше одной записи,
     * её ёмкость = число мест, поэтому put не блокируется */
    if (bqueue_put(&r->waiter_q, o, 1 + order_key(o)) != 0 || ksem_wait(c->served) != 0) {
        /* экстренная остановка: заказ мог остаться на кухне, его не читаем */
        log_event(who, "ушёл, не дождавшись заказа: экстренное закрытие");
        outcome = OUT_ABORTED;
    } else if (o->status == ORD_REJECTED) {
        CHECK(o->owner == c, "посетителю сообщили об отмене чужого заказа");
        log_event(who, "узнал, что заказ %d отменён, ушёл", o->id);
        outcome = OUT_CANCELLED;
    } else {
        CHECK(o->status == ORD_SERVED, "посетитель получил заказ, который ему не выдавали");
        CHECK(o->owner == c, "посетитель получил чужой заказ");
        CHECK(o->dishes_ready == o->ndishes, "посетитель получил заказ, где готовы не все блюда");
        wait = model_now() - c->t_arrive;
        mx_lock(&r->mx);
        r->st.waited++;
        r->st.wait_sum += wait;
        if (wait > r->st.wait_max)
            r->st.wait_max = wait;
        mx_unlock(&r->mx);
        eat = g_cfg.eat_min
            + (int)((unsigned)rand_r(&c->seed) % (unsigned)(g_cfg.eat_max - g_cfg.eat_min + 1));
        log_event(who, "получил свой заказ %d (ждал %.2f), ест", o->id, wait);
        if (nap(eat) == 0)
            log_event(who, "поел и ушёл");
        else
            log_event(who, "не доел: экстренное закрытие");
        outcome = OUT_SERVED;
    }
    mx_lock(&r->mx);
    r->st.hall_used--;
    mx_unlock(&r->mx);
    ksem_post(r->hall_seats);
out:
    mx_lock(&r->mx);
    c->outcome = outcome;
    c->finished = 1;
    mx_unlock(&r->mx);
    return NULL;
}

/* ------------------------------------------------------------------ официант */

void *waiter_thread(void *arg)
{
    worker_arg_t *a = arg;
    restaurant_t *r = a->r;
    char who[32], dishes[256];
    order_t *o;
    customer_t *c;
    int ok;

    snprintf(who, sizeof who, "Официант %d", a->id);
    /* выдача имеет ключ 0, поэтому готовые заказы уносят раньше новых */
    while ((o = bqueue_get(&r->waiter_q)) != NULL) {
        c = o->owner;
        if (o->status == ORD_NEW) {
            dishes_str(o, dishes, sizeof dishes);
            log_event(who, "принимает заказ %d у посетителя %d%s: %s",
                      o->id, c->id, o->vip ? " (VIP)" : "", dishes);
            if (nap(WAITER_TIME) != 0)
                break;
            mx_lock(&r->mx);
            r->st.accepted++;
            mx_unlock(&r->mx);
            o->status = ORD_KITCHEN;
            log_event(who, "передал заказ %d на кухню", o->id);
            /* ёмкость очереди заказов = числу мест в зале, put не блокируется */
            if (bqueue_put(&r->orders, o, order_key(o)) != 0 || ksem_post(r->bell) != 0)
                break;
            continue;
        }

        ok = (o->status == ORD_READY);
        CHECK(ok || o->status == ORD_CANCELLED, "официанту дали заказ в неверном состоянии");
        CHECK(&c->order == o && c->id == o->id, "заказ несут не своему посетителю");
        if (ok) {
            CHECK(o->dishes_ready == o->ndishes, "на выдачу попал заказ, где готовы не все блюда");
            log_event(who, "несёт заказ %d посетителю %d", o->id, c->id);
            if (nap(WAITER_TIME) != 0)
                break;
        }
        mx_lock(&r->mx);
        if (ok)
            r->st.served++;
        else
            r->st.cancelled++;
        r->st.slots_used--;
        CHECK(r->st.slots_used >= 0, "освобождён лишний слот зоны выдачи");
        mx_unlock(&r->mx);
        if (ok)
            log_event(who, "выдал заказ %d посетителю %d, слот зоны выдачи свободен", o->id, c->id);
        else
            log_event(who, "сообщил посетителю %d, что заказ %d отменён", c->id, o->id);
        o->status = ok ? ORD_SERVED : ORD_REJECTED;
        /* слот освобождает официант; после post посетителю заказ не трогаем */
        if (ksem_post(r->ready_slots) != 0 || ksem_post(r->bell) != 0
            || ksem_post(c->served) != 0)
            break;
    }
    return NULL;
}

/* -------------------------------------------------------- координатор кухни */

/* Поставить текущую операцию блюда в очередь операций. */
static int enqueue_op(restaurant_t *r, dish_t *d)
{
    const op_def_t *op = &d->def->ops[d->cur];
    long key = order_key(d->order);

    CHECK(d->cur == 0 || d->op_state[d->cur - 1] == OP_DONE,
          "в очередь ставится операция до завершения предыдущей");
    d->op_state[d->cur] = OP_QUEUED;
    log_event(COORD, "в очередь операций: %s, %s (заказ %d)",
              OP_NAME[op->eq], d->def->name, d->order->id);
    if (bqueue_tryput(&r->ops, d, key) == 0)
        return 0;
    if (sync_aborted())
        return -1;
    /* ждать здесь безопасно: повара отчитываются, не блокируясь */
    log_event(COORD, "очередь операций заполнена (%d мест), ждёт", g_cfg.op_queue);
    return bqueue_put(&r->ops, d, key);
}

/* Слот зоны выдачи уже занят: проверить заказ и запустить первые операции. */
static int start_order(restaurant_t *r, order_t *o)
{
    char dishes[256], chain[256];
    int i, k, n;

    mx_lock(&r->mx);
    n = ++r->st.slots_used;
    CHECK(n <= g_cfg.ready_slots, "занято слотов зоны выдачи больше, чем есть");
    mx_unlock(&r->mx);
    CHECK(o->status == ORD_KITCHEN, "координатор взял заказ в неверном состоянии");
    dishes_str(o, dishes, sizeof dishes);
    log_event(COORD, "взял заказ %d в работу: %s (слот зоны выдачи %d из %d)",
              o->id, dishes, n, g_cfg.ready_slots);

    /* невыполнимый заказ: для какой-то операции нет оборудования */
    for (i = 0; i < o->ndishes; i++) {
        const dish_def_t *def = o->dishes[i].def;
        for (k = 0; k < def->nops; k++) {
            if (g_cfg.eq_count[def->ops[k].eq] == 0) {
                log_event(COORD, "заказ %d отменён: для блюда %s нужно оборудование «%s», а его нет",
                          o->id, def->name, EQ_NAME[def->ops[k].eq]);
                o->status = ORD_CANCELLED;
                return bqueue_put(&r->waiter_q, o, 0);
            }
        }
    }
    for (i = 0; i < o->ndishes; i++) {
        dish_t *d = &o->dishes[i];
        d->cur = 0;
        for (k = 0; k < d->def->nops; k++)
            d->op_state[k] = OP_WAIT;
        dish_chain_str(d->def, chain, sizeof chain);
        log_event(COORD, "заказ %d, блюдо %s: создал операции %s", o->id, d->def->name, chain);
    }
    /* в очередь сразу идут только первые операции блюд: у них нет предшественников */
    for (i = 0; i < o->ndishes; i++)
        if (enqueue_op(r, &o->dishes[i]) != 0)
            return -1;
    return 0;
}

/* Отчёт повара: двигаем цепочку блюда, собираем заказ. */
static int handle_report(restaurant_t *r, dish_t *d)
{
    order_t *o = d->order;
    const op_def_t *op = &d->def->ops[d->cur];
    int k;

    CHECK(d->op_state[d->cur] == OP_DONE, "отчёт о невыполненной операции");
    log_event(COORD, "отчёт: %s, %s (заказ %d) выполнена", OP_NAME[op->eq], d->def->name, o->id);
    d->cur++;
    if (d->cur < d->def->nops)
        return enqueue_op(r, d);

    for (k = 0; k < d->def->nops; k++)
        CHECK(d->op_state[k] == OP_DONE, "блюдо готово, а не все его операции выполнены");
    o->dishes_ready++;
    mx_lock(&r->mx);
    r->st.dishes_done++;
    mx_unlock(&r->mx);
    log_event(COORD, "блюдо %s (заказ %d) готово, в зоне выдачи %d из %d блюд заказа",
              d->def->name, o->id, o->dishes_ready, o->ndishes);
    if (o->dishes_ready < o->ndishes)
        return 0;
    CHECK(o->dishes_ready == o->ndishes, "в заказе готово больше блюд, чем заказано");
    o->status = ORD_READY;
    log_event(COORD, "заказ %d собран, ждёт официанта", o->id);
    return bqueue_put(&r->waiter_q, o, 0);
}

void *coordinator_thread(void *arg)
{
    worker_arg_t *a = arg;
    restaurant_t *r = a->r;
    int full_logged = 0;
    void *it;

    /*
     * Координатор ждёт сразу двух источников (заказы и отчёты) и свободного
     * слота, поэтому спит на одном семафоре-"звонке": его поднимает каждый,
     * кто кладёт заказ, отчёт или освобождает слот. Проснувшись, координатор
     * разбирает всё, что доступно, не блокируясь.
     */
    for (;;) {
        if (ksem_wait(r->bell) != 0)
            break;
        while (bqueue_tryget(&r->reports, &it) == 0) {
            if (it == NULL)
                return NULL;            /* стоп от main */
            if (handle_report(r, it) != 0)
                return NULL;
        }
        /* новый заказ берём в работу, только заняв слот в зоне выдачи */
        while (bqueue_count(&r->orders) > 0) {
            if (ksem_trywait(r->ready_slots) != 0) {
                if (!full_logged && !sync_aborted())
                    log_event(COORD, "зона выдачи занята (слотов %d), новые заказы ждут",
                              g_cfg.ready_slots);
                full_logged = 1;
                break;
            }
            full_logged = 0;
            if (bqueue_tryget(&r->orders, &it) != 0) {
                ksem_post(r->ready_slots);
                break;
            }
            if (start_order(r, it) != 0)
                return NULL;
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------- повар */

void *cook_thread(void *arg)
{
    worker_arg_t *a = arg;
    restaurant_t *r = a->r;
    char who[32];
    dish_t *d;
    int busy = 0, unit, n, rc;

    snprintf(who, sizeof who, "Повар %d", a->id);
    while ((d = bqueue_get(&r->ops)) != NULL) {
        const op_def_t *op = &d->def->ops[d->cur];
        const char *opn = OP_NAME[op->eq], *dn = d->def->name;
        int id = d->order->id;
        pool_t *p = &r->eq[op->eq];

        CHECK(!busy, "повар взял вторую операцию, не закончив первую");
        CHECK(d->op_state[d->cur] == OP_QUEUED, "операцию взяли из очереди повторно");
        CHECK(d->cur == 0 || d->op_state[d->cur - 1] == OP_DONE,
              "операция начата до завершения предыдущей");
        d->op_state[d->cur] = OP_COOKING;
        busy = 1;
        log_event(who, "взял из очереди: %s, %s (заказ %d)", opn, dn, id);
        unit = pool_acquire(p, a->id, who);
        if (unit < 0)
            break;
        log_event(who, "взял %s #%d: %s, %s (заказ %d)", EQ_ACC[op->eq], unit, opn, dn, id);
        mx_lock(&r->mx);
        n = ++r->st.cooks_busy;
        CHECK(n <= g_cfg.cooks, "готовят больше поваров, чем есть");
        mx_unlock(&r->mx);
        log_event(who, "начал: %s, %s (заказ %d), %d ед.", opn, dn, id, op->dur);
        rc = nap(op->dur);
        mx_lock(&r->mx);
        r->st.cooks_busy--;
        if (rc == 0)
            r->st.ops_done++;
        mx_unlock(&r->mx);
        /* сначала пишем в лог, потом освобождаем: иначе следующий повар
         * может напечатать "взял" раньше нашего "освободил" */
        log_event(who, "%s: %s, %s (заказ %d), освободил %s #%d",
                  rc == 0 ? "закончил" : "прервал", opn, dn, id, EQ_ACC[op->eq], unit);
        pool_release(p, unit, a->id);
        if (rc != 0)
            break;
        d->op_state[d->cur] = OP_DONE;
        busy = 0;
        /* очередь отчётов вмещает все блюда в работе, put не блокируется */
        if (bqueue_put(&r->reports, d, 0) != 0 || ksem_post(r->bell) != 0)
            break;
    }
    return NULL;
}
