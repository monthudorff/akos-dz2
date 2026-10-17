/*
 * ИДЗ №2 по АКОС, вариант 32 "Кухня ресторана".
 * main читает параметры, создаёт потоки, по одному впускает посетителей,
 * ждёт завершения и печатает статистику. Сигналы SIGINT/SIGTERM принимает
 * отдельный поток через sigwait, в остальных потоках они заблокированы.
 */
#include "kitchen.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

config_t g_cfg;

/* Флаг сигнала: 0 - работаем, 1 - новых не пускаем, 2 - немедленный выход. */
static atomic_int g_stop;

typedef struct {
    sigset_t set;               /* какие сигналы ждём */
    atomic_int done;            /* main закончил работу, SIGUSR1 - выход */
} sig_arg_t;

/* Все потоки, кроме посетителей. */
typedef struct {
    pthread_t sig, coord, wt[MAX_WORKERS], ct[MAX_WORKERS];
    worker_arg_t coord_arg, wa[MAX_WORKERS], ca[MAX_WORKERS];
    sig_arg_t sa;
    int sig_ok, coord_ok, nw, nc;   /* сколько потоков реально создано */
} crew_t;

static void usage(void)
{
    log_err("Использование: kitchen_sem|kitchen_cv [-c конфиг] [-s seed] [-l лог] [-t мс] [-q]\n"
            "  -c файл  конфигурация (без -c основные параметры вводятся с клавиатуры)\n"
            "  -s seed  начальное значение генератора случайных чисел\n"
            "  -l файл  файл лога (по умолчанию kitchen.log)\n"
            "  -t мс    миллисекунд в единице модельного времени (по умолчанию 100)\n"
            "  -q       в stdout только итог, все события только в лог\n");
}

/* 0 - продолжаем, 1 - выйти с кодом 0 (-h), -1 - ошибка. */
static int parse_args(int argc, char **argv, const char **cfg_path)
{
    unsigned long v;
    char *end;
    int opt, seed_set = 0;

    g_cfg.log_path = "kitchen.log";
    while ((opt = getopt(argc, argv, "c:s:l:t:qh")) != -1) {
        switch (opt) {
        case 'c':
            *cfg_path = optarg;
            break;
        case 's':
            errno = 0;
            v = strtoul(optarg, &end, 10);
            if (optarg[0] == '-' || errno != 0 || end == optarg || *end != '\0' || v > UINT_MAX) {
                log_err("-s: нужно целое число от 0 до %u\n", UINT_MAX);
                return -1;
            }
            g_cfg.seed = (unsigned)v;
            seed_set = 1;
            break;
        case 'l':
            g_cfg.log_path = optarg;
            break;
        case 't':
            errno = 0;
            v = strtoul(optarg, &end, 10);
            if (optarg[0] == '-' || errno != 0 || end == optarg || *end != '\0' || v < 1 || v > 10000) {
                log_err("-t: нужно целое число миллисекунд от 1 до 10000\n");
                return -1;
            }
            g_cfg.unit_ms = (int)v;
            break;
        case 'q':
            g_cfg.quiet = 1;
            break;
        case 'h':
            usage();
            return 1;
        default:
            usage();
            return -1;
        }
    }
    if (optind < argc) {
        log_err("Лишний аргумент: %s\n", argv[optind]);
        usage();
        return -1;
    }
    if (!seed_set)
        g_cfg.seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
    return 0;
}

/* --------------------------------------------------------------- сигналы */

static void *signal_thread(void *arg)
{
    sig_arg_t *sa = arg;
    const char *who = "Сигналы", *name;
    int sig, rc, count = 0;

    for (;;) {
        if ((rc = sigwait(&sa->set, &sig)) != 0) {
            sys_fail("sigwait", rc);
            break;
        }
        if (sig == SIGUSR1) {
            if (atomic_load(&sa->done))
                break;                  /* main всё закончил */
            continue;                   /* чужой SIGUSR1 не считается */
        }
        name = sig == SIGINT ? "SIGINT" : "SIGTERM";
        count++;
        if (count == 1) {
            atomic_store(&g_stop, 1);
            log_event(who, "получен %s: новых посетителей не пускаем, доделываем принятые заказы "
                      "(повторный сигнал - немедленный выход)", name);
        } else if (count == 2) {
            atomic_store(&g_stop, 2);
            log_event(who, "повторный %s: немедленное завершение", name);
            sync_abort();
        }
    }
    return NULL;
}

