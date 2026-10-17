/*
 * Вывод событий, учёт ошибок и нарушений инвариантов, обёртки мьютексов.
 * Весь вывод идёт через write() в fd 1 и в файл лога под одним мьютексом,
 * поэтому строки разных потоков не перемешиваются.
 */
#ifndef LOG_H
#define LOG_H

#include <pthread.h>

/* Открыть лог (path может быть NULL), quiet - не писать события в stdout,
 * unit_ms - миллисекунд в единице модельного времени. 0 или -1. */
int    log_open(const char *path, int quiet, int unit_ms);
void   log_close(void);

/* Событие: "[ 12.40] who | текст" в stdout (если не quiet) и в лог. */
void   log_event(const char *who, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Строка итога без метки времени: всегда в stdout и в лог. */
void   log_plain(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Сообщение в stderr (до запуска потоков: ошибки конфига и аргументов). */
void   log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

double model_now(void);                     /* модельное время в единицах */

/* Ошибка системного вызова: в лог, флаг ошибки, аварийная остановка. */
void   sys_fail(const char *what, int err);
/* Нарушение инварианта: строка НАРУШЕНИЕ в лог, флаг для кода возврата 2. */
void   violation(const char *file, int line, const char *msg);
int    log_violations(void);
int    log_errors(void);

#define CHECK(cond, msg) \
    do { if (!(cond)) violation(__FILE__, __LINE__, (msg)); } while (0)

/* Обёртки мьютексов с проверкой кода возврата. */
int  mx_init(pthread_mutex_t *m);
void mx_destroy(pthread_mutex_t *m);
void mx_lock(pthread_mutex_t *m);
void mx_unlock(pthread_mutex_t *m);

#endif
