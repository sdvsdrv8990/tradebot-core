#!/usr/bin/env bash
# Развертывание одной командой: проверяет сборку (при необходимости —
# ./setup.sh all) и поднимает бота вместе с веб-мордой. Без привязки
# ко времени — работает до остановки.
#
# Использование:
#   ./run.sh            # запуск бота + веб-морды (работает до остановки)
#   ./run.sh stop       # корректная остановка бота (SIGINT, затем SIGTERM)
#   ./run.sh status     # показать запущен ли бот и порт морды
#
# Остановка (все способы эквивалентны, бот завершается корректно —
# печатает итоги и гасит веб-морду):
#   ./run.sh stop       # команда остановки
#   Ctrl+C              # если запущено из терминала
#   kill <pid>          # стандартная для Linux (SIGTERM)
#
# Переменные окружения (все опциональны):
#   TB_MODE     = replay (по умолчанию) | live | live-exec
#                   replay    — офлайн-демо на локальном mock-RPC;
#                   live      — живые котировки mainnet без отправки сделок;
#                   live-exec — живое исполнение из Safe (нужны config/key!)
#   TB_WEB_PORT = порт веб-морды (8080; если занят — бот возьмёт ближайший)
#   TB_CONFIG   = путь к конфигу (docs/arb_config.example.json)
#   TB_LEDGER   = файл журнала событий (./arb_demo.jsonl)
#   TB_DURATION = ограничение работы в МС (по умолчанию — нет, до остановки)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BIN="$ROOT/.build/arb_demo"
PIDFILE="$ROOT/.build/bot.pid"
WEB_DEFAULT_PORT=8080

bot_pid() { [ -f "$PIDFILE" ] && cat "$PIDFILE" 2>/dev/null || true; }
bot_alive() {
    local p; p="$(bot_pid)"
    [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

cmd_status() {
    if bot_alive; then
        echo "запущен: pid $(bot_pid), веб-морда http://127.0.0.1:${TB_WEB_PORT:-$WEB_DEFAULT_PORT}"
        return 0
    fi
    echo "не запущен"
    return 1
}

cmd_stop() {
    local p; p="$(bot_pid)"
    if ! bot_alive; then
        rm -f "$PIDFILE"
        echo "run.sh stop: бот не запущен"
        return 0
    fi
    echo "run.sh stop: SIGINT -> pid $p (корректный останов с итогами)"
    kill -INT "$p" 2>/dev/null || true
    for _ in $(seq 1 100); do          # до 10 с ждём самостоятельного выхода
        bot_alive || { rm -f "$PIDFILE"; echo "остановлен"; return 0; }
        sleep 0.1
    done
    echo "не вышел за 10 с — SIGTERM"
    kill -TERM "$p" 2>/dev/null || true
    for _ in $(seq 1 50); do
        bot_alive || { rm -f "$PIDFILE"; echo "остановлен"; return 0; }
        sleep 0.1
    done
    echo "не вышел — SIGKILL"
    kill -KILL "$p" 2>/dev/null || true
    rm -f "$PIDFILE"
}

cmd_start() {
    if bot_alive; then
        echo "run.sh: уже запущен (pid $(bot_pid)) — сначала ./run.sh stop" >&2
        exit 1
    fi
    rm -f "$PIDFILE"

    # Сборка: полный ./setup.sh all (зависимости+сборка+тесты), если
    # бинарника нет или исходники новее него. Идемпотентно, кэш не тянет.
    if [ ! -x "$BIN" ] || [ -n "$(find src include CMakeLists.txt -newer "$BIN" -print -quit 2>/dev/null)" ]; then
        echo "run.sh: нужна (пере)сборка — запускаю ./setup.sh all"
        ./setup.sh all
    fi

    local mode="${TB_MODE:-replay}"
    local cfg="${TB_CONFIG:-docs/arb_config.example.json}"
    local led="${TB_LEDGER:-./arb_demo.jsonl}"
    local port="${TB_WEB_PORT:-$WEB_DEFAULT_PORT}"

    local args=(--config "$cfg" --ledger "$led" --web-port "$port")
    case "$mode" in
        replay)    args+=(--replay) ;;
        live)      : ;;
        live-exec) args+=(--live-exec) ;;
        *) echo "run.sh: неизвестный TB_MODE='$mode' (replay | live | live-exec)" >&2; exit 1 ;;
    esac
    # Привязки ко времени нет; TB_DURATION — единственный способ ограничить.
    [ -n "${TB_DURATION:-}" ] && [ "$TB_DURATION" -gt 0 ] 2>/dev/null \
        && args+=(--duration "$TB_DURATION")

    mkdir -p "$ROOT/.build"
    echo "─── run.sh: режим=$mode │ веб-морда: http://127.0.0.1:$port │ журнал: $led"

    "$BIN" "${args[@]}" &
    local bot=$!
    echo "$bot" > "$PIDFILE"
    # Сигналы оболочке уходим боту; из терминала (Ctrl+C) он получает их и
    # сам — обработчик идемпотентен. Выход — прибираем pidfile.
    trap 'kill -INT "$bot" 2>/dev/null || true' INT
    trap 'kill -TERM "$bot" 2>/dev/null || true' TERM HUP
    trap 'rm -f "$PIDFILE"' EXIT
    set +e
    wait "$bot"
    local rc=$?
    # wait мог прерваться сработавшим trap-ом — дожидаемся реальный код
    while [ "$rc" -ge 128 ] && kill -0 "$bot" 2>/dev/null; do
        wait "$bot"; rc=$?
    done
    exit "$rc"
}

case "${1:-start}" in
    start)  cmd_start ;;
    stop)   cmd_stop ;;
    status) cmd_status ;;
    *) echo "использование: ./run.sh [start | stop | status]" >&2; exit 2 ;;
esac
