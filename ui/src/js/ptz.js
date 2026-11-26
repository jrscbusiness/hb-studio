  // ---------- PTZ + presets ----------
  let ptz = null, ptzCam = 0, mode = 'go', lastPvw = -1;
  const DEF_PORT = {onvif: 80, 'visca-ip': 52381};
  const camsEl = $('#ptz-cams');
  for (let i = 0; i < N; i++) {
    const b = document.createElement('button'); b.textContent = i + 1;
    b.onclick = () => { $('#ptz-follow').checked = false; ptzCam = i; renderPtz(); };
    camsEl.appendChild(b);
  }
  $('#ptz-follow').onchange = () => { if ($('#ptz-follow').checked && lastPvw >= 0) { ptzCam = lastPvw; renderPtz(); } };
  $$('#ptz-mode button').forEach(b => b.onclick = () => {
    mode = b.dataset.m; $$('#ptz-mode button').forEach(x => x.classList.toggle('on', x === b)); renderPtz();
  });
  const camCfg = () => ptz && ptz.cams[ptzCam];
  function renderPtz() {
    const c = camCfg(); if (!c) return;
    $$('#ptz-cams button').forEach((b, i) => b.classList.toggle('on', i === ptzCam));
    $('#ptz-state').textContent = c.connected ? 'CONECTADA' : 'SEM CÂMERA PTZ';
    const box = $('#presets'); box.className = 'presets mode-' + mode; box.innerHTML = '';
    c.names.forEach((nm, i) => {
      const b = document.createElement('button'); b.className = 'pre' + (nm ? ' named' : '');
      const n = document.createElement('b'); n.textContent = i + c.base;
      const l = document.createElement('span'); l.textContent = nm || '—';
      b.append(n, l); b.title = (nm || 'Preset ' + (i + c.base)) + (i < 10 ? '  ·  Ctrl+' + ((i + 1) % 10) : '');
      b.onclick = () => presetClick(i);
      box.appendChild(b);
    });
    const hint = {go: 'Clique chama o preset (Ctrl+1…0).', save: 'Clique grava a posição atual da câmera no preset (Ctrl+Shift+1…0).', edit: 'Clique para dar nome ou limpar o preset.'}[mode];
    $('#ptz-note').textContent = hint + (c.connected ? '' : ' Controle de movimento por rede ainda em desenvolvimento: nomes e configurações dos presets já ficam salvos.');
  }
  function presetClick(i) {
    if (mode === 'go') send('ptzgo', ptzCam, i);
    else if (mode === 'save') send('ptzsave', ptzCam, i);
    else openName(i);
  }
  const hold = (el, down, up) => {
    el.addEventListener('pointerdown', e => { el.setPointerCapture(e.pointerId); down(); });
    ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(ev => el.addEventListener(ev, () => { if (el._on) { el._on = false; up(); } }));
    el.addEventListener('pointerdown', () => { el._on = true; });
  };
  $$('.dpad button').forEach(b => {
    if (b.dataset.d === 'stop') { b.onclick = () => send('ptzstop', ptzCam); return; }
    hold(b, () => send('ptzmove', ptzCam, b.dataset.d), () => send('ptzstop', ptzCam));
  });
  $$('.zrow button[data-z]').forEach(b => hold(b, () => send('ptzzoom', ptzCam, b.dataset.z), () => send('ptzzoom', ptzCam, 'stop')));
  $$('.zrow button[data-f]').forEach(b => {
    if (b.dataset.f === 'auto') b.onclick = () => send('ptzfocus', ptzCam, 'auto');
    else hold(b, () => send('ptzfocus', ptzCam, b.dataset.f), () => send('ptzfocus', ptzCam, 'stop'));
  });

  // config sheet
  const closeModals = () => $$('.modal').forEach(m => m.classList.remove('on'));
  const portNote = () => { const p = $('#f-proto').value; const d = DEF_PORT[p];
    $('#f-port').placeholder = d ? String(d) : 'porta do conversor';
    $('#f-portnote').textContent = d ? 'Vazio ou 0 = padrão (' + d + ')' : 'Informe a porta TCP do conversor serial↔IP'; };
  $('#f-proto').onchange = portNote;
  $('#f-speed').oninput = () => $('#f-speedv').textContent = $('#f-speed').value;
  $('#ptz-cfg').onclick = () => {
    const c = camCfg(); if (!c) return;
    $('#cfg-title').textContent = 'CAM ' + (ptzCam + 1);
    $('#f-proto').value = c.protocol; $('#f-host').value = c.host; $('#f-port').value = c.port || '';
    $('#f-addr').value = c.address; $('#f-slots').value = String(c.slots); $('#f-base').value = String(c.base);
    $('#f-store').value = c.storage; $('#f-speed').value = c.speed; $('#f-speedv').textContent = c.speed;
    $('#f-ipan').checked = c.invPan; $('#f-itilt').checked = c.invTilt;
    portNote(); $('#modal').classList.add('on');
  };
  const readCfg = () => JSON.stringify({protocol: $('#f-proto').value, host: $('#f-host').value.trim(), port: +$('#f-port').value || 0,
    address: +$('#f-addr').value || 1, slots: +$('#f-slots').value, base: +$('#f-base').value, storage: $('#f-store').value,
    speed: +$('#f-speed').value, invPan: $('#f-ipan').checked, invTilt: $('#f-itilt').checked});
  $('#cfg-save').onclick = () => { send('ptzcfg', ptzCam, readCfg()); closeModals(); };
  $('#cfg-all').onclick = () => { send('ptzall', readCfg()); closeModals(); };
  $('#cfg-cancel').onclick = $('#cfg-x').onclick = closeModals;

  // preset name sheet
  let nameSlot = 0;
  function openName(i) {
    const c = camCfg(); nameSlot = i;
    $('#nm-cap').textContent = 'CAM ' + (ptzCam + 1) + ' · Preset';
    $('#nm-title').textContent = String(i + c.base);
    $('#nm-input').value = c.names[i] || ''; $('#modal2').classList.add('on');
    setTimeout(() => $('#nm-input').focus(), 30);
  }
  const saveName = v => { send('ptzname', ptzCam, nameSlot, v); closeModals(); };
  $('#nm-ok').onclick = () => saveName($('#nm-input').value);
  $('#nm-clear').onclick = () => saveName('');
  $('#nm-cancel').onclick = $('#nm-x').onclick = closeModals;
  $('#nm-input').addEventListener('keydown', e => { if (e.key === 'Enter') saveName($('#nm-input').value); });
  $$('.modal').forEach(m => m.addEventListener('mousedown', e => { if (e.target === m) closeModals(); }));


