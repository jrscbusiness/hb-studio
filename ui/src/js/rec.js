  // ---------- GRAVAÇÃO ----------
  const fmtBytes = b => b >= 1073741824 ? (b / 1073741824).toFixed(2).replace('.', ',') + ' GB' : (b / 1048576).toFixed(1).replace('.', ',') + ' MB';
  const fmtTime = s => { s = Math.floor(s); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); return (h ? h + ':' : '') + String(m).padStart(h ? 2 : 1, '0') + ':' + String(s % 60).padStart(2, '0'); };
  function renderRecProfiles(d) {
    recp = d; const cur = d.cfg.profile;
    for (const [id, kind] of [['#fmt-video', 'video'], ['#fmt-audio', 'audio']]) {
      const box = $(id); box.innerHTML = '';
      const list = d.profiles.filter(p => p.kind === kind);
      if (list.length) { const h = document.createElement('div'); h.className = 'cap'; h.style.margin = '12px 0 8px'; h.textContent = kind === 'video' ? 'Vídeo + áudio' : 'Somente áudio'; box.appendChild(h); }
      list.forEach(p => {
        const f = document.createElement('div'); f.className = 'fmt' + (p.id === cur ? ' on' : ''); f.setAttribute('role', 'radio'); f.tabIndex = 0;
        f.innerHTML = `<span class="rd"></span><div><b>${esc(p.label)}</b><em>${esc(p.desc)}</em></div><span class="ext">.${esc(p.ext || 'pasta')}</span>`;
        const pick = () => send('rec', 'cfg', 'profile', p.id);
        f.onclick = pick; f.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } };
        box.appendChild(f);
      });
    }
    const c = d.cfg;
    for (const k of ['folder', 'template', 'videoKbps', 'videoQuality', 'keyframeSec', 'audioKbps', 'audioRate', 'splitMin', 'splitMB', 'minFreeMB']) { const el = $('#rc-' + k); if (el && document.activeElement !== el) el.value = c[k]; }
    const id = cur || '';
    const show = {vk: /^mp4/.test(id), vq: /^(avi|jpeg)/.test(id), kf: /^mp4/.test(id), ak: /^(mp4|m4a|mp3)/.test(id), ar: true};
    document.querySelectorAll('#v-rec label[data-for]').forEach(l => l.style.display = show[l.dataset.for] ? '' : 'none');
    const p = d.profiles.find(x => x.id === cur);
    $('#rc-note').textContent = p ? (p.video ? `Vídeo gravado em ${d.video.w}×${d.video.h} a ${Math.round(d.video.fps)} fps (o formato do Program).` : 'Grava somente o áudio do Program.') : 'Escolha um formato.';
    if (!p && d.profiles.length) $('#rc-note').textContent = 'O formato salvo não está disponível neste computador. Escolha outro.';
    renderRec();
  }
  function renderRec() {
    const on = recst.state === 'recording' || recst.state === 'stopping';
    for (const b of [$('#rec-btn'), $('#rec-btn2')]) { if (!b) continue; b.classList.toggle('on', on); b.querySelector('span').textContent = on ? (b.id === 'rec-btn' ? fmtTime(recst.seconds || 0) : 'PARAR') : (b.id === 'rec-btn' ? 'REC' : 'GRAVAR'); b.disabled = recst.state === 'stopping'; }
    const rows = on ? [['Arquivo', (recst.file || '').split(/[\\/]/).pop()], ['Duração', fmtTime(recst.seconds || 0)], ['Tamanho', fmtBytes(recst.bytes || 0)], ['Espaço livre', fmtBytes(recst.freeBytes || 0)], ['Parte', recst.segment || 1], ['Quadros perdidos', recst.droppedVideo || 0], ['Quadros repetidos', recst.repeatedVideo || 0], ['Áudio perdido', (recst.droppedAudio || 0) ? 'sim' : '0']]
      : [['Estado', 'parado'], ['Último arquivo', recst.last ? recst.last.split(/[\\/]/).pop() : '—']];
    $('#rec-stat').innerHTML = rows.map(r => `<span class="k">${r[0]}</span><span class="v">${esc(r[1])}</span>`).join('');
    $('#rec-err').textContent = recst.error ? 'Erro: ' + recst.error : '';
  }
