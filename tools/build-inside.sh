#!/usr/bin/env bash
# Runs inside switchu-build with the repository bind-mounted at /src.

set -o pipefail
exec 2>&1

MODE="${1:-release}"
VARIANT="${2:-sysmodule}"
TERMINATION_QUEUE_TEST="${3:-off}"
PREFLIGHT_MATRIX_TEST="${4:-off}"
PREFLIGHT_EDGE_TEST="${5:-off}"
RESUME_FAILURE_TEST="${6:-off}"
cd /src || exit 1

case "$MODE" in release|debug) ;; *) echo "modo invalido: $MODE"; exit 2;; esac
case "$VARIANT" in sysmodule|homebrew) ;; *) echo "variante invalida: $VARIANT"; exit 2;; esac
case "$TERMINATION_QUEUE_TEST" in on|off) ;; *) echo "teste de fila invalido: $TERMINATION_QUEUE_TEST"; exit 2;; esac
case "$PREFLIGHT_MATRIX_TEST" in on|off) ;; *) echo "teste de preflight invalido: $PREFLIGHT_MATRIX_TEST"; exit 2;; esac
case "$PREFLIGHT_EDGE_TEST" in on|off) ;; *) echo "teste de borda de preflight invalido: $PREFLIGHT_EDGE_TEST"; exit 2;; esac
case "$RESUME_FAILURE_TEST" in on|off) ;; *) echo "teste de falha de retomada invalido: $RESUME_FAILURE_TEST"; exit 2;; esac

DIAGNOSTIC_TEST_COUNT=0
for DIAGNOSTIC_TEST_MODE in "$TERMINATION_QUEUE_TEST" "$PREFLIGHT_MATRIX_TEST" "$PREFLIGHT_EDGE_TEST" "$RESUME_FAILURE_TEST"; do
    if [ "$DIAGNOSTIC_TEST_MODE" = on ]; then
        DIAGNOSTIC_TEST_COUNT=$((DIAGNOSTIC_TEST_COUNT + 1))
    fi
done
if [ "$DIAGNOSTIC_TEST_COUNT" -gt 1 ]; then
    echo "use apenas um modo de diagnostico por build"
    exit 2
fi
if [ "$VARIANT" != sysmodule ] && [ "$DIAGNOSTIC_TEST_COUNT" -gt 0 ]; then
    echo "modos de diagnostico sao validos apenas para sysmodule"
    exit 2
fi

# CMake records the source directory. A cache produced outside Docker cannot
# safely be reused under /src, but an already-valid Docker cache is retained.
CACHE=build/espeak-ng-native/CMakeCache.txt
if [ -f "$CACHE" ] && ! grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/src/' "$CACHE"; then
    echo "cache CMake gerado para outro diretorio de origem, descartando"
    rm -rf build/espeak-ng-native
fi

# Windows dependency files have paths such as C:/..., which GNU make parses as
# a target separator. Remove only the affected Atmosphere build state.
STRAT=lib/Atmosphere-libs/libstratosphere
if [ -d "$STRAT/build" ] && grep -rlsE '(^|[^A-Za-z])[A-Za-z]:[/\\]' --include='*.d' "$STRAT/build" | head -1 | grep -q .; then
    echo "descartando objetos libstratosphere com paths Windows"
    rm -rf "$STRAT/build" "$STRAT/lib"
fi

if [ "$VARIANT" = homebrew ]; then
    HOMEBREW=--homebrew=y
else
    HOMEBREW=--homebrew=n
fi

if [ "$TERMINATION_QUEUE_TEST" = on ]; then
    TERMINATION_QUEUE_TEST_ARG=--termination_queue_test=y
else
    TERMINATION_QUEUE_TEST_ARG=--termination_queue_test=n
fi

if [ "$PREFLIGHT_MATRIX_TEST" = on ]; then
    PREFLIGHT_MATRIX_TEST_ARG=--preflight_matrix_test=y
else
    PREFLIGHT_MATRIX_TEST_ARG=--preflight_matrix_test=n
fi

if [ "$PREFLIGHT_EDGE_TEST" = on ]; then
    PREFLIGHT_EDGE_TEST_ARG=--preflight_edge_test=y
else
    PREFLIGHT_EDGE_TEST_ARG=--preflight_edge_test=n
fi

if [ "$RESUME_FAILURE_TEST" = on ]; then
    RESUME_FAILURE_TEST_ARG=--resume_failure_test=y
else
    RESUME_FAILURE_TEST_ARG=--resume_failure_test=n
fi

xmake f --yes -p cross -m "$MODE" -a aarch64 --toolchain=devkita64 "$HOMEBREW" "$TERMINATION_QUEUE_TEST_ARG" "$PREFLIGHT_MATRIX_TEST_ARG" "$PREFLIGHT_EDGE_TEST_ARG" "$RESUME_FAILURE_TEST_ARG" --root || exit $?
xmake --root -j"$(nproc)" || exit $?

DIST="dist/$VARIANT/$MODE"
rm -rf "$DIST"
xmake install -o "$DIST" --root || exit $?

if [ "$VARIANT" = sysmodule ]; then
    [ -d "$DIST/atmosphere" ] || { echo "faltou $DIST/atmosphere"; exit 1; }
    [ -d "$DIST/switch" ] || { echo "faltou $DIST/switch"; exit 1; }
    mkdir -p artifacts
    ZIP="$PWD/artifacts/OmniLaunch-sysmodule-$MODE.zip"
    rm -f "$ZIP"
    ( cd "$DIST" && zip -qr "$ZIP" atmosphere switch ) || exit $?
    echo "pronto: artifacts/OmniLaunch-sysmodule-$MODE.zip ($(du -h "$ZIP" | cut -f1))"
    echo "copie atmosphere/ e switch/ do zip para a raiz do cartao SD"
else
    echo "pronto: build/cross/aarch64/$MODE/SwitchU.nro"
fi

echo "simbolos para ns_debug.sh: build/cross/aarch64/$MODE/{SwitchU,switchu-daemon}"
