  // ---------- outputs: second/third-monitor windows + NDI output (CONFIG tab) ----------
  $('#out-pgm-open').addEventListener('click', () => send('outwin', 'program', 'open'));
  $('#out-pgm-close').addEventListener('click', () => send('outwin', 'program', 'close'));
  $('#out-pvw-open').addEventListener('click', () => send('outwin', 'preview', 'open'));
  $('#out-pvw-close').addEventListener('click', () => send('outwin', 'preview', 'close'));
  let ndiOutEnabled = false;
  function renderOutputs(d) {
    ndiOutEnabled = !!d.ndiEnabled;
    const btn = $('#out-ndi-toggle');
    btn.textContent = ndiOutEnabled ? 'DESLIGAR' : 'LIGAR';
    btn.classList.toggle('primary', ndiOutEnabled);
    if (document.activeElement !== $('#out-ndi-name')) $('#out-ndi-name').value = d.ndiName || 'HB Studio Program';
    const note = $('#out-ndi-note');
    note.textContent = ndiOutEnabled
      ? (d.ndiAvailable ? 'Ligada: outros programas NDI na rede (inclusive outro HB Studio) já enxergam "' + (d.ndiName || '') + '".'
                        : 'Instale o NDI Runtime (ndi.video/tools) para a saída NDI funcionar de verdade.')
      : 'Desligada. Ao ligar, o PROGRAM passa a ser visível como uma fonte NDI na rede local.';
  }

  /* FEATURE: remote */
  let remoteOn = false;
  function renderRemote(d) {
    remoteOn = !!d.enabled;
    const b = $('#rm-toggle'); b.textContent = remoteOn ? 'DESLIGAR' : 'LIGAR'; b.classList.toggle('primary', remoteOn);
    $('#rm-info').style.display = remoteOn ? '' : 'none';
    $('#rm-err').textContent = d.err || '';
    if (!remoteOn) return;
    $('#rm-urls').textContent = (d.urls || []).join('  ');
    $('#rm-pin').textContent = d.lockedMs > 0 ? 'bloqueado ' + Math.ceil(d.lockedMs / 1000) + ' s' : (d.pin || '——');
    const box = $('#rm-devs'); box.textContent = '';
    (d.devices || []).forEach(x => {
      const r = document.createElement('div'); r.className = 'row';
      const k = document.createElement('span'); k.className = 'k'; k.textContent = x.name + ' · ' + x.ip + (x.thumbs ? ' · miniaturas' : '');
      const v = document.createElement('span'); v.className = 'v'; v.textContent = (x.online ? 'conectado ' : 'offline ');
      const kb = document.createElement('button'); kb.className = 'btn sm'; kb.textContent = 'EXPULSAR'; kb.onclick = () => send('remote', 'kick', x.id);
      v.appendChild(kb); r.appendChild(k); r.appendChild(v); box.appendChild(r);
    });
  }
  $('#rm-toggle').addEventListener('click', () => send('remote', remoteOn ? 'off' : 'on'));
  $('#rm-newpin').addEventListener('click', () => send('remote', 'newpin'));
  $('#rm-revoke').addEventListener('click', () => send('remote', 'revoke'));
  /* /FEATURE */

