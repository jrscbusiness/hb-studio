// UI regression test: drives ui/index.html in a real browser (Chromium) like an operator would,
// while feeding it host state at ~15 Hz exactly like the real app does. The host bridge is replaced
// by a recorder, so every click/key is verified by the command it sends.
//
//   node ui_test.js <path/to/index.html>          (CHROME=/path/to/chrome to override the browser)
const { chromium } = require('playwright-core');
const path = require('path');

const html = path.resolve(process.argv[2] || '../../ui/index.html');
let failures = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { failures++; console.log('  FAIL: ' + msg); } else console.log('  ok:   ' + msg); };

const SLOT = (k, st, name, id, w, h) => ({ k, st, name: name || '', id: id || '', w: w || 0, h: h || 0, fps: k === 'dev' ? 30 : 0, msg: '' });
function stateMsg(over = {}) {
  return Object.assign({
    t: 's', pvw: 1, pgm: 0, transition: false, progress: 0, fps: 29.9, target: 30, frames: 1000, skipped: 0,
    tier: 'MEDIUM', cores: 8, ramGiB: 16, srcRec: 8, srcMax: 10, toast: null, log: [],
    slots: [SLOT('dev', 'online', 'OBS Virtual Camera', 'obs', 1920, 1080), ...Array.from({ length: 15 }, () => SLOT('test', 'test'))],
    vcam: { supported: true, installed: true, active: false, connected: false, busy: false, error: false, sent: 0, message: '' },
  }, over);
}
const PTZ = () => ({ t: 'ptz', cams: Array.from({ length: 16 }, () => ({ protocol: 'onvif', host: '', port: 0, address: 1, slots: 10, base: 1,
  storage: 'camera', speed: 50, invPan: false, invTilt: false, defPort: 80, connected: false, names: ['Altar', '', '', '', '', '', '', '', '', ''] })) });
const DEVICES = () => ({ t: 'devices', busy: false, list: [{ id: 'obs', name: 'OBS Virtual Camera', virt: true },
  { id: 'cam', name: 'Logitech C920 HD Pro Webcam', virt: false }, { id: 'link', name: 'Cam Link 4K', virt: false }] });

