/* Вывод событий и учёт ошибок (см. log.h). */
#include "log.h"
#include "sync.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINE_MAX_LEN 640

/* Поля ниже задаются в log_open до запуска потоков и потом только читаются. */
static int log_fd = -1;          /* файл лога или -1 */
static int log_quiet;            /* события не печатать в stdout */
static int log_unit_ms = 100;    /* мс в единице модельного времени */
static struct timespec log_t0;   /* момент старта модели */

static pthread_mutex_t log_mx = PTHREAD_MUTEX_INITIALIZER;
static int n_violations;         /* под log_mx */
static int n_errors;             /* под log_mx */

/* Записать буфер целиком, повторяя при частичной записи и EINTR. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Вывод строки; вызывается под log_mx. */
static void emit(const char *line, size_t len, int to_stdout)
{
    if (to_stdout && write_all(STDOUT_FILENO, line, len) < 0)
        n_errors++;
    if (log_fd >= 0 && write_all(log_fd, line, len) < 0)
        n_errors++;
}

static void lock_log(void)
{
    int rc = pthread_mutex_lock(&log_mx);
    if (rc != 0) {
        /* сообщить через лог нельзя, пишем прямо в stderr */
        static const char msg[] = "ОШИБКА: pthread_mutex_lock (лог)\n";
        (void)write_all(STDERR_FILENO, msg, sizeof msg - 1);
    }
}

static void unlock_log(void)
{
    int rc = pthread_mutex_unlock(&log_mx);
    if (rc != 0) {
        static const char msg[] = "ОШИБКА: pthread_mutex_unlock (лог)\n";
        (void)write_all(STDERR_FILENO, msg, sizeof msg - 1);
    }
}

/* Обрезать длину, которую вернул snprintf, до реально записанной. */
static size_t clamp_len(int n, size_t cap)
{
    if (n < 0)
        return 0;
    if ((size_t)n >= cap)
        return cap - 1;
    return (size_t)n;
}

int log_open(const char *path, int quiet, int unit_ms)
{
    log_quiet = quiet;
    log_unit_ms = unit_ms > 0 ? unit_ms : 1;
    if (clock_gettime(CLOCK_MONOTONIC, &log_t0) != 0) {
        log_err("ошибка clock_gettime: %s\n", strerror(errno));
        return -1;
    }
    if (path != NULL) {
        log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd < 0) {
            log_err("не удалось открыть лог %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void log_close(void)
{
    if (log_fd >= 0 && close(log_fd) != 0)
        log_err("ошибка close лога: %s\n", strerror(errno));
    log_fd = -1;
}

double model_now(void)
{
    struct timespec t;
    double ms;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0.0;
    ms = (double)(t.tv_sec - log_t0.tv_sec) * 1000.0
       + (double)(t.tv_nsec - log_t0.tv_nsec) / 1e6;
    return ms / log_unit_ms;
}

void log_event(const char *who, const char *fmt, ...)
{
    char msg[512], line[LINE_MAX_LEN];
    va_list ap;
    int n;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    lock_log();
    /* время берём под мьютексом, чтобы метки в логе шли по возрастанию */
    n = snprintf(line, sizeof line, "[%6.2f] %s | %s\n", model_now(), who, msg);
    emit(line, clamp_len(n, sizeof line), !log_quiet);
    unlock_log();
}

void log_plain(const char *fmt, ...)
{
    char line[LINE_MAX_LEN];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    lock_log();
    emit(line, clamp_len(n, sizeof line), 1);
    unlock_log();
}

void log_err(const char *fmt, ...)
{
    char line[LINE_MAX_LEN];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    (void)write_all(STDERR_FILENO, line, clamp_len(n, sizeof line));
}

void sys_fail(const char *what, int err)
{
    char ebuf[128], line[LINE_MAX_LEN];
    int n;

    if (strerror_r(err, ebuf, sizeof ebuf) != 0)
        snprintf(ebuf, sizeof ebuf, "код %d", err);
    lock_log();
    n_errors++;
    n = snprintf(line, sizeof line, "[%6.2f] ОШИБКА | %s: %s\n",
                 model_now(), what, ebuf);
    emit(line, clamp_len(n, sizeof line), 0);
    (void)write_all(STDERR_FILENO, line, clamp_len(n, sizeof line));
    unlock_log();
    /* после ошибки системного вызова продолжать нельзя: всех будим и выходим */
    sync_abort();
}

void violation(const char *file, int line_no, const char *msg)
{
    char line[LINE_MAX_LEN];
    int n;

    lock_log();
    n_violations++;
    n = snprintf(line, sizeof line, "[%6.2f] НАРУШЕНИЕ | %s (%s:%d)\n",
                 model_now(), msg, file, line_no);
    emit(line, clamp_len(n, sizeof line), 1);
    unlock_log();
}

int log_violations(void)
{
    int n;

    lock_log();
    n = n_violations;
    unlock_log();
    return n;
}

int log_errors(void)
{
    int n;

    lock_log();
    n = n_errors;
    unlock_log();
    return n;
}

int mx_init(pthread_mutex_t *m)
{
    int rc = pthread_mutex_init(m, NULL);

    if (rc != 0) {
        sys_fail("pthread_mutex_init", rc);
        return -1;
    }
    return 0;
}

void mx_destroy(pthread_mutex_t *m)
{
    int rc = pthread_mutex_destroy(m);

    if (rc != 0)
        sys_fail("pthread_mutex_destroy", rc);
}

void mx_lock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_lock(m);

    if (rc != 0)
        sys_fail("pthread_mutex_lock", rc);
}

void mx_unlock(pthread_mutex_t *m)
{
    int rc = pthread_mutex_unlock(m);

    if (rc != 0)
        sys_fail("pthread_mutex_unlock", rc);
}
