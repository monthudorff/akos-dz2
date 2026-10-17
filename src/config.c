/* Чтение конфигурации (см. config.h). */
#include "config.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TOKENS   16          /* слов в строке после ключа */
#define MAX_LINE     512
#define MAX_FILE     (1 << 20)   /* 1 МБ хватит любому конфигу */

const char *const EQ_KEY[EQ_COUNT]  = { "stove", "oven", "grill", "table" };
const char *const EQ_NAME[EQ_COUNT] = { "плита", "печь", "гриль", "стол" };
const char *const EQ_ACC[EQ_COUNT]  = { "плиту", "печь", "гриль", "стол" };
const char *const OP_NAME[EQ_COUNT] = { "варка", "запекание", "жарка", "нарезка" };
const char *const PRIO_KEY[]        = { "fifo", "vip", "short" };

/* Параметры для запуска без -c (остальное спрашиваем с клавиатуры). */
static const char DEFAULT_TEXT[] =
    "customers 12\n"
    "waiters 2\n"
    "cooks 3\n"
    "arrival 1 3\n"
    "equipment stove 2\n"
    "equipment oven 1\n"
    "equipment grill 1\n"
    "equipment table 2\n"
    "dish steak table:1 grill:4\n"
    "dish pizza table:2 oven:6\n"
    "dish soup stove:3 table:1\n"
    "dish salad table:2\n"
    "hall_queue 4\n"
    "op_queue 6\n"
    "ready_slots 3\n"
    "priority vip\n";

/* Целочисленные параметры вида "ключ число". */
static const struct {
    const char *key;
    size_t off;
    int lo, hi;
} INT_KEYS[] = {
    { "customers",   offsetof(config_t, customers),   0, MAX_CUSTOMERS },
    { "waiters",     offsetof(config_t, waiters),     1, MAX_WORKERS },
    { "cooks",       offsetof(config_t, cooks),       1, MAX_WORKERS },
    { "max_dishes",  offsetof(config_t, max_dishes),  1, MAX_ORDER_DISHES },
    { "vip",         offsetof(config_t, vip_percent), 0, 100 },
    { "hall_queue",  offsetof(config_t, hall_queue),  1, 1000 },
    { "op_queue",    offsetof(config_t, op_queue),    1, 1000 },
    { "ready_slots", offsetof(config_t, ready_slots), 1, 1000 },
};

typedef struct {
    const char *src;            /* имя файла для сообщений */
    int line;                   /* номер текущей строки */
} pctx_t;

static int perr(const pctx_t *p, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Сообщение об ошибке с номером строки; всегда возвращает -1. */
static int perr(const pctx_t *p, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (p->line > 0)
        log_err("Ошибка конфига %s, строка %d: %s\n", p->src, p->line, msg);
    else
        log_err("Ошибка конфига %s: %s\n", p->src, msg);
    return -1;
}

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->customers = 12;
    c->waiters = 2;
    c->cooks = 3;
    c->arrival_min = 1;
    c->arrival_max = 3;
    c->eat_min = 2;
    c->eat_max = 4;
    c->eq_count[EQ_STOVE] = 2;
    c->eq_count[EQ_OVEN] = 1;
    c->eq_count[EQ_GRILL] = 1;
    c->eq_count[EQ_TABLE] = 2;
    c->max_dishes = 3;
    c->vip_percent = 20;
    c->hall_queue = 4;
    c->op_queue = 6;
    c->ready_slots = 3;
    c->priority = PRIO_FIFO;
    c->unit_ms = 100;
}

/* Строка -> int в [lo, hi]; 0 или -1. */
static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static int find_eq(const char *s)
{
    int i;

    for (i = 0; i < EQ_COUNT; i++)
        if (strcmp(s, EQ_KEY[i]) == 0)
            return i;
    return -1;
}

static int find_dish(const config_t *c, const char *s)
{
    int i;

    for (i = 0; i < c->ndish_types; i++)
        if (strcmp(s, c->dishes[i].name) == 0)
            return i;
    return -1;
}

/* dish имя тип:время ... */
static int parse_dish(config_t *c, const pctx_t *p, char **tok, int n)
{
    dish_def_t *d;
    int i;

    if (n < 2)
        return perr(p, "нужно: dish имя тип:время ...");
    if (n - 1 > MAX_OPS)
        return perr(p, "в блюде больше %d операций", MAX_OPS);
    if (c->ndish_types == MAX_DISH_TYPES)
        return perr(p, "в меню больше %d блюд", MAX_DISH_TYPES);
    if (strlen(tok[0]) >= NAME_LEN)
        return perr(p, "слишком длинное имя блюда");
    if (find_dish(c, tok[0]) >= 0)
        return perr(p, "блюдо %s уже есть", tok[0]);
    d = &c->dishes[c->ndish_types];
    strcpy(d->name, tok[0]);
    d->nops = n - 1;
    for (i = 1; i < n; i++) {
        char *colon = strchr(tok[i], ':');
        if (colon == NULL)
            return perr(p, "операция %s: нужно тип:время", tok[i]);
        *colon = '\0';
        d->ops[i - 1].eq = find_eq(tok[i]);
        if (d->ops[i - 1].eq < 0)
            return perr(p, "неизвестное оборудование %s (stove, oven, grill, table)", tok[i]);
        if (parse_int(colon + 1, 1, 1000, &d->ops[i - 1].dur) != 0)
            return perr(p, "время операции %s: нужно целое от 1 до 1000", colon + 1);
    }
    c->ndish_types++;
    return 0;
}