/* Блокируем сигналы до создания потоков: маску наследуют все потоки. */
static int setup_signals(sigset_t *set)
{
    struct sigaction act;
    int rc;

    /* в фоновом процессе из скрипта SIGINT может быть игнорируемым,
     * тогда он не дойдёт и до sigwait - возвращаем действие по умолчанию */
    memset(&act, 0, sizeof act);
    act.sa_handler = SIG_DFL;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGINT, &act, NULL) != 0 || sigaction(SIGTERM, &act, NULL) != 0) {
        log_err("Ошибка sigaction: %s\n", strerror(errno));
        return -1;
    }
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGUSR1);            /* им main будит поток сигналов в конце */
    if ((rc = pthread_sigmask(SIG_BLOCK, set, NULL)) != 0) {
        log_err("Ошибка pthread_sigmask: %s\n", strerror(rc));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- ресторан */

static void restaurant_destroy(restaurant_t *r)
{
    int i;

    for (i = 0; i < EQ_COUNT; i++)
        pool_destroy(&r->eq[i]);
    ksem_destroy(r->hall_seats);
    ksem_destroy(r->ready_slots);
    ksem_destroy(r->bell);
    bqueue_destroy(&r->waiter_q);
    bqueue_destroy(&r->orders);
    bqueue_destroy(&r->ops);
    bqueue_destroy(&r->reports);
    mx_destroy(&r->mx);
}

static int restaurant_init(restaurant_t *r)
{
    int i, ok;

    memset(r, 0, sizeof *r);
    if (mx_init(&r->mx) != 0)
        return -1;
    /*
     * Ёмкости, при которых put не блокируется (см. отчёт, раздел о дедлоках):
     * в очереди официантов и заказов у каждого сидящего не больше одной
     * записи, отчётов не больше, чем блюд во всех заказах, занявших слот.
     */
    ok = bqueue_init(&r->waiter_q, "официантов", g_cfg.hall_queue) == 0
      && bqueue_init(&r->orders, "заказов", g_cfg.hall_queue) == 0
      && bqueue_init(&r->ops, "операций", g_cfg.op_queue) == 0
      && bqueue_init(&r->reports, "отчётов", g_cfg.ready_slots * config_order_max(&g_cfg)) == 0
      && (r->hall_seats = ksem_create((unsigned)g_cfg.hall_queue)) != NULL
      && (r->ready_slots = ksem_create((unsigned)g_cfg.ready_slots)) != NULL
      && (r->bell = ksem_create(0)) != NULL;
    for (i = 0; ok && i < EQ_COUNT; i++)
        ok = pool_init(&r->eq[i], i, g_cfg.eq_count[i]) == 0;
    if (!ok) {
        restaurant_destroy(r);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ посетители */

static void customer_free(customer_t *c)
{
    ksem_destroy(c->served);
    free(c);
}

/* Новый посетитель; состав заказа и VIP берутся из seed main. */
static customer_t *customer_new(restaurant_t *r, int id, unsigned *seed)
{
    customer_t *c = calloc(1, sizeof *c);
    const fixed_order_t *fo = NULL;
    order_t *o;
    int i;

    if (c == NULL) {
        sys_fail("calloc", errno);
        return NULL;
    }
    c->id = id;
    c->r = r;
    c->seed = g_cfg.seed ^ ((unsigned)id * 2654435761u);   /* свой seed потока */
    o = &c->order;
    o->id = id;
    o->owner = c;
    o->status = ORD_NEW;
    for (i = 0; i < g_cfg.norders; i++)
        if (g_cfg.orders[i].customer == id)
            fo = &g_cfg.orders[i];
    if (fo != NULL) {
        o->ndishes = fo->ndishes;
        for (i = 0; i < o->ndishes; i++)
            o->dishes[i].def = &g_cfg.dishes[fo->dish[i]];
    } else {
        o->ndishes = 1 + (int)((unsigned)rand_r(seed) % (unsigned)g_cfg.max_dishes);
        for (i = 0; i < o->ndishes; i++)
            o->dishes[i].def = &g_cfg.dishes[(unsigned)rand_r(seed) % (unsigned)g_cfg.ndish_types];
    }
    o->vip = (int)((unsigned)rand_r(seed) % 100u) < g_cfg.vip_percent;
    for (i = 0; i < o->ndishes; i++)
        o->dishes[i].order = o;
    c->served = ksem_create(0);
    if (c->served == NULL) {
        free(c);
        return NULL;
    }
    return c;
}

/* Забрать (join + free) ушедших посетителей. Прерванных не освобождаем:
 * на их заказы ещё могут ссылаться потоки кухни, их освободим в конце. */
static void reap_finished(restaurant_t *r, customer_t **list)
{
    customer_t **pp = list, *c;
    int done, rc;

    while ((c = *pp) != NULL) {
        mx_lock(&r->mx);
        done = c->finished && c->outcome != OUT_ABORTED;
        mx_unlock(&r->mx);
        if (!done) {
            pp = &c->next;
            continue;
        }
        if ((rc = pthread_join(c->tid, NULL)) != 0)
            sys_fail("pthread_join", rc);
        *pp = c->next;
        customer_free(c);
    }
}

/* Впускать посетителей по одному со случайным интервалом.
 * 1 - впустили всех, 0 - остановились по сигналу или ошибке. */
static int admit_customers(restaurant_t *r, customer_t **list)
{
    unsigned seed = g_cfg.seed;         /* seed main: заказы и интервалы */
    char who[32], dishes[256];
    customer_t *c;
    int id, gap, rc;

    for (id = 1; g_cfg.customers == 0 || id <= g_cfg.customers; id++) {
        if (atomic_load(&g_stop) != 0 || sync_aborted())
            return 0;
        c = customer_new(r, id, &seed);
        if (c == NULL)
            return 0;
        /* приход фиксирует main ("впускает"), дальше посетитель действует сам */
        dishes_str(&c->order, dishes, sizeof dishes);
        snprintf(who, sizeof who, "Посетитель %d", id);
        log_event(who, "пришёл%s, хочет: %s", c->order.vip ? " (VIP)" : "", dishes);
        c->t_arrive = model_now();
        if ((rc = pthread_create(&c->tid, NULL, customer_thread, c)) != 0) {
            sys_fail("pthread_create", rc);
            customer_free(c);
            return 0;
        }
        c->next = *list;
        *list = c;
        reap_finished(r, list);
        if (id == g_cfg.customers)
            break;
        gap = g_cfg.arrival_min
            + (int)((unsigned)rand_r(&seed) % (unsigned)(g_cfg.arrival_max - g_cfg.arrival_min + 1));
        if (sync_sleep((long)gap * g_cfg.unit_ms) != 0)
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- потоки */

static int start_crew(restaurant_t *r, crew_t *cr)
{
    int i, rc;

    if ((rc = pthread_create(&cr->sig, NULL, signal_thread, &cr->sa)) != 0)
        goto fail;
    cr->sig_ok = 1;
    for (i = 0; i < g_cfg.waiters; i++) {
        cr->wa[i].id = i + 1;
        cr->wa[i].r = r;
        if ((rc = pthread_create(&cr->wt[i], NULL, waiter_thread, &cr->wa[i])) != 0)
            goto fail;
        cr->nw++;
    }
    cr->coord_arg.id = 1;
    cr->coord_arg.r = r;
    if ((rc = pthread_create(&cr->coord, NULL, coordinator_thread, &cr->coord_arg)) != 0)
        goto fail;
    cr->coord_ok = 1;
    for (i = 0; i < g_cfg.cooks; i++) {
        cr->ca[i].id = i + 1;
        cr->ca[i].r = r;
        if ((rc = pthread_create(&cr->ct[i], NULL, cook_thread, &cr->ca[i])) != 0)
            goto fail;
        cr->nc++;
    }
    return 0;
fail:
    sys_fail("pthread_create", rc);     /* аварийная остановка уже созданных */
    return -1;
}

static void join_thread(pthread_t t)
{
    int rc = pthread_join(t, NULL);

    if (rc != 0)
        sys_fail("pthread_join", rc);
}

/* Дождаться посетителей, разослать NULL в очереди и дождаться остальных. */
static void stop_crew(restaurant_t *r, crew_t *cr, customer_t *list)
{
    customer_t *c;
    int i, rc;

    for (c = list; c != NULL; c = c->next)
        join_thread(c->tid);
    /* посетителей нет, значит всё приготовлено и выдано: можно звать "стоп" */
    for (i = 0; i < cr->nw; i++)
        if (bqueue_put(&r->waiter_q, NULL, LONG_MAX) != 0)
            break;
    for (i = 0; i < cr->nc; i++)
        if (bqueue_put(&r->ops, NULL, LONG_MAX) != 0)
            break;
    if (cr->coord_ok && bqueue_put(&r->reports, NULL, 0) == 0)
        ksem_post(r->bell);
    for (i = 0; i < cr->nw; i++)
        join_thread(cr->wt[i]);
    if (cr->coord_ok)
        join_thread(cr->coord);
    for (i = 0; i < cr->nc; i++)
        join_thread(cr->ct[i]);
    if (cr->sig_ok) {
        atomic_store(&cr->sa.done, 1);
        if ((rc = pthread_kill(cr->sig, SIGUSR1)) != 0)
            sys_fail("pthread_kill", rc);
        join_thread(cr->sig);
    }
}

/* ------------------------------------------------------------- итоги */

static void print_header(void)
{
    char chain[256];
    int i;

    log_event("Ресторан", "открыт. Синхронизация: %s, seed %u", sync_name(), g_cfg.seed);
    if (g_cfg.customers > 0)
        log_event("Ресторан", "посетителей %d, официантов %d, поваров %d",
                  g_cfg.customers, g_cfg.waiters, g_cfg.cooks);
    else
        log_event("Ресторан", "посетители без ограничения (до Ctrl+C), официантов %d, поваров %d",
                  g_cfg.waiters, g_cfg.cooks);
    log_event("Ресторан", "оборудование: плита %d, печь %d, гриль %d, стол %d",
              g_cfg.eq_count[EQ_STOVE], g_cfg.eq_count[EQ_OVEN],
              g_cfg.eq_count[EQ_GRILL], g_cfg.eq_count[EQ_TABLE]);
    for (i = 0; i < g_cfg.ndish_types; i++) {
        dish_chain_str(&g_cfg.dishes[i], chain, sizeof chain);
        log_event("Ресторан", "меню: %s = %s", g_cfg.dishes[i].name, chain);
    }
    log_event("Ресторан", "мест в зале %d, очередь операций %d, слотов зоны выдачи %d, "
              "приоритет %s, 1 ед. времени = %d мс",
              g_cfg.hall_queue, g_cfg.op_queue, g_cfg.ready_slots,
              PRIO_KEY[g_cfg.priority], g_cfg.unit_ms);
}

static void final_report(restaurant_t *r, int all_admitted)
{
    static const char *const yes_no[] = { "нет", "да" };
    stats_t st;
    double total = model_now();
    int i, aborted = sync_aborted(), stop = atomic_load(&g_stop);
    int queues_empty, eq_free = 1, balance;
    const char *how;

    mx_lock(&r->mx);
    st = r->st;
    mx_unlock(&r->mx);

    for (i = 0; i < EQ_COUNT; i++)
        if (pool_in_use(&r->eq[i]) != 0)
            eq_free = 0;
    queues_empty = bqueue_count(&r->waiter_q) == 0 && bqueue_count(&r->orders) == 0
                && bqueue_count(&r->ops) == 0 && bqueue_count(&r->reports) == 0;
    balance = st.accepted == st.served + st.cancelled;

    /* оборудование освобождают даже при экстренном выходе */
    CHECK(eq_free, "после завершения занято оборудование");
    CHECK(st.cooks_busy == 0, "после завершения кто-то из поваров готовит");
    if (!aborted) {
        CHECK(balance, "принятые заказы != обслуженные + отменённые");
        CHECK(st.arrived == st.left + st.seated, "пришедшие != ушедшие без места + севшие");
        CHECK(st.seated == st.accepted, "не у всех севших приняли заказ");
        CHECK(queues_empty, "после завершения в очередях остались элементы");
        CHECK(st.hall_used == 0 && st.slots_used == 0, "после завершения заняты места или слоты");
    }

    if (log_errors() > 0)
        how = "аварийное, из-за ошибки системного вызова";
    else if (stop == 2)
        how = "немедленное, по повторному сигналу";
    else if (stop == 1 && !all_admitted)
        how = "по сигналу: приём закрыт, принятые заказы доделаны";
    else
        how = "все посетители обслужены";

    log_plain("\n========== Итог ==========\n");
    log_plain("Синхронизация: %s\n", sync_name());
    log_plain("seed: %u\n", g_cfg.seed);
    log_plain("Завершение: %s\n", how);
    log_plain("Модельное время: %.2f ед. (1 ед. = %d мс)\n", total, g_cfg.unit_ms);
    log_plain("Посетители: пришли %d, ушли без места %d, сели в зал %d\n",
              st.arrived, st.left, st.seated);
    log_plain("Заказы: принято %d, обслужено %d, отменено %d, прервано %d\n",
              st.accepted, st.served, st.cancelled, st.seated - st.served - st.cancelled);
    if (st.waited > 0)
        log_plain("Ожидание заказа (от прихода до выдачи): среднее %.2f, максимальное %.2f ед.\n",
                  st.wait_sum / st.waited, st.wait_max);
    else
        log_plain("Ожидание заказа: никто не получил заказ\n");
    log_plain("Выполнено операций: %d, готово блюд: %d\n", st.ops_done, st.dishes_done);
    log_plain("Загрузка оборудования:\n");
    for (i = 0; i < EQ_COUNT; i++) {
        double load = (g_cfg.eq_count[i] > 0 && total > 0)
                    ? 100.0 * pool_busy_time(&r->eq[i]) / (g_cfg.eq_count[i] * total) : 0.0;
        log_plain("  %s: %d шт., занято %.1f%% времени\n", EQ_NAME[i], g_cfg.eq_count[i], load);
    }
    if (!aborted) {
        log_plain("Проверка: принятые (%d) = обслуженные (%d) + отменённые (%d): %s\n",
                  st.accepted, st.served, st.cancelled, yes_no[balance]);
        log_plain("Проверка: очереди пусты: %s, оборудование свободно: %s\n",
                  yes_no[queues_empty], yes_no[eq_free]);
    } else {
        /* в очередях остались прерванные заказы, баланс сходиться не обязан */
        log_plain("Проверка: оборудование свободно: %s (баланс заказов и пустота очередей "
                  "при немедленном выходе не проверяются)\n", yes_no[eq_free]);
    }
    log_plain("Нарушений инвариантов: %d, ошибок: %d\n", log_violations(), log_errors());
}

int main(int argc, char **argv)
{
    static crew_t crew;                 /* большой, поэтому не на стеке */
    restaurant_t r;
    customer_t *list = NULL, *c;
    const char *cfg_path = NULL;
    int rc, all_admitted = 0;

    config_defaults(&g_cfg);
    rc = parse_args(argc, argv, &cfg_path);
    if (rc != 0)
        return rc > 0 ? 0 : 1;
    if ((cfg_path ? config_load(&g_cfg, cfg_path) : config_interactive(&g_cfg)) != 0)
        return 1;
    if (setup_signals(&crew.sa.set) != 0)
        return 1;
    atomic_init(&crew.sa.done, 0);
    if (log_open(g_cfg.log_path, g_cfg.quiet, g_cfg.unit_ms) != 0)
        return 1;
    if (sync_init() != 0) {
        log_close();
        return 1;
    }
    if (restaurant_init(&r) != 0) {
        sync_done();
        log_close();
        return 1;
    }

    print_header();
    if (start_crew(&r, &crew) == 0)
        all_admitted = admit_customers(&r, &list);
    log_event("Ресторан", "приём посетителей закрыт, ждём уже принятых");
    stop_crew(&r, &crew, list);
    final_report(&r, all_admitted);

    while ((c = list) != NULL) {
        list = c->next;
        customer_free(c);
    }
    restaurant_destroy(&r);
    sync_done();
    rc = log_errors() > 0 ? 1 : (log_violations() > 0 ? 2 : 0);
    log_close();
    return rc;
}
