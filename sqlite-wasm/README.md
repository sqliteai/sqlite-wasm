# SQLite WASM

SQLite WASM conveniently wrapped as an ES Module. It includes the [sqlite-sync](https://github.com/sqliteai/sqlite-sync), [sqlite-vector](https://github.com/sqliteai/sqlite-vector), and [sqlite-memory](https://github.com/sqliteai/sqlite-memory) extensions that are automatically loaded at runtime. TypeScript types are from the [official sqlite-wasm repository](https://github.com/sqlite/sqlite-wasm).

## Features

- 🚀 SQLite WASM wrapped as an ES Module
- 🔄 Includes sqlite-sync, sqlite-vector, and sqlite-memory extensions
- 📝 Full TypeScript support
- 💾 OPFS (Origin Private File System) support for persistent storage
- ⚡ Worker thread support for better performance

## Installation

```bash
npm install @sqliteai/sqlite-wasm
```

## Vite Configuration

If you are using [Vite](https://vitejs.dev/), you need to add the following configuration options to your `vite.config.js`:

```js
import { defineConfig } from 'vite';

export default defineConfig({
  server: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  optimizeDeps: {
    exclude: ['@sqliteai/sqlite-wasm'],
  },
});
```

💡 **Example**: Check out the [sport-tracker-app example](https://github.com/sqliteai/sqlite-sync/tree/main/examples/sport-tracker-app) to see SQLite WASM in action with a worker thread implementation.

## Usage

There are three ways to use SQLite WASM:

1. **[Wrapped Worker](#wrapped-worker-with-opfs-support)** - Uses a wrapped worker (recommended)
2. **[Direct Worker](#direct-worker-with-opfs-support)** - Uses a direct worker implementation  
3. **[Main Thread](#main-thread-without-opfs)** - Runs in the main thread ⚠️ *sqlite-sync does not work in the main thread*

> 💡 **Note**: Only the worker versions support the Origin Private File System (OPFS) storage backend for persistent data storage.

### Wrapped Worker (with OPFS Support)

> [!Warning]
>
> For this to work, you need to set the following headers on your server:
>
> `Cross-Origin-Opener-Policy: same-origin`
>
> `Cross-Origin-Embedder-Policy: require-corp`

```js
import { sqlite3Worker1Promiser } from '@sqliteai/sqlite-wasm';

const log = console.log;
const error = console.error;

const initializeSQLite = async () => {
  try {
    log('Loading and initializing SQLite3 module...');

    const promiser = await new Promise((resolve) => {
      const _promiser = sqlite3Worker1Promiser({
        onready: () => resolve(_promiser),
      });
    });

    log('Done initializing. Running demo...');

    const configResponse = await promiser('config-get', {});
    log('Running SQLite3 version', configResponse.result.version.libVersion);

    const openResponse = await promiser('open', {
      filename: 'file:mydb.sqlite3?vfs=opfs',
    });
    const { dbId } = openResponse;
    log(
      'OPFS is available, created persisted database at',
      openResponse.result.filename.replace(/^file:(.*?)\?vfs=opfs$/, '$1'),
    );
    // Your SQLite code here.
  } catch (err) {
    if (!(err instanceof Error)) {
      err = new Error(err.result.message);
    }
    error(err.name, err.message);
  }
};

initializeSQLite();
```

> 📚 **API Reference**: The `promiser` object implements the [Worker1 API](https://sqlite.org/wasm/doc/trunk/api-worker1.md#worker1-methods).

### Direct Worker (with OPFS Support)

> [!Warning]
>
> For this to work, you need to set the following headers on your server:
>
> `Cross-Origin-Opener-Policy: same-origin`
>
> `Cross-Origin-Embedder-Policy: require-corp`

**Main thread (`main.js`):**
```js
const worker = new Worker('worker.js', { type: 'module' });
```

**Worker thread (`worker.js`):**
```js
import sqlite3InitModule from '@sqliteai/sqlite-wasm';

const log = console.log;
const error = console.error;

const start = (sqlite3) => {
  log('Running SQLite3 version', sqlite3.version.libVersion);
  const db =
    'opfs' in sqlite3
      ? new sqlite3.oo1.OpfsDb('/mydb.sqlite3')
      : new sqlite3.oo1.DB('/mydb.sqlite3', 'ct');
  log(
    'opfs' in sqlite3
      ? `OPFS is available, created persisted database at ${db.filename}`
      : `OPFS is not available, created transient database ${db.filename}`,
  );
  // Your SQLite code here.
};

const initializeSQLite = async () => {
  try {
    log('Loading and initializing SQLite3 module...');
    const sqlite3 = await sqlite3InitModule({ print: log, printErr: error });
    log('Done initializing. Running demo...');
    start(sqlite3);
  } catch (err) {
    error('Initialization error:', err.name, err.message);
  }
};

initializeSQLite();
```

> 📚 **API Reference**: The `db` object implements the [Object Oriented API #1](https://sqlite.org/wasm/doc/trunk/api-oo1.md).

### Main Thread (without OPFS)

```js
import sqlite3InitModule from '@sqliteai/sqlite-wasm';

const log = console.log;
const error = console.error;

const start = (sqlite3) => {
  log('Running SQLite3 version', sqlite3.version.libVersion);
  const db = new sqlite3.oo1.DB('/mydb.sqlite3', 'ct');
  // Your SQLite code here.
};

const initializeSQLite = async () => {
  try {
    log('Loading and initializing SQLite3 module...');
    const sqlite3 = await sqlite3InitModule({
      print: log,
      printErr: error,
    });
    log('Done initializing. Running demo...');
    start(sqlite3);
  } catch (err) {
    error('Initialization error:', err.name, err.message);
  }
};

initializeSQLite();
```

> 📚 **API Reference**: The `db` object implements the [Object Oriented API #1](https://sqlite.org/wasm/doc/trunk/api-oo1.md).

## Resources

- 📖 [SQLite WASM Documentation](https://sqlite.org/wasm)
- 🔧 [Worker1 API Reference](https://sqlite.org/wasm/doc/trunk/api-worker1.md)
- 🛠️ [Object Oriented API #1](https://sqlite.org/wasm/doc/trunk/api-oo1.md)
- 📦 [Package on NPM](https://www.npmjs.com/package/@sqliteai/sqlite-wasm)

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). You can use, copy, modify, and distribute it under the terms of the license for non-production use. For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.