/* order номер_посетителя блюдо ... */
static int parse_order(config_t *c, const pctx_t *p, char **tok, int n)
{
    fixed_order_t *o;
    int i, id;

    if (n < 2)
        return perr(p, "нужно: order номер_посетителя блюдо ...");
    if (n - 1 > MAX_ORDER_DISHES)
        return perr(p, "в заказе больше %d блюд", MAX_ORDER_DISHES);
    if (c->norders == MAX_FIXED_ORDERS)
        return perr(p, "больше %d строк order", MAX_FIXED_ORDERS);
    if (parse_int(tok[0], 1, MAX_CUSTOMERS, &id) != 0)
        return perr(p, "номер посетителя %s: нужно целое от 1 до %d", tok[0], MAX_CUSTOMERS);
    for (i = 0; i < c->norders; i++)
        if (c->orders[i].customer == id)
            return perr(p, "заказ посетителя %d уже задан в строке %d", id, c->orders[i].line);
    o = &c->orders[c->norders];
    o->customer = id;
    o->ndishes = n - 1;
    o->line = p->line;
    for (i = 1; i < n; i++) {
        o->dish[i - 1] = find_dish(c, tok[i]);
        if (o->dish[i - 1] < 0)
            return perr(p, "неизвестное блюдо %s (блюда задаются выше строкой dish)", tok[i]);
    }
    c->norders++;
    return 0;
}

static int parse_line(config_t *c, const pctx_t *p, char *line)
{
    char *save, *key, *t, *tok[MAX_TOKENS];
    char *hash = strchr(line, '#');
    int n = 0, i, a, b;

    if (hash != NULL)
        *hash = '\0';                   /* комментарий до конца строки */
    key = strtok_r(line, " \t\r", &save);
    if (key == NULL)
        return 0;                       /* пустая строка */
    while ((t = strtok_r(NULL, " \t\r", &save)) != NULL) {
        if (n == MAX_TOKENS)
            return perr(p, "слишком много значений");
        tok[n++] = t;
    }

    for (i = 0; i < (int)(sizeof INT_KEYS / sizeof INT_KEYS[0]); i++) {
        if (strcmp(key, INT_KEYS[i].key) != 0)
            continue;
        if (n != 1 || parse_int(tok[0], INT_KEYS[i].lo, INT_KEYS[i].hi,
                                (int *)((char *)c + INT_KEYS[i].off)) != 0)
            return perr(p, "%s: нужно одно целое число от %d до %d",
                        key, INT_KEYS[i].lo, INT_KEYS[i].hi);
        return 0;
    }
    if (strcmp(key, "arrival") == 0 || strcmp(key, "eat") == 0) {
        if (n != 2 || parse_int(tok[0], 0, 1000, &a) != 0
            || parse_int(tok[1], 0, 1000, &b) != 0 || a > b)
            return perr(p, "%s: нужно два целых min max, 0 <= min <= max <= 1000", key);
        if (key[0] == 'a') {
            c->arrival_min = a;
            c->arrival_max = b;
        } else {
            c->eat_min = a;
            c->eat_max = b;
        }
        return 0;
    }
    if (strcmp(key, "equipment") == 0) {
        if (n != 2 || (i = find_eq(tok[0])) < 0)
            return perr(p, "нужно: equipment stove|oven|grill|table число");
        if (parse_int(tok[1], 0, MAX_UNITS, &c->eq_count[i]) != 0)
            return perr(p, "число единиц оборудования: от 0 до %d", MAX_UNITS);
        return 0;
    }
    if (strcmp(key, "priority") == 0) {
        for (i = 0; n == 1 && i < 3; i++) {
            if (strcmp(tok[0], PRIO_KEY[i]) == 0) {
                c->priority = i;
                return 0;
            }
        }
        return perr(p, "priority: fifo, vip или short");
    }
    if (strcmp(key, "dish") == 0)
        return parse_dish(c, p, tok, n);
    if (strcmp(key, "order") == 0)
        return parse_order(c, p, tok, n);
    return perr(p, "неизвестный параметр %s", key);
}

