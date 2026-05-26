#!/usr/bin/env bash
set -euo pipefail

CPUS=1
if [ "${OS:-}" = "Windows_NT" ]; then
    CPUS=$(powershell -Command "[Environment]::ProcessorCount")
else
    if [ "$(uname -s | tr '[:upper:]' '[:lower:]')" = "darwin" ]; then
        CPUS=$(sysctl -n hw.ncpu)
        brew install wabt
    else
        CPUS=$(nproc)
        sudo apt update && sudo apt install -y wabt
    fi
fi

git clean -fdX

(cd modules/sqlite && git reset --hard HEAD && git clean -fd)
./modules/emsdk/emsdk install latest
./modules/emsdk/emsdk activate latest
cd modules/emsdk && . ./emsdk_env.sh && cd ../sqlite && ./configure --enable-all && cd ../..

makefile='modules/sqlite/ext/wasm/GNUmakefile'
for line in \
    "emcc.jsflags += -sFETCH" \
    "emcc.cflags += -I../../../sqlite-vector/libs" \
    "cflags.common += -I../../../sqlite-vector/libs" \
    "emcc.cflags += -I../../../sqlite-memory/src" \
    "cflags.common += -I../../../sqlite-memory/src" \
    "emcc.cflags += -I../../../sqlite-sync/modules/fractional-indexing" \
    "cflags.common += -I../../../sqlite-sync/modules/fractional-indexing"
do
    grep -F "$line" "$makefile" >/dev/null 2>&1 || echo "$line" >> "$makefile"
done

(cd modules/sqlite/ext/wasm && make -j$CPUS dist sqlite3_wasm_extra_init.c=../../../../wasm.c)
unzip modules/sqlite/ext/wasm/sqlite-wasm-*.zip -d tmp
mkdir -p sqlite-wasm/sqlite-wasm/jswasm
mv tmp/sqlite-wasm-*/jswasm sqlite-wasm/sqlite-wasm/.
rm -rf tmp

if [ ! -f sqlite-wasm/sqlite-wasm/jswasm/sqlite3.wasm ]; then
    echo "Missing sqlite-wasm/sqlite-wasm/jswasm/sqlite3.wasm after build"
    find sqlite-wasm/sqlite-wasm -maxdepth 4 -type f | sort
    exit 1
fi

cp modules/sqlite-wasm/.prettierrc sqlite-wasm/.
cp modules/sqlite-wasm/index.d.ts sqlite-wasm/.
cp modules/sqlite-wasm/index.mjs sqlite-wasm/.
cp modules/sqlite-wasm/node.mjs sqlite-wasm/.
cp modules/sqlite-wasm/tsconfig.json sqlite-wasm/.

PKG=sqlite-wasm/package.json
TMP=sqlite-wasm/package.tmp.json
WASM_VERSION="$(sed -n 's/^#define SQLITEAI_WASM_WRAPPER_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' wasm.c)"

if ! [[ "$WASM_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Missing or invalid SQLITEAI_WASM_WRAPPER_VERSION in wasm.c: $WASM_VERSION"
    exit 1
fi

jq --arg version "$(cat modules/sqlite/VERSION)-wasm.$WASM_VERSION-sync.$(cd modules/sqlite-sync && make version)-vector.$(cd modules/sqlite-vector && make version)-memory.$(cd modules/sqlite-memory && make version)" '.version = $version' "$PKG" > "$TMP" && mv "$TMP" "$PKG"
