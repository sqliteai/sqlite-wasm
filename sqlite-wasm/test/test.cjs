const { chromium, firefox, webkit } = require('playwright');
const packageJson = require('../package.json');

(async () => {
  [chromium, firefox, webkit].forEach(async (browser) => {
    const window = await browser.launch();
    const context = await window.newContext();
    const page = await context.newPage();

    await page.goto('http://127.0.0.1:3571/test/demo.html');

    //sleep 5s
    await new Promise((r) => setTimeout(r, 5000));

    const version = await page.evaluate(() => document.body.innerText);

    if (version != packageJson.version)
      throw Error(
        `Error: version mismatch. Expected ${packageJson.version} but got ${version}`,
      );
    console.log(`✅ ${browser.name()} sqlite-wasm test passed`);

    await window.close();
  });
})();
