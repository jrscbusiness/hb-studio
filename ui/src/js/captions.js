  // ---------- LEGENDAS AO VIVO ---------- FEATURE: captions
  const capLangs = [['pt', 'Português'], ['en', 'English'], ['es', 'Español'], ['fr', 'Français'], ['de', 'Deutsch'], ['it', 'Italiano'], ['auto', 'Detectar automaticamente']];
  let capSt = null, capModelSig = '', capBoxSig = '';
  const capRows = {};
  (function capInit() {
    const sel = $('#cap-lang'); if (!sel) return;
    sel.innerHTML = capLangs.map(l => `<option value="${l[0]}">${l[1]}</option>`).join('');
    sel.addEventListener('change', () => send('cap', 'set', 'language', sel.value));
    $('#cap-engine').addEventListener('change', e => send('cap', 'set', 'engine', e.target.value));
    $('#cap-prof').addEventListener('change', e => send('cap', 'set', 'profanity', e.target.checked ? 1 : 0));
    $('#cap-srt').addEventListener('change', e => send('cap', 'set', 'saveSrt', e.target.checked ? 1 : 0));
    $('#cap-curl').addEventListener('change', e => send('cap', 'set', 'cloudUrl', e.target.value.trim()));
    $('#cap-cmodel').addEventListener('change', e => send('cap', 'set', 'cloudModel', e.target.value.trim()));
    $('#cap-ckey').addEventListener('change', e => { send('cap', 'set', 'cloudKey', e.target.value.trim()); e.target.value = ''; });
    $('#cap-toggle').addEventListener('click', () => send('cap', capSt && capSt.on ? 'off' : 'on'));
    $('#cap-clear').addEventListener('click', () => send('cap', 'clear'));
  })();
  function renderCap(d) {
    if (!$('#cap-card')) return;
    capSt = d;
    const st = d.state || 'stopped';
    const tg = $('#cap-toggle'); tg.textContent = d.on ? 'DESLIGAR' : 'LIGAR';
    $('#cap-dot').className = 'capdot ' + (d.on ? st : '');
    $('#cap-status').textContent = !d.on ? 'Desligado' : st === 'loading' ? 'Carregando modelo…' : st === 'listening' ? 'Ouvindo' : st === 'error' ? 'Erro: ' + (d.error || 'falha ao iniciar') : 'Iniciando…';
    const lat = $('#cap-lat');
    lat.textContent = d.on && st === 'listening' ? (d.latencyMs || 0) + ' ms' : '— ms';
    lat.title = 'Tempo de reconhecimento + áudio acumulado na fila' + (d.droppedMs ? ' · ' + (d.droppedMs / 1000).toFixed(1).replace('.', ',') + ' s de áudio descartado (computador sem folga: use o modelo leve)' : '');
    lat.style.color = d.on && (d.latencyMs || 0) > 4000 ? 'var(--warn-ink)' : '';
    const c = d.cfg || {};
    const setv = (id, v) => { const el = $(id); if (el && document.activeElement !== el && el.value !== v) el.value = v; };
    setv('#cap-lang', c.language || 'pt'); setv('#cap-engine', c.engine || 'whisper');
    setv('#cap-curl', c.cloudUrl || ''); setv('#cap-cmodel', c.cloudModel || '');
    $('#cap-prof').checked = !!c.profanity; $('#cap-srt').checked = !!c.saveSrt;
    $('#cap-keyhint').textContent = c.hasKey ? '(chave salva)' : '(nenhuma salva)';
    const cloud = c.engine === 'cloud';
    $('#cap-cloud').style.display = cloud ? '' : 'none';
    $('#cap-local').style.display = cloud ? 'none' : '';
    // model rows are created once and updated in place (the page is refreshed ~4x/s; never rebuild under a click)
    const models = d.models || [];
    const sig = models.map(m => m.id).join(',');
    if (sig !== capModelSig) {
      capModelSig = sig; const box = $('#cap-models'); box.innerHTML = '';
      models.forEach(m => {
        const row = document.createElement('div'); row.className = 'fmt capmodel'; row.setAttribute('role', 'radio'); row.tabIndex = 0;
        row.innerHTML = '<span class="rd"></span><div class="grow"><b></b><em></em><div class="capbar" style="display:none"><i></i></div></div><button class="btn sm"></button>';
        const pick = () => send('cap', 'set', 'model', m.id);
        row.onclick = pick; row.onkeydown = e => { if (e.target === row && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); pick(); } };
        row.querySelector('button').onclick = e => { e.stopPropagation(); send('cap', row.dataset.dl === '1' ? 'dlcancel' : 'dl|' + m.id); };
        box.appendChild(row); capRows[m.id] = row;
      });
    }
    models.forEach(m => {
      const row = capRows[m.id]; if (!row) return;
      const dl = d.dl || {}; const busy = dl.active && dl.id === m.id;
      row.classList.toggle('on', c.model === m.id);
      row.dataset.dl = busy ? '1' : '0';
      row.querySelector('b').textContent = m.label;
      row.querySelector('em').textContent = `${m.note} · ${m.sizeMb} MB em disco · ~${m.ramMb} MB de RAM` + (dl.error && dl.id === m.id && !busy ? ' · ERRO: ' + dl.error : '');
      const bar = row.querySelector('.capbar'); bar.style.display = busy ? '' : 'none'; bar.firstChild.style.width = (dl.pct || 0) + '%';
      const b = row.querySelector('button'); b.textContent = busy ? 'CANCELAR ' + (dl.pct || 0) + '%' : m.installed ? 'INSTALADO' : 'BAIXAR';
      b.disabled = !busy && (m.installed || !!dl.active);
    });
    const msg = $('#cap-msg'); msg.textContent = d.msg || (d.srtFile && !d.on ? 'Legendas salvas em: ' + d.srtFile : ''); msg.classList.toggle('err', !!d.msg);
    const bsig = JSON.stringify([d.lines, d.partial]);
    if (bsig !== capBoxSig) {
      capBoxSig = bsig; const bx = $('#cap-box');
      bx.innerHTML = (d.lines || []).map(l => `<div>${esc(l)}</div>`).join('') + (d.partial ? `<div class="cp">${esc(d.partial.replace(/\n/g, ' '))}</div>` : '');
      bx.scrollTop = bx.scrollHeight;
    }
  }
