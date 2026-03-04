const { chromium } = require('playwright');
const { spawn } = require('child_process');

const apikey = process.env.APIKEY;
if (!apikey) {
  console.error('E2E FAILED: APIKEY environment variable not set');
  process.exit(1);
}

const PORT = 3572;

(async () => {
  // Start http-server with COOP/COEP headers for SharedArrayBuffer support
  const server = spawn(
    'npx',
    [
      'http-server',
      '-p',
      String(PORT),
      '-H',
      'Cross-Origin-Opener-Policy: same-origin',
      '-H',
      'Cross-Origin-Embedder-Policy: require-corp',
      '-c-1',
    ],
    { stdio: 'ignore', detached: true },
  );

  await new Promise((r) => setTimeout(r, 3000));

  try {
    const browser = await chromium.launch();
    const context = await browser.newContext();
    const page = await context.newPage();

    const url = `http://127.0.0.1:${PORT}/test/e2e.html?apikey=${encodeURIComponent(apikey)}`;
    await page.goto(url);

    // Wait for results (network calls can be slow)
    await page.waitForFunction(
      () => document.body.innerText.includes('E2E Results'),
      { timeout: 120000 },
    );

    const output = await page.evaluate(() => document.body.innerText);
    console.log(output);

    await browser.close();

    if (output.includes('E2E FAILED') || !output.includes('0 failed')) {
      process.exit(1);
    }
    console.log('✅ sqlite-memory WASM e2e tests passed');
  } finally {
    process.kill(-server.pid);
  }
})();
