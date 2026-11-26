  // ---------- GRÁFICOS (overlays) ---------- FEATURE: overlays
  const OV_TYPES = {timer: 'CRONÔMETRO', scoreboard: 'PLACAR', qr: 'QR CODE', lowerthird: 'LOWER THIRD', captions: 'LEGENDAS', chatpin: 'CHAT'};
  const OV_POS = [['Topo à esquerda', 0.03, 0.05, 0, 0], ['Topo ao centro', 0.5, 0.05, 0.5, 0], ['Topo à direita', 0.97, 0.05, 1, 0],
    ['Centro à esquerda', 0.03, 0.5, 0, 0.5], ['Centro', 0.5, 0.5, 0.5, 0.5], ['Centro à direita', 0.97, 0.5, 1, 0.5],
    ['Base à esquerda', 0.03, 0.95, 0, 1], ['Base ao centro', 0.5, 0.95, 0.5, 1], ['Base à direita', 0.97, 0.95, 1, 1]];
  const OV_ANIMS = [['none', 'Nenhuma'], ['fade', 'Surgir (fade)'], ['slide-left', 'Deslizar da esquerda'], ['slide-right', 'Deslizar da direita'], ['slide-up', 'Deslizar de baixo'], ['slide-down', 'Deslizar de cima']];
  // k = config key; kind: text|num|pct (stored 0..1)|color|check|select; t = types that show it (omitted: all)
  const OV_FIELDS = [
    {k: 'name', l: 'Nome na lista', kind: 'text', wide: 1},
    {k: 'label', l: 'Rótulo (texto pequeno acima)', kind: 'text', t: ['timer', 'scoreboard'], wide: 1},
    {k: 'mode', l: 'Contagem', kind: 'select', o: [['down', 'Regressiva'], ['up', 'Progressiva']], t: ['timer', 'scoreboard']},
    {k: 'durationSec', l: 'Duração (segundos)', kind: 'num', min: 0, max: 359999, t: ['timer', 'scoreboard'], presets: [60, 300, 600, 900, 1800, 3600]},
    {k: 'format', l: 'Formato', kind: 'select', o: [['auto', 'Automático'], ['mm:ss', 'mm:ss'], ['hh:mm:ss', 'hh:mm:ss']], t: ['timer', 'scoreboard']},
    {k: 'blinkAtZero', l: 'Piscar ao chegar a zero', kind: 'check', t: ['timer']},
    {k: 'teamA', l: 'Time A', kind: 'text', t: ['scoreboard']}, {k: 'teamB', l: 'Time B', kind: 'text', t: ['scoreboard']},
    {k: 'colorA', l: 'Cor do time A', kind: 'color', t: ['scoreboard']}, {k: 'colorB', l: 'Cor do time B', kind: 'color', t: ['scoreboard']},
    {k: 'scoreA', l: 'Pontos A', kind: 'num', min: 0, max: 999, t: ['scoreboard']}, {k: 'scoreB', l: 'Pontos B', kind: 'num', min: 0, max: 999, t: ['scoreboard']},
    {k: 'period', l: 'Período (ex.: 1º tempo)', kind: 'text', t: ['scoreboard']},
    {k: 'showClock', l: 'Mostrar relógio no placar', kind: 'check', t: ['scoreboard']},
    {k: 'qrText', l: 'Conteúdo do QR (link ou código PIX)', kind: 'text', t: ['qr'], wide: 1},
    {k: 'qrCaption', l: 'Texto sob o QR', kind: 'text', t: ['qr'], wide: 1},
    {k: 'qrEcc', l: 'Correção de erro', kind: 'select', o: [['L', 'L (7%)'], ['M', 'M (15%)'], ['Q', 'Q (25%)'], ['H', 'H (30%)']], t: ['qr']},
    {k: 'qrSize', l: 'Tamanho do QR (% da altura)', kind: 'pct', min: 10, max: 80, t: ['qr']},
    {k: 'title', l: 'Nome / título', kind: 'text', t: ['lowerthird'], wide: 1}, {k: 'subtitle', l: 'Função / subtítulo', kind: 'text', t: ['lowerthird'], wide: 1},
    {k: 'holdSec', l: 'Tempo na tela (s; 0 = fixo)', kind: 'num', min: 0, max: 3600, t: ['captions', 'chatpin']},
    {k: 'maxLines', l: 'Linhas no máximo', kind: 'num', min: 1, max: 6, t: ['captions', 'chatpin']},
    {k: 'widthPct', l: 'Largura máxima (% da tela)', kind: 'pct', min: 10, max: 100, t: ['captions', 'chatpin']},
    {k: '_pos', l: 'Posição', kind: 'select', o: OV_POS.map((p, i) => [String(i), p[0]]).concat([['c', 'Personalizada']])},
    {k: 'size', l: 'Tamanho do texto (% da altura)', kind: 'pct', min: 1.2, max: 30, step: 0.1},
    {k: 'font', l: 'Fonte', kind: 'text'}, {k: 'bold', l: 'Negrito', kind: 'check'},
    {k: 'textColor', l: 'Cor do texto', kind: 'color'}, {k: 'bgColor', l: 'Cor do fundo', kind: 'color', t: ['timer', 'scoreboard', 'lowerthird', 'captions', 'chatpin']},
    {k: 'accent', l: 'Cor de destaque', kind: 'color', t: ['timer', 'scoreboard', 'lowerthird', 'chatpin']},
    {k: 'bgAlpha', l: 'Opacidade do fundo (%)', kind: 'pct', min: 0, max: 100, t: ['timer', 'scoreboard', 'lowerthird', 'captions', 'chatpin']},
    {k: 'opacity', l: 'Opacidade geral (%)', kind: 'pct', min: 0, max: 100},
    {k: 'animIn', l: 'Animação de entrada', kind: 'select', o: OV_ANIMS}, {k: 'animOut', l: 'Animação de saída', kind: 'select', o: OV_ANIMS},
    {k: 'animMs', l: 'Duração da animação (ms)', kind: 'num', min: 0, max: 5000},
  ];
  let ovData = {elements: [], preview: false}, ovSig = '', ovEdit = null;
  const ovHex = n => '#' + (n >>> 0).toString(16).padStart(6, '0').slice(-6);
  const ovInt = h => parseInt(String(h).replace('#', ''), 16) || 0;
  const ovGfxCtx = $('#cv-gfx').getContext('2d');
  const ovLive = e => (e.type === 'timer' || (e.type === 'scoreboard' && e.showClock)) ? e.clock : '';
  function ovPosIndex(e) { const i = OV_POS.findIndex(p => Math.abs(p[1] - e.x) < 0.001 && Math.abs(p[2] - e.y) < 0.001 && p[3] === e.pivotX && p[4] === e.pivotY); return i < 0 ? 'c' : String(i); }
  function ovState(d) {
    ovData = d;
    const sig = JSON.stringify(d.elements.map(e => Object.assign({}, e, {clock: 0}))) + d.preview;
    if (sig !== ovSig) { ovSig = sig; ovRenderList(); }
    else d.elements.forEach(e => { const el = document.querySelector(`.ovrow[data-id="${e.id}"] .nm .live`); if (el) el.textContent = ovLive(e); });
    const pv = $('#ov-prev'); if (pv && document.activeElement !== pv) pv.checked = !!d.preview;
  }
  function ovRenderList() {
    const box = $('#ov-list'); box.innerHTML = '';
    $('#ov-count').textContent = ovData.elements.length;
    $('#ov-empty').style.display = ovData.elements.length ? 'none' : '';
    ovData.elements.forEach((e, k) => {
      const r = document.createElement('div'); r.className = 'ovrow' + (e.on ? ' on' : ''); r.dataset.id = e.id;
      const warn = e.type === 'qr' && !e.qrOk ? ' ⚠' : '';
      r.innerHTML = `<div class="nm">${esc(e.name || e.id)}<em>${OV_TYPES[e.type] || e.type}${warn}</em><span class="live">${esc(ovLive(e))}</span></div>
        <div class="acts"><button data-a="up" title="Subir (para frente)"${k === ovData.elements.length - 1 ? ' disabled' : ''}>↑</button><button data-a="dn" title="Descer (para trás)"${k === 0 ? ' disabled' : ''}>↓</button>
        <button data-a="edit">EDITAR</button><button data-a="del" title="Remover">✕</button><button data-a="air" class="air${e.on ? ' on' : ''}">${e.on ? 'NO AR' : 'FORA'}</button></div>`;
      const ctl = document.createElement('div'); ctl.className = 'ctl';
      const mk = (txt, cmd, title) => { const b = document.createElement('button'); b.textContent = txt; b.title = title || ''; b.onclick = () => send(...cmd); ctl.appendChild(b); };
      if (e.type === 'timer' || (e.type === 'scoreboard' && e.showClock)) {
        mk('▶ INICIAR', ['ovtimer', e.id, 'start']); mk('⏸ PAUSAR', ['ovtimer', e.id, 'pause']); mk('↺ ZERAR', ['ovtimer', e.id, 'reset']);
        mk('+1 min', ['ovtimer', e.id, 'add', 60]); mk('−1 min', ['ovtimer', e.id, 'add', -60]);
        [60, 300, 600, 900].forEach(s => mk(s / 60 + ' min', ['ovtimer', e.id, 'set', s], 'Definir ' + s / 60 + ' minutos'));
      }
      if (e.type === 'scoreboard') {
        mk('A +1', ['ovscore', e.id, 'a', 1]); mk('A −1', ['ovscore', e.id, 'a', -1]); mk('B +1', ['ovscore', e.id, 'b', 1]); mk('B −1', ['ovscore', e.id, 'b', -1]);
        mk('⇄ TROCAR', ['ovscore', e.id, 'swap']); mk('ZERAR PLACAR', ['ovscore', e.id, 'reset']);
        const sc = document.createElement('span'); sc.className = 'live'; sc.textContent = `${e.teamA} ${e.scoreA} × ${e.scoreB} ${e.teamB}`; ctl.appendChild(sc);
      }
      if (ctl.childNodes.length) r.appendChild(ctl);
      r.querySelector('[data-a=air]').onclick = () => send('ovshow', e.id, e.on ? 0 : 1);
      r.querySelector('[data-a=edit]').onclick = () => ovOpen(e);
      r.querySelector('[data-a=del]').onclick = () => { if (confirm('Remover "' + (e.name || e.id) + '"?')) send('ovdel', e.id); };
      r.querySelector('[data-a=up]').onclick = () => send('ovmove', e.id, 'up');
      r.querySelector('[data-a=dn]').onclick = () => send('ovmove', e.id, 'down');
      box.appendChild(r);
    });
  }
  function ovOpen(e) {
    ovEdit = e;
    $('#ovm-type').textContent = OV_TYPES[e.type] || e.type;
    $('#ovm-title').textContent = e.name || 'Gráfico';
    const box = $('#ovm-fields'); box.innerHTML = '';
    OV_FIELDS.filter(f => !f.t || f.t.includes(e.type)).forEach(f => {
      const lab = document.createElement('label'); if (f.wide) lab.style.gridColumn = '1/-1';
      const v = f.k === '_pos' ? ovPosIndex(e) : e[f.k];
      if (f.kind === 'check') {
        lab.className = 'chk'; lab.style.gridColumn = '1/-1'; lab.innerHTML = `<input type="checkbox" data-k="${f.k}"${v ? ' checked' : ''}><span class="box"></span>${esc(f.l)}`;
      } else if (f.kind === 'select') {
        lab.innerHTML = `${esc(f.l)}<select data-k="${f.k}">${f.o.map(o => `<option value="${esc(o[0])}"${String(v) === String(o[0]) ? ' selected' : ''}>${esc(o[1])}</option>`).join('')}</select>`;
      } else if (f.kind === 'color') {
        lab.innerHTML = `${esc(f.l)}<input type="color" data-k="${f.k}" value="${ovHex(v)}">`;
      } else if (f.kind === 'num') {
        lab.innerHTML = `${esc(f.l)}<input type="number" data-k="${f.k}" min="${f.min}" max="${f.max}" value="${v}">`;
        if (f.presets) { const p = document.createElement('div'); p.className = 'ovpre'; f.presets.forEach(s => { const b = document.createElement('button'); b.type = 'button'; b.textContent = s >= 3600 ? s / 3600 + ' h' : s / 60 + ' min'; b.onclick = ev => { ev.preventDefault(); lab.querySelector('input').value = s; }; p.appendChild(b); }); lab.appendChild(p); }
      } else if (f.kind === 'pct') {
        lab.innerHTML = `${esc(f.l)}<input type="number" data-k="${f.k}" min="${f.min}" max="${f.max}" step="${f.step || 1}" value="${Math.round(v * 1000) / 10}">`;
      } else {
        lab.innerHTML = `${esc(f.l)}<input type="text" data-k="${f.k}" maxlength="600" spellcheck="false" value="${esc(v == null ? '' : v)}">`;
      }
      box.appendChild(lab);
    });
    $('#modal6').classList.add('on');
  }
  function ovSave() {
    if (!ovEdit) return;
    const o = Object.assign({}, ovEdit); delete o.clock; delete o.on; delete o.running; delete o.finished; delete o.qrOk;
    OV_FIELDS.filter(f => !f.t || f.t.includes(ovEdit.type)).forEach(f => {
      const el = document.querySelector(`#ovm-fields [data-k="${f.k}"]`); if (!el) return;
      if (f.k === '_pos') { if (el.value !== 'c') { const p = OV_POS[+el.value]; o.x = p[1]; o.y = p[2]; o.pivotX = p[3]; o.pivotY = p[4]; } return; }
      if (f.kind === 'check') o[f.k] = el.checked;
      else if (f.kind === 'color') o[f.k] = ovInt(el.value);
      else if (f.kind === 'num') o[f.k] = Math.round(+el.value || 0);
      else if (f.kind === 'pct') o[f.k] = (+el.value || 0) / 100;
      else o[f.k] = el.value;
    });
    send('ovsave', JSON.stringify(o));
    $('#modal6').classList.remove('on'); ovEdit = null;
  }
  $('#ov-add').onclick = () => send('ovnew', $('#ov-type').value);
  $('#ov-prev').onchange = ev => send('ovpreview', ev.target.checked ? 1 : 0);
  $('#ovm-save').onclick = ovSave;
  $('#ovm-cancel').onclick = $('#ovm-x').onclick = () => $('#modal6').classList.remove('on');
  // ---------- /GRÁFICOS ----------

