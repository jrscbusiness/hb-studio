  // ---------- FEATURE: scenes (templates prontos + CENAS bank) ----------
  let scenesSt = {scenes: [], templates: [], max: 16};
  const SC_N = 16;
  const sceneBySlot = k => (scenesSt.scenes.find(e => e.slot === k) || {}).scene;
  function sceneLabel(i) { const sc = sceneBySlot(i - N); return sc ? sc.name : 'CENA ' + (i - N + 1); }
  function applyBankView() { tiles.classList.toggle('scenes', bank >= 2); }
  for (let k = 0; k < SC_N; k++) {
    const t = document.createElement('div');
    t.className = 'tile scene sc-empty'; t.dataset.i = N + k;
    t.innerHTML = `<canvas width="192" height="108"></canvas><div class="lb"><b>CENA ${k + 1}</b><em>VAZIA</em></div><div class="tally"></div>`;
    t.addEventListener('click', () => { if (sceneBySlot(k)) send('preview', N + k); else openPicker(); });
    t.addEventListener('dblclick', () => { if (sceneBySlot(k)) send('program', N + k); });
    t.addEventListener('contextmenu', e => { e.preventDefault(); if (sceneBySlot(k)) openSceneMenu(e, k); });
    tiles.appendChild(t);
    canv['c' + k] = t.querySelector('canvas').getContext('2d');
  }
  applyBankView();
  function renderSceneTiles() {
    for (let k = 0; k < SC_N; k++) {
      const t = document.querySelector(`.tile.scene[data-i="${N + k}"]`), sc = sceneBySlot(k);
      t.classList.toggle('sc-empty', !sc);
      t.querySelector('b').textContent = sc ? sc.name : 'CENA ' + (k + 1);
      t.querySelector('em').textContent = sc ? 'CENA' : 'VAZIA';
      if (!sc) { const c = canv['c' + k].canvas; canv['c' + k].clearRect(0, 0, c.width, c.height); }
    }
    if (lastState) render(lastState);
  }

  // Thumbnail / editor drawing: placeholder colours per role, no cameras needed.
  const phColor = role => { let h = 2166136261; for (const ch of (role || 'x')) { h ^= ch.charCodeAt(0); h = Math.imul(h, 16777619) >>> 0; } return `hsl(${h % 360} 38% 42%)`; };
  const layersByZ = sc => (sc.layers || []).map((l, i) => ({l, i})).sort((a, b) => ((a.l.z || 0) - (b.l.z || 0)) || (a.i - b.i));
  function rrPath(ctx, x, y, w, h, r) {
    r = Math.max(0, Math.min(r, w / 2, h / 2));
    ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  }
  function layerBox(l, W, H) {
    const r = l.rect || [0, 0, 1, 1];
    let x = r[0] * W, y = r[1] * H, w = r[2] * W, h = r[3] * H;
    if (l.fit === 'fit') {   // 16:9 content letterboxed inside the box
      const cw = Math.min(w, h * 16 / 9), ch = cw * 9 / 16;
      x += (w - cw) / 2; y += (h - ch) / 2; w = cw; h = ch;
    }
    return {x, y, w, h};
  }
  function drawScene(cv, sc, sel) {
    const ctx = cv.getContext('2d'), W = cv.width, H = cv.height;
    const bg = sc.background || {};
    if (bg.type === 'gradient') {
      const a = (bg.angle === undefined ? 90 : bg.angle) * Math.PI / 180, c = Math.cos(a), s = Math.sin(a);
      const len = Math.abs(W * c) + Math.abs(H * s);
      const g = ctx.createLinearGradient(W / 2 - c * len / 2, H / 2 - s * len / 2, W / 2 + c * len / 2, H / 2 + s * len / 2);
      g.addColorStop(0, bg.from || '#000'); g.addColorStop(1, bg.to || '#000'); ctx.fillStyle = g;
    } else ctx.fillStyle = bg.color || '#0b1220';
    ctx.fillRect(0, 0, W, H);
    for (const {l, i} of layersByZ(sc)) {
      const b = layerBox(l, W, H);
      ctx.save();
      ctx.globalAlpha = l.opacity === undefined ? 1 : l.opacity;
      const rad = (l.radius || 0) * Math.min(b.w, b.h);
      if (l.shadow) { ctx.shadowColor = 'rgba(0,0,0,' + (l.shadow.opacity || .5) + ')'; ctx.shadowBlur = (l.shadow.blur || .02) * H; ctx.shadowOffsetX = (l.shadow.dx || 0) * H; ctx.shadowOffsetY = (l.shadow.dy || 0) * H; }
      rrPath(ctx, b.x, b.y, b.w, b.h, rad); ctx.fillStyle = (l.slot >= 0 && slots[l.slot] && slots[l.slot].k === 'dev') ? '#2d6a4f' : phColor(l.role || 'slot' + l.slot); ctx.fill();
      ctx.shadowColor = 'transparent';
      if (l.border && l.border.width) { ctx.lineWidth = Math.max(1, l.border.width * H); ctx.strokeStyle = l.border.color || '#fff'; rrPath(ctx, b.x, b.y, b.w, b.h, rad); ctx.stroke(); }
      if (W >= 120) {
        ctx.fillStyle = 'rgba(255,255,255,.92)'; ctx.font = '600 ' + Math.max(9, Math.round(H * 0.055)) + 'px sans-serif'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
        ctx.fillText((l.role || (l.slot >= 0 ? 'CAM ' + (l.slot + 1) : 'camada')).slice(0, 22), b.x + b.w / 2, b.y + b.h / 2, b.w - 6);
      }
      ctx.restore();
      if (sel === i) {
        ctx.save(); ctx.strokeStyle = '#ffd24a'; ctx.lineWidth = 2; ctx.setLineDash([6, 4]); ctx.strokeRect(b.x, b.y, b.w, b.h); ctx.setLineDash([]);
        ctx.fillStyle = '#ffd24a'; ctx.fillRect(l.rect[0] * W + l.rect[2] * W - 9, l.rect[1] * H + l.rect[3] * H - 9, 9, 9); ctx.restore();
      }
    }
    for (const o of (sc.overlays || [])) {   // overlay presets are plain data: just hint them in the preview
      ctx.save(); ctx.textBaseline = 'middle';
      if (o.type === 'lowerthird') { ctx.fillStyle = 'rgba(31,107,255,.85)'; ctx.fillRect(W * .04, H * .78, W * .42, H * .13); if (W >= 120) { ctx.fillStyle = '#fff'; ctx.font = '600 ' + Math.round(H * .05) + 'px sans-serif'; ctx.textAlign = 'left'; ctx.fillText(String(o.text || ''), W * .06, H * .845, W * .38); } }
      else if (W >= 120) { ctx.fillStyle = '#fff'; ctx.textAlign = 'center'; ctx.font = '700 ' + Math.round(H * .09) + 'px sans-serif'; ctx.fillText(String(o.text || ''), W / 2, H * .47, W * .9); if (o.subtitle) { ctx.font = '500 ' + Math.round(H * .05) + 'px sans-serif'; ctx.fillText(String(o.subtitle), W / 2, H * .6, W * .9); } }
      ctx.restore();
    }
  }
  const mkThumb = (sc, w, h) => { const c = document.createElement('canvas'); c.width = w; c.height = h; drawScene(c, sc); return c; };

  // ----- picker: template list -> map roles -> apply -----
  const CATS = [['todos', 'Todos'], ['culto', 'Culto'], ['aula', 'Aula'], ['podcast', 'Podcast'], ['evento', 'Evento'], ['meus', 'Meus']];
  let pickCat = 'todos', pickT = null;
  function openPicker() { pickT = null; $('#scp-list').style.display = ''; $('#scp-map').style.display = 'none'; $('#scp-title').textContent = 'Escolha um template'; $('#sc-pick').classList.add('on'); renderPicker(); }
  function renderPicker() {
    if (pickT) return;
    const tabs = $('#scp-tabs'); tabs.innerHTML = '';
    CATS.forEach(([id, label]) => { const b = document.createElement('button'); b.textContent = label; b.dataset.cat = id; b.className = id === pickCat ? 'on' : ''; b.onclick = () => { pickCat = id; renderPicker(); }; tabs.appendChild(b); });
    const grid = $('#scp-grid'); grid.innerHTML = '';
    const list = scenesSt.templates.filter(t => pickCat === 'todos' || (pickCat === 'meus' ? !t.builtin : t.category === pickCat));
    if (!list.length) grid.innerHTML = '<p class="note">Nenhum template aqui ainda. Use “Importar template…” ou salve uma cena como “meu template”.</p>';
    list.forEach(t => {
      const card = document.createElement('button'); card.className = 'sc-card'; card.dataset.id = t.id;
      const mini = document.createElement('div'); mini.className = 'mini';
      t.scenes.slice(0, 3).forEach(sc => mini.appendChild(mkThumb(sc, 160, 90)));
      card.appendChild(mini);
      card.insertAdjacentHTML('beforeend', `<b>${esc(t.name)}</b><em>${t.scenes.length} cena(s)${t.roles.length ? ' · papéis: ' + esc(t.roles.join(', ')) : ' · sem câmeras'}</em>`);
      card.onclick = () => pickTemplate(t);
      grid.appendChild(card);
    });
  }
  function defaultRoleMap(roles) {
    const all = slots.length ? slots.map((x, i) => i) : Array.from({length: N}, (_, i) => i);
    const live = all.filter(i => slots[i] && slots[i].k === 'dev' && slots[i].en !== false);
    const pool = [...live, ...all.filter(i => !live.includes(i))];
    const out = {}; roles.forEach((r, k) => { out[r] = k < pool.length ? pool[k] : -1; }); return out;
  }
  function pickTemplate(t) {
    pickT = t;
    $('#scp-list').style.display = 'none'; $('#scp-map').style.display = ''; $('#scp-title').textContent = t.name;
    $('#scp-desc').textContent = t.description || '';
    const sb = $('#scp-scenes'); sb.innerHTML = '';
    t.scenes.forEach(sc => {
      const row = document.createElement('label'); row.className = 'sc-row chk'; row.style.flexDirection = 'row';
      const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = true; cb.dataset.scene = sc.id;
      row.appendChild(cb); row.appendChild(mkThumb(sc, 192, 108)); row.insertAdjacentHTML('beforeend', `<span>${esc(sc.name)}</span>`);
      sb.appendChild(row);
    });
    const rb = $('#scp-roles'); rb.innerHTML = '';
    const map = defaultRoleMap(t.roles);
    if (!t.roles.length) rb.innerHTML = '<p class="note">Este template não usa câmeras: é só fundo e gráficos.</p>';
    t.roles.forEach(r => {
      const lab = document.createElement('label'); lab.textContent = r;
      const sel = document.createElement('select'); sel.dataset.role = r;
      sel.innerHTML = '<option value="-1">— nenhuma (painel vazio) —</option>' + Array.from({length: N}, (_, i) => {
        const x = slots[i], nm = x && x.k === 'dev' ? x.name : 'TESTE';
        return `<option value="${i}">CAM ${i + 1} — ${esc(nm)}</option>`;
      }).join('');
      sel.value = String(map[r]); lab.appendChild(sel); rb.appendChild(lab);
    });
    const live = slots.filter(x => x.k === 'dev').length;
    $('#scp-note').textContent = t.roles.length && live < t.roles.length ? 'Há menos câmeras conectadas (' + live + ') do que papéis (' + t.roles.length + '). Entradas “TESTE” mostram o padrão de teste; troque quando conectar as câmeras.' : '';
    $('#scp-del').style.display = t.builtin ? 'none' : '';
  }
  $('#scp-back').onclick = openPicker;
  $('#scp-apply').onclick = () => {
    if (!pickT) return;
    const ids = $$('#scp-scenes input[type=checkbox]').filter(c => c.checked).map(c => c.dataset.scene);
    if (!ids.length) { toast('Marque ao menos uma cena.'); return; }
    const map = {}; $$('#scp-roles select').forEach(s => { map[s.dataset.role] = +s.value; });
    send('sceneapply', pickT.id, JSON.stringify(map), ids.join(','));
    closeModals();
    const b = document.querySelector('.bankbtn[data-bank="2"]'); if (b) b.click();   // show the CENAS bank
  };
  $('#scp-export').onclick = () => { if (pickT) send('sceneexport', pickT.id); };
  $('#scp-import').onclick = () => send('sceneimport');
  $('#scp-del').onclick = () => { if (pickT && !pickT.builtin) { send('templatedelete', pickT.id); openPicker(); } };
  $('#scp-x').onclick = closeModals;
  $('#sc-open').onclick = openPicker;

  // ----- scene context menu -----
  let scmSlot = -1, scmArm = 0;
  const sceneMenu = $('#scenemenu');
  function openSceneMenu(e, k) {
    scmSlot = k; $('#scm-del').textContent = 'Excluir cena'; scmArm = 0;
    sceneMenu.classList.add('on');
    sceneMenu.style.left = Math.min(e.clientX, innerWidth - sceneMenu.offsetWidth - 8) + 'px';
    sceneMenu.style.top = Math.min(e.clientY, innerHeight - sceneMenu.offsetHeight - 8) + 'px';
  }
  $('#scm-edit').onclick = () => { sceneMenu.classList.remove('on'); openEditor(scmSlot); };
  $('#scm-del').onclick = () => {
    const sc = sceneBySlot(scmSlot); if (!sc) return;
    if (!scmArm) { scmArm = 1; $('#scm-del').textContent = 'Clique de novo para excluir'; return; }
    send('scenedelete', sc.id); sceneMenu.classList.remove('on');
  };
  document.addEventListener('pointerdown', e => { if (!e.target.closest('#scenemenu')) sceneMenu.classList.remove('on'); });

  // ----- editor: drag/resize layers on a preview canvas + numeric fields -----
  let ed = null, edSel = 0, edDrag = null, edDelArm = 0;
  const edCv = $('#sc-cv');
  const edLayer = () => ed && ed.layers[edSel];
  function openEditor(k) {
    const sc = sceneBySlot(k); if (!sc) return;
    ed = JSON.parse(JSON.stringify(sc)); edSel = 0; edDelArm = 0; $('#sce-del').textContent = 'EXCLUIR CENA';
    $('#sce-name').value = ed.name; $('#sce-tname').value = '';
    $('#sce-slot').innerHTML = '<option value="-1">— nenhuma (painel vazio) —</option>' + Array.from({length: N}, (_, i) => { const x = slots[i]; return `<option value="${i}">CAM ${i + 1} — ${esc(x && x.k === 'dev' ? x.name : 'TESTE')}</option>`; }).join('');
    const bg = ed.background || (ed.background = {type: 'solid', color: '#0b1220'});
    $('#sce-bgt').value = bg.type === 'gradient' ? 'gradient' : 'solid';
    $('#sce-bg1').value = (bg.type === 'gradient' ? bg.from : bg.color) || '#0b1220'; $('#sce-bg2').value = bg.to || '#1c2f55';
    $('#sc-edit').classList.add('on'); edRefresh(true);
  }
  function edRefresh(fields) {
    if (!ed) return;
    const lb = $('#sce-layers'); lb.innerHTML = '';
    ed.layers.forEach((l, i) => { const b = document.createElement('button'); b.textContent = l.role || ('Camada ' + (i + 1)); b.className = i === edSel ? 'on' : ''; b.onclick = () => { edSel = i; edRefresh(true); }; lb.appendChild(b); });
    if (fields) edFields();
    drawScene(edCv, ed, edSel);
  }
  function edFields() {
    const l = edLayer(), dis = !l;
    $$('#sce-fields input,#sce-fields select,#sce-rm').forEach(e => { e.disabled = dis; });
    if (!l) return;
    const r = l.rect || (l.rect = [0, 0, 1, 1]), p = v => Math.round(v * 1000) / 10;
    $('#sce-slot').value = String(l.slot === undefined ? -1 : l.slot);
    $('#sce-x').value = p(r[0]); $('#sce-y').value = p(r[1]); $('#sce-w').value = p(r[2]); $('#sce-h').value = p(r[3]);
    $('#sce-fit').value = l.fit || 'fill'; $('#sce-op').value = Math.round((l.opacity === undefined ? 1 : l.opacity) * 100);
    $('#sce-rad').value = Math.round((l.radius || 0) * 100); $('#sce-bor').value = p(l.border ? l.border.width : 0);
    $('#sce-sh').value = l.shadow ? '1' : '0'; $('#sce-z').value = l.z || 0;
  }
  const num = (id, d) => { const v = parseFloat($(id).value); return isFinite(v) ? v : d; };
  function edRead() {
    const l = edLayer(); if (!l) return;
    l.slot = +$('#sce-slot').value;
    l.rect = [num('#sce-x', 0) / 100, num('#sce-y', 0) / 100, Math.max(.01, num('#sce-w', 100) / 100), Math.max(.01, num('#sce-h', 100) / 100)];
    l.fit = $('#sce-fit').value; l.opacity = Math.min(1, Math.max(0, num('#sce-op', 100) / 100));
    l.radius = Math.min(.5, Math.max(0, num('#sce-rad', 0) / 100));
    const bw = Math.min(.1, Math.max(0, num('#sce-bor', 0) / 100)); l.border = bw ? {width: bw, color: (l.border && l.border.color) || '#ffffff'} : undefined; if (!bw) delete l.border;
    if ($('#sce-sh').value === '1') l.shadow = l.shadow || {blur: .02, dx: .004, dy: .008, opacity: .55}; else delete l.shadow;
    l.z = Math.round(num('#sce-z', 0));
    drawScene(edCv, ed, edSel);
  }
  $$('#sce-fields input,#sce-fields select').forEach(e => e.addEventListener('input', edRead));
  $('#sce-bgt').onchange = $('#sce-bg1').oninput = $('#sce-bg2').oninput = () => {
    if (!ed) return;
    ed.background = $('#sce-bgt').value === 'gradient' ? {type: 'gradient', from: $('#sce-bg1').value, to: $('#sce-bg2').value, angle: (ed.background && ed.background.angle) || 90} : {type: 'solid', color: $('#sce-bg1').value};
    drawScene(edCv, ed, edSel);
  };
  $('#sce-add').onclick = () => { if (!ed || ed.layers.length >= 16) return; ed.layers.push({role: '', slot: -1, rect: [.25, .25, .5, .5], fit: 'fill', z: ed.layers.length}); edSel = ed.layers.length - 1; edRefresh(true); };
  $('#sce-rm').onclick = () => { if (!ed || !edLayer()) return; ed.layers.splice(edSel, 1); edSel = Math.max(0, edSel - 1); edRefresh(true); };
  const edPos = e => { const r = edCv.getBoundingClientRect(); return {x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height}; };
  edCv.addEventListener('pointerdown', e => {
    if (!ed) return;
    const p = edPos(e), order = layersByZ(ed).reverse();
    const hit = order.find(({l}) => { const r = l.rect; return p.x >= r[0] && p.x <= r[0] + r[2] && p.y >= r[1] && p.y <= r[1] + r[3]; });
    if (!hit) return;
    edSel = hit.i; const r = hit.l.rect;
    const corner = Math.abs(p.x - (r[0] + r[2])) < 0.03 && Math.abs(p.y - (r[1] + r[3])) < 0.05;
    edDrag = {mode: corner ? 'resize' : 'move', p0: p, r0: r.slice()};
    edCv.setPointerCapture(e.pointerId); edRefresh(true);
  });
  edCv.addEventListener('pointermove', e => {
    if (!edDrag || !ed) return;
    const p = edPos(e), dx = p.x - edDrag.p0.x, dy = p.y - edDrag.p0.y, r0 = edDrag.r0, l = edLayer();
    if (edDrag.mode === 'move') l.rect = [Math.min(1.5, Math.max(-.5, r0[0] + dx)), Math.min(1.5, Math.max(-.5, r0[1] + dy)), r0[2], r0[3]];
    else l.rect = [r0[0], r0[1], Math.min(3, Math.max(.03, r0[2] + dx)), Math.min(3, Math.max(.03, r0[3] + dy))];
    edFields(); drawScene(edCv, ed, edSel);
  });
  const edEnd = () => { edDrag = null; };
  edCv.addEventListener('pointerup', edEnd); edCv.addEventListener('pointercancel', edEnd);
  $('#sce-save').onclick = () => { if (!ed) return; ed.name = $('#sce-name').value.trim() || ed.name; send('scenesave', JSON.stringify(ed)); closeModals(); };
  $('#sce-tpl').onclick = () => {
    if (!ed) return;
    ed.name = $('#sce-name').value.trim() || ed.name;
    send('scenesave', JSON.stringify(ed));
    send('scenetemplate', ed.id, $('#sce-tname').value.trim() || ed.name);
    closeModals();
  };
  $('#sce-del').onclick = () => {
    if (!ed) return;
    if (!edDelArm) { edDelArm = 1; $('#sce-del').textContent = 'CONFIRMAR EXCLUSÃO'; return; }
    send('scenedelete', ed.id); closeModals();
  };
  $('#sce-cancel').onclick = $('#sce-close').onclick = closeModals;

  function onScenesState(d) {
    scenesSt = {scenes: d.scenes || [], templates: d.templates || [], max: d.max || 16};
    if (pickT) { const t = scenesSt.templates.find(x => x.id === pickT.id); if (!t) openPicker(); }
    renderSceneTiles();
    if ($('#sc-pick').classList.contains('on')) renderPicker();
  }
  // ---------- /FEATURE: scenes ----------

