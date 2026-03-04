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

  const EMBED_TEST_TEXT =
    'The quick brown fox jumps over the lazy dog. This is a test of the remote embedding API.';
  const EXPECTED_DIMENSION = 768;
  const EXPECTED_EMBEDDING = [0.05142519, 0.01374194, -0.02152035, 0.0277442];
  const EMBEDDING_TOLERANCE = 0.001;

  const runE2E = function (sqlite3, apikey) {
    const db = new sqlite3.oo1.DB('/e2e.sqlite3', 'ct');
    let passed = 0;
    let failed = 0;

    function run(name, fn) {
      try {
        const detail = fn();
        passed++;
        log(`  ${name}... PASSED${detail ? ' ' + detail : ''}`);
      } catch (e) {
        failed++;
        log(`  ${name}... FAILED: ${e.message}`);
      }
    }

    function exec(sql) {
      return db.exec({ sql, rowMode: 'array', returnValue: 'resultRows' });
    }

    function execScalar(sql) {
      const rows = exec(sql);
      return rows.length > 0 ? rows[0][0] : null;
    }

    log('E2E tests (sqlite-memory WASM networking):');

    // Phase 1: Setup

    run('memory_version', () => {
      const v = execScalar("SELECT memory_version();");
      if (!v || v.length === 0) throw new Error('empty version');
      return `(v${v})`;
    });

    run('vector_version', () => {
      const v = execScalar("SELECT vector_version();");
      if (!v || v.length === 0) throw new Error('empty version');
      return `(v${v})`;
    });

    run('memory_set_apikey', () => {
      execScalar(`SELECT memory_set_apikey('${apikey}');`);
    });

    run('memory_set_model', () => {
      execScalar("SELECT memory_set_model('llama', 'embeddinggemma-300m');");
    });

    // Phase 2: Configuration

    run('memory_set_get_option', () => {
      execScalar("SELECT memory_set_option('max_tokens', 512);");
      const v = execScalar("SELECT memory_get_option('max_tokens');");
      if (Number(v) !== 512) throw new Error(`expected 512, got ${v}`);

      const provider = execScalar("SELECT memory_get_option('provider');");
      if (provider !== 'llama') throw new Error(`expected llama, got ${provider}`);

      const model = execScalar("SELECT memory_get_option('model');");
      if (model !== 'embeddinggemma-300m')
        throw new Error(`expected embeddinggemma-300m, got ${model}`);

      execScalar("SELECT memory_set_option('max_tokens', 400);");
    });

    // Phase 3: Content Management (network calls)

    run('memory_add_text', () => {
      execScalar(`SELECT memory_add_text('${EMBED_TEST_TEXT}');`);
      const count = execScalar('SELECT COUNT(*) FROM dbmem_content;');
      if (count !== 1) throw new Error(`expected 1 content, got ${count}`);
      const chunks = execScalar('SELECT COUNT(*) FROM dbmem_vault;');
      if (chunks !== 1) throw new Error(`expected 1 chunk, got ${chunks}`);
    });

    run('verify_embedding', () => {
      const rows = db.exec({
        sql: 'SELECT embedding FROM dbmem_vault LIMIT 1;',
        rowMode: 'array',
        returnValue: 'resultRows',
      });
      if (rows.length === 0) throw new Error('no embedding found');
      const blob = rows[0][0];
      const floats = new Float32Array(
        blob.buffer,
        blob.byteOffset,
        blob.byteLength / 4,
      );
      if (floats.length !== EXPECTED_DIMENSION)
        throw new Error(
          `expected dim=${EXPECTED_DIMENSION}, got ${floats.length}`,
        );
      for (let i = 0; i < EXPECTED_EMBEDDING.length; i++) {
        const diff = Math.abs(floats[i] - EXPECTED_EMBEDDING[i]);
        if (diff > EMBEDDING_TOLERANCE)
          throw new Error(
            `embedding[${i}]=${floats[i]}, expected ${EXPECTED_EMBEDDING[i]}`,
          );
      }
      return `(dim=${floats.length}, values verified)`;
    });

    run('memory_add_text_context', () => {
      execScalar(
        "SELECT memory_add_text('SQLite is a C-language library that implements a small, fast, self-contained SQL database engine.', 'test-context');",
      );
      const ctx = execScalar(
        "SELECT context FROM dbmem_content WHERE context IS NOT NULL LIMIT 1;",
      );
      if (ctx !== 'test-context')
        throw new Error(`expected test-context, got ${ctx}`);
      const count = execScalar('SELECT COUNT(*) FROM dbmem_content;');
      if (count !== 2) throw new Error(`expected 2 content, got ${count}`);
    });

    run('memory_add_text_idempotent', () => {
      const before = execScalar('SELECT COUNT(*) FROM dbmem_vault;');
      execScalar(`SELECT memory_add_text('${EMBED_TEST_TEXT}');`);
      const after = execScalar('SELECT COUNT(*) FROM dbmem_vault;');
      if (after !== before)
        throw new Error(`expected ${before} chunks, got ${after}`);
    });

    // Phase 4: Search (network calls)

    run('memory_search', () => {
      const rows = db.exec({
        sql: "SELECT hash, path, context, snippet, ranking FROM memory_search('fox', 5);",
        rowMode: 'array',
        returnValue: 'resultRows',
      });
      if (rows.length === 0) throw new Error('no search results');
      const [hash, path, , snippet, ranking] = rows[0];
      if (!hash) throw new Error('hash is empty');
      if (!path || path.length === 0) throw new Error('path is empty');
      if (!snippet || snippet.length === 0) throw new Error('snippet is empty');
      if (ranking <= 0 || ranking > 1)
        throw new Error(`ranking out of bounds: ${ranking}`);
      if (!snippet.includes('fox'))
        throw new Error('snippet does not contain fox');
      return `(ranking=${ranking.toFixed(4)})`;
    });

    run('memory_search_ranking', () => {
      execScalar("SELECT memory_set_option('min_score', 0.0);");
      const rows = db.exec({
        sql: "SELECT ranking FROM memory_search('SQL database engine', 10);",
        rowMode: 'array',
        returnValue: 'resultRows',
      });
      if (rows.length === 0) throw new Error('no results');
      for (const [ranking] of rows) {
        if (ranking <= 0 || ranking > 1)
          throw new Error(`ranking out of bounds: ${ranking}`);
      }
      execScalar("SELECT memory_set_option('min_score', 0.7);");
      return `(${rows.length} results)`;
    });

    // Phase 5: Deletion

    run('memory_delete', () => {
      const hash = execScalar(
        'SELECT hash FROM dbmem_content WHERE context IS NULL LIMIT 1;',
      );
      const before = execScalar('SELECT COUNT(*) FROM dbmem_content;');
      execScalar(`SELECT memory_delete(${hash});`);
      const after = execScalar('SELECT COUNT(*) FROM dbmem_content;');
      if (after !== before - 1)
        throw new Error(`expected ${before - 1}, got ${after}`);
    });

    run('memory_delete_context', () => {
      execScalar("SELECT memory_delete_context('test-context');");
      const count = execScalar(
        "SELECT COUNT(*) FROM dbmem_content WHERE context = 'test-context';",
      );
      if (count !== 0) throw new Error(`expected 0, got ${count}`);
    });

    run('memory_cache_clear_model', () => {
      execScalar(
        "SELECT memory_cache_clear('llama', 'embeddinggemma-300m');",
      );
    });

    run('memory_cache_clear', () => {
      execScalar('SELECT memory_cache_clear();');
    });

    run('memory_clear', () => {
      execScalar('SELECT memory_clear();');
      const content = execScalar('SELECT COUNT(*) FROM dbmem_content;');
      if (content !== 0)
        throw new Error(`expected 0 content, got ${content}`);
      const vault = execScalar('SELECT COUNT(*) FROM dbmem_vault;');
      if (vault !== 0) throw new Error(`expected 0 vault, got ${vault}`);
    });

    db.close();
    log(`\n=== E2E Results: ${passed} passed, ${failed} failed ===`);
    return failed === 0;
  };

  if (globalThis.window !== globalThis) {
    let sqlite3Js = 'sqlite3.js';
    const urlParams = new URL(globalThis.location.href).searchParams;
    if (urlParams.has('sqlite3.dir')) {
      sqlite3Js = urlParams.get('sqlite3.dir') + '/' + sqlite3Js;
    }
    importScripts(sqlite3Js);
  }
  const urlParams = new URL(globalThis.location.href).searchParams;
  const apikey = urlParams.get('apikey') || '';

  globalThis
    .sqlite3InitModule({
      print: log,
      printErr: console.error,
    })
    .then(function (sqlite3) {
      try {
        if (!apikey) {
          error('E2E FAILED: apikey not provided');
          return;
        }
        const success = runE2E(sqlite3, apikey);
        if (!success) {
          error('E2E FAILED');
        }
      } catch (e) {
        error('Exception:', e.message);
      }
    });
})();