(async () => {
  const exe = process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
  const browser = await chromium.launch({ executablePath: exe, args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  page.on('dialog', d => d.accept());
  await page.addInitScript(() => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  });
  await page.goto('file://' + html);

  // Feed host state at 15 Hz, like the real feeder thread (this is what used to destroy clickable elements).
  await page.evaluate(([st, ptz, dev]) => {
    window.__state = st;
    window.__hbMessage(st); window.__hbMessage(ptz); window.__hbMessage(dev);
    window.__feed = setInterval(() => window.__hbMessage(window.__state), 66);
  }, [stateMsg(), PTZ(), DEVICES()]);
  const setState = over => page.evaluate(s => { window.__state = s; }, stateMsg(over));
  const sent = () => page.evaluate(() => window.__sent.slice());
  const clearSent = () => page.evaluate(() => { window.__sent.length = 0; });
  const lastSent = async () => { const s = await sent(); return s[s.length - 1]; };
  // press, hold ~150 ms (several state refreshes happen), release: a slow human click
  const humanClick = async (selector, nth = 0) => {
    const el = page.locator(selector).nth(nth);
    await el.scrollIntoViewIfNeeded();
    const b = await el.boundingBox();
    await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2);
    await page.mouse.down(); await page.waitForTimeout(150); await page.mouse.up();
    await page.waitForTimeout(40);
  };
  const tab = async v => { await page.click(`#tabs button[data-v=${v}]`); await page.waitForTimeout(100); };

  console.log('\n[1] PRODUÇÃO - switcher');
  await clearSent();
  await humanClick('#bus-pvw .key', 3);
  ok((await sent()).includes('preview|3'), 'Preview bus button 4 -> preview|3');
  await humanClick('#bus-pgm .key', 5);
  ok((await sent()).includes('program|5'), 'Program bus button 6 -> program|5');
  await humanClick('#b-cut');
  ok((await sent()).includes('cut'), 'CUT button -> cut');
  await humanClick('#b-dis');
  ok((await sent()).includes('dissolve|500'), 'DISSOLVE -> dissolve|500');
  await humanClick('#dur button', 2);
  await humanClick('#b-dis');
  ok((await lastSent()) === 'dissolve|1000', 'duration 1000 then DISSOLVE -> dissolve|1000');
  await humanClick('.tile', 4);
  ok((await sent()).includes('preview|4'), 'clicking an input tile selects it as preview');
  await clearSent();
  await page.keyboard.press('3'); await page.keyboard.press('Shift+4'); await page.keyboard.press('Space'); await page.keyboard.press('Enter');
  const keys = await sent();
  ok(keys.includes('preview|2') && keys.includes('program|3') && keys.includes('cut') && keys.some(k => k.startsWith('dissolve|')), 'keyboard: 3, Shift+4, Space, Enter');

  console.log('\n[1b] PRODUÇÃO - T-bar manual');
  ok(await page.locator('#tbar').count() === 1, 'T-bar fader is present alongside CUT/DISSOLVE');
  ok(+(await page.locator('#tbar').inputValue()) === 0, 'T-bar starts at rest (0)');
  await clearSent();
  await page.locator('#tbar').evaluate(el => { el.value = 500; el.dispatchEvent(new Event('input', { bubbles: true })); });
  ok((await sent()).includes('tbar|0.5000'), 'dragging the T-bar to the middle sends tbar|0.5');
  await page.locator('#tbar').evaluate(el => { el.value = 1000; el.dispatchEvent(new Event('input', { bubbles: true })); });
  ok((await sent()).includes('tbar|1.0000'), 'dragging the T-bar to the end sends tbar|1');
  // While actively dragging (pointerdown), the periodic state feed must not override the T-bar's value.
  // Dispatch pointerdown FIRST (engaging the drag guard), THEN (re)set the value to 1000 and update
  // window.__state - all inside the SAME in-page script, with no await boundary anywhere in between.
  // This closes two gaps at once: (1) the well-known one, where the 66ms state-feed interval could
  // fire between "the pointerdown listener runs" and "the new state is in place"; and (2) a subtler
  // one that actually caused the observed flake - the PRECEDING step only set el.value via raw JS,
  // without ever engaging the guard, so the interval could reset it back to the (non-dragging)
  // engine state in the gap before this guarded block even started. Re-asserting the value here,
  // after the guard is already engaged, makes the end state deterministic regardless of what the
  // interval did in that earlier gap.
  await page.evaluate(st => {
    const el = document.querySelector('#tbar');
    el.dispatchEvent(new Event('pointerdown'));
    el.value = 1000;
    window.__state = st;
  }, stateMsg({ transition: false, progress: 0 }));
  await page.waitForTimeout(100);
  ok(+(await page.locator('#tbar').inputValue()) === 1000, 'T-bar keeps the operator\'s position while pointer is down, ignoring the state feed');
  await page.evaluate(() => window.dispatchEvent(new Event('pointerup')));
  await setState({ transition: true, progress: 0.3 });
  await page.waitForTimeout(100);
  ok(+(await page.locator('#tbar').inputValue()) === 300, 'once released, the T-bar follows the engine\'s own transition progress');
  await setState({});

  console.log('\n[1c] PRODUÇÃO - 16 câmeras em 2 fileiras (banco ativo)');
  ok(await page.locator('.tile').count() === 16, '16 input tiles are shown (2 rows of 8)');
  ok(await page.locator('#bus-pgm .key').count() === 8 && await page.locator('#bus-pvw .key').count() === 8,
    'the PROGRAM/PREVIEW number-button rows stay at a fixed 8 (one bank at a time)');
  ok(await page.locator('.bankbtn.on').count() === 1 && (await page.locator('.bankbtn.on').getAttribute('data-bank')) === '0',
    'bank 1 (CAM 1–8) is active by default');
  ok(await page.locator('.tile[data-i="0"]').evaluate(el => !el.classList.contains('offbank')), 'a tile in the active bank is not dimmed');
  ok(await page.locator('.tile[data-i="8"]').evaluate(el => el.classList.contains('offbank')), 'a tile in the inactive bank is dimmed');
  await clearSent();
  await page.keyboard.press('3');
  ok((await sent()).includes('preview|2'), 'bank 1 active: pressing 3 selects preview|2 (CAM 3)');
  await humanClick('.bankbtn[data-bank="1"]');
  ok(await page.locator('.bankbtn[data-bank="1"]').evaluate(el => el.classList.contains('on')), 'clicking bank 2 activates it');
  ok((await page.locator('#bus-pgm .key').first().textContent()) === '9', 'the PROGRAM number-row now starts at CAM 9 (bank 2)');
  ok(await page.locator('.tile[data-i="0"]').evaluate(el => el.classList.contains('offbank')), 'CAM 1 is now the dimmed (inactive-bank) one');
  ok(await page.locator('.tile[data-i="8"]').evaluate(el => !el.classList.contains('offbank')), 'CAM 9 is now in the active bank');
  await clearSent();
  await page.keyboard.press('3');
  ok((await sent()).includes('preview|10'), 'bank 2 active: pressing 3 now selects preview|10 (CAM 11)');
  await clearSent();
  await page.locator('.tile[data-i="1"]').click();   // CAM 2, in the INACTIVE bank
  ok((await sent()).includes('preview|1'), 'clicking any tile with the mouse works regardless of the active bank');
  ok((await page.evaluate(() => localStorage.getItem('hb.bank'))) === '1', 'the chosen bank persists in localStorage');
  await humanClick('.bankbtn[data-bank="0"]');  // back to the default bank for the rest of the suite

  console.log('\n[1d] PRODUÇÃO - PROGRAM em tela cheia');
  ok(await page.locator('#pgm-fs').count() === 1, 'PROGRAM: fullscreen button is present');
  // Headless Chromium refuses a real Fullscreen API grant (no user gesture, no compositor), so this
  // stubs requestFullscreen/exitFullscreen to verify the wiring, then simulates the browser's own
  // confirmation (fullscreenElement + the fullscreenchange event) to verify the icon follows it.
  await page.evaluate(() => {
    window.__fsCalls = 0;
    document.querySelector('.mon.pgm').requestFullscreen = () => { window.__fsCalls++; return Promise.resolve(); };
    document.exitFullscreen = () => Promise.resolve();
  });
  await page.locator('#pgm-fs').click();
  ok(await page.evaluate(() => window.__fsCalls) === 1, 'PROGRAM: the button requests fullscreen on the PROGRAM card');
  await page.locator('.mon.pgm .screen').dblclick();
  ok(await page.evaluate(() => window.__fsCalls) === 2, 'PROGRAM: double-clicking the picture also requests fullscreen');
  await page.evaluate(() => {
    Object.defineProperty(document, 'fullscreenElement', { value: document.querySelector('.mon.pgm'), configurable: true });
    document.dispatchEvent(new Event('fullscreenchange'));
  });
  ok((await page.locator('#pgm-fs').textContent()) === '⤢', 'PROGRAM: the button flips to the exit-fullscreen icon once confirmed');
  await page.evaluate(() => {
    Object.defineProperty(document, 'fullscreenElement', { value: null, configurable: true });
    document.dispatchEvent(new Event('fullscreenchange'));
  });
  ok((await page.locator('#pgm-fs').textContent()) === '⛶', 'PROGRAM: the button flips back once fullscreen ends');

  console.log('\n[2] PRODUÇÃO - câmera virtual');
  await clearSent();
  await humanClick('#v-main');
  ok((await sent()).includes('vcam|activate'), 'installed + inactive: button sends vcam|activate');
  await setState({ vcam: { supported: true, installed: true, active: true, connected: true, busy: false, error: false, sent: 99, message: '' } });
  await page.waitForTimeout(150);
  await clearSent();
  await humanClick('#v-main');
  ok((await sent()).includes('vcam|deactivate'), 'active: button sends vcam|deactivate');
  await setState({ vcam: { supported: true, installed: false, active: false, connected: false, busy: false, error: false, sent: 0, message: '' } });
  await page.waitForTimeout(150);
  ok(await page.locator('#v-main').isDisabled(), 'not installed: main button disabled (no fake action)');
  await setState({});

  console.log('\n[3] PRODUÇÃO - PTZ e presets');
  await page.waitForTimeout(150);
  await clearSent();
  await humanClick('#presets .pre', 0);
  ok((await sent()).some(m => /^ptzgo\|\d\|0$/.test(m)), 'preset 1 (Ir para) -> ptzgo|cam|0');
  await humanClick('#ptz-mode button[data-m=save]');
  await humanClick('#presets .pre', 2);
  ok((await sent()).some(m => /^ptzsave\|\d\|2$/.test(m)), 'Salvar mode: preset 3 -> ptzsave|cam|2');
  await humanClick('#ptz-mode button[data-m=edit]');
  await humanClick('#presets .pre', 1);
  ok(await page.locator('#modal2').evaluate(e => e.classList.contains('on')), 'Editar mode opens the name dialog');
  await page.fill('#nm-input', 'Púlpito');
  await humanClick('#nm-ok');
  ok((await sent()).some(m => /^ptzname\|\d\|1\|Púlpito$/.test(m)), 'name dialog OK -> ptzname|cam|1|Púlpito');
  await humanClick('#ptz-mode button[data-m=go]');
  const presetCount = await page.locator('#presets .pre').count();
  ok(presetCount >= 10, `at least 10 preset buttons (found ${presetCount})`);
  await humanClick('#ptz-cfg');
  ok(await page.locator('#modal').evaluate(e => e.classList.contains('on')), 'gear opens the PTZ configuration dialog');
  await page.selectOption('#f-proto', 'visca-ip');
  await page.selectOption('#f-slots', '16');
  await clearSent();
  await humanClick('#cfg-save');
  ok((await sent()).some(m => /^ptzcfg\|\d\|.*"visca-ip".*"slots":16/.test(m)), 'config SALVAR -> ptzcfg with protocol and slot count');
  await humanClick('#ptz-cfg');
  await page.selectOption('#f-proto', 'visca-ip');
  await clearSent();
  await humanClick('#cfg-all');
  ok((await sent()).some(m => /^ptzall\|.*"visca-ip"/.test(m)), 'APLICAR A TODAS AS CÂMERAS -> ptzall');
  await clearSent();
  await humanClick('.dpad button[data-d=left]');
  const mv = await sent();
  ok(mv.some(m => /^ptzmove\|\d\|left$/.test(m)) && mv.some(m => /^ptzstop\|\d$/.test(m)), 'D-pad: press sends move, release sends stop');

  console.log('\n[4] CÂMERAS');
  await tab('cam');
  await page.waitForTimeout(200);
  ok((await sent()).includes('devrefresh'), 'opening the tab asks the host to scan devices');
  ok(await page.locator('#dev-list .dev').count() === 3, 'lists the 3 detected devices');
  ok((await page.locator('#dev-list').innerText()).includes('OBS Virtual Camera'), 'OBS Virtual Camera is listed');
  await clearSent();
  await humanClick('#dev-list .dev:nth-child(2) .assign button', 2);   // device 2 -> CAM 3
  ok((await sent()).includes('devassign|2|cam'), 'click number 3 on "Logitech C920" -> devassign|2|cam');
  await humanClick('#dev-list .dev:nth-child(3) .assign button', 7);
  ok((await sent()).includes('devassign|7|link'), 'click number 8 on "Cam Link 4K" -> devassign|7|link');
  await humanClick('#dev-list .dev:nth-child(1) .assign button', 0);
  ok((await sent()).includes('devassign|0|obs'), 'click number 1 on "OBS Virtual Camera" -> devassign|0|obs');
  await humanClick('#slot-list .slot:nth-child(1) .x');
  ok((await sent()).includes('devclear|0'), 'Remover on CAM 1 -> devclear|0');
  await clearSent();
  await humanClick('#dev-refresh');
  ok((await sent()).includes('devrefresh'), 'ATUALIZAR LISTA -> devrefresh');
  // state of slots is reflected
  await setState({ slots: [SLOT('dev', 'nosignal', 'OBS Virtual Camera', 'obs', 1920, 1080), SLOT('dev', 'error', 'Cam X', 'x', 0, 0), ...Array.from({ length: 14 }, () => SLOT('test', 'test'))] });
  await page.waitForTimeout(200);
  const slotText = await page.locator('#slot-list').innerText();
  ok(/SEM SINAL/.test(slotText) && /ERRO/.test(slotText) && /TESTE/.test(slotText), 'slot list shows SEM SINAL / ERRO / TESTE states');
  await page.evaluate(() => window.__hbMessage({ t: 'devices', busy: false, list: [] }));
  await page.waitForTimeout(100);
  ok((await page.locator('#dev-list').innerText()).includes('Nenhum dispositivo'), 'empty list shows a helpful empty state');

  console.log('\n[4a] CÂMERAS: fontes NDI na rede');
  await page.evaluate(a => window.__hbMessage(a), { t: 'devices', busy: false, ndiMsg: '', list: [
    { id: 'ndi:169.254.1.1:5961', name: 'PC-1 (Camera 1)', virt: false, ndi: true },
    { id: 'cam', name: 'Logitech C920 HD Pro Webcam', virt: false }] });
  await page.waitForTimeout(100);
  ok(await page.locator('#dev-list .dev').count() === 2, 'NDI source is listed alongside a regular webcam');
  ok((await page.locator('#dev-list').innerText()).includes('PC-1 (Camera 1)'), 'NDI source name is shown');
  ok((await page.locator('#dev-list .dev:nth-child(1) .badge').textContent()) === 'NDI', 'NDI source gets its own badge, not "CÂMERA"');
  await clearSent();
  await humanClick('#dev-list .dev:nth-child(1) .assign button', 3);
  ok((await sent()).includes('devassign|3|ndi:169.254.1.1:5961'), 'assigning an NDI source sends its "ndi:" id');
  await page.evaluate(a => window.__hbMessage(a), { t: 'devices', busy: false, ndiMsg: 'Instale o NDI Runtime (ndi.video/tools) para detectar câmeras NDI na rede.', list: [] });
  await page.waitForTimeout(100);
  ok((await page.locator('#ndi-note').textContent()).includes('NDI Runtime'), 'missing NDI Runtime shows a clear install hint');
  await page.evaluate(a => window.__hbMessage(a), { t: 'devices', busy: false, ndiMsg: '', list: [] });
  await page.waitForTimeout(100);
  ok(await page.locator('#ndi-note').isHidden(), 'the hint disappears once the Runtime note is cleared');
  await setState({});

  console.log('\n[4b] PLUGINS');
  await tab('plg');
  await page.evaluate(() => window.__hbMessage({ t: 'plugins', ver: 1, dir: 'C:\\Users\\x\\AppData\\Local\\HBStudio\\plugins', list: [
    { id: 'com.t.theme', name: 'Tema Meia-noite', version: '1.0.0', author: 'HB', description: 'Azul escuro', license: 'MIT', type: 'theme', valid: true, supported: true, enabled: false, loaded: false, removed: false, error: '' },
    { id: 'com.t.src', name: 'Fontes de exemplo', version: '1.0.0', author: 'HB', description: 'Duas fontes', license: 'MIT', type: 'source', valid: true, supported: true, enabled: true, loaded: true, removed: false, error: '' },
    { id: 'com.t.bad', name: 'Quebrado', version: '0.1.0', author: '', description: '', license: '', type: 'source', valid: true, supported: true, enabled: false, loaded: false, removed: false, error: 'a biblioteca não confere com o sha256' }] }));
  await page.waitForTimeout(100);
  ok(await page.locator('#plg-list .plg').count() === 3, 'lists the 3 installed plugins');
  ok((await page.locator('#plg-list').innerText()).includes('a biblioteca não confere'), 'plugin error is shown to the user');
  await clearSent();
  await humanClick('#plg-install');
  ok((await sent()).includes('pluginpick'), 'INSTALAR PLUGIN… -> pluginpick (native file dialog)');
  await humanClick('#plg-list .plg:nth-child(1) .acts button', 0);
  ok((await sent()).includes('pluginenable|com.t.theme|1'), 'ATIVAR on the theme -> pluginenable|id|1');
  await humanClick('#plg-list .plg:nth-child(2) .acts button', 0);
  ok((await sent()).includes('pluginenable|com.t.src|0'), 'ATIVO (click) -> pluginenable|id|0');
  await humanClick('#plg-list .plg:nth-child(1) .acts button', 1);
  ok((await sent()).includes('pluginremove|com.t.theme'), 'Remover (after confirmation) -> pluginremove|id');
  // review dialog: theme (no risk) then native (consent required)
  await page.evaluate(() => window.__hbMessage({ t: 'plugin_review', ok: true, token: 'tok123', id: 'com.t.theme', name: 'Tema Meia-noite', version: '1.0.0', author: 'HB', description: 'x', license: 'MIT', type: 'theme', native: false, files: 1, size: 800, replaces: false, installedVersion: '', sha: 'abcdef0123456789' }));
  await page.waitForTimeout(100);
  ok(await page.locator('#modal3').evaluate(e => e.classList.contains('on')), 'review dialog opens');
  ok(!(await page.locator('#rv-risk').isVisible()) && !(await page.locator('#rv-ok').isDisabled()), 'theme: no risk warning, INSTALAR enabled');
  await clearSent();
  await humanClick('#rv-ok');
  ok((await sent()).includes('pluginconfirm|tok123'), 'INSTALAR -> pluginconfirm|token');
  await page.evaluate(() => window.__hbMessage({ t: 'plugin_review', ok: true, token: 'tok456', id: 'com.t.src', name: 'Fontes', version: '2.0.0', author: 'X', description: '', license: '', type: 'source', native: true, files: 2, size: 90000, replaces: true, installedVersion: '1.0.0', sha: '0011223344556677' }));
  await page.waitForTimeout(100);
  ok(await page.locator('#rv-risk').isVisible() && await page.locator('#rv-ok').isDisabled(), 'native: risk warning shown and INSTALAR disabled until consent');
  ok((await page.locator('#rv-body').innerText()).includes('substitui a 1.0.0'), 'review says it replaces version 1.0.0');
  await page.locator('#rv-ack').evaluate(e => e.click());
  ok(!(await page.locator('#rv-ok').isDisabled()), 'consent checkbox enables INSTALAR');
  await clearSent();
  await humanClick('#rv-cancel');
  ok((await sent()).includes('plugincancel|tok456') && !(await page.locator('#modal3').evaluate(e => e.classList.contains('on'))), 'CANCELAR -> plugincancel|token and dialog closes');
  await page.evaluate(() => window.__hbMessage({ t: 'plugin_review', ok: false, error: 'o arquivo não confere' }));
  await page.waitForTimeout(100);
  ok(await page.locator('#rv-ok').isHidden() && (await page.locator('#rv-err').innerText()).includes('não confere'), 'invalid package: error shown, no install button');
  await humanClick('#rv-cancel');
  // theme application is validated on the UI side too
  await page.evaluate(() => window.__hbMessage({ t: 'theme', id: 'x', scheme: 'dark', vars: { '--bg': '#0A1226', '--evil': 'url(http://x)', '--ink': 'red', '--blue': 'rgb(1,2,3)' } }));
  const css = await page.evaluate(() => ({ bg: document.documentElement.style.getPropertyValue('--bg'), evil: document.documentElement.style.getPropertyValue('--evil'),
    ink: document.documentElement.style.getPropertyValue('--ink'), scheme: document.documentElement.style.colorScheme }));
  ok(css.bg === '#0A1226' && css.scheme === 'dark', 'theme variables are applied');
  ok(css.evil === '' && css.ink === '', 'non-colour values (url(), names) are refused by the UI as well');
  await page.evaluate(() => window.__hbMessage({ t: 'theme', id: '', scheme: '', vars: {} }));
  ok((await page.evaluate(() => document.documentElement.style.getPropertyValue('--bg'))) === '', 'empty theme restores the default look');

  console.log('\n[5] ÁUDIO e CONFIG');
  await tab('aud');
  ok(await page.locator('#v-aud').isVisible(), 'ÁUDIO tab opens');
  await tab('cfg');
  ok(await page.locator('#v-cfg').isVisible(), 'CONFIG tab opens');
  // layout: nothing may spill out of its card (the formats table once escaped its box)
  const spill = await page.evaluate(() => {
    const out = [];
    document.querySelectorAll('#v-cfg .card').forEach((card, ci) => {
      const cr = card.getBoundingClientRect();
      card.querySelectorAll('*').forEach(el => {
        const r = el.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) return;
        if (r.bottom > cr.bottom + 1 || r.right > cr.right + 1 || r.left < cr.left - 1 || r.top < cr.top - 1)
          out.push(`card${ci} ${el.tagName}.${el.className} ${el.textContent.trim().slice(0, 20)}`);
      });
    });
    return out;
  });
  ok(spill.length === 0, 'CONFIG: every element stays inside its card' + (spill.length ? ' -> ' + spill.slice(0, 4).join(' | ') : ''));
  const rows = await page.locator('#v-cfg table tr').evaluateAll(trs => trs.map(t => { const r = t.getBoundingClientRect(); return [r.top, r.bottom]; }));
  let overlap = false;
  for (let i = 1; i < rows.length; i++) if (rows[i][0] < rows[i - 1][1] - 1) overlap = true;
  ok(rows.length >= 6 && !overlap, 'CONFIG: formats table rows are stacked without overlapping');


  console.log('\n[5b] ÁUDIO: canais, efeitos, gravação');
  const AUDIO = () => ({ t: 'audio', ver: 1, strips: Array.from({ length: 8 }, (_, i) => ({ i, name: ['MIC 1', 'MIC 2', 'LINHA', 'SISTEMA', 'FAIXA 5', 'FAIXA 6', 'FAIXA 7', 'FAIXA 8'][i],
      input: { kind: i === 0 ? 'device' : 'none', id: i === 0 ? 'cap1' : '', name: i === 0 ? 'Microfone USB' : '', state: i === 0 ? 'online' : 'off', msg: '' },
      trimDb: 0, faderDb: 0, mute: false, solo: false, pan: 0, phase: false, mono: false, delayMs: 0, quality: 'medium', toProgram: true, toMonitor: false,
      fx: i === 0 ? [{ type: 'hb.compressor', name: 'Compressor', bypass: false, missing: false, params: [
        { id: 'threshold_db', label: 'Limiar', unit: 'dB', min: -60, max: 0, def: -18, log: false, choices: [], value: -20 },
        { id: 'attack_ms', label: 'Ataque', unit: 'ms', min: 0.1, max: 200, def: 10, log: true, choices: [], value: 10 },
        { id: 'detect', label: 'Detecção', unit: '', min: 0, max: 1, def: 0, log: false, choices: ['RMS', 'Pico'], value: 0 }] },
      { type: 'vst3:abc123', name: 'ValhallaRoom (fixture)', bypass: false, missing: false, params: [] }] : [] })),
    master: { faderDb: 0, limiter: true, ceilingDb: -1 }, monitor: { deviceId: '', level: 0.75, mute: false, state: 'off', msg: '' },
    devices: [{ id: 'cap1', name: 'Microfone USB', kind: 'capture', isDefault: true }, { id: 'lb1', name: 'Alto-falantes', kind: 'loopback', isDefault: true }, { id: 'pb1', name: 'Fones', kind: 'playback', isDefault: true }],
    effects: [{ typeId: 'hb.compressor', name: 'Compressor', category: 'Dinâmica', source: 'builtin' }, { typeId: 'hb.highpass', name: 'Filtro passa-alta', category: 'Equalização', source: 'builtin' }] });
  await page.evaluate(a => window.__hbMessage(a), AUDIO());
  await tab('aud'); await page.waitForTimeout(100);
  ok(await page.locator('#strips .strip[data-i]').count() === 8, 'AUDIO: 8 channel strips are shown');
  ok(await page.locator('#strips .strip.master').count() === 1, 'AUDIO: the Program master strip is shown');
  const sent0 = await page.evaluate(() => window.__sent.length);
  await page.locator('.strip[data-i="1"] .tg button.m').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|strip|1|mute|1'), 'AUDIO: MUTE button sends aud|strip|1|mute|1');
  await page.locator('.strip[data-i="1"] .tg button.s').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|strip|1|solo|1'), 'AUDIO: SOLO button sends the command');
  await page.locator('.strip[data-i="0"] .fader').evaluate(el => { el.value = -12; el.dispatchEvent(new Event('input', { bubbles: true })); });
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|strip|0|faderDb|-12'), 'AUDIO: fader sends dB value');
  await page.locator('.strip[data-i="2"] select[data-k=input]').selectOption('device|lb1');
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|input|2|device|lb1'), 'AUDIO: input select sends device id (loopback offered)');
  await page.locator('.strip[data-i="0"]').click({ position: { x: 68, y: 20 } });
  await page.locator('.strip[data-i="0"] .st').click();
  ok(await page.locator('#fx-list .fxcard').count() === 2, 'AUDIO: effect chain of the selected channel is listed (compressor + VST3 fixture)');
  await page.locator('#fx-list .fxcard .prm input[type=range]').first().evaluate(el => { el.value = 300; el.dispatchEvent(new Event('input', { bubbles: true })); });
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).some(m => m.startsWith('aud|fxparam|0|0|threshold_db|')), 'AUDIO: effect slider sends aud|fxparam');
  await page.locator('#fx-list .fxcard').nth(1).locator('[data-a=editor]').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('fxeditor|0|1|open'), 'AUDIO: VST3 effect EDITOR button opens its native window');
  ok((await page.locator('#fx-list .fxcard').nth(1).textContent()).includes('própria interface do plugin'), 'AUDIO: VST3 card explains controls live in its own editor');
  ok(await page.locator('#fx-list .fxcard').first().locator('[data-a=editor]').count() === 0, 'AUDIO: a built-in effect does not get an EDITOR button');
  await page.locator('#fx-list .fxcard').first().locator('[data-a=byp]').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|fxbyp|0|0|1'), 'AUDIO: effect bypass toggle');
  await page.locator('#fx-add').selectOption('hb.highpass'); await page.locator('#fx-addbtn').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|fxadd|0|hb.highpass'), 'AUDIO: adding an effect from the list');
  await page.locator('#fx-list .fxcard').first().locator('[data-a=del]').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent0)).includes('aud|fxdel|0|0'), 'AUDIO: removing an effect');
  // meters move with levels
  await page.evaluate(() => window.__hbMessage({ t: 'audiolv', strips: Array.from({ length: 8 }, () => ({ pk: -12, hold: -6, rms: -20, ls: -23, gr: [4.2] })), master: { pk: -3, hold: -3, rms: -18, lm: -20, ls: -16.4, tp: -2, gr: 0 }, xr: 0 }));
  const mh = await page.locator('.strip[data-i="0"] .meter i').first().evaluate(el => parseFloat(el.style.height));
  ok(mh > 70 && mh < 90, 'AUDIO: level meter height follows the peak (' + mh.toFixed(0) + '%)');
  ok((await page.locator('#m-lufs').textContent()).includes('-16,4'), 'AUDIO: master shows LUFS');
  // a config update while dragging must not rebuild the strips
  // Same single-script trick as the T-bar test above: dispatch pointerdown and feed the config
  // update in one evaluate() call so there is no await boundary the interval timer could land in.
  // The preceding real clicks (del/add/bypass buttons, all inside #v-aud) each end with their own
  // real pointerup, which schedules `setTimeout(audFlush, 0)` to clear audDown shortly after. Flush
  // that queue first so none of those stale, already-pending timeouts can land audDown=false right
  // after we set it true below (observed to cause exactly that: a leftover audFlush firing between
  // our dispatch and the render check, even though our own two statements are one sync script).
  await page.evaluate(() => new Promise(resolve => setTimeout(resolve, 0)));
  await page.evaluate(a => {
    window.__marker = document.querySelector('.strip[data-i="3"]');
    // audDown is set by a document-level listener (it matches e.target.closest('#v-aud')), so the
    // dispatched event must bubble - unlike the T-bar's listener, which is attached directly on
    // the element itself.
    document.querySelector('.strip[data-i="3"] .fader').dispatchEvent(new Event('pointerdown', { bubbles: true }));
    window.__hbMessage(a);
  }, AUDIO());
  ok(await page.evaluate(() => window.__marker === document.querySelector('.strip[data-i="3"]')), 'AUDIO: strips are not rebuilt while the operator drags a fader');
  await page.evaluate(() => document.dispatchEvent(new Event('pointerup'))); await page.waitForTimeout(50);

  const REC = () => ({ t: 'recprofiles', ver: 1, profiles: [
      { id: 'mp4-h264-aac', label: 'MP4 — H.264 + AAC (recomendado)', desc: 'Compatível com tudo.', ext: 'mp4', kind: 'video', video: true, audio: true },
      { id: 'avi-mjpeg', label: 'AVI — Motion JPEG', desc: 'Edição fácil.', ext: 'avi', kind: 'video', video: true, audio: true },
      { id: 'wav', label: 'WAV 24 bits', desc: 'Sem perdas.', ext: 'wav', kind: 'audio', video: false, audio: true },
      { id: 'flac', label: 'FLAC', desc: 'Sem perdas, menor.', ext: 'flac', kind: 'audio', video: false, audio: true }],
    cfg: { profile: 'mp4-h264-aac', folder: 'C:\\Users\\x\\Videos\\HB Studio', template: 'HBStudio_{date}_{time}', videoKbps: 8000, videoQuality: 85, audioKbps: 192, audioRate: 48000, audioBits: 24, keyframeSec: 2, splitMin: 0, splitMB: 0, minFreeMB: 500 },
    video: { w: 1920, h: 1080, fps: 30 } });
  await page.evaluate(a => window.__hbMessage(a), REC());
  await tab('rec'); await page.waitForTimeout(100);
  ok(await page.locator('#v-rec .fmt').count() === 4, 'REC: lists every available format');
  ok(await page.locator('#v-rec .fmt.on').count() === 1, 'REC: the current format is highlighted');
  ok(await page.locator('label[data-for=vk]').isVisible() && !(await page.locator('label[data-for=vq]').isVisible()), 'REC: MP4 shows bitrate, hides JPEG quality');
  const sent1 = await page.evaluate(() => window.__sent.length);
  await page.locator('#v-rec .fmt', { hasText: 'FLAC' }).click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent1)).includes('rec|cfg|profile|flac'), 'REC: choosing a format sends rec|cfg|profile');
  await page.locator('#rc-audioKbps').selectOption('128');
  ok((await page.evaluate(n => window.__sent.slice(n), sent1)).includes('rec|cfg|audioKbps|128'), 'REC: audio bitrate setting');
  await page.locator('#rc-pick').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent1)).includes('rec|pickfolder'), 'REC: folder picker requested');
  await page.locator('#rec-btn').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent1)).includes('rec|start'), 'REC: header button starts recording');
  await page.evaluate(() => window.__hbMessage({ t: 'rec', state: 'recording', file: 'C:\\v\\HBStudio_1.mp4', seconds: 75, bytes: 52428800, freeBytes: 5e10, segment: 1, droppedVideo: 0, repeatedVideo: 2, droppedAudio: 0, error: '' }));
  ok((await page.locator('#rec-btn span').textContent()) === '1:15', 'REC: header shows elapsed time');
  ok((await page.locator('#rec-stat').textContent()).includes('50,0 MB'), 'REC: status shows file size');
  await page.locator('#rec-btn').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sent1)).includes('rec|stop'), 'REC: button stops while recording');
  await page.evaluate(() => window.__hbMessage({ t: 'rec', state: 'idle', error: 'Disco cheio', last: 'C:\\v\\a.mp4' }));
  ok((await page.locator('#rec-err').textContent()).includes('Disco cheio'), 'REC: errors are shown');


  console.log('\n[5c] Tema claro/escuro nativo (sem plugin)');
  await tab('cfg');
  ok(await page.evaluate(() => getComputedStyle(document.documentElement).colorScheme !== undefined), 'CONFIG: tema nativo presente (sem plugin algum instalado)');
  ok((await page.locator('#theme-seg button.on').getAttribute('data-th')) === 'system', 'TEMA: começa em "Sistema" (segue o Windows)');
  const sentT0 = await page.evaluate(() => window.__sent.length);
  const bgBefore = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  await page.locator('#theme-seg button[data-th=dark]').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sentT0)).includes('uitheme|dark'), 'TEMA: escolher ESCURO avisa o host (uitheme|dark)');
  ok(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === 'dark', 'TEMA: atributo data-theme="dark" aplicado');
  const bgAfter = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  ok(bgAfter !== bgBefore, 'TEMA: a cor de fundo realmente muda ao ligar o modo escuro');
  await page.locator('#theme-seg button[data-th=light]').click();
  ok(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === 'light', 'TEMA: escolher CLARO volta ao claro');
  await page.locator('#theme-seg button[data-th=system]').click();
  ok(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === null, 'TEMA: "Sistema" remove o atributo (volta a seguir o SO)');

  console.log('\n[5d] CONFIG: Saídas (monitor externo + NDI)');
  await clearSent();
  await page.locator('#out-pgm-open').click();
  ok((await sent()).includes('outwin|program|open'), 'ABRIR na janela de PROGRAM -> outwin|program|open');
  await page.locator('#out-pvw-close').click();
  ok((await sent()).includes('outwin|preview|close'), 'FECHAR na janela de PREVIEW -> outwin|preview|close');
  await page.evaluate(() => window.__hbMessage({ t: 'outputs', ndiEnabled: false, ndiName: 'HB Studio Program', ndiAvailable: true }));
  ok((await page.locator('#out-ndi-toggle').textContent()) === 'LIGAR', 'saída NDI desligada: botão mostra LIGAR');
  await clearSent();
  await page.locator('#out-ndi-toggle').click();
  ok((await sent()).includes('ndiout|on|HB Studio Program'), 'LIGAR envia ndiout|on com o nome configurado');
  await page.evaluate(() => window.__hbMessage({ t: 'outputs', ndiEnabled: true, ndiName: 'HB Studio Program', ndiAvailable: false }));
  ok((await page.locator('#out-ndi-toggle').textContent()) === 'DESLIGAR', 'saída NDI ligada: botão mostra DESLIGAR');
  ok((await page.locator('#out-ndi-note').textContent()).includes('NDI Runtime'), 'sem o NDI Runtime instalado, o aviso aparece mesmo com a saída "ligada"');
  await page.locator('#out-ndi-name').fill('Estúdio Principal');
  await clearSent();
  await page.locator('#out-ndi-name').dispatchEvent('change');
  ok((await sent()).includes('ndiout|on|Estúdio Principal'), 'mudar o nome com a saída ligada já reenvia ndiout|on com o novo nome');

  console.log('\n[5e] TRANSMISSÃO');
  await tab('xmit');
  ok((await page.locator('#xmit-list .devempty').count()) === 1, 'TRANSMISSÃO: lista vazia mostra a dica de "adicionar destino"');
  await page.evaluate(() => window.__hbMessage({
    t: 'xmit_dest', list: [
      { id: 'd1', name: 'YouTube culto', platform: 'youtube', protocol: 'rtmp', serverUrl: 'rtmp://a.rtmp.youtube.com/live2',
        keySet: true, keyHint: '••••ab12', enabled: true, encoder: 'auto', videoKbps: 6000, audioKbps: 160,
        useOwnFormat: false, width: 1920, height: 1080, fpsNum: 30, fpsDen: 1 },
      { id: 'd2', name: 'Twitch', platform: 'twitch', protocol: 'rtmp', serverUrl: 'rtmp://live.twitch.tv/app',
        keySet: false, keyHint: '', enabled: false, encoder: 'auto', videoKbps: 4500, audioKbps: 160,
        useOwnFormat: false, width: 1920, height: 1080, fpsNum: 30, fpsDen: 1 },
    ],
  }));
  ok((await page.locator('.xmitcard').count()) === 2, 'TRANSMISSÃO: os dois destinos aparecem como cards');
  ok((await page.locator('.xmitcard').nth(1).locator('[data-a=run]').isDisabled()), 'TRANSMISSÃO: INICIAR fica desabilitado para um destino desativado');
  await page.evaluate(() => window.__hbMessage({ t: 'xmit_status', list: [{ id: 'd1', running: true, publishing: true, message: 'Transmitindo.', videoUnits: 10, audioUnits: 20 }] }));
  ok((await page.locator('.xmitcard').first().locator('.xc-st').textContent()).trim() === 'AO VIVO', 'TRANSMISSÃO: status "publishing" mostra AO VIVO');

  await clearSent();
  await page.locator('.xmitcard').nth(1).locator('[data-a=enable]').click();
  ok((await sent()).includes('xmitenable|d2|1'), 'TRANSMISSÃO: ATIVAR envia xmitenable|id|1');

  await clearSent();
  await page.locator('.xmitcard').first().locator('[data-a=run]').click();
  ok((await sent()).includes('xmitstop|d1'), 'TRANSMISSÃO: destino em execução -> botão vira PARAR -> envia xmitstop');

  await page.click('#xmit-add');
  ok(await page.locator('#modal4.on').count() === 1, 'TRANSMISSÃO: "+ Adicionar destino" abre o editor');
  ok((await page.inputValue('#xm-server')) === 'rtmp://a.rtmp.youtube.com/live2', 'TRANSMISSÃO: plataforma padrão (YouTube) já preenche o servidor');
  await page.selectOption('#xm-platform', 'custom');
  await page.evaluate(() => document.querySelector('#xm-platform').dispatchEvent(new Event('change')));
  // "custom" has no preset; switching to it must not clobber text already in the server field.
  ok((await page.inputValue('#xm-server')) === 'rtmp://a.rtmp.youtube.com/live2', 'TRANSMISSÃO: trocar para "Personalizado" não apaga um servidor já preenchido');
  await page.fill('#xm-server', '');
  await clearSent();
  await page.click('#xm-save');
  ok((await page.locator('#xm-note').textContent()).length > 0, 'TRANSMISSÃO: salvar sem nome/servidor mostra aviso e não envia nada');
  ok((await sent()).length === 0, 'TRANSMISSÃO: nenhum comando é enviado quando a validação falha');

  await page.fill('#xm-name', 'Meu servidor RTMP');
  await page.fill('#xm-server', 'rtmp://meuservidor.exemplo.com/live');
  await page.fill('#xm-key', 'chave-123');
  await clearSent();
  await page.click('#xm-save');
  const savedMsg = (await sent()).find(m => m.startsWith('xmitsave|'));
  ok(!!savedMsg, 'TRANSMISSÃO: SALVAR envia xmitsave com o payload');
  const savedPayload = JSON.parse(savedMsg.slice('xmitsave|'.length));
  ok(savedPayload.name === 'Meu servidor RTMP' && savedPayload.serverUrl === 'rtmp://meuservidor.exemplo.com/live' &&
    savedPayload.streamKey === 'chave-123' && savedPayload.platform === 'custom', 'TRANSMISSÃO: payload salvo contém os campos preenchidos');
  ok(await page.locator('#modal4.on').count() === 0, 'TRANSMISSÃO: salvar fecha o editor');

  // Editing an existing destination never shows its real key, only a masked hint as a placeholder.
  await page.locator('.xmitcard').first().locator('[data-a=edit]').click();
  ok((await page.inputValue('#xm-key')) === '', 'TRANSMISSÃO: o campo de chave nunca vem preenchido com a chave real ao editar');
  ok((await page.getAttribute('#xm-key', 'placeholder')).includes('••••ab12'), 'TRANSMISSÃO: a dica mascarada aparece como placeholder');
  await clearSent();
  await page.click('#xm-save');   // leaving the key field blank on an edit must NOT wipe the stored key
  const editMsg = (await sent()).find(m => m.startsWith('xmitsave|'));
  const editPayload = JSON.parse(editMsg.slice('xmitsave|'.length));
  ok(editPayload.id === 'd1' && editPayload.streamKey === '', 'TRANSMISSÃO: editar sem mexer na chave envia streamKey vazio (o host mantém a chave salva)');

  await clearSent();
  await page.locator('.xmitcard').first().locator('[data-a=remove]').click();   // the test harness auto-accepts confirm()
  ok((await sent()).includes('xmitremove|d1'), 'TRANSMISSÃO: remover (após confirmar) envia xmitremove|id');

  await clearSent();
  await page.click('#xmit-startall');
  ok((await sent()).includes('xmitstartall'), 'TRANSMISSÃO: "INICIAR TODAS" envia xmitstartall');
  await page.click('#xmit-stopall');
  ok((await sent()).includes('xmitstopall'), 'TRANSMISSÃO: "ENCERRAR TODAS" envia xmitstopall');

  console.log('\n[6] Layout em janelas diferentes');
  // The PRODUÇÃO stage (monitors + switcher + 16-camera strip) uses a 3-row CSS grid where the
  // monitors and inputs rows flex to fill whatever's left after the switcher card's own natural
  // size — by construction this always sums to exactly the stage's height, so it must never need
  // to scroll, down to the app's own declared minimum window size (webview_host's minWidth x
  // minHeight, 1100x680).
  for (const [w, h] of [[1100, 680], [1280, 800], [1366, 768], [1920, 1080]]) {
    await page.setViewportSize({ width: w, height: h });
    await tab('prod'); await page.waitForTimeout(100);
    const tileCount = await page.locator('.tile').count();
    ok(tileCount === 16, `${w}x${h}: all 16 input tiles are rendered (found ${tileCount})`);
    const tileH = await page.locator('.tile').first().evaluate(el => el.getBoundingClientRect().height);
    ok(tileH > 10, `${w}x${h}: tiles have a real, non-collapsed height (${Math.round(tileH)}px)`);
    const noScroll = await page.evaluate(() => {
      const stage = document.querySelector('.stage');
      return stage.scrollHeight <= stage.clientHeight + 1;
    });
    ok(noScroll, `${w}x${h}: the production stage fits without scrolling`);
  }
  for (const [w, h] of [[1280, 800], [1024, 720], [1920, 1080]]) {
    await page.setViewportSize({ width: w, height: h });
    for (const v of ['xmit', 'prod', 'cam', 'aud', 'rec', 'plg', 'cfg']) {
      await tab(v);
      const hs = await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1);
      ok(hs, `${w}x${h} ${v}: no horizontal overflow`);
    }
  }
  await tab('prod');

  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors[0] : ''));
  await browser.close();
  console.log(`\n${checks - failures}/${checks} checks passed`);
  process.exit(failures ? 1 : 0);
})();
