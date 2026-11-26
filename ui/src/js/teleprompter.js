  // ---------- FEATURE: teleprompter ----------
  // The host is authoritative for position (src/teleprompter/controller.*). It sends {t:'tp', state,
  // pos, rate, ...} on every change plus a ~2 Hz re-sync; this page (and the presenter window)
  // extrapolate pos + rate*dt between messages, so both stay in step and nothing here keeps its own clock.
  const tp = {st: null, recv: 0, script: null, style: null, list: null, H: 1, V: 1, mark: .35, raf: 0, dragging: false, saveT: 0, dirty: false, loadedId: ''};
  const tpEl = {txt: $('#tp-txt'), view: $('#tp-view'), mark: $('#tp-mark'), cd: $('#tp-cd'), area: $('#tp-text')};
  const tpFmt = s => { s = Math.max(0, Math.round(s || 0)); return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };
  const tpPos = () => !tp.st ? 0 : tp.st.state === 'playing' ? Math.min(1, tp.st.pos + tp.st.rate * (performance.now() - tp.recv) / 1000) : tp.st.pos;
  const tpActive = () => !!document.querySelector('#v-tp.on');
  function tpFrame() {
    cancelAnimationFrame(tp.raf);
    const p = tpPos();
    tpEl.txt.style.transform = 'translate3d(0,' + (tp.mark * tp.V - p * tp.H).toFixed(2) + 'px,0)';
    $('#tp-progbar').style.transform = 'scaleX(' + p.toFixed(5) + ')';
    const st = tp.st || {};
    $('#tp-pct').textContent = Math.round(p * 100) + '%';
    const rate = st.rate || 0;
    const remain = st.state === 'playing' && rate > 0 ? (1 - p) / rate : st.remain;
    $('#tp-remain').textContent = 'restam ' + (st.remain || remain ? tpFmt(remain) : '—');
    $('#tp-total').textContent = 'total ' + (st.total ? tpFmt(st.total) : '—');
    if (st.state === 'countdown') {
      const left = Math.max(0, st.cd - (performance.now() - tp.recv) / 1000);
      tpEl.cd.style.display = 'flex'; tpEl.cd.textContent = String(Math.max(1, Math.ceil(left)));
    } else tpEl.cd.style.display = 'none';
    if (tpActive() && (st.state === 'playing' || st.state === 'countdown')) tp.raf = requestAnimationFrame(tpFrame);
  }
  function tpMeasure() {   // only on script/style/resize, never per frame
    const w = tpEl.view.clientWidth || 1;
    tp.V = Math.max(1, tpEl.view.clientHeight);
    const s = tp.style;
    if (s) tpEl.txt.style.fontSize = (s.font * Math.max(.18, w / 1280)).toFixed(1) + 'px';
    tp.H = Math.max(1, tpEl.txt.offsetHeight);
    tpFrame();
    clearTimeout(tp.layoutT);
    // The presenter window reports the real layout when it is open; the host ignores this one then.
    tp.layoutT = setTimeout(() => send('tp', 'layout', 'o', Math.round(tp.H), Math.round(tp.V)), 200);
  }
  function tpApplyStyle() {
    const s = tp.style; if (!s) return;
    const v = tpEl.view.style;
    v.background = s.bg; v.color = s.fg;
    tpEl.txt.style.lineHeight = s.lh; tpEl.txt.style.left = tpEl.txt.style.right = s.margin + '%';
    tp.mark = s.markerpos / 100;
    tpEl.mark.style.display = s.marker ? 'block' : 'none'; tpEl.mark.style.top = (tp.mark * 100) + '%';
    const set = (id, val, out) => { const e = $('#' + id); if (!tp.dragging || document.activeElement !== e) e.value = val; if (out) $('#' + id + 'v').textContent = out; };
    set('tp-font', s.font, s.font); set('tp-lh', s.lh, (+s.lh).toFixed(2).replace(/0$/, '')); set('tp-margin', s.margin, s.margin + '%');
    set('tp-markpos', s.markerpos, s.markerpos + '%'); set('tp-cdsec', s.cdSec, s.cdSec + 's');
    $('#tp-fg').value = s.fg; $('#tp-bg').value = s.bg;
    $('#tp-mirror').checked = !!s.mirror; $('#tp-flip').checked = !!s.flip; $('#tp-marker').checked = !!s.marker;
    tpMeasure();
  }
  function tpBuild() {
    const d = tp.script; if (!d) return;
    tpEl.txt.textContent = '';
    for (const b of d.blocks || []) {
      const p = document.createElement('p');
      if (b.k === 'c') { p.className = 'c'; const sp = document.createElement('span'); sp.textContent = b.t || '·'; p.appendChild(sp); }
      else { p.className = 'p'; p.textContent = b.t; p.dataset.at = b.at; p.addEventListener('click', () => send('tp', 'seek', (p.offsetTop / tp.H).toFixed(5))); }
      tpEl.txt.appendChild(p);
    }
    tpMeasure();
  }
  function tpRenderState() {
    const st = tp.st; if (!st) return;
    const names = {stopped: 'PARADO', countdown: 'CONTAGEM', playing: 'NO AR', paused: 'PAUSADO', finished: 'FIM'};
    $('#tp-state').textContent = names[st.state] || st.state;
    const moving = st.state === 'playing' || st.state === 'countdown';
    $('#tp-play').textContent = moving ? 'PAUSE' : 'PLAY';
    $$('#tp-mode button').forEach(b => b.classList.toggle('on', b.dataset.m === st.mode));
    const sp = $('#tp-speed'), isW = st.mode === 'wpm';
    sp.min = isW ? 20 : 10; sp.max = isW ? 400 : 800;
    if (!tp.dragging) sp.value = isW ? st.wpm : st.pxs;
    $('#tp-speedv').textContent = Math.round(isW ? st.wpm : st.pxs);
    $('#tp-speedlbl').textContent = isW ? 'Palavras/min' : 'Pixels/s';
    $('#tp-pop').disabled = !!st.presenter; $('#tp-popfs').disabled = !st.presenter; $('#tp-popx').disabled = !st.presenter;
    tpFrame();
  }
  function tpRenderList() {
    const d = tp.list; if (!d) return;
    const box = $('#tp-list'); box.textContent = '';
    for (const s of d.scripts || []) {
      const b = document.createElement('button');
      b.className = s.id === d.current ? 'on' : '';
      const n = document.createElement('span'); n.textContent = s.name;
      const w = document.createElement('em'); w.textContent = s.words + ' pal.';
      b.append(n, w);
      b.addEventListener('click', () => { tpFlushSave(); send('tp', 'select', s.id); });
      box.appendChild(b);
    }
  }
  function tpOnScript(d) {
    tp.script = d;
    const a = tpEl.area;
    if (d.id !== tp.loadedId || (document.activeElement !== a && a.value !== d.text)) { a.value = d.text; tp.loadedId = d.id; }
    if (document.activeElement !== $('#tp-name')) $('#tp-name').value = d.name || '';
    $('#tp-edstat').textContent = d.words + ' palavras · ' + d.paragraphs + ' parágrafos';
    tpBuild();
  }
  function tpOnMsg(m) {
    let d; try { d = typeof m === 'string' ? JSON.parse(m) : m; } catch (e) { return; }
    if (!d || typeof d.t !== 'string' || d.t.slice(0, 2) !== 'tp') return;
    if (d.t === 'tp') { tp.st = d; tp.recv = performance.now(); tpRenderState(); }
    else if (d.t === 'tplist') { tp.list = d; tpRenderList(); }
    else if (d.t === 'tpscript') tpOnScript(d);
    else if (d.t === 'tpstyle') { tp.style = d; tpApplyStyle(); }
  }
  window.__tpMessage = tpOnMsg;   // test hook (the browser harness has no host event channel)
  if (host) host.addEventListener('message', e => tpOnMsg(e.data));
  // editor: debounced autosave, flushed before switching script
  function tpFlushSave() {
    clearTimeout(tp.saveT);
    if (tp.dirty && tp.loadedId) { tp.dirty = false; send('tp', 'save', tp.loadedId, tpEl.area.value); }
  }
  tpEl.area.addEventListener('input', () => { tp.dirty = true; clearTimeout(tp.saveT); tp.saveT = setTimeout(tpFlushSave, 500); });
  tpEl.area.addEventListener('blur', tpFlushSave);
  $('#tp-new').onclick = () => { tpFlushSave(); send('tp', 'new', 'Novo roteiro'); };
  $('#tp-del').onclick = () => { if (tp.loadedId && confirm('Excluir o roteiro "' + (tp.script ? tp.script.name : '') + '"?')) send('tp', 'delete', tp.loadedId); };
  const tpRename = () => { const n = $('#tp-name').value.trim(); if (n && tp.loadedId) send('tp', 'rename', tp.loadedId, n); };
  $('#tp-rename').onclick = tpRename;
  $('#tp-name').addEventListener('keydown', e => { if (e.key === 'Enter') tpRename(); });
  $('#tp-import').onclick = () => $('#tp-file').click();
  // Old Brazilian .txt files are often Windows-1252 or UTF-16 (Notepad "Unicode"): detect, never trust blindly.
  async function tpDecode(file) {
    const u = new Uint8Array(await file.arrayBuffer());
    if (u.length >= 2 && u[0] === 0xFF && u[1] === 0xFE) return new TextDecoder('utf-16le').decode(u.subarray(2));
    if (u.length >= 2 && u[0] === 0xFE && u[1] === 0xFF) return new TextDecoder('utf-16be').decode(u.subarray(2));
    try { return new TextDecoder('utf-8', {fatal: true}).decode(u); } catch (e) { return new TextDecoder('windows-1252').decode(u); }
  }
  $('#tp-file').addEventListener('change', async ev => {
    const f = ev.target.files && ev.target.files[0]; ev.target.value = '';
    if (!f) return;
    if (f.size > 1024 * 1024) { $('#tp-edstat').textContent = 'Arquivo grande demais (máximo 1 MB).'; return; }
    try { tpFlushSave(); send('tp', 'import', f.name.replace(/\.[^.]*$/, ''), await tpDecode(f)); }
    catch (e) { $('#tp-edstat').textContent = 'Não foi possível ler o arquivo.'; }
  });
  // transport
  $('#tp-play').onclick = () => send('tp', 'toggle');
  $('#tp-restart').onclick = () => send('tp', 'restart');
  $('#tp-prog').addEventListener('click', e => { const r = e.currentTarget.getBoundingClientRect(); send('tp', 'seek', Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)).toFixed(4)); });
  $('#tp-pop').onclick = () => send('tp', 'presenter', 'open');
  $('#tp-popfs').onclick = () => send('tp', 'presenter', 'fullscreen');
  $('#tp-popx').onclick = () => send('tp', 'presenter', 'close');
  $$('#tp-mode button').forEach(b => b.onclick = () => send('tp', 'mode', b.dataset.m));
  // settings: sliders send on input; while dragging, incoming state must not fight the thumb
  addEventListener('pointerup', () => { tp.dragging = false; });
  for (const [id, cmd] of [['tp-speed', 'speed'], ['tp-font', 'font'], ['tp-lh', 'lh'], ['tp-margin', 'margin'], ['tp-markpos', 'markerpos'], ['tp-cdsec', 'cd']]) {
    const el = $('#' + id);
    el.addEventListener('pointerdown', () => { tp.dragging = true; });
    el.addEventListener('input', () => {
      if (cmd === 'speed') send('tp', 'speed', el.value);
      else if (cmd === 'cd') send('tp', 'countdown', el.value);
      else send('tp', 'style', cmd, el.value);
    });
  }
  for (const [id, key] of [['tp-fg', 'fg'], ['tp-bg', 'bg']]) $('#' + id).addEventListener('input', e => send('tp', 'style', key, e.target.value.toUpperCase()));
  for (const [id, key] of [['tp-mirror', 'mirror'], ['tp-flip', 'flip'], ['tp-marker', 'marker']]) $('#' + id).addEventListener('change', e => send('tp', 'style', key, e.target.checked ? '1' : '0'));
  addEventListener('resize', () => { if (tpActive()) tpMeasure(); });
  $$('#tabs button').forEach(b => b.addEventListener('click', () => { if (b.dataset.v === 'tp') setTimeout(tpMeasure, 0); }));
  // Keyboard, scoped to this tab (capture phase + stopImmediatePropagation so Space does NOT reach the
  // switcher's global "cut" handler). Ignored while typing in a text field/select/textarea.
  addEventListener('keydown', e => {
    if (!tpActive() || e.ctrlKey || e.metaKey || e.altKey) return;
    const a = document.activeElement || {};
    const typing = a.tagName === 'TEXTAREA' || a.tagName === 'SELECT' || (a.tagName === 'INPUT' && /^(text|number|password|search|)$/.test(a.type || ''));
    if (typing || document.querySelector('.modal.on')) return;
    let cmd = null;
    if (e.code === 'Space') cmd = ['toggle'];
    else if (e.key === 'ArrowUp') cmd = ['nudge', 10];
    else if (e.key === 'ArrowDown') cmd = ['nudge', -10];
    else if (e.key === 'Home') cmd = ['restart'];
    if (!cmd) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (!e.repeat || cmd[0] === 'nudge') send('tp', ...cmd);
  }, true);
  if (host) send('tp', 'hello');
  // ---------- /FEATURE: teleprompter ----------
