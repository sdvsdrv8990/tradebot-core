#!/usr/bin/env bash
# stop-storm (H1.3): остановка ПОСЕРЕДИ решающей фазы — 12 запусков с
# SIGTERM в разные моменты первого батча. rc=124 — вышёл по TERM от
# timeout (норма); провал: segfault 139, KILL после зависания 137
# (опция -k 20: реальный дедлок не выйдет и за 20 с; короткие грейсы
#  давали ложные провалы под нагрузкой — сборка/ctest на той же машине), любой иной код. Под ASan-ногой CI ловит гонки
# освобождения batch/decide/eval/мьютекса.
set -u
demo="$1"
ledger="$2/stop-storm.jsonl"
rc=0
# v2: детерминированная остановка посреди оценщиков (3 прогона):
#    процесс обязан выйти чисто, а журнал — содержать decision.evaluated
#    (= комбинер дождался ВСЕХ воркеров; ревью H1)
for i in 1 2 3; do
    TB_ARB_TEST_STOP_MID=1 timeout -s TERM -k 8 12 "$demo" \
        --replay --duration 6000 --no-web --ledger "$ledger" \
        >/dev/null 2>&1
    r=$?
    if [ "$r" -ne 0 ] && [ "$r" -ne 124 ]; then
        echo "stop-storm(v2): iter=$i rc=$r" >&2
        rc=1
    fi
    if ! grep -q '"decision.evaluated"' "$ledger" 2>/dev/null; then
        echo "stop-storm(v2): iter=$i — комбинер не дошёл (нет decision.evaluated)" >&2
        rc=1
    fi
done
# тайминговая составляющая остаётся (ловит состояния вне решающей фазы)
for d in 100 150 200 250 300 400 450 550 600 700 800 950; do
    timeout -s TERM -k 20 "0.$d" "$demo" \
        --replay --no-web --ledger "$ledger" >/dev/null 2>&1
    r=$?
    if [ "$r" -ne 0 ] && [ "$r" -ne 124 ]; then
        echo "stop-storm: d=$d rc=$r" >&2
        rc=1
    fi
done
exit $rc
