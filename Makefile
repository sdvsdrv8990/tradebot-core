# ============================================================================
#  tradebot-core — запасная сборка без CMake.
#
#    make            — сборка библиотек и демо (release)
#    make debug      — отладочная сборка (-O0 -g)
#    make markers    — сборка с printf-маркерами диагностики (-DTB_EVENT_MARKERS=1)
#    make test       — сборка и прогон всех тестов
#    make run        — сборка и запуск демо bot_demo (12 секунд)
#    make run-events — сборка и запуск демо реестра событий (30 секунд)
#    make run-exec   — сборка и запуск демо механизма торговли (14 секунд)
#    make clean      — удалить артефакты
#
#  Каталог сборки: ./build (можно переопределить: make BUILD=дир)
# ============================================================================

CC      ?= cc
CXX     ?= c++
AR      ?= ar
BUILD   ?= build
OPT     ?= -O2 -g

CFLAGS    := -std=gnu11 $(OPT) -Iinclude -pthread -Wall -Wextra -Wno-format-truncation -Wno-missing-field-initializers
CXXFLAGS  := -std=c++20 $(OPT) -Iinclude -Ithird_party/rapidjson/include -pthread -Wall -Wextra -Wno-format-truncation -Wno-missing-field-initializers
LDFLAGS   := -pthread -lm
# mbedTLS добавляется к линковке целям, зависящим от tb_net (см. правила)
LDFLAGS_NET := $(BUILD)/libmbed_tls.a

