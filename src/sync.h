/*
 * Общий интерфейс синхронизации.
 * Две реализации: sync_sem.c (семафоры POSIX) и sync_cv.c (мьютекс +
 * условная переменная). Весь остальной код пользуется только этим файлом,
 * поэтому программы kitchen_sem и kitchen_cv отличаются одним модулем.
 */
#ifndef SYNC_H
#define SYNC_H

/* Счётный семафор модели: ожидание, пока счётчик > 0, затем уменьшение. */
typedef struct ksem ksem_t;

int         sync_init(void);           /* 0 - успех, -1 - ошибка */
void        sync_done(void);
const char *sync_name(void);           /* что используется для синхронизации */

ksem_t *ksem_create(unsigned value);   /* NULL при ошибке */
void    ksem_destroy(ksem_t *s);
int     ksem_wait(ksem_t *s);          /* 0 - взяли, -1 - аварийная остановка */
int     ksem_trywait(ksem_t *s);       /* 0 - взяли, -1 - счётчик 0 или остановка */
int     ksem_post(ksem_t *s);          /* 0 - успех, -1 - ошибка */

/* Сон для имитации времени; прерывается аварийной остановкой (-1). */
int  sync_sleep(long ms);

/* Аварийная остановка: будит всех ждущих, дальше любое ожидание сразу -1. */
void sync_abort(void);
int  sync_aborted(void);

#endif
