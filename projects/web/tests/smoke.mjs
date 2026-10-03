import { chromium } from "playwright";

const url = process.argv[2];
const browser = await chromium.launch();
const page = await browser.newPage();
page.on("console", message => console.log(`[console] ${message.text()}`));
page.on("pageerror", error => console.log(`[pageerror] ${error.message}`));

await page.goto(url);
await page.waitForFunction(() => document.documentElement.dataset.result, null, { timeout: 120_000 });
const result = await page.evaluate(() => document.documentElement.dataset.result);
console.log(await page.textContent("#results"));
console.log(await page.textContent("#status"));
await browser.close();
process.exit(result === "passed" ? 0 : 1);