# ---------- mbedTLS (вендор, TLS для HTTP-транспорта v0.3) ----------
# Прямая компиляция исходников (конфиги PSA уже в дереве, генерация не нужна).
MBEDTLS_SRC := $(wildcard third_party/mbedtls/library/*.c)
MBEDTLS_OBJ := $(patsubst third_party/mbedtls/library/%.c,$(BUILD)/mbedtls/%.o,$(MBEDTLS_SRC))
MBEDTLS_CFLAGS := -std=c99 -O2 -g -Ithird_party/mbedtls/include -Ithird_party/mbedtls/library

$(BUILD)/libmbed_tls.a: $(MBEDTLS_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/mbedtls/%.o: third_party/mbedtls/library/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MBEDTLS_CFLAGS) -c -o $@ $<

# ---------- источники ----------
CORE_SRC := \
  src/core/error.c \
  src/core/error_registry.c \
  src/core/ids.c \
  src/core/log.c \
  src/core/time.c \
  src/core/alloc.c \
  src/arch/arch.c \
  src/sched/task.c \
  src/sched/pqueue.c \
  src/sched/timer.c \
  src/sched/loop.c \
  src/sched/threadpool.c \
  src/sched/backoff.c \
  src/runtime/ratelimit.c \
  src/runtime/breaker.c \
  src/events/events.c \
  src/events/builtins.c \
  src/ledger/ledger.c \
  src/exec/u256.c \
  src/exec/route.c \
  src/exec/wallet.c \
  src/exec/backend_sim.c \
  src/exec/engine.c

RUNTIME_SRC := src/json/envelope.cpp src/json/config.cpp
NET_SRC     := src/net/http.c
DEX_SRC     := src/exec/backend_dex.c
USER_SRC    := src/user/filters.c src/user/decision.c src/user/arb.c

CORE_OBJ   := $(addprefix $(BUILD)/,$(CORE_SRC:.c=.o))
RUNTIME_OBJ:= $(addprefix $(BUILD)/,$(RUNTIME_SRC:.cpp=.o))
NET_OBJ    := $(addprefix $(BUILD)/,$(NET_SRC:.c=.o))
DEX_OBJ    := $(addprefix $(BUILD)/,$(DEX_SRC:.c=.o))
USER_OBJ   := $(addprefix $(BUILD)/,$(USER_SRC:.c=.o))
DEMO_OBJ   := $(BUILD)/src/demo/bot_demo.o
EVTDEMO_OBJ:= $(BUILD)/src/demo/events_demo.o
EXECDEMO_OBJ:= $(BUILD)/src/demo/exec_demo.o
ARBDEMO_OBJ := $(BUILD)/src/demo/arb_demo.o

TEST_C_BINS  := test_error test_registry test_arch test_pqueue test_timer \
        test_backoff test_runtime test_events test_ledger test_loop \
        test_u256 test_route test_wallet test_net test_backend_dex \
        test_filters test_decision test_config test_ids
TEST_CXX_BINS:= test_envelope test_exec
TEST_BINS    := $(TEST_C_BINS) $(TEST_CXX_BINS)

.PHONY: all debug markers test run run-events run-exec clean

all: $(BUILD)/libtb_core.a $(BUILD)/libtb_runtime.a $(BUILD)/bot_demo \
     $(BUILD)/events_demo $(BUILD)/exec_demo $(BUILD)/arb_demo

debug:
	$(MAKE) OPT="-O0 -g -DTB_DEBUG" all

markers:
	$(MAKE) OPT="-O2 -g -DTB_EVENT_MARKERS=1" all

test: $(addprefix $(BUILD)/tests/,$(TEST_BINS))
	@fail=0; for b in $^; do \
        echo "== $$b"; ./$$b || fail=1; \
        done; \
        if [ $$fail -eq 0 ]; then echo "ВСЕ ТЕСТЫ ПРОЙДЕНЫ"; else echo "ЕСТЬ ПРОВАЛЕННЫЕ ТЕСТЫ"; exit 1; fi

run: all
	./$(BUILD)/bot_demo --duration 12000 --ledger ./tradebot_events.jsonl

run-events: all
	./$(BUILD)/events_demo --duration 30000 --ledger ./events_demo.jsonl

run-exec: all
	./$(BUILD)/exec_demo --duration 14000 --ledger ./exec_demo.jsonl

clean:
	rm -rf $(BUILD)

# ---------- правила ----------
$(BUILD)/libtb_core.a: $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/libtb_runtime.a: $(RUNTIME_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/libtb_net.a: $(NET_OBJ) | $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/libtb_dex.a: $(DEX_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/libtb_user.a: $(USER_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/bot_demo: $(DEMO_OBJ) $(BUILD)/libtb_core.a $(BUILD)/libtb_runtime.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(DEMO_OBJ) $(BUILD)/libtb_runtime.a $(BUILD)/libtb_core.a $(LDFLAGS)

$(BUILD)/events_demo: $(EVTDEMO_OBJ) $(BUILD)/libtb_core.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(EVTDEMO_OBJ) $(BUILD)/libtb_core.a $(LDFLAGS)

$(BUILD)/exec_demo: $(EXECDEMO_OBJ) $(BUILD)/libtb_core.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(EXECDEMO_OBJ) $(BUILD)/libtb_core.a $(LDFLAGS)

$(BUILD)/tests/test_envelope: tests/test_envelope.cpp $(BUILD)/libtb_core.a $(BUILD)/libtb_runtime.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $< $(BUILD)/libtb_runtime.a $(BUILD)/libtb_core.a $(LDFLAGS)

$(BUILD)/tests/test_exec: tests/test_exec.cpp $(BUILD)/libtb_core.a $(BUILD)/libtb_runtime.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $< $(BUILD)/libtb_runtime.a $(BUILD)/libtb_core.a $(LDFLAGS)

# тесты, зависящие от tb_net/tb_dex (+mbedTLS)
$(BUILD)/tests/test_net: tests/test_net.c $(BUILD)/libtb_core.a $(BUILD)/libtb_net.a $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/mbedtls/include -o $@ $< $(BUILD)/libtb_net.a $(BUILD)/libtb_core.a $(LDFLAGS_NET) $(LDFLAGS)

$(BUILD)/tests/test_backend_dex: tests/test_backend_dex.c $(BUILD)/libtb_core.a $(BUILD)/libtb_net.a $(BUILD)/libtb_dex.a $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/mbedtls/include -o $@ $< $(BUILD)/libtb_dex.a $(BUILD)/libtb_net.a $(BUILD)/libtb_core.a $(LDFLAGS_NET) $(LDFLAGS)

$(BUILD)/tests/test_filters: tests/test_filters.c $(BUILD)/libtb_user.a $(BUILD)/libtb_core.a $(BUILD)/libtb_net.a $(BUILD)/libtb_dex.a $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/mbedtls/include -o $@ $< $(BUILD)/libtb_user.a $(BUILD)/libtb_dex.a $(BUILD)/libtb_net.a $(BUILD)/libtb_core.a $(LDFLAGS_NET) $(LDFLAGS)

$(BUILD)/tests/test_decision: tests/test_decision.c $(BUILD)/libtb_user.a $(BUILD)/libtb_core.a $(BUILD)/libtb_net.a $(BUILD)/libtb_dex.a $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/mbedtls/include -o $@ $< $(BUILD)/libtb_user.a $(BUILD)/libtb_dex.a $(BUILD)/libtb_net.a $(BUILD)/libtb_core.a $(LDFLAGS_NET) $(LDFLAGS)

$(BUILD)/tests/test_config: tests/test_config.c $(BUILD)/libtb_runtime.a $(BUILD)/libtb_user.a $(BUILD)/libtb_core.a $(BUILD)/libtb_net.a $(BUILD)/libtb_dex.a $(BUILD)/libmbed_tls.a
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Ithird_party/mbedtls/include -o $@ $< $(BUILD)/libtb_user.a $(BUILD)/libtb_runtime.a $(BUILD)/libtb_dex.a $(BUILD)/libtb_net.a $(BUILD)/libtb_core.a $(LDFLAGS_NET) $(LDFLAGS)

$(BUILD)/tests/test_%: tests/test_%.c $(BUILD)/libtb_core.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libtb_core.a $(LDFLAGS)

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/src/net/http.o: src/net/http.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/mbedtls/include -c -o $@ $<

$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
