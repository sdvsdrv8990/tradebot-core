#!/usr/bin/env bash
# stop-storm (H1.3): остановка ПОСЕРЕДИ решающей фазы — 12 запусков с
# SIGTERM в разные моменты первого батча. rc=124 — вышёл по TERM от
# timeout (норма); провал: segfault 139, KILL после зависания 137
# (опция -k 8: реальный дедлок не выйдет и за 8 с; 2 с давали ложный провал под нагрузкой сборки), любой иной код. Под ASan-ногой CI ловит гонки
# освобождения batch/decide/eval/мьютекса.
set -u
demo="$1"
ledger="$2/stop-storm.jsonl"
rc=0
for d in 100 150 200 250 300 400 450 550 600 700 800 950; do
    timeout -s TERM -k 8 "0.$d" "$demo" \
        --replay --no-web --ledger "$ledger" >/dev/null 2>&1
    r=$?
    if [ "$r" -ne 0 ] && [ "$r" -ne 124 ]; then
        echo "stop-storm: d=$d rc=$r" >&2
        rc=1
    fi
done
exit $rc
