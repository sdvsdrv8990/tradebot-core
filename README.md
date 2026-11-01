# tradebot-core — ядро торгового бота

Ядро децентрализованной арбитражной торговли на C11/C++20 (Linux):
событийный планировщик, реестр событий с цепочками шагов, Event Ledger,
конвейер исполнения с атомарными слотами, живые котировки Uniswap V3
(QuoterV2 через публичный RPC) и живое исполнение из Safe
(EIP-1559, EIP-712). Фильтры 1–4 + Decision Engine ищут минимальную
выгодную ставку межпулового арбитража WETH→TOKEN→WETH.

Подробности устройства — в [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Развёртывание и запуск

Одна команда из корневого каталога проекта: при необходимости собирает
(зависимости локально в `.deps/`, без root; сборка в `.build/`; тесты)
и поднимает бота вместе с веб-мордой. Привязки ко времени нет — бот
работает до остановки.

```bash
./run.sh            # сборка (если нужно) + бот + веб-морда http://127.0.0.1:8080
./run.sh stop       # корректная остановка (итоги, журнал, морда гасится)
./run.sh status     # запущен ли бот и порт морды
```

Остановка любым стандартным способом: `./run.sh stop`, `Ctrl+C` из
терминала, `kill <pid>` — бот завершается штатно, печатает итоги и
закрывает журнал.

Режимы и параметры — переменными окружения:

```bash
TB_MODE=replay      ./run.sh   # по умолчанию: офлайн-демо на mock-RPC
TB_MODE=live        ./run.sh   # живые котировки mainnet, без отправки сделок
TB_MODE=live-exec   ./run.sh   # живое исполнение из Safe (см. ниже)
TB_WEB_PORT=8080 TB_CONFIG=docs/arb_config.example.json TB_LEDGER=./arb_demo.jsonl
```

Отдельные стадии сборки: `./setup.sh [deps | build | test]` (без
аргумента — всё). Запасной путь без CMake: `make -j$(nproc) && make test`.
Тесты: 26 (unit + интеграционные, офлайн — сеть не нужна).

Демо подсистем (после сборки лежат в `.build/`):

| Демо | Что показывает |
|---|---|
| `arb_demo` | арбитражный конвейер: фильтры 1–4, Decision Engine (минимальная ставка до верха контракта), `--replay` — офлайн, `--live-exec` — из Safe |
| `bot_demo` | опрос источников: 429/Retry-After, backoff+jitter, circuit breaker, Event Ledger |
| `events_demo` | реестр событий: цепочка шагов, загрузка/выгрузка на ходу, мосты к планировщику и журналу |
| `exec_demo` | механизм торговли: preflight→commit, атомарность слота, PENDING, slippage-защита |

### Живое исполнение из Safe (контракт средств)

Кастодия — **Safe 1-из-2**: твой кошелёк + горячий ключ бота, порог 1.
«Контракт на N эфира» = содержимое Safe: бот торгует через
`execTransaction` изнутри контракта, principal и прибыль его не
покидают; закрытие = снять бота с владельцев / вывести всё.
Компрометация ключа бота теряет только содержимое Safe.

1. Создать Safe на [app.safe.global](https://app.safe.global)
   (сеть Ethereum), добавить вторым владельцем адрес ключа бота
   (печатается при старте демо), порог 1.
2. Пополнить контракт WETH.
3. Ключ: `openssl rand -hex 32 > bot_key.hex` (не коммитить!), в
   конфиге `network` задать `safe`, `key_file`, `live_exec: true`.
4. `TB_MODE=live-exec ./run.sh` — газ реальный (~$0.5–2/сделка).
   Контракт и историю адреса видно в app.safe.global и Zerion/DeBank.

Конфиг: `docs/arb_config.example.json` (venues/RPC/пулы, пороги
фильтров, decision-параметры, network).

## Структура проекта

```
tradebot-core/
├── run.sh                     # развёртывание: бот + веб-морда одной командой
├── setup.sh                   # стадии сборки: deps | build | test | всё
├── CMakeLists.txt / Makefile   # сборка (основная + запасной путь без CMake)
├── include/tb/                 # публичные заголовки (C-ABI)
│   ├── core/     #   error, registry, ids, detail, keccak, log, time, alloc
│   ├── sched/    #   task, pqueue, timer, loop, threadpool, backoff
│   ├── runtime/  #   ratelimit, breaker
│   ├── events/   #   events (реестр), builtins (шаги-шлюзы)
│   ├── ledger/   #   ledger (JSONL-журнал)
│   ├── exec/     #   codes, u256, route, wallet, backend, backend_dex,
│   │             #   safe_exec (живое исполнение из Safe), engine
│   ├── net/      #   http (транспорт net + TLS / replay)
│   ├── sign/     #   tx (ключ, RLP, EIP-1559)
│   ├── user/     #   arb (фильтры 1–4, Decision Engine, рантайм)
│   ├── json/     #   envelope, config (C++/rapidjson с C-ABI)
│   └── arch/     #   arch (asm-хот-споты)
├── src/            # реализация (зеркалит include/)
├── tests/          # 26 тестов
├── docs/           # ARCHITECTURE.md, образцы журналов, arb_config.example.json
└── third_party/    # rapidjson, mbedtls, secp256k1 (вендор)
```

Библиотеки: `tb_core` (C11), `tb_net` (HTTP/TLS), `tb_sign`
(EIP-1559), `tb_dex` (DEX-адаптер + Safe), `tb_user` (арбитраж),
`tb_runtime` (C++/rapidjson). Демо и тесты линкуются против них.
