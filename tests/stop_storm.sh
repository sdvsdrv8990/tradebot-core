#!/usr/bin/env bash
# stop-storm (H1.3): остановка ПОСЕРЕДИ решающей фазы — 6 запусков с
# SIGTERM в разные моменты первого батча. rc=124 — вышёл по TERM от
# timeout (норма); провал: segfault 139, KILL после зависания 137
# (опция -k 2), любой иной код. Под ASan-ногой CI ловит гонки
# освобождения batch/decide/eval/мьютекса.
set -u
demo="$1"
ledger="$2/stop-storm.jsonl"
rc=0
for d in 100 200 300 450 600 800; do
    timeout -s TERM -k 2 "0.$d" "$demo" \
        --replay --no-web --ledger "$ledger" >/dev/null 2>&1
    r=$?
    if [ "$r" -ne 0 ] && [ "$r" -ne 124 ]; then
        echo "stop-storm: d=$d rc=$r" >&2
        rc=1
    fi
done
exit $rc
