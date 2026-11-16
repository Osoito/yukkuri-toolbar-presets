#!/bin/sh
# The game is a 32-bit HSP3 process, so everything that touches it must be i686.
set -e
# cc1.exe needs the toolchain DLLs alongside it, so put them on PATH.
MINGW32_BIN=${MINGW32_BIN:-/c/msys64/mingw32/bin}
PATH="$MINGW32_BIN:$PATH"
export PATH
CC=${CC:-gcc}
CFLAGS="-O2 -Wall -Wextra -std=gnu99 -Isrc -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE"
LDFLAGS="-static-libgcc -s"
cd "$(dirname "$0")"
mkdir -p build

echo "== slots_test.exe =="
$CC $CFLAGS -o build/slots_test.exe src/slots.c src/slots_test.c $LDFLAGS

if [ -f src/mod.c ]; then
  echo "== YukkuriToolbarMod.dll =="
  $CC $CFLAGS -shared -o build/YukkuriToolbarMod.dll \
      src/slots.c src/presets.c src/names.c src/overlay.c src/mod.c \
      $LDFLAGS -lgdi32 -luser32
fi

if [ -f src/inject.c ]; then
  echo "== YukkuriToolbarMod.exe =="
  $CC $CFLAGS -o build/YukkuriToolbarMod.exe src/inject.c $LDFLAGS -luser32
fi

echo "ok"
