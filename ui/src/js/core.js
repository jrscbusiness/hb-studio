  const N = 16;          // 2 banks of 8 cameras (CAM 1–16)
  const BANK_SIZE = 8;
  const $ = s => document.querySelector(s);
  const $$ = s => [...document.querySelectorAll(s)];
  const host = window.chrome && window.chrome.webview;
  const send = (...a) => { if (host) host.postMessage(a.join('|')); };

  // ---------- build static DOM ----------
  // All 16 tiles are always live (mouse click/double-click on ANY tile works regardless of the
  // active bank). The active bank only decides which 8 of the 16 the keyboard (1–8/Shift+1–8) and
  // the small PROGRAM/PREVIEW number-button rows (fixed at 8 slots) currently address — see the
  // bank bar to the left of the tiles and renderBusKeys() below.
  let lastState = null;
  const tiles = $('#tiles'), busP = $('#bus-pgm'), busV = $('#bus-pvw');
  const canv = {pgm: $('#cv-pgm').getContext('2d'), pvw: $('#cv-pvw').getContext('2d')};
  for (let i = 0; i < N; i++) {
    const t = document.createElement('div');
    t.className = 'tile'; t.dataset.i = i;
    t.innerHTML = `<canvas width="192" height="108"></canvas><div class="lb"><b>CAM ${i+1}</b><em>TESTE</em></div><div class="tally"></div>`;
    t.addEventListener('click', () => send('preview', i));
    t.addEventListener('dblclick', () => send('program', i));
    t.addEventListener('contextmenu', e => { e.preventDefault(); openTileMenu(e, i); });
    tiles.appendChild(t);
    canv['s' + i] = t.querySelector('canvas').getContext('2d');
  }
  let bank = 0;
  try { const b = +localStorage.getItem('hb.bank'); if (b >= 0 && b <= 3) bank = b;   /* FEATURE: scenes: banks 2-3 = CENAS */ } catch (e) {}
  const bankBase = () => bank * BANK_SIZE;
  function renderBusKeys() {
    for (const [bus, cmd] of [[busP, 'program'], [busV, 'preview']]) {
      bus.innerHTML = '';
      for (let j = 0; j < BANK_SIZE; j++) {
        const i = bankBase() + j;
        const k = document.createElement('button');
        k.className = 'key'; k.dataset.i = i; k.textContent = i >= N ? 'C' + (i - N + 1) : i + 1;
        k.addEventListener('click', () => send(cmd, i));
        bus.appendChild(k);
      }
    }
    $$('.tile').forEach(t => t.classList.toggle('offbank', +t.dataset.i < bankBase() || +t.dataset.i >= bankBase() + BANK_SIZE));
    applyBankView();   // FEATURE: scenes
  }
  renderBusKeys();
  $$('.bankbtn').forEach(b => b.addEventListener('click', () => {
    bank = +b.dataset.bank;
    $$('.bankbtn').forEach(x => x.classList.toggle('on', x === b));
    try { localStorage.setItem('hb.bank', bank); } catch (e) {}
    renderBusKeys();
    if (lastState) render(lastState);   // immediately re-apply PGM/PVW highlighting to the new key set
  }));
  $$('.bankbtn').forEach(b => b.classList.toggle('on', +b.dataset.bank === bank));
  let duration = 500;
  $$('#dur button').forEach(b => b.addEventListener('click', () => {
    duration = +b.dataset.ms; $$('#dur button').forEach(x => x.classList.toggle('on', x === b)); try { localStorage.setItem('hb.duration', duration); } catch (e) {}
  }));
  $('#b-cut').addEventListener('click', () => send('cut'));
  $('#b-dis').addEventListener('click', () => send('dissolve', duration));
  // PROGRAM tela cheia: usa a Fullscreen API do próprio navegador no card do PROGRAM (não abre uma
  // janela nova do sistema - isso já existe em CONFIG > Saídas, para um segundo/terceiro monitor).
  const pgmMon = $('.mon.pgm');
  const toggleProgramFullscreen = () => { if (document.fullscreenElement) document.exitFullscreen(); else pgmMon.requestFullscreen().catch(() => {}); };
  $('#pgm-fs').addEventListener('click', toggleProgramFullscreen);
  pgmMon.querySelector('.screen').addEventListener('dblclick', toggleProgramFullscreen);
  document.addEventListener('fullscreenchange', () => {
    const on = document.fullscreenElement === pgmMon;
    $('#pgm-fs').textContent = on ? '⤢' : '⛶';
    $('#pgm-fs').title = on ? 'Sair da tela cheia (Esc)' : 'Tela cheia (ou duplo clique na imagem)';
  });
  // T-bar: manual fader alongside CUT/DISSOLVE. While the operator is dragging it, the periodic
  // state feed must not fight the drag (same guard style as the ÁUDIO faders).
  let tbarDown = false;
  const tbarEl = $('#tbar');
  tbarEl.addEventListener('pointerdown', () => { tbarDown = true; });
  addEventListener('pointerup', () => { tbarDown = false; });
  tbarEl.addEventListener('input', () => send('tbar', (+tbarEl.value / 1000).toFixed(4)));
  $$('#tabs button').forEach(b => b.addEventListener('click', () => {
    $$('#tabs button').forEach(x => x.classList.toggle('on', x === b));
    $$('.view').forEach(v => v.classList.toggle('on', v.id === 'v-' + b.dataset.v));
    send('tab', b.dataset.v);   // FEATURE: perf - the host lowers monitor/tile rates on tabs that do not show them
    try { b.scrollIntoView({block: 'nearest', inline: 'nearest'}); } catch (e) {}   // nav scrolls when the 10 tabs don't fit
  }));
  addEventListener('keydown', e => {
    if (e.repeat) return;
    if (document.querySelector('.modal.on') || /INPUT|SELECT|TEXTAREA/.test((document.activeElement || {}).tagName || '')) {
      if (e.key === 'Escape') closeModals();
      return;
    }
    if ((e.ctrlKey || e.metaKey) && /^Digit[0-9]$/.test(e.code || '')) {   // Ctrl+1..0 = PTZ preset 1..10
      e.preventDefault();
      const slot = e.code === 'Digit0' ? 9 : +e.code.slice(5) - 1;
      send(e.shiftKey || mode === 'save' ? 'ptzsave' : 'ptzgo', ptzCam, slot);
      return;
    }
    if (e.key >= '1' && e.key <= '8') { send(e.shiftKey ? 'program' : 'preview', bankBase() + (+e.key - 1)); }
    else if (e.code && /^Digit[1-8]$/.test(e.code)) { send(e.shiftKey ? 'program' : 'preview', bankBase() + (+e.code.slice(5) - 1)); }
    else if (e.code === 'Space') { e.preventDefault(); send('cut'); }
    else if (e.code === 'Enter') { send('dissolve', duration); }
  });


