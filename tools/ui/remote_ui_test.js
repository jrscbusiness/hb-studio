// Browser test of the MOBILE remote page against the REAL server (build/hb_remote_sim, fake host).
//   node remote_ui_test.js <path/to/hb_remote_sim>     (CHROME=/path/to/chrome to override the browser)
const { chromium } = require('playwright-core');
const { spawn } = require('child_process');

const sim = process.argv[2] || '../../build/hb_remote_sim';
let failures = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { failures++; console.log('  FAIL: ' + msg); } else console.log('  ok:   ' + msg); };
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const child = spawn(sim, [], { stdio: ['ignore', 'pipe', 'inherit'] });
  const lines = [];
  let buf = '';
  child.stdout.on('data', d => { buf += d; let i; while ((i = buf.indexOf('\n')) >= 0) { lines.push(buf.slice(0, i)); buf = buf.slice(i + 1); } });
  const until = async (f, ms = 4000) => { const t = Date.now(); while (Date.now() - t < ms) { if (f()) return true; await sleep(25); } return f(); };
  ok(await until(() => lines.some(l => l.startsWith('READY'))), 'simulator started');
  const [, port, pin] = lines.find(l => l.startsWith('READY')).split(' ');
  const cmds = () => lines.filter(l => l.startsWith('CMD ')).map(l => l.slice(4));

  const exe = process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
  const browser = await chromium.launch({ executablePath: exe, args: ['--no-sandbox'] });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 780 }, hasTouch: true, colorScheme: 'dark' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  await page.goto(`http://127.0.0.1:${port}/`);

  ok(await page.locator('#pair.show').count() === 1, 'unpaired device sees the PIN screen');
  ok(await page.locator('#main.show').count() === 0, 'controls are hidden before pairing');
  await page.fill('#pin', '000000' === pin ? '111111' : '000000');
  await page.click('#pairbtn');
  await page.waitForFunction(() => /incorreto/.test(document.querySelector('#pairmsg').textContent));
  ok(true, 'wrong PIN is refused with a message');
  await page.fill('#pin', pin);
  await page.fill('#devname', 'Teste Chromium');
  await page.click('#pairbtn');
  await page.waitForSelector('#main.show');
  ok(true, 'correct PIN pairs and shows the controls');
  const token = await page.evaluate(() => localStorage.getItem('hbr_token'));
  ok(token && token.length === 64, 'a 64-hex session token is stored in the browser');

  await page.waitForSelector('#srcs button:nth-child(8)');
  const names = await page.$$eval('#srcs button span', e => e.map(x => x.textContent));
  ok(names.length === 8 && names[0] === '1 CAM 1', 'bank 1 shows 8 source buttons with names');
  ok(await page.locator('#srcs button.pvw').count() === 1, 'current preview source is highlighted');
  await page.tap('#srcs button:nth-child(3)');
  ok(await until(() => cmds().includes('preview|2')), 'tapping a source sends preview|2');
  await page.tap('#mode');
  await page.tap('#srcs button:nth-child(4)');
  ok(await until(() => cmds().includes('program|3')), 'program mode: tapping sends program|3');
  await page.tap('#bank1');
  await page.tap('#srcs button:nth-child(1)');
  ok(await until(() => cmds().includes('program|8')), 'bank 2 maps to sources 9-16');
  await page.tap('#cut');
  ok(await until(() => cmds().includes('cut')), 'CUT sends cut');
  await page.tap('#auto');
  ok(await until(() => cmds().includes('dissolve|500')), 'AUTO sends dissolve with the chosen duration');
  await page.evaluate(() => { const t = document.querySelector('#tbar'); t.value = 500; t.dispatchEvent(new Event('input')); t.dispatchEvent(new Event('change')); });
  ok(await until(() => cmds().includes('tbar|0.500')), 'T-bar sends tbar|0.500');

  await page.tap('#tabs button[data-t=a]');
  ok(await page.locator('#strips .strip').count() === 2, 'audio tab shows the mixer strips');
  await page.tap('#strips .strip:nth-child(1) button:nth-child(2)');
  ok(await until(() => cmds().includes('aud|strip|0|mute|1')), 'mute button sends the mute command');
  await page.evaluate(() => { const s = document.querySelector('#strips .strip input'); s.value = -12; s.dispatchEvent(new Event('change')); });
  ok(await until(() => cmds().includes('aud|strip|0|faderDb|-12.0')), 'fader sends faderDb');

  await page.tap('#tabs button[data-t=o]');
  await page.tap('#rec');
  ok(await until(() => cmds().includes('rec|start')), 'REC sends rec|start');
  await page.waitForFunction(() => /GRAVANDO/.test(document.querySelector('#rec').textContent));
  ok(true, 'REC button reflects the recording state pushed by the host');
  await page.tap('#tx button');
  ok(await until(() => cmds().includes('xmitstart|d1')), 'destination button sends xmitstart|d1');
  await page.tap('#tp-play');
  ok(await until(() => cmds().includes('tp|play')), 'teleprompter play is forwarded');
  ok(!(await page.content()).includes('streamKey'), 'page never mentions stream keys');

  await page.tap('#thumbs');
  ok(await page.locator('#thumbs.on').count() === 1, 'thumbnails are opt-in (button toggles on)');

  const noScrollX = await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1);
  ok(noScrollX, 'portrait: no horizontal overflow');
  await page.setViewportSize({ width: 780, height: 390 });
  await page.tap('#tabs button[data-t=v]');
  ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), 'landscape: no horizontal overflow');
  ok(await page.evaluate(() => getComputedStyle(document.body).backgroundColor) === 'rgb(14, 17, 24)', 'dark color scheme is respected');
  await page.setViewportSize({ width: 390, height: 780 });

  // Reload: token login without the PIN.
  await page.reload();
  await page.waitForSelector('#main.show');
  ok(true, 'reload logs in again with the stored token (no PIN)');

  // Connection loss -> banner; restart -> auto reconnect is impossible with a new PIN/session, so just check the banner.
  child.kill();
  await page.waitForSelector('#banner.show', { timeout: 8000 });
  ok(true, 'connection-lost banner appears when the host goes away');
  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors[0] : ''));
  await browser.close();
  console.log(`\n${checks - failures}/${checks} checks passed`);
  process.exit(failures ? 1 : 0);
})();
