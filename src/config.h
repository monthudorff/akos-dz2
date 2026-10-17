/* Параметры модели: чтение конфига (open/read) и ввод с клавиатуры. */
#ifndef CONFIG_H
#define CONFIG_H

enum { EQ_STOVE, EQ_OVEN, EQ_GRILL, EQ_TABLE, EQ_COUNT };   /* типы оборудования */
enum { PRIO_FIFO, PRIO_VIP, PRIO_SHORT };                   /* правило приоритета */

#define MAX_DISH_TYPES   16     /* блюд в меню */
#define MAX_OPS          8      /* операций в одном блюде */
#define MAX_ORDER_DISHES 8      /* блюд в одном заказе */
#define MAX_FIXED_ORDERS 256    /* строк order в конфиге */
#define MAX_UNITS        16     /* единиц оборудования одного типа */
#define MAX_WORKERS      64     /* официантов или поваров */
#define MAX_CUSTOMERS    100000
#define NAME_LEN         32

typedef struct {
    int eq;                     /* тип оборудования */
    int dur;                    /* длительность в единицах времени */
} op_def_t;

typedef struct {
    char name[NAME_LEN];
    int nops;
    op_def_t ops[MAX_OPS];      /* цепочка: каждая ждёт предыдущую */
} dish_def_t;

typedef struct {
    int customer;               /* номер посетителя */
    int ndishes;
    int dish[MAX_ORDER_DISHES]; /* индексы в меню */
    int line;                   /* строка конфига, для сообщений */
} fixed_order_t;

typedef struct {
    int customers;              /* 0 - бесконечно, до сигнала */
    int waiters, cooks;
    int arrival_min, arrival_max;   /* интервал между посетителями */
    int eat_min, eat_max;           /* сколько посетитель ест */
    int eq_count[EQ_COUNT];
    int ndish_types;
    dish_def_t dishes[MAX_DISH_TYPES];
    int norders;
    fixed_order_t orders[MAX_FIXED_ORDERS];
    int max_dishes;             /* блюд в случайном заказе: 1..max_dishes */
    int vip_percent;            /* доля VIP-посетителей, % */
    int hall_queue;             /* мест в зале */
    int op_queue;               /* ёмкость очереди операций */
    int ready_slots;            /* слотов в зоне готовых блюд */
    int priority;
    /* из командной строки */
    unsigned seed;
    int unit_ms;
    int quiet;
    const char *log_path;
} config_t;

/* Конфиг: заполняется в main до запуска потоков, дальше только читается. */
extern config_t g_cfg;

extern const char *const EQ_KEY[EQ_COUNT];    /* stove, oven, ... */
extern const char *const EQ_NAME[EQ_COUNT];   /* плита, печь, ... */
extern const char *const EQ_ACC[EQ_COUNT];    /* плиту, печь, ... (кого? что?) */
extern const char *const OP_NAME[EQ_COUNT];   /* варка, запекание, ... */
extern const char *const PRIO_KEY[];          /* fifo, vip, short */

void config_defaults(config_t *c);
int  config_load(config_t *c, const char *path);    /* 0 или -1 (сообщение выведено) */
int  config_interactive(config_t *c);               /* 0 или -1 */
int  config_order_max(const config_t *c);           /* наибольший размер заказа */

#endif
