'use strict';
(function () {
  const logHtml = function (cssClass, ...args) {
    postMessage({
      type: 'log',
      payload: { cssClass, args },
    });
  };
  const log = (...args) => logHtml('', ...args);
  const error = (...args) => logHtml('error', ...args);

  const testVersion = function (sqlite3) {
    const db = new sqlite3.oo1.DB('/mydb.sqlite3', 'ct');

    try {
      let testString = '';
      db.exec({
        sql: 'select sqlite_version();',
        rowMode: 'array',
        callback: function (version) {
          testString += version;
        },
      });
      db.exec({
        sql: 'select cloudsync_version();',
        rowMode: 'array',
        callback: function (version) {
          testString += `-sync.${version}`;
        },
      });
      db.exec({
        sql: 'select vector_version();',
        rowMode: 'array',
        callback: function (version) {
          testString += `-vector.${version}`;
        },
      });
      log(testString);
    } finally {
      db.close();
    }
  };

  if (globalThis.window !== globalThis) {
    let sqlite3Js = 'sqlite3.js';
    const urlParams = new URL(globalThis.location.href).searchParams;
    if (urlParams.has('sqlite3.dir')) {
      sqlite3Js = urlParams.get('sqlite3.dir') + '/' + sqlite3Js;
    }
    importScripts(sqlite3Js);
  }
  globalThis
    .sqlite3InitModule({
      print: log,
      printErr: console.error,
    })
    .then(function (sqlite3) {
      try {
        testVersion(sqlite3);
      } catch (e) {
        error('Exception:', e.message);
      }
    });
})();
