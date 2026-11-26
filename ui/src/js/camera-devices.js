  // ---------- input devices (CÂMERAS tab) ----------
  let devs = null, slots = [], devBusy = false;
  const camSvg = '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><rect x="3" y="7" width="12" height="10" rx="2.5"/><path d="M15 11l6-3v8l-6-3"/></svg>';
  const stTxt = {online: 'AO VIVO', connecting: 'CONECTANDO', nosignal: 'SEM SINAL', error: 'ERRO', test: 'TESTE', off: 'DESLIGADA'};
  function renderDevices() {
    const box = $('#dev-list'); box.innerHTML = '';
    if (!devs) { box.innerHTML = '<div class="devempty">Procurando dispositivos…</div>'; return; }
    if (!devs.length) { box.innerHTML = '<div class="devempty"><b>Nenhum dispositivo de vídeo encontrado.</b><br>Conecte uma webcam ou ative uma câmera virtual (ex.: no OBS: <i>Iniciar Câmera Virtual</i>) e clique em “Atualizar lista”. Verifique também em Configurações › Privacidade e segurança › Câmera se apps da área de trabalho têm permissão.</div>'; return; }
    devs.forEach(d => {
      const row = document.createElement('div'); row.className = 'dev';
      const cur = slots.findIndex(x => x && x.k === 'dev' && x.id === d.id);
      const badgeTxt = d.ndi ? 'NDI' : (d.virt ? 'VIRTUAL' : 'CÂMERA');
      row.innerHTML = '<div class="ico">' + camSvg + '</div><div class="nm"><b></b><em></em></div><span class="badge' + (d.virt || d.ndi ? ' v' : '') + '">' + badgeTxt + '</span><div class="assign"></div>';
      row.querySelector('b').textContent = d.name;
      row.querySelector('em').textContent = cur >= 0 ? 'Em uso na CAM ' + (cur + 1) : 'Disponível';
      const as = row.querySelector('.assign');
      for (let i = 0; i < N; i++) {
        const b = document.createElement('button'); b.textContent = i + 1; b.title = 'Usar na CAM ' + (i + 1);
        if (slots[i] && slots[i].k === 'dev' && slots[i].id === d.id) b.className = 'on';
        b.onclick = () => send('devassign', i, d.id);
        as.appendChild(b);
      }
      box.appendChild(row);
    });
  }
  function renderSlots() {
    const box = $('#slot-list'); box.innerHTML = '';
    for (let i = 0; i < N; i++) {
      const x = slots[i] || {k: 'test', st: 'test', name: '', en: true};
      const r = document.createElement('div'); r.className = 'slot';
      const dims = x.k === 'dev' && x.w ? x.w + '×' + x.h + (x.fps ? ' · ' + x.fps.toFixed(0) + ' fps' : '') : '';
      r.innerHTML = '<span class="n">CAM ' + (i + 1) + '</span><div class="w"><b></b><em></em></div><span class="st ' + x.st + '">' + stTxt[x.st] + '</span>' +
        (x.k === 'dev' ? '<button class="icon pwr" title="Ligar/desligar esta câmera">⏻</button><button class="icon cfg" title="Configurações">⚙</button><button class="x">Remover</button>' : '');
      r.querySelector('b').textContent = x.k === 'dev' ? x.name : 'Fonte de teste (gerada pelo programa)';
      r.querySelector('em').textContent = x.msg || dims || (x.k === 'dev' ? '' : 'Não é uma câmera real');
      const rm = r.querySelector('.x'); if (rm) rm.onclick = () => send('devclear', i);
      const pwr = r.querySelector('.pwr');
      if (pwr) { pwr.classList.toggle('off', x.en === false); pwr.onclick = () => send(x.en === false ? 'devenable' : 'devdisable', i); }
      const cfg = r.querySelector('.cfg');
      if (cfg) cfg.onclick = () => openCamSettings(i, x);
      box.appendChild(r);
    }
  }
  // Redraw only when the data really changed, and never between mouse-down and mouse-up: replacing
  // a button while it is being clicked cancels the click (this is what made "assign to CAM n" fail).
  let listSig = '', pointerDown = false, listPending = false;
  const drawLists = () => { renderSlots(); renderDevices(); };
  function renderLists(force) {
    const sig = JSON.stringify([slots.map(x => x && [x.k, x.st, x.name, x.id, x.w, x.h, Math.round(x.fps || 0), x.msg, x.en]), devs]);
    if (!force && sig === listSig) return;
    if (pointerDown) { listPending = true; return; }
    listSig = sig; listPending = false; drawLists();
  }
  const releasePointer = () => { pointerDown = false; if (listPending) renderLists(true); };
  addEventListener('pointerdown', () => { pointerDown = true; }, true);
  addEventListener('pointerup', () => setTimeout(releasePointer, 0), true);
  addEventListener('pointercancel', releasePointer, true);
  addEventListener('blur', releasePointer);
  $('#dev-refresh').onclick = () => { devs = null; renderLists(true); send('devrefresh'); };
  $$('#tabs button').forEach(b => b.addEventListener('click', () => { if (b.dataset.v === 'cam') { send('devrefresh'); } }));

