#!/bin/bash
# Тесты: tests/run_tests.sh ./kitchen_sem ./kitchen_cv
# Для каждой программы: код возврата и отсутствие НАРУШЕНИЕ в логе.
# пути к программам запоминаем до перехода в каталог tests
bins=()
for b in "$@"; do bins+=("$(realpath "$b")"); done
cd "$(dirname "$0")" || exit 1
T=5                       # мс в единице времени
TMO=60                    # тайм-аут одного запуска, с
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0

report() {  # имя, успех(0/1)
    if [ "$2" = 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}

# run имя ожидаемый_код конфиг [доп. аргументы]
run() {
    local name=$1 want=$2 cfg=$3; shift 3
    timeout $TMO "$BIN" -c "$cfg" -t $T -q -l "$tmp/$name.log" "$@" > "$tmp/$name.out" 2>&1
    local code=$?
    local ok=0
    [ "$code" = "$want" ] || { ok=1; echo "       код $code, ожидался $want"; }
    if grep -q НАРУШЕНИЕ "$tmp/$name.log" "$tmp/$name.out" 2>/dev/null; then ok=1; echo "       есть НАРУШЕНИЕ"; fi
    report "$name" $ok
}

for BIN in "${bins[@]}"; do
    echo "== $BIN"
    run basic       0 basic.cfg    -s 1
    run parallel    0 parallel.cfg -s 2
    run onegrill    0 onegrill.cfg -s 3
    run queues1     0 queues1.cfg  -s 4
    run vip         0 vip.cfg      -s 5
    run short       0 short.cfg    -s 6

    # невыполнимый заказ: отмена, не пусто в «отменено»
    run nooven      0 nooven.cfg   -s 7
    grep -q "заказ 1 отменён" "$tmp/nooven.log"; r1=$?
    grep -q "сообщил посетителю 1, что заказ 1 отменён" "$tmp/nooven.log"; r2=$?
    report "nooven: отмена и уведомление посетителя" $((r1 | r2))

    # битый конфиг: код 1 и номер строки
    run bad         1 bad.cfg
    grep -q "строка 6" "$tmp/bad.out"; report "bad: указан номер строки" $?
    run nofile      1 no_such_file.cfg

    # неверные аргументы
    "$BIN" -t 0 -c basic.cfg -l "$tmp/x.log" > /dev/null 2>&1
    report "аргумент -t 0 отвергнут (код 1)" $([ $? = 1 ] && echo 0 || echo 1)

    # повтор по seed: одинаковые заказы и исход (время в логе может отличаться)
    "$BIN" -c basic.cfg -t $T -q -s 99 -l "$tmp/r1.log" > /dev/null
    "$BIN" -c basic.cfg -t $T -q -s 99 -l "$tmp/r2.log" > /dev/null
    f() { grep -E "хочет:" "$1" | sed 's/^\[[^]]*\] //' | sort; }
    [ "$(f "$tmp/r1.log")" = "$(f "$tmp/r2.log")" ] && [ -n "$(f "$tmp/r1.log")" ]
    report "повтор по seed: те же посетители и заказы" $?

    # бесконечный режим + SIGINT: первый — доделать принятые
    "$BIN" -c inf.cfg -t $T -q -s 8 -l "$tmp/inf1.log" > /dev/null 2>&1 &
    pid=$!; sleep 1; kill -INT $pid; wait $pid; code=$?
    ok=0; [ $code = 0 ] || ok=1
    grep -q НАРУШЕНИЕ "$tmp/inf1.log" && ok=1
    grep -q "Проверка: принятые" "$tmp/inf1.log" || ok=1
    grep -q "Нарушений инвариантов: 0" "$tmp/inf1.log" || ok=1
    report "бесконечный режим + один SIGINT (код 0, баланс сходится)" $ok

    # второй SIGINT: немедленный выход, лог и статистика сохранены
    "$BIN" -c inf.cfg -t 50 -q -s 9 -l "$tmp/inf2.log" > /dev/null 2>&1 &
    pid=$!; sleep 1; kill -INT $pid; sleep 0.2; kill -INT $pid; wait $pid; code=$?
    ok=0; [ $code = 0 ] || ok=1
    grep -q НАРУШЕНИЕ "$tmp/inf2.log" && ok=1
    grep -q "немедленное" "$tmp/inf2.log" || ok=1
    grep -q "Итог" "$tmp/inf2.log" || ok=1
    report "бесконечный режим + два SIGINT (быстрый выход, итог в логе)" $ok

    # SIGTERM как первый сигнал
    "$BIN" -c inf.cfg -t $T -q -s 10 -l "$tmp/inf3.log" > /dev/null 2>&1 &
    pid=$!; sleep 1; kill -TERM $pid; wait $pid; code=$?
    report "бесконечный режим + SIGTERM (код 0)" $code
done

[ $fail = 0 ] && echo "ВСЕ ТЕСТЫ ПРОЙДЕНЫ" || echo "ЕСТЬ ПРОВАЛЫ"
exit $fail
