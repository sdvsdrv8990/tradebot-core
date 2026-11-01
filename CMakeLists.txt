# ============================================================================
#  tradebot-core v0.2 — ядро торгового бота (asm / C11 / C++20)
#
#  Слои:
#    tb_core    — C-ядро: обработчик ошибок + реестр кодов, планировщик задач
#                 (event loop + таймерное колесо + пул потоков), политики
#                 повтора (backoff/jitter, rate limiter, circuit breaker),
#                 Event Ledger с дедупликацией, реестр событий, механизм
#                 торговли (маршруты, кошелёк со слотами, движок исполнения),
#                 asm-хот-споты.
#    tb_runtime — C++20-обвязка: API Response Envelope поверх rapidjson.
#
#  Основная сборка. Запасной путь без CMake — корневой Makefile.
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(tradebot-core
  VERSION 0.2.0
  LANGUAGES C CXX
  DESCRIPTION "Ядро торгового бота: обработчик ошибок, планировщик задач, каркас Universal API Runtime, механизм торговли")

set(CMAKE_C_STANDARD 11)            # gnu11: C11 + POSIX/Linux API
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Фиксированные буферы с осознанным усечением и идиома нулевой инициализации
# {0} — проектный стиль ядра: переполнение невозможно, усечение
# диагностируется по длине, нулевое состояние ошибки = «нет ошибки».
add_compile_options(-Wall -Wextra -Wno-format-truncation -Wno-missing-field-initializers)

# printf-маркеры быстрой диагностики во всех компонентах библиотеки
# (в демо events_demo маркеры включены всегда — см. target ниже).
option(TB_ENABLE_MARKERS "Включить printf-маркеры TB_MARK в библиотеке" OFF)
if(TB_ENABLE_MARKERS)
  add_compile_definitions(TB_EVENT_MARKERS=1)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
#  mbedTLS (вендор в third_party) — TLS для HTTP-транспорта (v0.3).
#  Собирается только по требованию: цели строятся при линковке на них.
# ---------------------------------------------------------------------------
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

# secp256k1 (вендор) — подпись ECDSA для живых транзакций (v0.4)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/secp256k1 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
#  tb_core — C11-ядро
# ---------------------------------------------------------------------------
add_library(tb_core STATIC
  src/core/error.c
  src/core/error_registry.c
  src/core/ids.c
  src/core/detail.c
  src/core/keccak.c
  src/core/log.c
  src/core/time.c
  src/core/alloc.c
  src/arch/arch.c
  src/sched/task.c
  src/sched/pqueue.c
  src/sched/timer.c
  src/sched/loop.c
  src/sched/threadpool.c
  src/sched/backoff.c
  src/runtime/ratelimit.c
  src/runtime/breaker.c
  src/events/events.c
  src/events/builtins.c
  src/ledger/ledger.c
  src/exec/u256.c
  src/exec/route.c
  src/exec/wallet.c
  src/exec/backend_sim.c
  src/exec/engine.c
)
target_include_directories(tb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_core PUBLIC Threads::Threads m)

# ---------------------------------------------------------------------------
#  tb_net — HTTP-транспорт (net + replay), TLS через вендоренный mbedTLS
# ---------------------------------------------------------------------------
add_library(tb_net STATIC
  src/net/http.c
)
target_include_directories(tb_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_net PUBLIC tb_core mbedtls mbedx509 mbedcrypto)

# ---------------------------------------------------------------------------
#  tb_dex — живой DEX-адаптер за tb_exec_backend_t (QuoterV2 через RPC)
# ---------------------------------------------------------------------------
add_library(tb_dex STATIC
  src/exec/backend_dex.c
)
target_include_directories(tb_dex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_dex PUBLIC tb_net tb_core)

# ---------------------------------------------------------------------------
#  tb_user — USER-домен: фильтры 1–4, Decision Engine, рантайм арбитража
# ---------------------------------------------------------------------------
add_library(tb_user STATIC
  src/user/filters.c
  src/user/decision.c
  src/user/arb.c
)
target_include_directories(tb_user PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_user PUBLIC tb_dex tb_net tb_core)

# ---------------------------------------------------------------------------
#  tb_runtime — C++20-обвязка (готовый rapidjson вендорится в third_party)
# ---------------------------------------------------------------------------
add_library(tb_runtime STATIC
  src/json/envelope.cpp
  src/json/config.cpp
)
target_include_directories(tb_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidjson/include
)
target_link_libraries(tb_runtime PUBLIC tb_core)

# ---------------------------------------------------------------------------
#  Демо: симулятор единого событийного цикла опроса источников
# ---------------------------------------------------------------------------
add_executable(bot_demo src/demo/bot_demo.c)
target_link_libraries(bot_demo PRIVATE tb_runtime)

# ---------------------------------------------------------------------------
#  Демо: модуль регистрации событий (маркеры — всегда включены)
# ---------------------------------------------------------------------------
add_executable(events_demo src/demo/events_demo.c)
target_link_libraries(events_demo PRIVATE tb_core)
target_compile_definitions(events_demo PRIVATE TB_EVENT_MARKERS=1)

# ---------------------------------------------------------------------------
#  Демо: механизм торговли (конвейер исполнения на живом цикле)
# ---------------------------------------------------------------------------
add_executable(exec_demo src/demo/exec_demo.c)
target_link_libraries(exec_demo PRIVATE tb_core)

# ---------------------------------------------------------------------------
#  Демо: арбитражный конвейер (живые котировки + shadow-исполнение)
# ---------------------------------------------------------------------------
add_executable(arb_demo src/demo/arb_demo.c)
target_link_libraries(arb_demo PRIVATE tb_user tb_runtime tb_dex tb_net tb_core)

# ---------------------------------------------------------------------------
#  Тесты (CTest)
# ---------------------------------------------------------------------------
enable_testing()

set(TB_C_TESTS
  test_error
  test_registry
  test_arch
  test_pqueue
  test_timer
  test_backoff
  test_runtime
  test_events
  test_ledger
  test_loop
  test_u256
  test_route
  test_wallet
  test_net
  test_backend_dex
  test_filters
  test_decision
  test_config
  test_ids
  test_details
  test_ctx
  test_keccak
)
foreach(name IN LISTS TB_C_TESTS)
  add_executable(${name} tests/${name}.c)
  target_link_libraries(${name} PRIVATE tb_core)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endforeach()
target_link_libraries(test_net PRIVATE tb_net)
target_link_libraries(test_backend_dex PRIVATE tb_dex)
target_link_libraries(test_details PRIVATE tb_dex tb_user)
target_link_libraries(test_filters PRIVATE tb_user)
target_link_libraries(test_decision PRIVATE tb_user)
target_link_libraries(test_config PRIVATE tb_runtime)

add_executable(test_envelope tests/test_envelope.cpp)
target_link_libraries(test_envelope PRIVATE tb_runtime)
add_test(NAME test_envelope COMMAND test_envelope)
set_tests_properties(test_envelope PROPERTIES TIMEOUT 30)

add_executable(test_exec tests/test_exec.cpp)
target_link_libraries(test_exec PRIVATE tb_runtime)
add_test(NAME test_exec COMMAND test_exec)
set_tests_properties(test_exec PROPERTIES TIMEOUT 30)
