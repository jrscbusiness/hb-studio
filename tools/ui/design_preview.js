// Design-time preview ONLY (not shipped): renders ui/index.html in headless Chromium
// with fake data so the layout can be reviewed on any OS.
//   node design_preview.js <index.html> <out.png> [view]
const { chromium } = require('playwright-core');
const [,, html, out, view] = process.argv;
(async () => {
  const exe = process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
  const b = await chromium.launch({ executablePath: exe, args: ['--no-sandbox'] });
  const p = await b.newPage({ viewport: { width: +(process.env.VW || 1600), height: +(process.env.VH || 900) } });
  await p.goto('file://' + require('path').resolve(html));
  const themeFile = process.env.THEME;
  const themeMsg = themeFile ? (() => { const m = JSON.parse(require('fs').readFileSync(themeFile, 'utf8')); return { t: 'theme', id: m.id, scheme: m.theme.colorScheme, vars: m.theme.vars }; })() : null;
  await p.evaluate(([t, r, th]) => { window.__tier = t; window.__rec = r; window.__theme = th; }, [process.env.TIER || 'MEDIUM', +(process.env.REC || 8), themeMsg]);
  await p.evaluate(async (view) => {
    const mk = (w, h, hue, label) => { const c = document.createElement('canvas'); c.width = w; c.height = h; const x = c.getContext('2d');
      const g = x.createLinearGradient(0, 0, 0, h); g.addColorStop(0, `hsl(${hue},60%,70%)`); g.addColorStop(1, `hsl(${hue},80%,30%)`); x.fillStyle = g; x.fillRect(0, 0, w, h);
      x.strokeStyle = 'rgba(255,255,255,.25)'; for (let i = 0; i < 16; i++) { x.beginPath(); x.moveTo(i * w / 16, 0); x.lineTo(i * w / 16, h); x.stroke(); }
      x.fillStyle = '#fff'; x.beginPath(); x.arc(w * .62, h * .4, h * .11, 0, 7); x.fill(); x.font = `bold ${h * .4}px monospace`; x.textAlign = 'center'; x.fillText(label, w / 2, h * .68);
      return c.toDataURL('image/jpeg', .8).split(',')[1]; };
    for (let i = 0; i < 8; i++) window.__hbMessage({ t: 'f', id: 's' + i, d: mk(192, 108, 200 + i * 15, i + 1) });
    window.__hbMessage({ t: 'f', id: 'pvw', d: mk(640, 360, 230, 2) });
    window.__hbMessage({ t: 'f', id: 'pgm', d: mk(640, 360, 215, 1) });
    window.__hbMessage({ t: 's', slots: [{k:'dev',st:'online',w:1920,h:1080,fps:30,name:'OBS Virtual Camera',id:'a',msg:''},{k:'dev',st:'nosignal',w:1280,h:720,fps:0,name:'Logitech C920 HD Pro Webcam',id:'b',msg:'Sem imagem. Se for uma câmera virtual (ex.: OBS), inicie-a no programa de origem.'},{k:'test',st:'test',name:'',id:'',msg:''},{k:'test',st:'test',name:'',id:'',msg:''},{k:'test',st:'test',name:'',id:'',msg:''},{k:'test',st:'test',name:'',id:'',msg:''},{k:'test',st:'test',name:'',id:'',msg:''},{k:'test',st:'test',name:'',id:'',msg:''}], pvw: 1, pgm: 0, transition: false, progress: 0, fps: 29.9, target: 30, produced: 5421, frames: 5421, skipped: 0,
      tier: window.__tier, cores: 8, ramGiB: 16, srcRec: window.__rec, srcMax: 10,
      vcam: { supported: true, installed: true, active: true, connected: true, busy: false, sent: 4210, message: '' },
      log: [{ t: '16:20:01 [INFO] [engine] perfil MEDIUM, 8 núcleos', l: 'INFO' }, { t: '16:20:01 [INFO] [program] program.started', l: 'INFO' }, { t: '16:20:09 [INFO] [switcher] switcher.programChanged pvw=0 pgm=1', l: 'INFO' }] });
    const names = ['Altar','Púlpito','Louvor','Banda','Plateia','','Batismo','','Coral','Entrada'];
    window.__hbMessage({ t: 'ptz', cams: Array.from({length: 8}, (_, i) => ({ protocol: 'onvif', host: '192.168.15.10' + i, port: 0, address: 1, slots: 10, base: 1, storage: 'camera', speed: 50, invPan: false, invTilt: false, defPort: 80, connected: false, names }))});
    window.__hbMessage({ t: 'devices', list: [
      { id: 'a', name: 'OBS Virtual Camera', virt: true }, { id: 'b', name: 'Logitech C920 HD Pro Webcam', virt: false }, { id: 'c', name: 'Cam Link 4K', virt: false } ]});
    window.__hbMessage({ t: 'plugins', ver: 1, dir: 'C:\\Users\\Usuario\\AppData\\Local\\HBStudio\\plugins', list: [
      { id: 'com.hbstudio.theme.midnight', name: 'Tema Meia-noite (azul escuro)', version: '1.0.0', author: 'HB Studio', description: 'Interface em azul-marinho escuro, ideal para ambientes com pouca luz.', license: 'MIT', type: 'theme', valid: true, supported: true, enabled: !!window.__theme, loaded: false, removed: false, error: '' },
      { id: 'com.hbstudio.example.sources', name: 'Fontes de exemplo (barras e rampa)', version: '1.0.0', author: 'HB Studio', description: 'Plugin de demonstração: duas fontes de vídeo de teste.', license: 'MIT', type: 'source', valid: true, supported: true, enabled: true, loaded: true, removed: false, error: '' },
      { id: 'com.acme.capture', name: 'Placa de captura Acme', version: '0.9.2', author: 'Acme', description: '', license: '', type: 'source', valid: true, supported: true, enabled: false, loaded: false, removed: false, error: 'a biblioteca não confere com o sha256 do manifesto: ativação recusada' }] });
    if (window.__theme) window.__hbMessage(window.__theme);
    if (view === 'review') window.__hbMessage({ t: 'plugin_review', ok: true, token: 't', id: 'com.acme.capture', name: 'Placa de captura Acme', version: '0.9.3', author: 'Acme Ltda', description: 'Entrada de vídeo para placas Acme SDI.', license: 'MIT', type: 'source', native: true, files: 3, size: 482000, replaces: true, installedVersion: '0.9.2', sha: 'a1b2c3d4e5f60718' });
    if (view === 'modal') document.querySelector('#ptz-cfg').click();
    else if (view === 'edit') { document.querySelector('#ptz-mode button[data-m=edit]').click(); }
    else if (view === 'review') document.querySelector('#tabs button[data-v=plg]').click();
    else if (view) document.querySelector(`#tabs button[data-v=${view}]`).click();
  }, view);
  await p.waitForTimeout(400);
  await p.screenshot({ path: out });
  await b.close();
})();
