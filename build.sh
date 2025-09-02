setup() {
    local CPUS=1
    if [[ "$OS" == "Windows_NT" ]]; then
        CPUS=$(powershell -Command "[Environment]::ProcessorCount")
    else
        if [[ "$(uname -s | tr '[:upper:]' '[:lower:]')" == "darwin" ]]; then
            CPUS=$(sysctl -n hw.ncpu)
            brew install wabt
        else
            CPUS=$(nproc)
            sudo apt install wabt
        fi
    fi
    echo "$CPUS"
}

git clean -fdX

(cd modules/sqlite && git reset --hard HEAD && git clean -fd)
./modules/emsdk/emsdk install latest
./modules/emsdk/emsdk activate latest
cd modules/emsdk && source ./emsdk_env.sh && cd ../sqlite && ./configure --enable-all && cd ../..

makefile='modules/sqlite/ext/wasm/GNUmakefile'
for line in \
    "emcc.jsflags += -sFETCH" \
    "emcc.cflags += -I../../../sqlite-vector/libs" \
    "cflags.common += -I../../../sqlite-vector/libs"
do
    grep -F "$line" "$makefile" >/dev/null 2>&1 || echo "$line" >> "$makefile"
done

(cd modules/sqlite/ext/wasm && make -j$(setup) dist sqlite3_wasm_extra_init.c=../../../../wasm.c)
unzip modules/sqlite/ext/wasm/sqlite-wasm-*.zip -d tmp
mkdir -p sqlite-wasm/sqlite-wasm/jswasm
mv tmp/sqlite-wasm-*/jswasm sqlite-wasm/sqlite-wasm/.
rm -rf tmp

cp modules/sqlite-wasm/.prettierrc sqlite-wasm/.
cp modules/sqlite-wasm/index.d.ts sqlite-wasm/.
cp modules/sqlite-wasm/index.mjs sqlite-wasm/.
cp modules/sqlite-wasm/node.mjs sqlite-wasm/.
cp modules/sqlite-wasm/tsconfig.json sqlite-wasm/.

PKG=sqlite-wasm/package.json
TMP=sqlite-wasm/package.tmp.json

jq --arg version "$(cat modules/sqlite/VERSION)-sync.$(cd modules/sqlite-sync && make version)-vector.$(cd modules/sqlite-vector && make version)" '.version = $version' "$PKG" > "$TMP" && mv "$TMP" "$PKG"
(cd sqlite-wasm && npm i && npm run fix && npm run publint && npm run check-types)