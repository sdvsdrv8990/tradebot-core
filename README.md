# tradebot-core — ядро торгового бота

Ядро децентрализованной арбитражной торговли на C11/C++20 (Linux):
событийный планировщик, реестр событий с цепочками шагов, Event Ledger,
конвейер исполнения с атомарными слотами, живые котировки Uniswap V3
(QuoterV2 через публичный RPC) и живое исполнение из Safe
(EIP-1559, EIP-712). Фильтры 1–4 + Decision Engine ищут минимальную
выгодную ставку межпулового арбитража WETH→TOKEN→WETH.

Подробности устройства — в [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Установка и сборка

Всё поднимается **одной командой из корневого каталога проекта**:
зависимости ставятся локально в `.deps/` (python-venv с cmake/ninja,
без root), вендорные библиотеки (rapidjson, mbedTLS, secp256k1) — уже
в `third_party/`, сборка — в `.build/`, тесты прогоняются.

```bash
./setup.sh          # зависимости + сборка + тесты (одна команда)
```

Отдельные стадии:

```bash
./setup.sh deps     # только зависимости
./setup.sh build    # только конфигурация и сборка
./setup.sh test     # только тесты

# Основной путь руками (то же, что build):
cmake -B .build -DCMAKE_BUILD_TYPE=RelWithDebInfo -G Ninja
cmake --build .build -j

# Запасной путь без CMake — из корня собирает всё, включая вендор:
make -j$(nproc) && make test
```

Тесты: 26 (unit + интеграционные, офлайн — сеть не нужна).

## Запуск

Демо лежат в `.build/` (запасной путь — `build/`). Общие аргументы:
`--duration МС`, `--ledger ПУТЬ`, `--seed N`; выход — `Ctrl+C`,
журнал закрывается штатно.

```bash
# Арбитраж офлайн: mock-RPC со спредом, полный конвейер (8 секунд)
./.build/arb_demo --replay --duration 8000 --ledger ./arb_demo.jsonl

# Арбитраж: живые котировки mainnet, shadow-исполнение (25 секунд)
./.build/arb_demo --duration 25000 --ledger ./arb_demo.jsonl

# Живое исполнение из Safe (см. ниже)
./.build/arb_demo --live-exec --duration 60000 --ledger ./arb_demo.jsonl

# Демо подсистем: опрос источников / реестр событий / механизм торговли
./.build/bot_demo    --duration 15000 --ledger ./bot.jsonl --seed 42
./.build/events_demo --duration 30000 --ledger ./events.jsonl
./.build/exec_demo   --duration 14000 --ledger ./exec.jsonl --seed 42
```

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
4. `./.build/arb_demo --live-exec …` — газ реальный (~$0.5–2/сделка).
   Контракт и историю адреса видно в app.safe.global и Zerion/DeBank.

Конфиг: `docs/arb_config.example.json` (venues/RPC/пулы, пороги
фильтров, decision-параметры, network).

## Структура проекта

```
tradebot-core/
├── setup.sh                    # одна команда: зависимости + сборка + тесты
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
