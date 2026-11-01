#!/usr/bin/env bash
# ============================================================================
#  tradebot-core — установка зависимостей, сборка и проверка одной командой.
#
#    ./setup.sh            — зависимости + сборка + тесты (всё сразу)
#    ./setup.sh deps       — только зависимости
#    ./setup.sh build      — только конфигурация и сборка
#    ./setup.sh test       — только тесты
#
#  Все зависимости проекта живут ЛОКАЛЬНО в скрытом каталоге .deps/
#  (python-venv с cmake и ninja): система не трогается, root не нужен.
#  Системные требования — только toolchain: gcc/g++ (C11/C++20), make,
#  python3. rapidjson вендорится в third_party/, внешних библиотек
#  линковки нет (только libc/pthread/m).
#
#  Сеть: сначала обычный pip. Если локальный DNS-стаб (systemd-resolved)
#  не резолвит pypi.org (глюк перехватывающего прокси), wheel'ы качаются
#  curl'ом напрямую через Fastly anycast IP --resolve и ставятся offline.
#  Скачанные wheel'ы кэшируются в .deps/wheels/ — повторная установка
#  работает и вовсе без сети.
#
#  Каталог сборки — .build/ (скрытый, в git не попадает).
#  Ручная активация окружения:  . .deps/env.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

DEPS="$ROOT/.deps"
VENV="$DEPS/venv"
WHEELS="$DEPS/wheels"
BUILD="$ROOT/.build"

# Локальные cmake/ninja/ctest — в PATH с самого начала, чтобы CMake
# находил генератор Ninja без активации venv.
export PATH="$VENV/bin:$PATH"
CMAKE="$VENV/bin/cmake"
CTEST="$VENV/bin/ctest"

# pypi.org и files.pythonhosted.org — один Fastly anycast
PYPI_IP="151.101.0.223"

msg()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mОШИБКА:\033[0m %s\n' "$*" >&2; exit 1; }

check_toolchain() {
  local missing=()
  command -v python3 >/dev/null 2>&1 || missing+=("python3")
  command -v cc       >/dev/null 2>&1 || missing+=("gcc")
  command -v c++      >/dev/null 2>&1 || missing+=("gcc-c++")
  command -v make     >/dev/null 2>&1 || missing+=("make")
  if [ ${#missing[@]} -gt 0 ]; then
    fail "нет системных пакетов: ${missing[*]} — поставьте: sudo dnf install python3 gcc gcc-c++ make"
  fi
}

# Скачать wheel пакета в $WHEELS, минуя локальный DNS.
#   fetch_wheel <пакет> <glob-имени-wheel>
fetch_wheel() {
  local pkg="$1" glob="$2"
  local idx="$WHEELS/${pkg}_index.html" whl url
  curl_pypi "https://pypi.org/simple/$pkg/" "$idx" \
    || fail "не скачался индекс $pkg"
  whl=$(grep -oE "$glob" "$idx" | grep -v aarch64 | grep -v i686 | sort -Vu | tail -1)
  [ -n "$whl" ] || fail "в индексе $pkg не найден wheel под x86_64"
  if [ ! -s "$WHEELS/$whl" ]; then
    msg "  загрузка $whl"
    url=$(grep -oE "https://files\.pythonhosted\.org/packages/[^\"#]*$whl" "$idx" | head -1)
    [ -n "$url" ] || fail "в индексе $pkg нет ссылки на $whl"
    curl_pypi "$url" "$WHEELS/$whl" || fail "не скачался $whl"
  fi
}

curl_pypi() {  # curl_pypi <url> <выходной-файл>
  curl -fsSL --connect-timeout 10 --retry 2 -o "$2" \
    --resolve pypi.org:443:"$PYPI_IP" \
    --resolve files.pythonhosted.org:443:"$PYPI_IP" "$1"
}

install_deps() {
  msg "Установка зависимостей в $DEPS (локально, без root)"
  if [ ! -x "$CMAKE" ]; then
    [ -d "$VENV" ] || python3 -m venv "$VENV"

    # Уже скачанные wheel'ы ставим сразу, без сети
    if ls "$WHEELS"/*.whl >/dev/null 2>&1; then
      "$VENV/bin/pip" install --quiet --no-index --find-links "$WHEELS" cmake ninja \
        || true
    fi

    # Чего не хватило — добираем с PyPI
    if [ ! -x "$CMAKE" ] || [ ! -x "$VENV/bin/ninja" ]; then
      if ! "$VENV/bin/pip" install --quiet --retries 1 --timeout 8 cmake ninja; then
        msg "PyPI недоступен через локальный DNS — обход через прямой IP ($PYPI_IP)"
        mkdir -p "$WHEELS"
        fetch_wheel cmake 'cmake-[0-9][^"<]*manylinux[^"<]*x86_64[^"<]*\.whl'
        fetch_wheel ninja 'ninja-[0-9][^"<]*manylinux[^"<]*x86_64[^"<]*\.whl'
        "$VENV/bin/pip" install --quiet --no-index --find-links "$WHEELS" cmake ninja
      fi
    fi
    [ -x "$CMAKE" ] || fail "cmake не установился"
  fi

  cat > "$DEPS/env.sh" <<EOF
# Локальное окружение tradebot-core: cmake, ctest, ninja
export PATH="$VENV/bin:\$PATH"
EOF
  msg "Окружение готово: $($CMAKE --version | head -1)"
}

configure_build() {
  [ -x "$CMAKE" ] || fail "cmake не установлен — запустите: ./setup.sh deps"
  local gen=()
  [ -x "$VENV/bin/ninja" ] && gen=(-G Ninja)
  msg "Конфигурация сборки в $BUILD (${gen[*]:-Unix Makefiles})"
  "$CMAKE" -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo "${gen[@]}"
}

build_all() {
  configure_build
  msg "Сборка"
  "$CMAKE" --build "$BUILD" -j"$(nproc)"
}

run_tests() {
  [ -f "$BUILD/CTestTestfile.cmake" ] || fail "сборки нет — запустите: ./setup.sh build"
  msg "Тесты (ctest)"
  "$CTEST" --test-dir "$BUILD" --output-on-failure -j"$(nproc)"
}

check_toolchain
case "${1:-all}" in
  deps)  install_deps ;;
  build) build_all ;;
  test)  run_tests ;;
  all)   install_deps; build_all; run_tests; msg "ГОТОВО: зависимости + сборка + тесты — всё чисто" ;;
  *)     fail "неизвестная команда '$1'; используйте: deps | build | test | (пусто) = всё" ;;
esac
