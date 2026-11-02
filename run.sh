#!/usr/bin/env bash
# Развёртывание одной командой: проверяет сборку (при необходимости —
# ./setup.sh all) и поднимает бота вместе с веб-мордой. Привязки ко
# времени нет — работает до остановки.
#
# Команды:
#   ./run.sh            # бот + веб-морда; режим — из конфига
#                          (arb_config.json рядом, иначе docs/пример;
#                           network.live_exec: true = боевое исполнение)
#   ./run.sh stop       # корректная остановка (то же: Ctrl+C, kill <pid>)
#   ./run.sh status     # запущен ли бот и порт морды
#   ./run.sh addr       # адрес ключа бота — добавить владельцем в Safe
#
# Переменные (опционально): TB_WEB_PORT, TB_CONFIG, TB_LEDGER, TB_MODE
# (replay — офлайн-демо на mock-RPC), TB_DURATION (МС).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BIN="$ROOT/.build/arb_demo"
PIDFILE="$ROOT/.build/bot.pid"
WEB_DEFAULT_PORT=8080

# Личный конфиг имеет приоритет; example — для первого запуска (replay/live-котировки).
CFG_DEFAULT="$ROOT/arb_config.json"
[ -f "$CFG_DEFAULT" ] || CFG_DEFAULT="$ROOT/docs/arb_config.example.json"

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
    # до 20 с: при остановке бот ждёт в-полёта eth_call (транспорт без
    # прерывания сокета, ~8 с) — за 10 с итоги не успевали печататься
    for _ in $(seq 1 200); do
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

# Локальные логотипы для морды: качаем раз (потом только новые пулы/биржи).
# Сбой сети не фатален: страница покажет буквенные бейджи.
ensure_logos() {
    local cfg="$1"
    python3 - "$cfg" <<'PYEOF2' || true
import json, subprocess, os, sys
cfg = json.load(open(sys.argv[1]))
def slug(n):
    n = n.lower()
    for suf in ("-v3", "-v2"):
        if n.endswith(suf):
            n = n[: -len(suf)]
    return n
slugs = {slug(v["name"]) for v in cfg.get("rpc", []) if v.get("name")}
slugs |= {slug(p["dex"]) for p in cfg.get("pools", []) if p.get("dex")}
# URL требует checksummed-регистр, файл храним в нижнем (так просит морда)
toks = [p["token"] for p in cfg.get("pools", []) if p.get("token")]
if cfg.get("weth"):
    toks.append(cfg["weth"])
def get(url, path):
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    r = subprocess.run(["curl", "-fsSL", "-m", "20", "-o", path, url],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) == 0:
        try:
            os.remove(path)
        except OSError:
            pass
for s in sorted(slugs):
    get(f"https://icons.llamao.fi/icons/protocols/{s}?w=48&h=48",
        f"logos/dex/{s}.webp")
for a in sorted(set(toks)):
    get("https://raw.githubusercontent.com/trustwallet/assets/master/"
        f"blockchains/ethereum/assets/{a}/logo.png",
        f"logos/token/{a.lower()}.png")
PYEOF2
}

ensure_built() {
    # Полный ./setup.sh all (зависимости+сборка+тесты), если бинарника нет
    # или исходники новее. Идемпотентно, кэш не тянет.
    if [ ! -x "$BIN" ] || [ -n "$(find src include CMakeLists.txt -newer "$BIN" -print -quit 2>/dev/null)" ]; then
        echo "run.sh: нужна (пере)сборка — запускаю ./setup.sh all"
        ./setup.sh all
    fi
}

cmd_addr() {
    ensure_built
    exec "$BIN" --config "${TB_CONFIG:-$CFG_DEFAULT}" --print-addr
}

cmd_start() {
    if bot_alive; then
        echo "run.sh: уже запущен (pid $(bot_pid)) — сначала ./run.sh stop" >&2
        exit 1
    fi
    rm -f "$PIDFILE"
    ensure_built

    local cfg="${TB_CONFIG:-$CFG_DEFAULT}"
    local led="${TB_LEDGER:-./arb_demo.jsonl}"
    local port="${TB_WEB_PORT:-$WEB_DEFAULT_PORT}"
    ensure_logos "$cfg"

    local args=(--config "$cfg" --ledger "$led" --web-port "$port")
    case "${TB_MODE:-}" in
        "")         : ;; # режим из конфига (live_exec: true = бой)
        replay)    args+=(--replay) ;;
        live-exec) args+=(--live-exec) ;;
        *) echo "run.sh: неизвестный TB_MODE='$TB_MODE' (replay | live-exec)" >&2; exit 1 ;;
    esac
    # Привязки ко времени нет; TB_DURATION — единственный способ ограничить.
    [ -n "${TB_DURATION:-}" ] && [ "$TB_DURATION" -gt 0 ] 2>/dev/null \
        && args+=(--duration "$TB_DURATION")

    mkdir -p "$ROOT/.build"
    echo "─── run.sh: конфиг $cfg │ веб-морда: http://127.0.0.1:$port │ журнал: $led"

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
    addr)   cmd_addr ;;
    *) echo "использование: ./run.sh [start | stop | status | addr]" >&2; exit 2 ;;
esac
