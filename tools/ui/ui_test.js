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

const SLOT = (k, st, name, id, w, h, en) => ({ k, st, name: name || '', id: id || '', w: w || 0, h: h || 0, fps: k === 'dev' ? 30 : 0, msg: '', en: en === undefined ? true : en });
function stateMsg(over = {}) {
  return Object.assign({
    t: 's', pvw: 1, pgm: 0, transition: false, progress: 0, fps: 29.9, target: 30, pw: 1920, ph: 1080, frames: 1000, skipped: 0,
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
  ok(await page.locator('.tile:not(.scene)').count() === 16, '16 input tiles are shown (2 rows of 8)');
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

  console.log('\n[4b] CÂMERAS: ligar/desligar e configurações por câmera');
  await setState({ slots: [SLOT('dev', 'online', 'OBS Virtual Camera', 'obs', 1920, 1080),
    SLOT('dev', 'off', 'Logitech C920', 'cam', 0, 0, false), ...Array.from({ length: 14 }, () => SLOT('test', 'test'))] });
  await page.waitForTimeout(100);
  const slot0 = page.locator('#slot-list .slot').nth(0), slot1 = page.locator('#slot-list .slot').nth(1);
  ok(await slot0.locator('.pwr').count() === 1, 'CÂMERAS: uma câmera atribuída tem botão de ligar/desligar');
  ok(!(await slot0.locator('.pwr').evaluate(e => e.classList.contains('off'))), 'CÂMERAS: câmera ligada mostra o botão "aceso"');
  ok(await slot1.locator('.pwr').evaluate(e => e.classList.contains('off')), 'CÂMERAS: câmera desligada mostra o botão apagado');
  ok((await slot1.locator('.st').textContent()).trim() === 'DESLIGADA', 'CÂMERAS: status da câmera desligada aparece como DESLIGADA, mantendo o nome');
  await clearSent();
  await slot0.locator('.pwr').click();
  ok((await sent()).includes('devdisable|0'), 'CÂMERAS: clicar no botão de uma câmera ligada envia devdisable');
  await clearSent();
  await slot1.locator('.pwr').click();
  ok((await sent()).includes('devenable|1'), 'CÂMERAS: clicar no botão de uma câmera desligada envia devenable');

  await clearSent();
  await slot0.locator('.cfg').click();
  ok(await page.locator('#modal5.on').count() === 1, 'CÂMERAS: engrenagem abre o diálogo de configurações');
  ok((await sent()).includes('devformats|obs'), 'CÂMERAS: abrir o diálogo pede os formatos suportados pela câmera');
  ok((await page.locator('#camcfg-note').textContent()).length > 0, 'CÂMERAS: mostra uma mensagem enquanto busca os formatos');
  await page.evaluate(() => window.__hbMessage({ t: 'devformats', id: 'obs', list: [
    { w: 1920, h: 1080, fpsNum: 30, fpsDen: 1 }, { w: 1280, h: 720, fpsNum: 60, fpsDen: 1 }] }));
  ok(await page.locator('#camcfg-res option').count() === 3, 'CÂMERAS: lista "Automática" + os formatos recebidos');
  await page.selectOption('#camcfg-res', '1280,720,60,1');
  await clearSent();
  await page.click('#camcfg-save');
  ok((await sent()).includes('devformat|0|1280|720|60|1'), 'CÂMERAS: salvar a resolução escolhida envia devformat com os números certos');
  ok(await page.locator('#modal5.on').count() === 0, 'CÂMERAS: salvar fecha o diálogo');

  // a devformats reply for a camera the operator is no longer looking at must be ignored
  await slot1.locator('.cfg').click();
  await page.evaluate(() => window.__hbMessage({ t: 'devformats', id: 'obs', list: [{ w: 9999, h: 9999, fpsNum: 1, fpsDen: 1 }] }));
  ok(await page.locator('#camcfg-res option').count() === 1, 'CÂMERAS: uma resposta de devformats atrasada de outra câmera é ignorada');
  await page.click('#camcfg-cancel');

  // ---- FEATURE: chroma ---- "Fundo" section of the camera-settings dialog
  const chromaSlot = (o = {}) => Object.assign({ mode: 'off', q: 'med', repl: 'color', color: '#101010', tol: 45, soft: 25, spill: 50, blur: 40,
    applyMs: 0, inferMs: 0, inferences: 0, dropped: 0, img: '', imgErr: '', aiActive: false, aiWaiting: false, aiFailed: false, aiMsg: '', slowAi: false, slowApply: false }, o);
  const chromaState = (over = {}, slot0 = {}) => Object.assign({ t: 'chroma', runtime: false, images: true,
    model: { state: 'none', received: 0, total: 462338, mb: 0.44, msg: '', license: 'Apache-2.0' }, ai: { state: 'idle', msg: '' },
    slots: Array.from({ length: 16 }, (_, i) => chromaSlot(i === 0 ? slot0 : {})) }, over);
  const feedChroma = o => page.evaluate(a => window.__hbMessage(a), o);
  await feedChroma(chromaState());
  await slot0.locator('.cfg').click();
  ok(await page.locator('#chr').isVisible(), 'FUNDO: a seção aparece no diálogo da câmera');
  ok(await page.$eval('#chr-ai-opt', e => e.disabled), 'FUNDO: sem ONNX Runtime a opção IA fica desabilitada e explicada');
  ok((await page.locator('#chr-model').textContent()).includes('ONNX'), 'FUNDO: mensagem honesta sobre o build sem runtime');
  ok(!(await page.locator('#chr-tol').isVisible()), 'FUNDO: desligado esconde os controles');
  await clearSent();
  await page.selectOption('#chr-mode', 'green');
  ok((await sent()).includes('chroma|0|mode|green'), 'FUNDO: escolher Chroma verde envia chroma|slot|mode|green');
  await feedChroma(chromaState({}, { mode: 'green' }));
  ok(await page.locator('#chr-tol').isVisible() && await page.locator('#chr-spill').isVisible() && !(await page.locator('#chr-q').isVisible()),
    'FUNDO: chroma clássico mostra tolerância/suavidade/reflexo e não a qualidade da IA');
  await clearSent();
  await page.locator('#chr-tol').evaluate(el => { el.value = 70; el.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.waitForTimeout(200);
  ok((await sent()).includes('chroma|0|tol|70'), 'FUNDO: slider de tolerância envia chroma|slot|tol|valor');
  await page.selectOption('#chr-repl', 'image');
  await feedChroma(chromaState({}, { mode: 'green', repl: 'image' }));
  await clearSent();
  await page.click('#chr-img');
  ok((await sent()).includes('chromaimg|0'), 'FUNDO: ESCOLHER… pede o seletor de imagem ao host');
  await page.selectOption('#chr-repl', 'blur');
  await feedChroma(chromaState({}, { mode: 'green', repl: 'blur' }));
  ok(await page.locator('#chr-blur').isVisible() && !(await page.locator('#chr-color').isVisible()), 'FUNDO: desfoque mostra só a intensidade');
  // AI available, model missing -> download flow
  await feedChroma(chromaState({ runtime: true }, { mode: 'ai' }));
  ok(!(await page.$eval('#chr-ai-opt', e => e.disabled)), 'FUNDO: com runtime a opção IA fica disponível');
  ok(await page.locator('#chr-dl').isVisible() && await page.locator('#chr-q').isVisible(), 'FUNDO: IA sem modelo oferece BAIXAR MODELO e a qualidade');
  await clearSent();
  await page.click('#chr-dl');
  ok((await sent()).includes('chromamodel|download'), 'FUNDO: BAIXAR MODELO envia chromamodel|download');
  await feedChroma(chromaState({ runtime: true, model: { state: 'downloading', received: 231169, total: 462338, mb: 0.44, msg: '', license: 'Apache-2.0' } }, { mode: 'ai' }));
  ok((await page.locator('#chr-model').textContent()).includes('50%') && await page.locator('#chr-bar').isVisible() && await page.locator('#chr-cancel').isVisible(),
    'FUNDO: mostra o progresso do download com cancelar');
  await feedChroma(chromaState({ runtime: true, model: { state: 'error', received: 0, total: 0, mb: 0.44, msg: 'O arquivo baixado não confere com a assinatura (SHA-256) esperada e foi descartado.', license: 'x' } }, { mode: 'ai' }));
  ok((await page.locator('#chr-model').textContent()).includes('SHA-256') && (await page.locator('#chr-dl').textContent()).includes('TENTAR'), 'FUNDO: erro de download é mostrado com TENTAR DE NOVO');
  await feedChroma(chromaState({ runtime: true, model: { state: 'ready', received: 1, total: 1, mb: 0.44, msg: '', license: 'Apache-2.0' }, ai: { state: 'ready', msg: '' } },
    { mode: 'ai', aiActive: true, inferMs: 95, inferences: 40, slowAi: true }));
  ok((await page.locator('#chr-warn').textContent()).includes('lento para IA') && (await page.locator('#chr-warn').textContent()).includes('reduza a qualidade'),
    'FUNDO: aviso "seu computador está lento para IA: reduza a qualidade"');
  await feedChroma(chromaState({ runtime: true, model: { state: 'ready', received: 1, total: 1, mb: 0.44, msg: '', license: 'Apache-2.0' } }, { mode: 'ai', aiActive: true, inferMs: 20, inferences: 40 }));
  ok(!(await page.locator('#chr-warn').isVisible()), 'FUNDO: sem aviso quando a IA acompanha');
  for (const [w, h] of [[1024, 720], [1280, 800]]) {
    await page.setViewportSize({ width: w, height: h });
    const fit = await page.evaluate(() => { const s = document.querySelector('#modal5 .sheet'); return [s.scrollHeight, s.clientHeight, document.documentElement.scrollWidth, innerWidth]; });
    ok(fit[0] <= fit[1] + 1 && fit[2] <= fit[3] + 1, `${w}x${h}: diálogo com a seção FUNDO cabe sem rolagem (${fit.join('/')})`);
  }
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.click('#camcfg-cancel');
  // ---- /FEATURE: chroma ----

  console.log('\n[4b2] PRODUÇÃO: menu de contexto (botão direito) nos quadros de entrada');
  await tab('prod'); await page.waitForTimeout(100);
  await page.locator('.tile[data-i="0"]').click({ button: 'right' });
  ok(await page.locator('#tilemenu.on').count() === 1, 'botão direito num quadro com câmera abre o menu');
  ok(await page.locator('#tm-deactivate').isEnabled(), 'câmera ligada: "Desligar" está habilitado');
  ok(!(await page.locator('#tm-activate').isEnabled()), 'câmera ligada: "Ativar" está desabilitado');
  await clearSent();
  await page.click('#tm-deactivate');
  ok((await sent()).includes('devdisable|0'), 'menu de contexto "Desligar" envia devdisable');
  ok(await page.locator('#tilemenu.on').count() === 0, 'escolher uma ação fecha o menu');

  await page.locator('.tile[data-i="1"]').click({ button: 'right' });
  ok(await page.locator('#tm-activate').isEnabled(), 'câmera desligada: "Ativar" está habilitado');
  ok(!(await page.locator('#tm-deactivate').isEnabled()), 'câmera desligada: "Desligar" está desabilitado');
  await page.click('#tm-cfg');
  ok(await page.locator('#modal5.on').count() === 1, 'menu de contexto "Configurações…" abre o diálogo da câmera');
  await page.click('#camcfg-cancel');

  await page.locator('.tile[data-i="2"]').click({ button: 'right' });   // a pure test-pattern slot, no device ever assigned
  ok(!(await page.locator('#tm-activate').isEnabled()) && !(await page.locator('#tm-deactivate').isEnabled()) && !(await page.locator('#tm-cfg').isEnabled()),
    'quadro sem câmera atribuída: todas as ações do menu ficam desabilitadas');
  await page.keyboard.press('Escape');
  ok(await page.locator('#tilemenu.on').count() === 0, 'Esc fecha o menu de contexto');
  await setState({});

  console.log('\n[4c] PLUGINS');
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

  // ---- FEATURE: marketplace ----
  console.log('\n[4d] PLUGINS: Marketplace');
  await clearSent();
  await humanClick('#plg-seg button[data-s=mkt]');
  ok((await sent()).includes('mktlist') && (await sent()).includes('mktrefresh'), 'MARKETPLACE sub-view: asks the host for the cached list and a refresh');
  ok(await page.locator('#mk-grid').isVisible() && !(await page.locator('#plg-list').isVisible()), 'MARKETPLACE replaces the installed list');
  const mkE = (o) => Object.assign({ id: 'com.x.a', name: 'A', author: 'Autor', desc: 'Descrição', cat: 'theme', type: 'theme', native: false, verified: false, license: 'MIT', homepage: '', repository: '',
    tags: [], screenshots: [], permissions: [], compatible: true, reason: '', version: '1.0.0', size: 2048, minApp: '0.1.0', notes: '', released: '', status: 'available', installedVersion: '' }, o);
  const mkCat = (list, over) => page.evaluate(m => window.__hbMessage(m), Object.assign({ t: 'mkt_catalog', loaded: true, offline: false, fromCache: false, error: '', dropped: 0, installing: false,
    catalogUrl: 'https://raw.example.com/index.json', defaultUrl: 'https://raw.example.com/index.json', list }, over || {}));
  await mkCat([
    mkE({ id: 'com.x.theme', name: 'Tema Aurora', verified: true, status: 'installed', installedVersion: '1.0.0', tags: ['escuro'], screenshots: ['https://example.com/1.png', 'javascript:alert(1)'] }),
    mkE({ id: 'com.x.src', name: 'Fonte Câmera X', author: 'Beltrano', cat: 'video-source', type: 'source', native: true, permissions: ['native-code'], size: 3 * 1048576 }),
    mkE({ id: 'com.x.fx', name: 'Efeito Eco', cat: 'audio-effect', type: 'effect', native: true, status: 'update', installedVersion: '0.9.0', version: '1.1.0' }),
    mkE({ id: 'com.x.old', name: 'Futuro', compatible: false, reason: 'requer o HB Studio 9.0.0 ou mais novo' }),
    mkE({ id: 'com.x.xss', name: '<img src=x onerror=window.__pwned=1>', desc: '<b>negrito</b>' }),
  ]);
  await page.waitForTimeout(100);
  ok(await page.locator('#mk-grid .mk-card').count() === 5, 'MARKETPLACE: 5 cards');
  const grid = await page.locator('#mk-grid').innerText();
  ok(grid.includes('VERIFICADO') && grid.includes('NÃO VERIFICADO') && grid.includes('CÓDIGO NATIVO') && grid.includes('ATUALIZAR') && grid.includes('INSTALADO'),
    'cards show verified / not verified / native code / update / installed');
  ok(!(await page.evaluate(() => window.__pwned)) && await page.locator('#mk-grid img').count() === 0, 'catalog text is rendered as text, never as HTML');
  await page.fill('#mk-q', 'eco');
  ok(await page.locator('#mk-grid .mk-card').count() === 1, 'search filters by name');
  await page.fill('#mk-q', '');
  await page.selectOption('#mk-cat', 'video-source');
  ok(await page.locator('#mk-grid .mk-card').count() === 1 && (await page.locator('#mk-grid').innerText()).includes('Fonte Câmera X'), 'category filter');
  await page.selectOption('#mk-cat', '');
  ok(await page.locator('.mk-card[data-id="com.x.old"] .mk-act').isDisabled(), 'incompatible entry cannot be installed');
  ok(await page.locator('.mk-card[data-id="com.x.theme"] .mk-act').isDisabled(), 'installed entry: button disabled');
  await clearSent();
  await humanClick('.mk-card[data-id="com.x.src"] .mk-act');
  ok((await sent()).includes('mktinstall|com.x.src'), 'INSTALAR -> mktinstall|id');
  await page.evaluate(() => window.__hbMessage({ t: 'mkt_progress', id: 'com.x.src', phase: 'download', pct: 42 }));
  await page.locator('.mk-card[data-id="com.x.src"]').click();
  await page.waitForTimeout(60);
  ok(await page.locator('#mk-drawer').isVisible(), 'click on a card opens the details drawer');
  const dr = await page.locator('#mk-drawer').innerText();
  ok(dr.includes('Publicador não verificado') && dr.includes('código nativo') && dr.includes('sandbox'), 'drawer: not-verified and native-code warnings');
  await page.evaluate(() => window.__hbMessage({ t: 'mkt_done', id: 'com.x.src', ok: true, message: '' }));
  await page.locator('.mk-card[data-id="com.x.theme"]').click();
  await page.waitForTimeout(60);
  ok(await page.locator('#mk-dbody img[loading=lazy]').count() === 1, 'drawer: only https screenshots, lazy loaded');
  await clearSent();
  await page.locator('#mk-dbody button', { hasText: 'REMOVER' }).click();
  ok((await sent()).includes('pluginremove|com.x.theme'), 'REMOVER (after confirmation) -> pluginremove|id (existing flow)');
  // review dialog banner for unverified publishers
  await page.evaluate(() => { window.__hbMessage({ t: 'mkt_review', id: 'com.x.src', verified: false, native: true });
    window.__hbMessage({ t: 'plugin_review', ok: true, token: 'tk9', id: 'com.x.src', name: 'Fonte Câmera X', version: '1.0.0', author: 'B', description: '', license: 'MIT', type: 'source', native: true, files: 2, size: 9000, replaces: false, installedVersion: '', sha: 'aa' }); });
  await page.waitForTimeout(80);
  ok(await page.locator('#rv-unver').isVisible() && await page.locator('#rv-risk').isVisible() && await page.locator('#rv-ok').isDisabled(), 'review dialog from the marketplace: "publisher not verified" shown, consent still required');
  await humanClick('#rv-cancel');
  await page.evaluate(() => window.__hbMessage({ t: 'plugin_review', ok: true, token: 'tk10', id: 'com.x.file', name: 'Arquivo', version: '1.0.0', author: 'B', description: '', license: '', type: 'theme', native: false, files: 1, size: 9, replaces: false, installedVersion: '', sha: 'bb' }));
  await page.waitForTimeout(60);
  ok(!(await page.locator('#rv-unver').isVisible()), 'banner is not shown for a file installed by hand');
  await humanClick('#rv-cancel');
  // offline / empty states
  await mkCat([], { loaded: false, offline: true, error: 'Não foi possível acessar o catálogo (sem internet).' });
  await page.waitForTimeout(60);
  ok((await page.locator('#mk-msg').innerText()).includes('sem internet') && (await page.locator('#mk-grid').innerText()).includes('Catálogo indisponível'), 'offline without cache: graceful message');
  await mkCat([], {});
  await page.waitForTimeout(60);
  ok((await page.locator('#mk-grid').innerText()).includes('catálogo está vazio'), 'empty catalog state');
  await mkCat([mkE({}), mkE({ id: 'com.x.b', name: 'B' })], { error: 'Sem conexão com o catálogo; mostrando a última cópia salva.', offline: true, fromCache: true });
  await page.waitForTimeout(60);
  ok(await page.locator('#mk-msg').isVisible() && await page.locator('#mk-grid .mk-card').count() === 2, 'offline with cache: copy is shown with a notice');
  for (const [w, h] of [[1024, 720], [1280, 800]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.locator('.mk-card').first().click();
    const fit = await page.evaluate(() => ({ x: document.documentElement.scrollWidth <= window.innerWidth + 1, y: document.documentElement.scrollHeight <= window.innerHeight + 1,
      drawer: document.querySelector('#mk-drawer').getBoundingClientRect().right <= document.querySelector('#v-plg').getBoundingClientRect().right + 1 }));
    ok(fit.x && fit.y && fit.drawer, `${w}x${h}: marketplace fits without page scrolling`);
  }
  await page.evaluate(() => { document.querySelector('#mk-url-save').click(); });
  ok((await sent()).some(m => m.startsWith('mktcfg|')), 'CONFIG catalog URL: SALVAR -> mktcfg|url');
  await humanClick('#plg-seg button[data-s=inst]');
  ok(await page.locator('#plg-list').isVisible(), 'back to INSTALADOS');
  // ---- /FEATURE ----

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
  const rows = await page.locator('#outfmt-list .fmt').evaluateAll(trs => trs.map(t => { const r = t.getBoundingClientRect(); return [r.top, r.bottom]; }));
  let overlap = false;
  for (let i = 1; i < rows.length; i++) if (rows[i][0] < rows[i - 1][1] - 1) overlap = true;
  ok(rows.length === 6 && !overlap, 'CONFIG: output format presets are stacked without overlapping');

  console.log('\n[5a2] CONFIG: Formatos de saída comuns (selecionáveis)');
  await page.evaluate(() => window.__hbMessage({ t: 'outfmt', w: 1920, h: 1080, fpsNum: 30, fpsDen: 1, activeW: 1920, activeH: 1080, activeFpsNum: 30, activeFpsDen: 1 }));
  ok((await page.locator('#outfmt-list .fmt.on').count()) === 1, 'the saved preset is marked selected');
  ok((await page.locator('#outfmt-list .fmt.on .ext').textContent()).includes('ativo agora'), 'when saved == active, it reads "ativo agora"');
  await clearSent();
  const fmt720p60 = page.locator('#outfmt-list .fmt', { hasText: '720p60' });
  await fmt720p60.click();
  ok((await sent()).includes('outfmt|1280|720|60|1'), 'clicking a preset sends outfmt with its w/h/fps');
  await page.evaluate(() => window.__hbMessage({ t: 'outfmt', w: 1280, h: 720, fpsNum: 60, fpsDen: 1, activeW: 1920, activeH: 1080, activeFpsNum: 30, activeFpsDen: 1 }));
  ok((await fmt720p60.evaluate(e => e.classList.contains('on'))), 'the newly chosen preset becomes the one marked selected');
  ok((await fmt720p60.locator('.ext').textContent()).includes('aplica ao reiniciar'), 'a pending choice different from the active format says it needs a restart');
  ok((await page.locator('#outfmt-note').textContent()).includes('próxima vez'), 'the note explains the change only applies on next launch');
  await page.evaluate(() => window.__hbMessage({ t: 'outfmt', w: 1920, h: 1080, fpsNum: 30, fpsDen: 1, activeW: 1920, activeH: 1080, activeFpsNum: 30, activeFpsDen: 1 }));
  await setState({ pw: 1280, ph: 720 });
  await tab('prod'); await page.waitForTimeout(100);
  ok((await page.locator('#pgm-res').textContent()).includes('1280×720'), 'PROGRAM resolution readout follows the real active output size (s.pw/s.ph), not a hardcoded 1920×1080');
  await setState({});

  console.log('\n[5a3] CONFIG: painel de Registro (logs)');
  await tab('cfg');
  const logLines = Array.from({ length: 12 }, (_, i) => ({ t: `12:00:0${i % 10} [INFO] [cam] linha ${i}`, l: i === 5 ? 'WARNING' : i === 9 ? 'ERROR' : 'INFO' }));
  await setState({ log: logLines });
  await page.waitForTimeout(100);
  ok((await page.locator('#log-cfg div').count()) === 12, 'the CONFIG log panel shows every line sent, not just the last 9 like the small sidebar widget');
  ok((await page.locator('#log-cfg div.w').count()) === 1, 'a WARNING line gets the warning style');
  ok((await page.locator('#log-cfg div.e').count()) === 1, 'an ERROR line gets the error style');
  await clearSent();
  await page.evaluate(() => { navigator.clipboard.writeText = t => { window.__copied = t; return Promise.resolve(); }; });
  await page.locator('#log-cfg-copy').click();
  await page.waitForTimeout(50);
  const copied = await page.evaluate(() => window.__copied);
  ok(typeof copied === 'string' && copied.split('\n').length === 12 && copied.includes('linha 5'), 'COPIAR REGISTRO copies every visible log line as plain text');
  ok((await page.locator('#log-cfg-note').textContent()).includes('copiado'), 'copying confirms success to the user');
  await setState({});

  console.log('\n[5a4] CONFIG: Qualidade do Preview (selecionável, aplica sem reiniciar)');
  const pqRows = await page.locator('#previewq-list .fmt').count();
  ok(pqRows === 4, 'four preview-quality presets are listed');
  ok((await page.locator('#previewq-list .fmt.on').count()) === 1, '"alta" (the default) is marked selected before any previewq message arrives');
  await clearSent();
  const pqBaixa = page.locator('#previewq-list .fmt', { hasText: 'Baixa' });
  await pqBaixa.click();
  ok((await sent()).includes('previewq|baixa'), 'clicking a preset sends previewq with its id');
  await page.evaluate(() => window.__hbMessage({ t: 'previewq', id: 'baixa' }));
  ok((await pqBaixa.evaluate(e => e.classList.contains('on'))), 'the chosen preset becomes the one marked selected');
  ok((await page.locator('#previewq-list .fmt.on').count()) === 1, 'only one preset is marked selected at a time');
  await page.evaluate(() => window.__hbMessage({ t: 'previewq', id: 'alta' }));

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

  // ---- FEATURE: multitrack ----
  const ISO = (extra) => Object.assign({ t: 'rec_iso', locked: false,
    cfg: { res: 'native', fps: 'program', vkbps: 0, afmt: 'wav', tap: 'pre', rotmin: 0, expert: false },
    cams: [{ slot: 0, label: 'CAM 1', device: 'Logitech C920', live: true, w: 1920, h: 1080, fps: 30, sel: true }, { slot: 1, label: 'CAM 2', device: '', live: false, w: 0, h: 0, fps: 0, sel: false }],
    chans: [{ strip: 0, name: 'MIC 1', sel: true }, { strip: 1, name: 'MIC 2', sel: false }],
    formats: [{ id: 'wav', label: 'WAV 24 bits' }, { id: 'wav-f32', label: 'WAV float' }],
    est: { video: 1, audio: 1, mbps: 2.3, gbh: 8.3, load: 66.7, maxCams: 2, minutes: 120, tier: 'MEDIUM', verdict: 'ok', notes: [] },
    sess: { active: false, dir: '' } }, extra || {});
  await page.evaluate(m => window.__hbMessage(m), ISO());
  ok(await page.locator('#mt-card').isVisible(), 'MULTIPISTA: the section is shown in the REC tab');
  ok(await page.locator('#mt-cams input').count() === 2 && await page.locator('#mt-chans input').count() === 2, 'MULTIPISTA: one checkbox per camera and per audio channel');
  ok(await page.locator('#mt-cams input').first().isChecked(), 'MULTIPISTA: the saved selection is reflected');
  ok((await page.locator('#mt-est').textContent()).includes('2,3 MB/s'), 'MULTIPISTA: shows the disk-rate estimate');
  ok((await page.locator('#mt-est').textContent()).includes('67%'), 'MULTIPISTA: shows the encode load against the safe cap');
  const sentM = await page.evaluate(() => window.__sent.length);
  await page.locator('#mt-chans input').nth(1).check();
  await page.locator('#mt-cams input').first().uncheck();
  await page.selectOption('#mt-afmt', 'wav-f32');
  await page.selectOption('#mt-tap', 'post');
  await page.fill('#mt-rotmin', '10'); await page.locator('#mt-rotmin').dispatchEvent('change');
  await page.locator('#mt-expert').check();
  const smm = await page.evaluate(n => window.__sent.slice(n), sentM);
  ok(smm.includes('rec|iso|ch|1|1') && smm.includes('rec|iso|cam|0|0'), 'MULTIPISTA: checkboxes send rec|iso|ch / rec|iso|cam');
  ok(smm.includes('rec|iso|cfg|afmt|wav-f32') && smm.includes('rec|iso|cfg|tap|post') && smm.includes('rec|iso|cfg|rotmin|10') && smm.includes('rec|iso|cfg|expert|1'), 'MULTIPISTA: format / tap / rotation / expert send rec|iso|cfg');
  await page.locator('#mt-open').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sentM)).includes('rec|iso|open'), 'MULTIPISTA: open-folder button sends rec|iso|open');
  await page.evaluate(m => window.__hbMessage(m), ISO({ locked: true, est: { video: 6, audio: 1, mbps: 9, gbh: 32, load: 300, maxCams: 2, minutes: 4, tier: 'LOW', verdict: 'blocked', notes: ['Carga alta'] },
    sess: { active: true, seconds: 75, free: 5e10, dir: 'C:\\v\\2026', warnings: ['Espaço baixo'], tracks: [
      { id: 'CAM01', name: 'Logitech', kind: 'video', state: 'recording', error: '', seconds: 75, segment: 1, repeated: 3, dropped: 2, silence: 0, bytes: 1, drift: 0 },
      { id: 'CH01', name: 'MIC 1', kind: 'audio', state: 'error', error: 'disco removido', seconds: 10, segment: 1, repeated: 0, dropped: 0, silence: 480, bytes: 1, drift: 0 }] } }));
  ok(await page.locator('#mt-cams input').first().isDisabled(), 'MULTIPISTA: selection is locked while recording');
  ok((await page.locator('#mt-est').textContent()).includes('Bloqueado'), 'MULTIPISTA: a blocked plan is flagged');
  ok(await page.locator('#mt-tracks .mt-trk').count() === 3, 'MULTIPISTA: per-track live status rows (header + 2 tracks)');
  ok((await page.locator('#mt-tracks').textContent()).includes('disco removido'), 'MULTIPISTA: a failing track shows its own error');
  for (const theme of ['light', 'dark', 'gray']) {
    await page.evaluate(th => document.documentElement.setAttribute('data-theme', th), theme);
    const fits = await page.evaluate(() => { const c = document.querySelector('#mt-card').getBoundingClientRect(); return c.right <= window.innerWidth + 1 && document.documentElement.scrollWidth <= window.innerWidth + 1; });
    ok(fits, `MULTIPISTA: fits the width in the ${theme} theme`);
  }
  await page.evaluate(() => document.documentElement.setAttribute('data-theme', 'light'));
  await page.evaluate(m => window.__hbMessage(m), ISO());
  // ---- /FEATURE ----


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
  const sentT1 = await page.evaluate(() => window.__sent.length);
  const bgBeforeGray = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  await page.locator('#theme-seg button[data-th=gray]').click();
  ok((await page.evaluate(n => window.__sent.slice(n), sentT1)).includes('uitheme|gray'), 'TEMA: escolher CINZA avisa o host (uitheme|gray)');
  ok(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === 'gray', 'TEMA: atributo data-theme="gray" aplicado');
  const bgAfterGray = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  ok(bgAfterGray !== bgBeforeGray, 'TEMA CINZA: a cor de fundo muda e é diferente do claro');
  ok(bgAfterGray !== bgAfter, 'TEMA CINZA: tem cor própria, diferente do escuro (não é um alias dele)');
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

  // ---- FEATURE: overlays ---- GRÁFICOS tab
  console.log('\n[5f] GRÁFICOS (overlays dinâmicos)');
  const OVB = e => Object.assign({ x: 0.97, y: 0.05, pivotX: 1, pivotY: 0, z: 0, size: 0.07, font: 'Segoe UI', bold: true, textColor: 16777215, bgColor: 0, accent: 2058239,
    bgAlpha: 0.6, opacity: 1, animIn: 'fade', animOut: 'fade', animMs: 400, mode: 'down', durationSec: 300, format: 'auto', blinkAtZero: true, label: '',
    teamA: 'CASA', teamB: 'VISITANTE', period: '', colorA: 1, colorB: 2, scoreA: 0, scoreB: 0, showClock: false, qrText: '', qrCaption: '', qrEcc: 'M', qrSize: 0.28,
    qrFg: 0, qrBg: 16777215, title: '', subtitle: '', holdSec: 4, maxLines: 2, widthPct: 0.8, clock: '05:00', on: false, running: false, finished: false, qrOk: true }, e);
  const ovMsg = (els, preview = false) => ({ t: 'overlay_state', preview, elements: els });
  const ovEls = [OVB({ id: 'ov1', type: 'timer', name: 'Contagem' }), OVB({ id: 'ov2', type: 'scoreboard', name: 'Placar do jogo', teamA: 'AAA', teamB: 'BBB', scoreA: 2 }),
    OVB({ id: 'ov3', type: 'qr', name: 'Doação', qrOk: false })];
  await page.evaluate(m => window.__hbMessage(m), ovMsg(ovEls));
  await tab('gfx');
  ok(await page.locator('.ovrow').count() === 3, 'GRÁFICOS: uma linha por elemento');
  ok((await page.textContent('#ov-count')) === '3', 'GRÁFICOS: contador de elementos');
  ok((await page.textContent('.ovrow[data-id=ov3] .nm em')).includes('⚠'), 'GRÁFICOS: QR com conteúdo inválido mostra aviso');
  await clearSent();
  await humanClick('.ovrow[data-id=ov1] [data-a=air]');
  ok((await sent()).includes('ovshow|ov1|1'), 'GRÁFICOS: NO AR envia ovshow|id|1');
  await humanClick('.ovrow[data-id=ov1] .ctl button >> text=INICIAR');
  ok((await sent()).includes('ovtimer|ov1|start'), 'GRÁFICOS: INICIAR envia ovtimer|id|start');
  await humanClick('.ovrow[data-id=ov1] .ctl button >> text=PAUSAR');
  await humanClick('.ovrow[data-id=ov1] .ctl button >> text=ZERAR');
  ok((await sent()).includes('ovtimer|ov1|pause') && (await sent()).includes('ovtimer|ov1|reset'), 'GRÁFICOS: PAUSAR e ZERAR');
  await humanClick('.ovrow[data-id=ov1] .ctl button >> text=10 min');
  ok((await sent()).includes('ovtimer|ov1|set|600'), 'GRÁFICOS: preset de 10 min envia ovtimer|id|set|600');
  await humanClick('.ovrow[data-id=ov2] .ctl button >> text=A +1');
  await humanClick('.ovrow[data-id=ov2] .ctl button >> text=B −1');
  ok((await sent()).includes('ovscore|ov2|a|1') && (await sent()).includes('ovscore|ov2|b|-1'), 'GRÁFICOS: pontos do placar');
  // the live clock updates in place: the row element must survive state refreshes (never rebuilt while clicking)
  await page.evaluate(() => { window.__row = document.querySelector('.ovrow[data-id=ov1]'); });
  await page.evaluate(m => window.__hbMessage(m), ovMsg(ovEls.map(e => Object.assign({}, e, { clock: '04:59' }))));
  ok(await page.evaluate(() => window.__row === document.querySelector('.ovrow[data-id=ov1]')), 'GRÁFICOS: o relógio ao vivo não recria a lista');
  ok((await page.textContent('.ovrow[data-id=ov1] .nm .live')) === '04:59', 'GRÁFICOS: texto do relógio atualizado');
  await page.evaluate(m => window.__hbMessage(m), ovMsg(ovEls.map(e => Object.assign({}, e, { on: e.id === 'ov1' }))));
  ok(await page.locator('.ovrow.on').count() === 1 && (await page.textContent('.ovrow[data-id=ov1] [data-a=air]')) === 'NO AR', 'GRÁFICOS: estado NO AR refletido na lista');
  await clearSent();
  await humanClick('.ovrow[data-id=ov1] [data-a=edit]');
  ok(await page.locator('#modal6.on').count() === 1, 'GRÁFICOS: EDITAR abre o diálogo');
  ok(await page.locator('#ovm-fields [data-k=durationSec]').count() === 1 && await page.locator('#ovm-fields [data-k=qrText]').count() === 0, 'GRÁFICOS: o diálogo só mostra campos do tipo');
  await page.fill('#ovm-fields [data-k=name]', 'Minha contagem');
  await page.click('#ovm-fields .ovpre button >> text=15 min');
  await page.selectOption('#ovm-fields [data-k=_pos]', '6');
  await page.click('#ovm-save');
  const ovs = (await sent()).find(m => m.startsWith('ovsave|'));
  ok(!!ovs, 'GRÁFICOS: SALVAR envia ovsave');
  const ovp = JSON.parse(ovs.slice('ovsave|'.length));
  ok(ovp.id === 'ov1' && ovp.name === 'Minha contagem' && ovp.durationSec === 900 && ovp.x === 0.03 && ovp.y === 0.95 && ovp.pivotY === 1 && !('clock' in ovp), 'GRÁFICOS: payload do ovsave (id, nome, duração, posição, sem campos vivos)');
  ok(await page.locator('#modal6.on').count() === 0, 'GRÁFICOS: salvar fecha o diálogo');
  await clearSent();
  await page.selectOption('#ov-type', 'scoreboard');
  await page.click('#ov-add');
  ok((await sent()).includes('ovnew|scoreboard'), 'GRÁFICOS: ADICIONAR envia ovnew|tipo');
  await page.click('label.chk:has(#ov-prev)');
  ok((await sent()).includes('ovpreview|1'), 'GRÁFICOS: opção de preview envia ovpreview|1');
  await clearSent();
  await humanClick('.ovrow[data-id=ov3] [data-a=del]');   // the harness auto-accepts confirm()
  ok((await sent()).includes('ovdel|ov3'), 'GRÁFICOS: remover envia ovdel|id');
  await humanClick('.ovrow[data-id=ov1] [data-a=up]');
  ok((await sent()).includes('ovmove|ov1|up'), 'GRÁFICOS: ordem (z) envia ovmove');
  await page.evaluate(m => window.__hbMessage(m), ovMsg([]));
  ok(await page.locator('.ovrow').count() === 0 && await page.isVisible('#ov-empty'), 'GRÁFICOS: lista vazia mostra a dica');
  await tab('prod');
  // ---- /FEATURE ----

  console.log('\n[6] Layout em janelas diferentes');
  // The PRODUÇÃO stage (monitors + switcher + 16-camera strip) uses a 3-row CSS grid where the
  // monitors and inputs rows flex to fill whatever's left after the switcher card's own natural
  // size — by construction this always sums to exactly the stage's height, so it must never need
  // to scroll, down to the app's own declared minimum window size (webview_host's minWidth x
  // minHeight, 1100x680).
  for (const [w, h] of [[1100, 680], [1280, 800], [1366, 768], [1920, 1080]]) {
    await page.setViewportSize({ width: w, height: h });
    await tab('prod'); await page.waitForTimeout(100);
    const tileCount = await page.locator('.tile:not(.scene)').count();
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
    for (const v of ['xmit', 'gfx', 'chat', 'prod', 'cam', 'aud', 'rec', 'plg', 'cfg']) {  // gfx: FEATURE overlays
      await tab(v);
      const hs = await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1);
      ok(hs, `${w}x${h} ${v}: no horizontal overflow`);
    }
  }
  await tab('prod');

  // ---- FEATURE: ai ----
  await tab('cfg'); await clearSent();
  ok((await page.locator('#ai-card').count()) === 1, 'IA: card "Inteligência Artificial" existe em CONFIG');
  ok((await page.locator('#ai-url').inputValue()) === 'https://openrouter.ai/api/v1', 'IA: endereço padrão do OpenRouter preenchido');
  await page.selectOption('#ai-kind', '9router');
  ok((await page.locator('#ai-url').inputValue()) === 'http://localhost:20128/v1', 'IA: 9router usa http://localhost:20128/v1');
  await page.selectOption('#ai-kind', 'openrouter');
  await page.fill('#ai-key', 'sk-or-v1-SEGREDO1234');
  await page.fill('#ai-model', 'meta/x:free');
  await page.click('#ai-save-btn');
  const aiSave = (await sent()).find(m => m.startsWith('aisave|'));
  ok(!!aiSave && aiSave.includes('"apiKey":"sk-or-v1-SEGREDO1234"') && aiSave.includes('meta/x:free'), 'IA: SALVAR envia aisave|json com a chave digitada');
  await page.evaluate(() => window.__hbMessage({ t: 'ai_state', active: 'p1', configured: true, canPersistKeys: true, keysLost: false, savedId: 'p1',
    list: [{ id: 'p1', name: 'OpenRouter', kind: 'openrouter', baseUrl: 'https://openrouter.ai/api/v1', model: 'meta/x:free', keySet: true, keyHint: '••••1234' }] }));
  ok((await page.locator('#ai-key').inputValue()) === '', 'IA: campo da chave fica vazio após salvar (a chave nunca volta)');
  ok((await page.locator('#ai-key').getAttribute('placeholder')).includes('••••1234'), 'IA: placeholder mostra só a dica mascarada');
  ok((await page.locator('#ai-active-note').textContent()).includes('Ativo: OpenRouter'), 'IA: nota mostra provedor ativo');
  await page.evaluate(() => window.__hbMessage({ t: 'ai_models', ok: true, id: 'p1', error: '', models: [
    { id: 'meta/x:free', name: 'X', free: true, ctx: 1000 }, { id: 'pago/y', name: 'Y', free: false, ctx: 1000 }] }));
  ok((await page.locator('#ai-models-dl option').count()) === 2, 'IA: lista de modelos carregada');
  await page.check('#ai-onlyfree');
  ok((await page.locator('#ai-models-dl option').count()) === 1, 'IA: filtro "só grátis" funciona');
  ok((await page.locator('#ai-free.on').count()) === 1, 'IA: selo "grátis" aparece para modelo grátis');
  await page.fill('#ai-model', 'pago/y');
  await page.evaluate(() => document.querySelector('#ai-model').dispatchEvent(new Event('input')));
  ok((await page.locator('#ai-free.on').count()) === 0, 'IA: selo some para modelo pago');
  await clearSent();
  await page.click('#ai-test-btn');
  ok((await sent()).some(m => m.startsWith('aitest|{"id":"p1"')), 'IA: TESTAR CONEXÃO envia aitest com o id salvo');
  await page.evaluate(() => window.__hbMessage({ t: 'ai_test', ok: false, latencyMs: 12, kind: 'invalid_key', message: 'Chave de API inválida.' }));
  ok((await page.locator('#ai-res').textContent()).includes('Chave de API inválida'), 'IA: erro do teste é mostrado');
  await page.evaluate(() => window.__hbMessage({ t: 'ai_test', ok: true, latencyMs: 321, kind: 'none', message: 'Conexão OK' }));
  ok((await page.locator('#ai-res').textContent()).includes('321 ms'), 'IA: sucesso mostra latência');
  await page.fill('#ai-topic', 'culto de domingo');
  await page.click('#ai-sug-btn');
  ok((await sent()).includes('aisuggest|{"topic":"culto de domingo"}'), 'IA: SUGERIR envia aisuggest');
  await page.evaluate(() => window.__hbMessage({ t: 'ai_suggest', ok: true, title: 'Culto ao vivo', description: 'Venha.', error: '' }));
  ok((await page.locator('#ai-sug-title').inputValue()) === 'Culto ao vivo', 'IA: título sugerido preenchido');
  await clearSent();
  await page.click('#ai-del-btn');
  ok((await sent()).includes('airemove|p1'), 'IA: REMOVER envia airemove|id');
  await tab('prod');
  // ---- /FEATURE ----

  // ---- FEATURE: remote (CONFIG card "Controle remoto") ----
  {
    await tab('cfg');
    ok(await page.locator('#remote-card').count() === 1, 'remote: CONFIG has the "Controle remoto" card');
    ok(await page.locator('#rm-info').isHidden(), 'remote: PIN/URL are hidden while the remote is off');
    await page.evaluate(() => { window.__sent.length = 0; });
    await page.click('#rm-toggle');
    ok(await page.evaluate(() => window.__sent.includes('remote|on')), 'remote: toggle sends remote|on');
    await page.evaluate(() => window.__hbMessage({ t: 'remote', enabled: true, port: 8787, urls: ['http://192.168.0.10:8787/'], pin: '123456', lockedMs: 0, err: '',
      devices: [{ id: 3, name: 'iPad <b>x</b>', ip: '192.168.0.22', online: true, thumbs: false, idle: 1 }] }));
    ok(await page.locator('#rm-pin').textContent() === '123456', 'remote: shows the PIN when on');
    ok((await page.locator('#rm-urls').textContent()).includes('192.168.0.10:8787'), 'remote: shows the LAN URL');
    ok(await page.locator('#rm-devs .row').count() === 1 && await page.locator('#rm-devs b').count() === 0, 'remote: device list rendered as text (no HTML injection)');
    await page.evaluate(() => { window.__sent.length = 0; });
    await page.click('#rm-devs button');
    ok(await page.evaluate(() => window.__sent.includes('remote|kick|3')), 'remote: kick sends remote|kick|3');
    await page.click('#rm-revoke');
    ok(await page.evaluate(() => window.__sent.includes('remote|revoke')), 'remote: revoke-all sends remote|revoke');
    await page.click('#rm-newpin');
    ok(await page.evaluate(() => window.__sent.includes('remote|newpin')), 'remote: new PIN sends remote|newpin');
    ok((await page.locator('#remote-card').textContent()).includes('sem criptografia'), 'remote: card states the no-TLS limit');
    await page.evaluate(() => window.__hbMessage({ t: 'remote', enabled: false, err: 'x' }));
    ok(await page.locator('#rm-info').isHidden(), 'remote: info hidden again when turned off');
    for (const [w, h] of [[1024, 720], [1280, 800]]) {
      await page.setViewportSize({ width: w, height: h });
      ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1), `remote ${w}x${h}: card causes no horizontal overflow`);
    }
    await page.setViewportSize({ width: 1280, height: 800 });
  }
  // ---- /FEATURE ----
  // ---- FEATURE: scenes ----
  console.log('\n[7] Templates prontos e banco CENAS');
  await page.setViewportSize({ width: 1024, height: 720 });
  await tab('prod');
  const L = (role, slot, rect, extra) => Object.assign({ role, slot, rect, fit: 'fill' }, extra || {});
  const TEMPLATES = [
    { id: 'culto', name: 'Culto', category: 'culto', description: 'Cenas de culto', builtin: true, roles: ['Pregador', 'Slides'],
      scenes: [{ id: 'pregador', name: 'Pregador em tela cheia', background: { type: 'solid', color: '#000000' }, layers: [L('Pregador', -1, [0, 0, 1, 1])] },
               { id: 'lado', name: 'Lado a lado', background: { type: 'gradient', from: '#0b1220', to: '#1c2f55', angle: 90 },
                 layers: [L('Pregador', -1, [0.02, 0.26, 0.47, 0.47], { radius: 0.05, border: { width: 0.004, color: '#fff' }, shadow: { blur: 0.02, dx: 0, dy: 0.01, opacity: 0.5 } }), L('Slides', -1, [0.51, 0.26, 0.47, 0.47], { fit: 'fit' })] }] },
    { id: 'u-meu', name: 'Meu layout', category: 'meus', description: '', builtin: false, roles: ['Host'],
      scenes: [{ id: 'a', name: 'A', background: { type: 'solid', color: '#123456' }, layers: [L('Host', -1, [0, 0, 1, 1])], overlays: [{ type: 'lowerthird', text: '{nome}' }] }] },
  ];
  const sceneDemo = { id: 'culto.pregador', name: 'Pregador em tela cheia', background: { type: 'solid', color: '#000000' }, layers: [L('Pregador', 0, [0.1, 0.1, 0.5, 0.5])] };
  const feedScenes = scenes => page.evaluate(m => window.__hbMessage(m), { t: 'scenes_state', max: 16, templates: TEMPLATES, scenes });
  await feedScenes([]);
  ok(await page.locator('.tile.scene').count() === 16, '16 scene tiles exist (CENAS bank)');
  ok(await page.locator('.tile.scene').first().evaluate(el => getComputedStyle(el).display === 'none'), 'scene tiles stay hidden while a camera bank is active (16-slot UI untouched)');
  await page.click('#sc-open'); await page.waitForTimeout(150);
  ok(await page.locator('#sc-pick.on').count() === 1, 'TEMPLATES opens the picker');
  ok(await page.locator('#scp-grid .sc-card').count() === 2, 'picker lists every template');
  const thumbOk = await page.locator('#scp-grid .sc-card canvas').first().evaluate(c => { const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i]) n++; return n > c.width * c.height / 2; });
  ok(thumbOk, 'template thumbnails are rendered from placeholder colours (no cameras needed)');
  await page.click('#scp-tabs button[data-cat="meus"]');
  ok(await page.locator('#scp-grid .sc-card').count() === 1, 'category tab filters ("Meus")');
  await page.click('#scp-tabs button[data-cat="todos"]');
  await page.click('.sc-card[data-id="culto"]');
  ok(await page.locator('#scp-roles select').count() === 2, 'mapping step: one selector per role');
  ok(await page.locator('#scp-roles select[data-role="Pregador"]').inputValue() === '0', 'roles are prefilled with the first connected camera');
  ok(await page.locator('#scp-roles select[data-role="Slides"]').inputValue() !== '0', 'each role gets a different input');
  await clearSent();
  await page.click('#scp-apply'); await page.waitForTimeout(100);
  const ap = await lastSent();
  ok(/^sceneapply\|culto\|\{.*"Pregador":0.*\}\|pregador,lado$/.test(ap || ''), 'APPLY sends sceneapply|template|roleMap|scenes (' + ap + ')');
  ok(await page.locator('#sc-pick.on').count() === 0, 'picker closes after applying');
  await feedScenes([{ slot: 0, scene: sceneDemo }]);
  ok(await page.locator('.bankbtn[data-bank="2"]').evaluate(el => el.classList.contains('on')), 'the CENAS bank is shown after applying');
  ok(await page.locator('.tile.scene[data-i="16"]').isVisible() && !(await page.locator('.tile:not(.scene)').first().isVisible()), 'scene tiles replace camera tiles in the CENAS bank');
  ok((await page.locator('.tile.scene[data-i="16"] b').textContent()) === 'Pregador em tela cheia', 'scene tile shows the scene name');
  ok(await page.locator('.tile.scene[data-i="17"]').evaluate(el => el.classList.contains('sc-empty')), 'unused scene slots are marked empty');
  ok((await page.locator('#bus-pvw .key').first().textContent()) === 'C1', 'bus keys of the CENAS bank are labelled C1..C8');
  await clearSent();
  await page.locator('.tile.scene[data-i="16"]').click();
  ok((await lastSent()) === 'preview|16', 'clicking a scene tile sends preview|16 (switch bus index 16)');
  await page.locator('.tile.scene[data-i="16"]').dblclick();
  ok((await sent()).includes('program|16'), 'double click sends program|16');
  await page.keyboard.press('Digit1');
  ok((await lastSent()) === 'preview|16', 'keyboard 1 addresses the first scene in the CENAS bank');
  await setState({ pgm: 16, pvw: 0 });
  await page.waitForTimeout(150);
  ok((await page.locator('#pgm-name').textContent()) === 'Pregador em tela cheia', 'PGM monitor names the scene on air');
  await setState({});
  await page.locator('.tile.scene[data-i="16"]').click({ button: 'right' });
  await page.click('#scm-edit'); await page.waitForTimeout(100);
  ok(await page.locator('#sc-edit.on').count() === 1, 'context menu opens the scene editor');
  const box = await page.locator('#sc-cv').boundingBox();
  await page.mouse.move(box.x + box.width * 0.35, box.y + box.height * 0.35);
  await page.mouse.down(); await page.mouse.move(box.x + box.width * 0.55, box.y + box.height * 0.45, { steps: 4 }); await page.mouse.up();
  const nx = +(await page.locator('#sce-x').inputValue());
  ok(nx > 25 && nx < 35, 'dragging a layer on the preview canvas moves it (x=' + nx + '%)');
  await page.fill('#sce-w', '40'); await page.fill('#sce-name', 'Minha cena');
  await clearSent();
  await page.click('#sce-save'); await page.waitForTimeout(100);
  const sv = await lastSent();
  let svj = null; try { svj = JSON.parse(sv.slice('scenesave|'.length)); } catch (e) {}
  ok(svj && svj.id === 'culto.pregador' && svj.name === 'Minha cena' && Math.abs(svj.layers[0].rect[2] - 0.4) < 1e-6 && svj.layers[0].rect[0] > 0.25, 'SAVE sends scenesave|<scene json> with the edited rect and name');
  await page.locator('.tile.scene[data-i="16"]').click({ button: 'right' });
  await page.click('#scm-del');
  ok(!(await sent()).includes('scenedelete|culto.pregador'), 'deleting a scene needs a second click');
  await page.click('#scm-del');
  ok((await sent()).includes('scenedelete|culto.pregador'), 'second click sends scenedelete|id');
  await page.locator('.tile.scene[data-i="16"]').click({ button: 'right' });
  await page.click('#scm-edit'); await page.fill('#sce-tname', 'Meu culto'); await clearSent();
  await page.click('#sce-tpl'); await page.waitForTimeout(100);
  const ss = await sent();
  ok(ss.length === 2 && ss[0].startsWith('scenesave|') && ss[1] === 'scenetemplate|culto.pregador|Meu culto', 'save-as-my-template sends scenetemplate|id|name');
  for (const [w, h] of [[1024, 720], [1280, 800]]) {
    await page.setViewportSize({ width: w, height: h });
    const fit = await page.evaluate(() => { const s = document.querySelector('.stage'); return [s.scrollHeight, s.clientHeight, document.documentElement.scrollWidth, innerWidth]; });
    ok(fit[0] <= fit[1] + 1 && fit[2] <= fit[3] + 1, `${w}x${h}: CENAS bank fits without scrolling (${fit.join('/')})`);
  }
  await page.click('#sc-open'); await page.click('.sc-card[data-id="u-meu"]');
  ok(await page.locator('#scp-del').isVisible(), 'user templates can be deleted');
  await clearSent(); await page.click('#scp-export');
  ok((await lastSent()) === 'sceneexport|u-meu', 'EXPORT sends sceneexport|id (host opens the save dialog)');
  await page.click('#scp-back'); await clearSent(); await page.click('#scp-import');
  ok((await lastSent()) === 'sceneimport', 'IMPORT sends sceneimport (host opens the open dialog)');
  await page.keyboard.press('Escape');
  await humanClick('.bankbtn[data-bank="0"]');
  ok(await page.locator('.tile:not(.scene)').first().isVisible(), 'back to the camera banks: the 16 camera tiles are untouched');
  await feedScenes([]);
  // ---- /FEATURE ----

  // ---- FEATURE: teleprompter ----
  console.log('\n[tp] TELEPROMPTER');
  await page.setViewportSize({ width: 1280, height: 800 });
  await tab('tp');
  ok(await page.locator('#v-tp.on').count() === 1, 'TELEPROMPTER tab opens its view');
  const tpMsg = m => page.evaluate(x => window.__tpMessage(x), m);
  const TPS = (over = {}) => Object.assign({ t: 'tp', seq: 1, state: 'stopped', pos: 0, rate: 0, cd: 0, cdSec: 3, remain: 0, total: 120, mode: 'wpm', wpm: 150, pxs: 50, words: 300, script: 's1', presenter: false }, over);
  await tpMsg({ t: 'tplist', current: 's1', scripts: [{ id: 's1', name: 'Abertura', words: 300 }, { id: 's2', name: 'Final', words: 10 }] });
  await tpMsg({ t: 'tpstyle', font: 64, lh: 1.4, margin: 8, fg: '#FFFFFF', bg: '#000000', mirror: false, flip: false, marker: true, markerpos: 35, cdSec: 3 });
  const long = Array.from({ length: 12 }, (_, i) => ({ k: 'p', t: 'Parágrafo número ' + i + ' com bastante texto para ocupar linhas no teleprompter do operador.', w: 14, line: i * 2 + 1, at: i / 12 }));
  await tpMsg({ t: 'tpscript', id: 's1', name: 'Abertura', text: 'x', words: 300, paragraphs: 12, blocks: [{ k: 'c', t: 'RESPIRAR', w: 0, line: 1 }, ...long] });
  await tpMsg(TPS());
  ok(await page.locator('#tp-list button').count() === 2, 'script list shows saved scripts');
  ok(await page.locator('#tp-txt p.c').count() === 1 && await page.locator('#tp-txt p.p').count() === 12, 'operator view shows cues (as tags) and paragraphs');
  ok(await page.inputValue('#tp-name') === 'Abertura', 'name field shows the current script');
  await clearSent();
  await page.click('#tp-play');
  ok((await sent()).includes('tp|toggle'), 'PLAY -> tp|toggle');
  await clearSent();
  await page.click('#tp-restart');
  ok((await sent()).includes('tp|restart'), 'rewind -> tp|restart');
  await clearSent();
  await page.click('#tp-list button:nth-child(2)');
  ok((await sent()).includes('tp|select|s2'), 'clicking a script -> tp|select|<id>');
  // keyboard scoped to the tab: Space must not CUT here
  await page.evaluate(() => document.activeElement && document.activeElement.blur());
  await clearSent();
  await page.keyboard.press('Space'); await page.keyboard.press('ArrowUp'); await page.keyboard.press('ArrowDown'); await page.keyboard.press('Home');
  const tk = await sent();
  ok(tk.includes('tp|toggle') && tk.includes('tp|nudge|10') && tk.includes('tp|nudge|-10') && tk.includes('tp|restart'), 'keys: Space/up/down/Home -> toggle/nudge/restart');
  ok(!tk.includes('cut'), 'Space in the TELEPROMPTER tab does not CUT the switcher');
  await clearSent();
  await page.focus('#tp-text'); await page.keyboard.press('Space'); await page.keyboard.press('Home');
  ok(!(await sent()).some(x => x.startsWith('tp|toggle') || x === 'tp|restart' || x === 'cut'), 'typing in the editor does not trigger shortcuts');
  await page.evaluate(() => document.activeElement.blur());
  // editor autosave
  await clearSent();
  await page.fill('#tp-text', 'novo texto\n\nsegundo');
  await page.waitForTimeout(800);
  ok((await sent()).some(x => x.startsWith('tp|save|s1|novo texto')), 'editing autosaves -> tp|save|<id>|<text>');
  // speed + style controls
  await clearSent();
  await page.locator('#tp-speed').evaluate(el => { el.value = 180; el.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.locator('#tp-font').evaluate(el => { el.value = 90; el.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.check('#tp-mirror'); await page.check('#tp-flip');
  await page.locator('#tp-fg').evaluate(el => { el.value = '#ffff00'; el.dispatchEvent(new Event('input', { bubbles: true })); });
  const sc = await sent();
  ok(sc.includes('tp|speed|180') && sc.includes('tp|style|font|90') && sc.includes('tp|style|mirror|1') && sc.includes('tp|style|flip|1') && sc.includes('tp|style|fg|#FFFF00'), 'speed/font/mirror/flip/color controls -> tp|speed / tp|style');
  await page.click('#tp-mode button[data-m=pxs]');
  ok((await sent()).includes('tp|mode|pxs'), 'unit switch -> tp|mode|pxs');
  // paragraph click jumps
  await tpMsg(TPS({ state: 'paused', pos: 0 }));
  await clearSent();
  await page.locator('#tp-txt p.p').nth(6).click();
  const seek = (await sent()).find(x => x.startsWith('tp|seek|'));
  ok(seek && +seek.split('|')[2] > 0.3 && +seek.split('|')[2] < 0.7, 'clicking paragraph 7 of 12 -> tp|seek|~0.5 (' + seek + ')');
  await clearSent();
  const pb = await page.locator('#tp-prog').boundingBox();
  await page.mouse.click(pb.x + pb.width * 0.25, pb.y + pb.height / 2);
  const seek2 = (await sent()).find(x => x.startsWith('tp|seek|'));
  ok(seek2 && Math.abs(+seek2.split('|')[2] - 0.25) < 0.03, 'clicking the progress bar -> tp|seek|~0.25');
  // moving: extrapolates from pos+rate between host messages, with time display
  await tpMsg(TPS({ state: 'playing', pos: 0.1, rate: 0.05, remain: 18, total: 20 }));
  const y0 = await page.locator('#tp-txt').evaluate(el => new DOMMatrix(getComputedStyle(el).transform).m42);
  await page.waitForTimeout(500);
  const y1 = await page.locator('#tp-txt').evaluate(el => new DOMMatrix(getComputedStyle(el).transform).m42);
  ok(y1 < y0 - 1, 'while playing the view scrolls between host messages (' + y0.toFixed(1) + ' -> ' + y1.toFixed(1) + ')');
  ok(/PAUSE/.test(await page.textContent('#tp-play')) && /NO AR/.test(await page.textContent('#tp-state')), 'PLAY button becomes PAUSE while playing');
  ok(/restam 0:/.test(await page.textContent('#tp-remain')) && /total 0:20/.test(await page.textContent('#tp-total')), 'remaining/total time estimates are shown');
  await tpMsg(TPS({ state: 'countdown', pos: 0, cd: 3 }));
  ok(await page.locator('#tp-cd').evaluate(el => getComputedStyle(el).display) === 'flex' && /3/.test(await page.textContent('#tp-cd')), 'countdown overlay shows the seconds left');
  await tpMsg(TPS({ state: 'paused', pos: 0.3, presenter: true }));
  ok(await page.locator('#tp-pop').isDisabled() && !(await page.locator('#tp-popfs').isDisabled()), 'presenter buttons follow the window state');
  await clearSent();
  await page.click('#tp-popfs');
  ok((await sent()).includes('tp|presenter|fullscreen'), 'TELA CHEIA -> tp|presenter|fullscreen');
  await tpMsg(TPS({ presenter: false }));
  await clearSent();
  await page.click('#tp-pop');
  ok((await sent()).includes('tp|presenter|open'), 'ABRIR APRESENTADOR -> tp|presenter|open');
  // import: UTF-8 and legacy Windows-1252 files
  await clearSent();
  await page.setInputFiles('#tp-file', { name: 'Meu roteiro.txt', mimeType: 'text/plain', buffer: Buffer.from('coração\nação', 'utf8') });
  await page.waitForTimeout(200);
  ok((await sent()).includes('tp|import|Meu roteiro|coração\nação'), 'import .txt (UTF-8) -> tp|import|<name>|<text>');
  await clearSent();
  await page.setInputFiles('#tp-file', { name: 'antigo.txt', mimeType: 'text/plain', buffer: Buffer.from([0x63, 0x6f, 0x72, 0x61, 0xe7, 0xe3, 0x6f]) });
  await page.waitForTimeout(200);
  ok((await sent()).includes('tp|import|antigo|coração'), 'import .txt (Windows-1252) is decoded to UTF-8');
  // layout at several sizes: nothing may scroll the page, no horizontal overflow
  for (const [w, h] of [[1024, 720], [1280, 800], [1920, 1080]]) {
    await page.setViewportSize({ width: w, height: h });
    await page.waitForTimeout(150);
    const fit = await page.evaluate(() => ({ x: document.documentElement.scrollWidth <= innerWidth + 1, y: document.documentElement.scrollHeight <= innerHeight + 1,
      vis: (() => { const r = document.querySelector('#tp-play').getBoundingClientRect(); const f = document.querySelector('#tp-view').getBoundingClientRect(); return r.bottom <= innerHeight && f.height > 100; })() }));
    ok(fit.x && fit.y && fit.vis, `${w}x${h} TELEPROMPTER: fits without page scroll and the view/controls are visible`);
  }
  // themes
  for (const th of ['light', 'dark', 'gray']) {
    await page.evaluate(t => document.documentElement.setAttribute('data-theme', t), th);
    const c = await page.locator('#tp-list button').first().evaluate(el => getComputedStyle(el).color);
    ok(!!c, 'theme ' + th + ' renders the TELEPROMPTER tab');
  }
  await page.evaluate(() => document.documentElement.removeAttribute('data-theme'));
  await page.setViewportSize({ width: 1280, height: 800 });
  // presenter page (extracted from index.html exactly like the host does)
  {
    const raw = require('fs').readFileSync(html, 'utf8');
    const open = '<template id="tpPresenterPage">';
    const b = raw.indexOf(open), e = raw.indexOf('</template>', b);
    const pres = raw.slice(b + open.length, e);
    ok(pres.startsWith('<!doctype html>'), 'presenter page is a complete document inside the template');
    const p2 = await browser.newPage({ viewport: { width: 1280, height: 720 } });
    await p2.goto('about:blank');
    await p2.evaluate(() => { window.__sent = []; window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } }; });
    await p2.setContent(pres);
    const pm = m => p2.evaluate(x => window.__tpPresenterMessage(x), m);
    const ty = () => p2.locator('#txt').evaluate(el => new DOMMatrix(getComputedStyle(el).transform).m42);
    ok((await p2.evaluate(() => window.__sent)).includes('tp|hello'), 'presenter announces itself with tp|hello');
    await pm({ t: 'tpstyle', font: 60, lh: 1.4, margin: 8, fg: '#FFFF00', bg: '#001122', mirror: true, flip: false, marker: true, markerpos: 35, cdSec: 3 });
    await pm({ t: 'tpscript', id: 's1', name: 'x', blocks: [{ k: 'c', t: 'SEGREDO DO OPERADOR', w: 0, line: 1 }, ...long] });
    ok(await p2.locator('#txt p').count() === 12 && !(await p2.textContent('#txt')).includes('SEGREDO'), 'presenter shows paragraphs only: cues/comments are hidden');
    ok(await p2.locator('#stage').evaluate(el => el.style.transform.replace(/\s/g, '')) === 'scale(-1,1)', 'mirror horizontal is applied to the presenter stage');
    await pm({ t: 'tp', seq: 1, state: 'paused', pos: 0, rate: 0, cd: 0 });
    const ym = await ty();
    ok(Math.abs(ym - 0.35 * 720) < 1.5, 'at pos 0 the first line sits on the reading line (' + ym.toFixed(1) + ')');
    await pm({ t: 'tp', seq: 2, state: 'playing', pos: 0.1, rate: 0.05, cd: 0 });
    const a0 = await ty();
    await p2.waitForTimeout(400);
    const a1 = await ty();
    ok(a1 < a0 - 1, 'presenter scrolls smoothly from (pos, rate) between host syncs');
    await pm({ t: 'tp', seq: 3, state: 'paused', pos: 0.5, rate: 0, cd: 0 });
    const a2 = await ty();
    await p2.waitForTimeout(200);
    const a3 = await ty();
    ok(a2 === a3 && a2 < ym, 'a host re-sync/pause snaps the presenter to the host position and holds it');
    await pm({ t: 'tp', seq: 4, state: 'countdown', pos: 0, rate: 0, cd: 3 });
    ok(await p2.locator('#cd').evaluate(el => getComputedStyle(el).display) === 'flex', 'presenter shows the countdown');
    await p2.evaluate(() => { window.__sent.length = 0; });
    await p2.keyboard.press('Space'); await p2.keyboard.press('ArrowUp'); await p2.keyboard.press('ArrowDown'); await p2.keyboard.press('Home'); await p2.keyboard.press('F11'); await p2.mouse.dblclick(100, 100);
    const ps = await p2.evaluate(() => window.__sent);
    ok(['tp|toggle', 'tp|nudge|10', 'tp|nudge|-10', 'tp|restart', 'tp|presenter|fullscreen'].every(x => ps.includes(x)), 'presenter keys/double click send tp commands');
    await p2.setViewportSize({ width: 1281, height: 720 });
    await p2.waitForTimeout(400);
    ok((await p2.evaluate(() => window.__sent)).some(x => /^tp\|layout\|p\|\d+\|720$/.test(x)), 'presenter reports its real layout for the px/s model');
    await p2.close();
  }
  await tab('prod');
  // ---- /FEATURE ----

  // ---- FEATURE: chat ----
  {
    await page.setViewportSize({ width: 1280, height: 800 });
    await tab('chat');
    const hm = o => page.evaluate(m => window.__hbMessage(m), o);
    ok((await page.locator('.chatplat').count()) === 4, 'CHAT: os quatro cards de plataforma aparecem antes de qualquer status');
    await hm({ t: 'chat_status', platforms: [
      { p: 'youtube', state: 'connected', detail: 'Conectado ao chat ao vivo', error: '', quota: 55, want: true },
      { p: 'facebook', state: 'needs_auth', detail: 'Informe o token', error: '', quota: 0, want: false },
      { p: 'instagram', state: 'unavailable', detail: 'Indisponível: o Instagram não oferece leitura', error: '', quota: 0, want: false },
      { p: 'twitch', state: 'disconnected', detail: '', error: '', quota: 0, want: false }], auth: { state: 'idle', message: '' }, muted: [], total: 0 });
    await hm({ t: 'chat_settings', youtube: { videoId: 'abc', clientId: 'cid', hasSecret: true, secretHint: '••••1234', authorized: true },
      facebook: { liveVideoId: '', hasToken: false, tokenHint: '' }, twitch: { channel: 'meucanal' }, persist: false, net: true });
    ok((await page.locator('.chatplat[data-p=instagram] .st').textContent()).includes('INDISPON'), 'CHAT: Instagram aparece como limitado/indisponível com explicação');
    ok((await page.locator('.chatplat[data-p=instagram]').textContent()).includes('não oferece'), 'CHAT: a explicação do Instagram é exibida');
    ok((await page.locator('.chatplat[data-p=instagram] [data-act=save]').count()) === 0, 'CHAT: Instagram não tem formulário de credenciais (nada fingido)');
    ok((await page.locator('.chatplat[data-p=youtube]').textContent()).includes('55 unidades'), 'CHAT: cota estimada do YouTube é mostrada');
    await clearSent();
    await page.locator('.chatplat[data-p=facebook] [data-act=connect]').click();
    ok((await sent()).includes('chatconnect|facebook'), 'CHAT: CONECTAR envia chatconnect|plataforma');
    await page.locator('.chatplat[data-p=youtube] [data-act=disconnect]').click();
    ok((await sent()).includes('chatdisconnect|youtube'), 'CHAT: DESCONECTAR envia chatdisconnect|plataforma');
    await page.locator('.chatplat[data-p=twitch] [data-act=cfg]').click();
    ok(await page.locator('#chat-tw-ch').isVisible(), 'CHAT: CONFIGURAR abre o formulário');
    ok((await page.inputValue('#chat-tw-ch')) === 'meucanal', 'CHAT: o canal salvo é mostrado');
    await page.fill('#chat-tw-ch', 'outro_canal');
    await page.locator('.chatplat[data-p=twitch] [data-act=save]').click();
    ok((await sent()).includes('chatsave|twitch|{"channel":"outro_canal"}'), 'CHAT: SALVAR envia chatsave com JSON');
    await page.locator('.chatplat[data-p=youtube] [data-act=cfg]').click();
    ok((await page.getAttribute('#chat-yt-sec', 'placeholder')).includes('1234'), 'CHAT: segredo salvo aparece só como dica mascarada');
    ok((await page.inputValue('#chat-yt-sec')) === '', 'CHAT: o campo de segredo nunca é preenchido com o valor salvo');
    await page.locator('.chatplat[data-p=youtube] [data-act=auth]').click();
    ok((await sent()).includes('chatauth|youtube'), 'CHAT: AUTORIZAR envia chatauth|youtube');

    const M = (n, p, a, x, extra = {}) => Object.assign({ n, p, a, uk: p + ':' + a, x, ts: 1700000000000 + n * 1000, b: 0 }, extra);
    await hm({ t: 'chat_msgs', seq: 1, dropped: 0, msgs: [
      M(1, 'youtube', 'Ana', 'olá <b>mundo</b>'), M(2, 'twitch', 'Bia', '!sorteio', { cmd: 1 }),
      M(3, 'twitch', 'Nightbot', 'siga o canal', { bot: 1 }), M(4, 'facebook', 'Carlos', 'parabéns!', { b: 1 })] });
    ok((await page.locator('#chat-feed .cm').count()) === 4, 'CHAT: as 4 mensagens aparecem no feed');
    ok((await page.locator('#chat-feed .cm').first().locator('.cx').textContent()) === 'olá <b>mundo</b>', 'CHAT: texto do chat é exibido literalmente (sem HTML)');
    ok((await page.locator('#chat-feed b').count()) === 0, 'CHAT: nenhuma marcação HTML vinda do chat é interpretada');
    ok((await page.locator('#chat-feed .cm').nth(3).locator('.cb').textContent()) === 'MOD', 'CHAT: selo de moderador');
    await hm({ t: 'chat_msgs', seq: 2, dropped: 0, msgs: [M(5, 'youtube', 'Duda', 'segunda leva')] });
    ok((await page.locator('#chat-feed .cm').count()) === 5, 'CHAT: um novo lote é acrescentado');
    await page.click('#chat-hidecmd');
    ok((await page.locator('#chat-feed .cm').count()) === 4, 'CHAT: "ocultar comandos" esconde !comandos');
    await page.click('#chat-hidebot');
    ok((await page.locator('#chat-feed .cm').count()) === 3, 'CHAT: "ocultar bots" esconde bots conhecidos');
    await page.click('#chat-filters [data-p=youtube]');
    ok((await page.locator('#chat-feed .cm .cchip.p-youtube').count()) === 0, 'CHAT: o filtro de plataforma esconde o YouTube');
    await page.click('#chat-filters [data-p=youtube]');
    await page.fill('#chat-search', 'parab');
    ok((await page.locator('#chat-feed .cm').count()) === 1, 'CHAT: a busca filtra por texto');
    await page.fill('#chat-search', '');
    await page.click('#chat-hidecmd'); await page.click('#chat-hidebot');
    ok((await page.locator('#chat-feed .cm').count()) === 5, 'CHAT: removendo os filtros todas as mensagens voltam');

    await clearSent();
    await page.locator('#chat-feed .cm').nth(3).locator('.cx').click();
    ok((await sent()).includes('chatpin|4'), 'CHAT: clicar numa mensagem envia chatpin|n');
    ok((await page.locator('#chat-feed .cm.pinned').count()) === 1, 'CHAT: a mensagem destacada fica marcada');
    await page.click('#chat-unpin');
    ok((await sent()).includes('chatunpin') && (await page.locator('#chat-feed .cm.pinned').count()) === 0, 'CHAT: LIMPAR DESTAQUE envia chatunpin');
    await page.locator('#chat-feed .cm').first().hover();
    await page.locator('#chat-feed .cm').first().locator('.cmute').click();
    ok((await sent()).includes('chatmute|1'), 'CHAT: silenciar envia chatmute|n (sem destacar)');
    ok(!(await sent()).includes('chatpin|1'), 'CHAT: silenciar não dispara o destaque');
    await hm({ t: 'chat_status', platforms: [], auth: { state: 'idle', message: '' }, muted: ['youtube:Ana'], total: 5 });
    ok((await page.locator('#chat-feed .cm').count()) === 4, 'CHAT: mensagens de usuário silenciado somem do feed');
    ok((await page.locator('.chat-muted button').count()) === 1, 'CHAT: lista de silenciados com botão para liberar');
    await page.locator('.chat-muted button').click();
    ok((await sent()).includes('chatunmute|youtube:Ana'), 'CHAT: liberar envia chatunmute|chave');
    await hm({ t: 'chat_status', platforms: [], auth: { state: 'idle', message: '' }, muted: [], total: 5 });

    // auto-scroll + pause
    const many = Array.from({ length: 60 }, (_, i) => M(100 + i, 'twitch', 'u' + i, 'mensagem longa número ' + i));
    await hm({ t: 'chat_msgs', seq: 3, dropped: 0, msgs: many });
    ok(await page.evaluate(() => { const f = document.querySelector('#chat-feed'); return f.scrollHeight - f.scrollTop - f.clientHeight < 8; }), 'CHAT: rolagem automática acompanha as novas mensagens');
    await page.click('#chat-pause');
    ok((await page.textContent('#chat-pause')).includes('RETOMAR'), 'CHAT: pausar a rolagem muda o botão');
    await page.evaluate(() => { document.querySelector('#chat-feed').scrollTop = 0; });
    await hm({ t: 'chat_msgs', seq: 4, dropped: 0, msgs: [M(200, 'twitch', 'late', 'nova com pausa')] });
    ok((await page.evaluate(() => document.querySelector('#chat-feed').scrollTop)) === 0, 'CHAT: com a rolagem pausada a lista não se mexe');
    await page.click('#chat-pause');

    // gap in the batch sequence asks the host for the history again; a reset batch replaces the list
    await clearSent();
    await hm({ t: 'chat_msgs', seq: 9, dropped: 0, msgs: [M(300, 'twitch', 'gap', 'depois do buraco')] });
    ok((await sent()).includes('chatget'), 'CHAT: lote fora de sequência pede o histórico (chatget)');
    await hm({ t: 'chat_msgs', seq: 10, reset: true, dropped: 0, msgs: [M(301, 'twitch', 'novo', 'só esta')] });
    ok((await page.locator('#chat-feed .cm').count()) === 1, 'CHAT: lote com reset substitui o histórico');

    // compact mode hides the connection panel and nothing overflows at the smallest sizes
    await page.click('#chat-compact');
    ok(!(await page.locator('.chatside').isVisible()), 'CHAT: modo compacto esconde o painel de conexões');
    await page.click('#chat-compact');
    for (const [w, h] of [[1024, 720], [1100, 680], [1920, 1080]]) {
      await page.setViewportSize({ width: w, height: h });
      await page.waitForTimeout(80);
      const fit = await page.evaluate(() => {
        const v = document.querySelector('#v-chat'), de = document.documentElement;
        return de.scrollWidth <= window.innerWidth + 1 && de.scrollHeight <= window.innerHeight + 1 && v.scrollHeight <= v.clientHeight + 1;
      });
      ok(fit, `CHAT ${w}x${h}: cabe sem rolagem da página`);
    }
    await page.setViewportSize({ width: 1280, height: 800 });
    await clearSent();
    await page.check('#chat-persist');
    ok((await sent()).includes('chatopt|persist|1'), 'CHAT: o salvamento em arquivo é opt-in (chatopt|persist|1)');
    await tab('prod');
  }
  // ---- /FEATURE ----

  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors[0] : ''));
  await browser.close();
  console.log(`\n${checks - failures}/${checks} checks passed`);
  process.exit(failures ? 1 : 0);
})();
