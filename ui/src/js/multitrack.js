  // ---- FEATURE: multitrack ----
  let miso = null;
  const mtNum = (n, d) => Number(n).toFixed(d == null ? 1 : d).replace('.', ',');
  function renderMultitrack(d) {
    miso = d;
    const lock = !!d.locked;
    const chip = (kind, id, label, on, live, title) => `<label class="mt-chip${on ? ' on' : ''}${live ? '' : ' off'}" title="${esc(title || '')}"><input type="checkbox" data-mt="${kind}" data-id="${id}" ${on ? 'checked' : ''} ${lock ? 'disabled' : ''}>${esc(label)}</label>`;
    $('#mt-cams').innerHTML = d.cams.length ? d.cams.map(c => chip('cam', c.slot, c.label + (c.device ? ' · ' + c.device : '') + (c.live ? '' : ' (sem sinal)'), c.sel, c.live, c.w ? c.w + '×' + c.h : '')).join('') : '<span class="note">Nenhuma câmera ao vivo.</span>';
    $('#mt-chans').innerHTML = d.chans.map(c => chip('ch', c.strip, c.name, c.sel, true)).join('');
    const fa = $('#mt-afmt'); if (document.activeElement !== fa) fa.innerHTML = d.formats.map(f => `<option value="${esc(f.id)}">${esc(f.label)}</option>`).join('');
    const set = (id, v) => { const el = $(id); if (el && document.activeElement !== el) el.value = v; };
    set('#mt-res', d.cfg.res); set('#mt-fps', d.cfg.fps); set('#mt-afmt', d.cfg.afmt); set('#mt-tap', d.cfg.tap); set('#mt-vkbps', d.cfg.vkbps); set('#mt-rotmin', d.cfg.rotmin);
    $('#mt-expert').checked = !!d.cfg.expert;
    for (const id of ['#mt-res', '#mt-fps', '#mt-afmt', '#mt-tap', '#mt-vkbps', '#mt-rotmin', '#mt-expert']) $(id).disabled = lock;
    const e = d.est, nTracks = e.video + e.audio;
    let h;
    if (!nTracks) h = 'Nada selecionado: grava somente o Program, como sempre.';
    else {
      h = `<b>${e.video}</b> câmera(s) + <b>${e.audio}</b> canal(is): disco <b>${mtNum(e.mbps)} MB/s</b> (~${mtNum(e.gbh)} GB/h, com o Program)` +
        (e.minutes >= 0 ? `, espaço para ~<b>${Math.round(e.minutes)} min</b>` : '') +
        `<br>Carga de codificação estimada: <b>${Math.round(e.load)}%</b> do limite seguro (nível ${esc(e.tier)}${e.video ? ', até ' + e.maxCams + ' câmera(s) nesse formato' : ''}).`;
    }
    if (e.verdict === 'blocked') h += '<br><b>Bloqueado:</b> reduza o plano ou ative o modo avançado.';
    else if (e.verdict === 'warning') h += '<br><b>Atenção:</b> perto ou acima do limite seguro.';
    if (e.notes && e.notes.length) h += '<br>' + e.notes.map(esc).join('<br>');
    const box = $('#mt-est'); box.className = 'mt-est ' + e.verdict; box.innerHTML = h;
    const s = d.sess; let t = '';
    if (s && s.tracks && s.tracks.length) {
      t = '<div class="mt-trk h"><span>Trilha</span><span>Nome</span><span>Estado</span><span>Tempo</span><span>Perdidos</span><span>Repet./sil.</span></div>' +
        s.tracks.map(k => `<div class="mt-trk"><span class="n">${esc(k.id)}</span><span class="n" title="${esc(k.error || '')}">${esc(k.name)}${k.error ? ' <span class="e">' + esc(k.error) + '</span>' : ''}</span><span>${esc({recording: 'gravando', finished: 'finalizada', error: 'erro', 'disk-stopped': 'parada: disco', 'not-started': '—'}[k.state] || k.state)}</span><span class="n">${fmtTime(k.seconds)}</span><span class="n">${k.dropped}</span><span class="n">${k.kind === 'video' ? k.repeated : k.silence}</span></div>`).join('');
      if (s.warnings && s.warnings.length) t += '<p class="note err" style="margin-top:6px">' + s.warnings.map(esc).join('<br>') + '</p>';
      if (s.active) t += `<p class="note" style="margin-top:6px">Espaço livre: ${fmtBytes(s.free || 0)} · pasta: ${esc(s.dir)}</p>`;
    }
    $('#mt-tracks').innerHTML = t;
  }
  $('#mt-cams').addEventListener('change', ev => { const c = ev.target; if (c.dataset.mt) send('rec', 'iso', c.dataset.mt, c.dataset.id, c.checked ? 1 : 0); });
  $('#mt-chans').addEventListener('change', ev => { const c = ev.target; if (c.dataset.mt) send('rec', 'iso', c.dataset.mt, c.dataset.id, c.checked ? 1 : 0); });
  for (const [id, key] of [['#mt-res', 'res'], ['#mt-fps', 'fps'], ['#mt-afmt', 'afmt'], ['#mt-tap', 'tap'], ['#mt-vkbps', 'vkbps'], ['#mt-rotmin', 'rotmin']])
    $(id).addEventListener('change', ev => send('rec', 'iso', 'cfg', key, ev.target.value));
  $('#mt-expert').addEventListener('change', ev => send('rec', 'iso', 'cfg', 'expert', ev.target.checked ? 1 : 0));
  $('#mt-open').onclick = () => send('rec', 'iso', 'open');
  // ---- /FEATURE ----