/* Разбор текста конфига построчно. */
static int parse_text(config_t *c, const char *src, const char *text)
{
    pctx_t p = { src, 0 };
    char line[MAX_LINE];
    const char *s = text, *nl;
    size_t len;
    int i;

    c->ndish_types = 0;
    c->norders = 0;
    while (*s != '\0') {
        p.line++;
        nl = strchr(s, '\n');
        len = nl ? (size_t)(nl - s) : strlen(s);
        if (len >= sizeof line)
            return perr(&p, "слишком длинная строка");
        memcpy(line, s, len);
        line[len] = '\0';
        if (parse_line(c, &p, line) != 0)
            return -1;
        s += len + (nl ? 1 : 0);
    }
    if (c->ndish_types == 0) {
        p.line = 0;
        return perr(&p, "нет ни одного блюда (строка dish)");
    }
    for (i = 0; i < c->norders; i++) {
        if (c->customers > 0 && c->orders[i].customer > c->customers) {
            p.line = c->orders[i].line;
            return perr(&p, "посетителя %d нет: всего посетителей %d",
                        c->orders[i].customer, c->customers);
        }
    }
    return 0;
}

int config_load(config_t *c, const char *path)
{
    char *buf = NULL, *nbuf;
    size_t len = 0, cap = 0;
    ssize_t n = -1;
    int fd, rc;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_err("Не удалось открыть конфиг %s: %s\n", path, strerror(errno));
        return -1;
    }
    for (;;) {
        if (len + 1 >= cap) {
            cap = cap ? cap * 2 : 4096;
            if (cap > MAX_FILE) {
                log_err("Конфиг %s больше 1 МБ\n", path);
                break;
            }
            nbuf = realloc(buf, cap);
            if (nbuf == NULL) {
                log_err("Не хватило памяти для конфига\n");
                break;
            }
            buf = nbuf;
        }
        n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_err("Ошибка чтения конфига %s: %s\n", path, strerror(errno));
            break;
        }
        if (n == 0) {
            buf[len] = '\0';
            break;
        }
        len += (size_t)n;
    }
    rc = (buf != NULL && n == 0) ? 0 : -1;
    if (close(fd) != 0) {
        log_err("Ошибка закрытия конфига %s: %s\n", path, strerror(errno));
        rc = -1;
    }
    if (rc == 0)
        rc = parse_text(c, path, buf);
    free(buf);
    return rc;
}

/* Строка с клавиатуры через read(); длина или -1 при конце ввода. */
static int read_line(char *buf, size_t size)
{
    size_t len = 0;
    ssize_t r;
    char ch;

    for (;;) {
        r = read(STDIN_FILENO, &ch, 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {
            if (len == 0)
                return -1;
            break;
        }
        if (ch == '\n')
            break;
        if (ch != '\r' && ch != ' ' && ch != '\t' && len + 1 < size)
            buf[len++] = ch;
    }
    buf[len] = '\0';
    return (int)len;
}

/* Спросить число; пустой ввод или конец ввода - значение по умолчанию. */
static void ask_int(const char *prompt, int lo, int hi, int *val)
{
    char buf[64];

    for (;;) {
        log_plain("%s [%d]: ", prompt, *val);
        if (read_line(buf, sizeof buf) < 0) {
            log_plain("\n");
            return;
        }
        if (buf[0] == '\0' || parse_int(buf, lo, hi, val) == 0)
            return;
        log_plain("Нужно целое число от %d до %d\n", lo, hi);
    }
}

int config_interactive(config_t *c)
{
    char buf[64];
    int i;

    if (parse_text(c, "встроенный", DEFAULT_TEXT) != 0)
        return -1;
    log_plain("Конфиг не задан (-c), введите основные параметры, Enter - по умолчанию.\n");
    ask_int("Посетителей (0 - бесконечно, до Ctrl+C)", 0, MAX_CUSTOMERS, &c->customers);
    ask_int("Официантов", 1, MAX_WORKERS, &c->waiters);
    ask_int("Поваров", 1, MAX_WORKERS, &c->cooks);
    for (;;) {
        log_plain("Приоритет заказов fifo/vip/short [%s]: ", PRIO_KEY[c->priority]);
        if (read_line(buf, sizeof buf) < 0) {
            log_plain("\n");
            break;
        }
        if (buf[0] == '\0')
            break;
        for (i = 0; i < 3 && strcmp(buf, PRIO_KEY[i]) != 0; i++)
            ;
        if (i < 3) {
            c->priority = i;
            break;
        }
        log_plain("Нужно fifo, vip или short\n");
    }
    return 0;
}

int config_order_max(const config_t *c)
{
    int i, m = c->max_dishes;

    for (i = 0; i < c->norders; i++)
        if (c->orders[i].ndishes > m)
            m = c->orders[i].ndishes;
    return m;
}
