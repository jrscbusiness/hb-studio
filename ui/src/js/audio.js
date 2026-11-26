  // ---------- ÁUDIO ----------
  let aud = null, audSel = 0, audPending = null, recp = null, recst = {state: 'idle'};
  const audEls = [];   // per strip: {meter, hold, db, st}
  const dbTxt = v => v <= -99 ? '−∞' : (v > 0 ? '+' : '') + (+v).toFixed(1).replace('.', ',');
  const esc = s => String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
  const audBusy = () => audDown || (document.activeElement && document.activeElement.matches && document.activeElement.matches('#v-aud input[type=text], #v-aud input.nm'));
  let audDown = false;
  document.addEventListener('pointerdown', e => { if (e.target.closest && e.target.closest('#v-aud')) audDown = true; });
  const audFlush = () => { audDown = false; if (audPending && !audBusy()) { const p = audPending; audPending = null; renderAudio(p); } };
  document.addEventListener('pointerup', () => setTimeout(audFlush, 0));
  document.addEventListener('focusout', () => setTimeout(audFlush, 0));
  const A = (...a) => send('aud', ...a);
  const meterPct = db => Math.max(0, Math.min(100, (db + 60) / 60 * 100));
  function inputOptions(st) {
    const dv = aud.devices || [];
    const cur = st.input.kind === 'device' ? st.input.id : '';
    let h = `<option value="none|"${st.input.kind === 'none' ? ' selected' : ''}>— sem entrada —</option>`;
    const grp = (label, kind) => { const l = dv.filter(d => d.kind === kind); if (!l.length) return ''; return `<optgroup label="${label}">` + l.map(d => `<option value="device|${esc(d.id)}"${cur === d.id ? ' selected' : ''}>${esc(d.name)}${d.isDefault ? ' (padrão)' : ''}</option>`).join('') + '</optgroup>'; };
    h += grp('Microfones e entradas', 'capture') + grp('Áudio do computador (loopback)', 'loopback');
    if (st.input.kind === 'device' && !dv.some(d => d.id === st.input.id)) h += `<option value="device|${esc(st.input.id)}" selected>${esc(st.input.name || st.input.id)} (ausente)</option>`;
    return h;
  }
  function renderAudio(d) {
    aud = d;
    const box = $('#strips'); box.innerHTML = ''; audEls.length = 0;
    d.strips.forEach((st, i) => {
      const e = document.createElement('div'); e.className = 'strip' + (i === audSel ? ' sel' : ''); e.dataset.i = i;
      const state = st.input.kind === 'none' ? 'off' : st.input.state;
      e.innerHTML = `<input class="nm" value="${esc(st.name)}" maxlength="24" aria-label="Nome do canal ${i + 1}">
        <select data-k="input" aria-label="Entrada do canal ${i + 1}">${inputOptions(st)}</select>
        <div class="st ${state}" title="${esc(st.input.msg || '')}">${{online: 'AO VIVO', connecting: 'CONECTANDO', nosignal: 'SEM SINAL', error: 'ERRO', off: 'SEM ENTRADA'}[state] || state}</div>
        <div class="faderbox"><div class="meter"><i></i><b></b></div><input type="range" class="fader" min="-60" max="10" step="0.5" value="${st.faderDb <= -100 ? -60 : st.faderDb}" aria-label="Volume do canal ${i + 1}"><div class="meter"><i></i><b></b></div></div>
        <div class="sdb">${dbTxt(st.faderDb)} dB</div>
        <div class="tg"><button class="m${st.mute ? ' on' : ''}" data-k="mute" title="Mudo">M</button><button class="s${st.solo ? ' on' : ''}" data-k="solo" title="Solo">S</button></div>
        <div class="lbl"><span>PAN</span><span>${st.pan === 0 ? 'C' : (st.pan < 0 ? 'E ' + Math.round(-st.pan * 100) : 'D ' + Math.round(st.pan * 100))}</span></div>
        <input type="range" class="h" data-k="pan" min="-1" max="1" step="0.05" value="${st.pan}" aria-label="Balanço do canal ${i + 1}">
        <div class="tg"><button data-k="toProgram" class="${st.toProgram ? 'on' : ''}" title="Enviar ao Program">PGM</button><button data-k="toMonitor" class="${st.toMonitor ? 'on' : ''}" title="Ouvir nos fones (pré-fader)">FONE</button></div>`;
      const ms = e.querySelectorAll('.meter');
      audEls.push({l: ms[0], r: ms[1], db: e.querySelector('.sdb'), st: e.querySelector('.st')});
      e.addEventListener('click', ev => { if (ev.target.closest('input,select,button')) return; selStrip(i); });
      e.querySelector('input.nm').addEventListener('change', ev => A('strip', i, 'name', ev.target.value));
      e.querySelector('select[data-k=input]').addEventListener('change', ev => { const [k, id] = ev.target.value.split('|'); A('input', i, k, id || ''); });
      const fd = e.querySelector('.fader'); fd.addEventListener('input', () => { const v = +fd.value; e.querySelector('.sdb').textContent = dbTxt(v <= -60 ? -120 : v) + ' dB'; A('strip', i, 'faderDb', v <= -60 ? -120 : v); });
      fd.addEventListener('dblclick', () => { fd.value = 0; A('strip', i, 'faderDb', 0); });
      e.querySelectorAll('.tg button').forEach(b => b.addEventListener('click', () => A('strip', i, b.dataset.k, b.classList.contains('on') ? 0 : 1)));
      e.querySelector('input[data-k=pan]').addEventListener('input', ev => A('strip', i, 'pan', ev.target.value));
      e.querySelector('input[data-k=pan]').addEventListener('dblclick', ev => { ev.target.value = 0; A('strip', i, 'pan', 0); });
      box.appendChild(e);
    });
    // master strip
    const m = d.master, mo = d.monitor;
    const ms = document.createElement('div'); ms.className = 'strip master';
    ms.innerHTML = `<div class="cap" style="text-align:center;color:var(--ink)">PROGRAM</div>
      <div class="st online">SAÍDA</div>
      <div class="faderbox"><div class="meter"><i></i><b></b></div><input type="range" class="fader" id="m-fader" min="-60" max="10" step="0.5" value="${m.faderDb <= -100 ? -60 : m.faderDb}" aria-label="Volume geral"><div class="meter"><i></i><b></b></div></div>
      <div class="sdb" id="m-db">${dbTxt(m.faderDb)} dB</div>
      <div class="sdb" id="m-lufs" title="Volume percebido (LUFS curto prazo). Transmissão costuma usar −14 a −16.">— LUFS</div>
      <div class="tg"><button id="m-lim" class="${m.limiter ? 'on' : ''}" title="Limitador: impede estouro (clipping)">LIMIT</button><button id="m-rst" title="Zerar medidores">ZERAR</button></div>
      <div class="lbl"><span>TETO</span><span id="m-ceilv">${m.ceilingDb} dB</span></div>
      <input type="range" class="h" id="m-ceil" min="-12" max="0" step="0.5" value="${m.ceilingDb}" aria-label="Teto do limitador">
      <div class="lbl"><span>FONES</span><span>${mo.state === 'off' ? 'OFF' : mo.state.toUpperCase()}</span></div>
      <select id="m-mon" aria-label="Saída dos fones"><option value="">— desligado —</option>${(d.devices || []).filter(x => x.kind === 'playback').map(x => `<option value="${esc(x.id)}"${mo.deviceId === x.id ? ' selected' : ''}>${esc(x.name)}</option>`).join('')}</select>
      <input type="range" class="h" id="m-monlv" min="0" max="1" step="0.01" value="${mo.level}" aria-label="Volume dos fones">`;
    const mm = ms.querySelectorAll('.meter'); audEls.master = {l: mm[0], r: mm[1], lufs: ms.querySelector('#m-lufs')};
    ms.querySelector('#m-fader').addEventListener('input', ev => { const v = +ev.target.value; ms.querySelector('#m-db').textContent = dbTxt(v <= -60 ? -120 : v) + ' dB'; A('master', 'faderDb', v <= -60 ? -120 : v); });
    ms.querySelector('#m-lim').addEventListener('click', ev => A('master', 'limiter', ev.target.classList.contains('on') ? 0 : 1));
    ms.querySelector('#m-rst').addEventListener('click', () => A('reset'));
    ms.querySelector('#m-ceil').addEventListener('input', ev => { ms.querySelector('#m-ceilv').textContent = ev.target.value + ' dB'; A('master', 'ceilingDb', ev.target.value); });
    ms.querySelector('#m-mon').addEventListener('change', ev => A('monitor', 'deviceId', ev.target.value));
    ms.querySelector('#m-monlv').addEventListener('input', ev => A('monitor', 'level', ev.target.value));
    box.appendChild(ms);
    renderFx();
  }
  function selStrip(i) { audSel = i; document.querySelectorAll('.strip[data-i]').forEach(s => s.classList.toggle('sel', +s.dataset.i === i)); renderFx(); }
  function fmtVal(p, v) { if (p.choices && p.choices.length) return p.choices[Math.round(v)] || ''; const d = Math.abs(v) >= 100 ? 0 : Math.abs(v) >= 10 ? 1 : 2; return (+v).toFixed(d).replace('.', ',') + (p.unit ? ' ' + p.unit : ''); }
  function renderFx() {
    if (!aud) return; const st = aud.strips[audSel]; if (!st) return;
    $('#fx-title').textContent = 'Canal ' + (audSel + 1) + ' · ' + st.name;
    $('#strip-set').innerHTML = `<label>Entrada − trim (dB)<input type="number" data-s="trimDb" min="-60" max="24" step="0.5" value="${st.trimDb}"></label>
      <label>Atraso (ms) — sincronia com vídeo<input type="number" data-s="delayMs" min="0" max="2000" step="1" value="${st.delayMs}"></label>
      <label>Qualidade do áudio<select data-s="quality"><option value="linear"${st.quality === 'linear' ? ' selected' : ''}>Rápida (linear)</option><option value="medium"${st.quality === 'medium' ? ' selected' : ''}>Equilibrada</option><option value="high"${st.quality === 'high' ? ' selected' : ''}>Alta (menor distorção)</option></select></label>
      <label>Fase / canais<div class="tg"><button data-s="phase" class="${st.phase ? 'on' : ''}" title="Inverte a polaridade (corrige microfones em oposição de fase)">INVERTER Ø</button><button data-s="mono" class="${st.mono ? 'on' : ''}" title="Soma esquerda+direita">MONO</button></div></label>`;
    $('#strip-set').querySelectorAll('input[data-s]').forEach(x => x.addEventListener('change', () => A('strip', audSel, x.dataset.s, x.value)));
    $('#strip-set').querySelectorAll('select[data-s]').forEach(x => x.addEventListener('change', () => A('quality', audSel, x.value)));
    $('#strip-set').querySelectorAll('button[data-s]').forEach(x => x.addEventListener('click', () => A('strip', audSel, x.dataset.s, x.classList.contains('on') ? 0 : 1)));
    const list = $('#fx-list'); list.innerHTML = '';
    if (!st.fx.length) list.innerHTML = '<p class="note" style="margin-bottom:10px">Nenhum efeito neste canal. Adicione abaixo (ex.: <b>Filtro passa-alta</b> para tirar ruído grave, <b>Compressor</b> para nivelar a voz, <b>Limitador</b>).</p>';
    st.fx.forEach((fx, k) => {
      const c = document.createElement('div'); c.className = 'fxcard' + (fx.bypass ? ' byp' : '');
      const isVst3 = typeof fx.type === 'string' && fx.type.startsWith('vst3:');
      c.innerHTML = `<div class="hd"><b>${k + 1}. ${esc(fx.name)}${fx.missing ? ' <span class="warn">AUSENTE</span>' : ''}</b><span class="gr" data-gr="${k}"></span>
        ${isVst3 ? '<button data-a="editor" title="Abrir a interface própria do plugin">EDITOR</button>' : ''}
        <button data-a="up" title="Subir"${k === 0 ? ' disabled' : ''}>↑</button><button data-a="dn" title="Descer"${k === st.fx.length - 1 ? ' disabled' : ''}>↓</button>
        <button data-a="byp" class="${fx.bypass ? '' : 'on'}" title="${fx.bypass ? 'Desligado (clique para ligar)' : 'Ligado (clique para desligar)'}">⏻</button><button data-a="del" title="Remover">✕</button></div>`;
      if (isVst3) c.querySelector('[data-a=editor]').onclick = () => send('fxeditor', audSel, k, 'open');
      fx.params.forEach(p => {
        const r = document.createElement('div'); r.className = 'prm';
        if (p.choices && p.choices.length) {
          r.innerHTML = `<span>${esc(p.label)}</span><select>${p.choices.map((ch, n) => `<option value="${n}"${Math.round(p.value) === n ? ' selected' : ''}>${esc(ch)}</option>`).join('')}</select>`;
          r.querySelector('select').addEventListener('change', ev => A('fxparam', audSel, k, p.id, ev.target.value));
        } else {
          const lg = p.log && p.min > 0; const toPos = v => lg ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
          const fromPos = x => lg ? p.min * Math.pow(p.max / p.min, x) : p.min + x * (p.max - p.min);
          r.innerHTML = `<span>${esc(p.label)}</span><input type="range" min="0" max="1000" value="${Math.round(toPos(p.value) * 1000)}" aria-label="${esc(p.label)}"><span class="vv">${fmtVal(p, p.value)}</span>`;
          const inp = r.querySelector('input'), vv = r.querySelector('.vv');
          inp.addEventListener('input', () => { const v = fromPos(inp.value / 1000); vv.textContent = fmtVal(p, v); A('fxparam', audSel, k, p.id, v); });
          inp.addEventListener('dblclick', () => { inp.value = Math.round(toPos(p.def) * 1000); vv.textContent = fmtVal(p, p.def); A('fxparam', audSel, k, p.id, p.def); });
        }
        c.appendChild(r);
      });
      if (isVst3 && !fx.params.length) {
        const r = document.createElement('p'); r.className = 'note'; r.style.marginTop = '4px';
        r.textContent = 'Os controles ficam na própria interface do plugin — clique em EDITOR acima.';
        c.appendChild(r);
      }
      c.querySelector('[data-a=up]').onclick = () => A('fxmove', audSel, k, k - 1);
      c.querySelector('[data-a=dn]').onclick = () => A('fxmove', audSel, k, k + 1);
      c.querySelector('[data-a=byp]').onclick = () => A('fxbyp', audSel, k, fx.bypass ? 0 : 1);
      c.querySelector('[data-a=del]').onclick = () => A('fxdel', audSel, k);
      list.appendChild(c);
    });
    const sel = $('#fx-add'); const groups = {};
    (aud.effects || []).forEach(e => (groups[e.category] = groups[e.category] || []).push(e));
    sel.innerHTML = Object.keys(groups).map(g => `<optgroup label="${esc(g)}">${groups[g].map(e => `<option value="${esc(e.typeId)}">${esc(e.name)}</option>`).join('')}</optgroup>`).join('');
    $('#fx-addbtn').disabled = st.fx.length >= 8 || !sel.value;
  }
  $('#fx-addbtn').onclick = () => { const v = $('#fx-add').value; if (v) A('fxadd', audSel, v); };
  function audioLevels(d) {
    if (!aud) return;
    d.strips.forEach((s, i) => {
      const e = audEls[i]; if (!e) return;
      const p = meterPct(s.pk); e.l.firstChild.style.height = p + '%'; e.r.firstChild.style.height = meterPct(s.rms) + '%';
      e.l.lastChild.style.bottom = meterPct(s.hold) + '%'; e.l.classList.toggle('clip', s.pk > -0.5);
      (document.querySelectorAll('[data-gr]') || []).forEach(g => { if (+g.dataset.gr >= 0 && i === audSel) g.textContent = ''; });
      if (i === audSel && s.gr && s.gr.length) s.gr.forEach((g, k) => { const el = document.querySelector('[data-gr="' + k + '"]'); if (el) el.textContent = g > 0.1 ? 'GR −' + g.toFixed(1).replace('.', ',') + ' dB' : ''; });
    });
    const m = d.master, e = audEls.master; if (!e) return;
    e.l.firstChild.style.height = meterPct(m.pk) + '%'; e.r.firstChild.style.height = meterPct(m.rms) + '%'; e.l.lastChild.style.bottom = meterPct(m.hold) + '%';
    e.l.classList.toggle('clip', m.pk > -0.5); e.lufs.textContent = m.ls <= -100 ? '— LUFS' : m.ls.toFixed(1).replace('.', ',') + ' LUFS';
  }

