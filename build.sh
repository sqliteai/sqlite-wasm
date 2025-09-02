njobs() {
    local CPUS=1
    if [[ "$OS" == "Windows_NT" ]]; then
        CPUS=$(powershell -Command "[Environment]::ProcessorCount")
    else
        if [[ "$(uname -s | tr '[:upper:]' '[:lower:]')" == "darwin" ]]; then
            CPUS=$(sysctl -n hw.ncpu)
        else
            CPUS=$(nproc)
        fi
    fi
    echo "$CPUS"
}

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

(cd modules/sqlite/ext/wasm && make -j$(njobs) dist sqlite3_wasm_extra_init.c=../../../../wasm.c)
#mv modules/sqlite/ext/wasm/sqlite-wasm-*.zip $(TARGET)