(cd modules/sqlite/ext/wasm && make clean)
./modules/emsdk/emsdk install latest
./modules/emsdk/emsdk activate latest
cd modules/emsdk && source ./emsdk_env.sh && cd ../sqlite && ./configure --enable-all && cd ../..

grep 'emcc.jsflags += -sFETCH' 'modules/sqlite/ext/wasm/GNUmakefile' >/dev/null 2>&1 || echo 'emcc.jsflags += -sFETCH' >> 'modules/sqlite/ext/wasm/GNUmakefile'
grep 'emcc.cflags += -I../../../sqlite-vector/libs' 'modules/sqlite/ext/wasm/GNUmakefile' >/dev/null 2>&1 || echo 'emcc.cflags += -I../../../sqlite-vector/libs' >> 'modules/sqlite/ext/wasm/GNUmakefile'
grep 'cflags.common += -I../../../sqlite-vector/libs' 'modules/sqlite/ext/wasm/GNUmakefile' >/dev/null 2>&1 || echo 'cflags.common += -I../../../sqlite-vector/libs' >> 'modules/sqlite/ext/wasm/GNUmakefile'

(cd modules/sqlite/ext/wasm && make dist sqlite3_wasm_extra_init.c=../../../../wasm.c)
#mv modules/sqlite/ext/wasm/sqlite-wasm-*.zip $(TARGET)