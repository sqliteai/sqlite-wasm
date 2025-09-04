# SQLite WASM

SQLite WASM conveniently wrapped as an ES Module. It includes the [sqlite-sync](https://github.com/sqliteai/sqlite-sync) and [sqlite-vector](https://github.com/sqliteai/sqlite-vector) extensions that are automatically loaded at runtime. TypeScript types are from the [official sqlite-wasm repository](https://github.com/sqlite/sqlite-wasm).

## Installation

```bash
npm install @sqliteai/sqlite-wasm
```

## Vite Usage Example

If you are using [vite](https://vitejs.dev/), you need to add the following
config option in `vite.config.js`:

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

Check out the [sport-tracker-app example](https://github.com/sqliteai/sqlite-sync/tree/main/examples/sport-tracker-app) that shows this in action while using SQLite WASM in a worker thread.

## Usage

There are three ways to use SQLite Wasm:

- [in the main thread with a wrapped worker](#in-a-wrapped-worker-with-opfs-if-available)
- [in a worker](#in-a-worker-with-opfs-if-available)
- [in the main thread](#in-the-main-thread-without-opfs) (⚠️ sqlite-sync does not work in the main thread)

Only the worker versions allow you to use the origin private file system (OPFS)
storage back-end.

### In a wrapped worker (with OPFS if available):

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

The `promiser` object above implements the
[Worker1 API](https://sqlite.org/wasm/doc/trunk/api-worker1.md#worker1-methods).

### In a worker (with OPFS if available):

> [!Warning]
>
> For this to work, you need to set the following headers on your server:
>
> `Cross-Origin-Opener-Policy: same-origin`
>
> `Cross-Origin-Embedder-Policy: require-corp`

```js
// In `main.js`.
const worker = new Worker('worker.js', { type: 'module' });
```

```js
// In `worker.js`.
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

The `db` object above implements the
[Object Oriented API #1](https://sqlite.org/wasm/doc/trunk/api-oo1.md).

### In the main thread (without OPFS):

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

The `db` object above implements the
[Object Oriented API #1](https://sqlite.org/wasm/doc/trunk/api-oo1.md).

## License

Apache 2.0.

## Acknowledgements

This project is based on [SQLite Wasm](https://sqlite.org/wasm), which it
conveniently wraps as an ES Module and publishes to npm as
[`@sqliteai/sqlite-wasm`](https://www.npmjs.com/package/@sqliteai/sqlite-wasm).
