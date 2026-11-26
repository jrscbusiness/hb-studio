  // ---------- FEATURE: marketplace ----------
  const mkCats = {theme: 'TEMA', 'video-source': 'FONTE DE VÍDEO', 'audio-effect': 'EFEITO DE ÁUDIO', graphic: 'GRÁFICO', template: 'TEMPLATE'};
  let mkt = null, mkSel = null, mkProg = null, mkAsked = false, mkErr = {}, mkCtx = {};
  const mkEl = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  const mkSize = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
  function mkShow(on) {
    $('#mk').classList.toggle('on', on); $('#mk-top-note').style.display = on ? '' : 'none';
    const w = document.querySelector('#v-plg .plgwrap'); if (w) w.style.display = on ? 'none' : '';
    $$('#plg-seg button').forEach(b => b.classList.toggle('on', (b.dataset.s === 'mkt') === on));
    if (on && !mkAsked) { mkAsked = true; send('mktlist'); send('mktrefresh'); }
  }
  $$('#plg-seg button').forEach(b => b.onclick = () => mkShow(b.dataset.s === 'mkt'));
  const mkActLabel = e => e.status === 'update' ? 'ATUALIZAR' : e.status === 'installed' ? 'INSTALADO' : 'INSTALAR';
  function mkInstall(e) { mkErr[e.id] = ''; mkProg = {id: e.id, phase: 'download', pct: 0}; renderMkt(); send('mktinstall', e.id); }
  function mkActBtn(e, cls) {
    const b = mkEl('button', 'mk-act ' + (cls || ''), mkProg && mkProg.id === e.id ? 'BAIXANDO…' : mkActLabel(e));
    b.disabled = !e.compatible || e.status === 'installed' || !!mkProg || !!(mkt && mkt.installing);
    if (!e.compatible) b.title = e.reason || ''; else if (e.status !== 'installed') b.onclick = ev => { ev.stopPropagation(); mkInstall(e); };
    return b;
  }
  function renderMkt() {
    const grid = $('#mk-grid'), msg = $('#mk-msg');
    if (!mkt) { grid.innerHTML = ''; grid.appendChild(mkEl('div', 'mk-empty', 'Carregando o catálogo…')); return; }
    msg.textContent = mkt.error || (mkt.dropped ? mkt.dropped + ' entrada(s) inválida(s) do catálogo foram ignoradas.' : '');
    msg.classList.toggle('on', !!msg.textContent);
    if (document.activeElement !== $('#mk-url')) $('#mk-url').value = mkt.catalogUrl === mkt.defaultUrl ? '' : mkt.catalogUrl;
    $('#mk-url').placeholder = mkt.defaultUrl || '';
    const q = $('#mk-q').value.trim().toLowerCase(), cat = $('#mk-cat').value;
    const list = (mkt.list || []).filter(e => (!cat || e.cat === cat) && (!q || (e.name + ' ' + e.author + ' ' + e.tags.join(' ') + ' ' + e.desc).toLowerCase().includes(q)));
    grid.innerHTML = '';
    if (!list.length) {
      const empty = mkEl('div', 'mk-empty');
      if (!mkt.loaded) empty.append(mkEl('b', '', 'Catálogo indisponível.'), document.createElement('br'), 'Verifique a internet e clique em ATUALIZAR LISTA. Os plugins já instalados continuam funcionando; você também pode instalar um arquivo .hbplugin na aba INSTALADOS.');
      else if (!(mkt.list || []).length) empty.append(mkEl('b', '', 'O catálogo está vazio.'), document.createElement('br'), 'Ainda não há extensões publicadas. Veja docs/MARKETPLACE.md para publicar a sua.');
      else empty.append(mkEl('b', '', 'Nada encontrado.'), document.createElement('br'), 'Tente outra busca ou categoria.');
      grid.appendChild(empty);
    }
    list.forEach(e => {
      const c = mkEl('div', 'mk-card' + (mkSel === e.id ? ' sel' : '')); c.tabIndex = 0; c.dataset.id = e.id; c.setAttribute('role', 'button');
      c.append(mkEl('b', '', e.name));
      c.append(mkEl('div', 'meta', e.author + ' · v' + (e.version || '—') + (e.size ? ' · ' + mkSize(e.size) : '')));
      c.append(mkEl('p', '', e.desc));
      const f = mkEl('div', 'foot');
      f.append(mkEl('span', 'mk-b', mkCats[e.cat] || e.cat));
      f.append(e.verified ? mkEl('span', 'mk-b ok', 'VERIFICADO') : mkEl('span', 'mk-b warn', 'NÃO VERIFICADO'));
      if (e.native) f.append(mkEl('span', 'mk-b warn', 'CÓDIGO NATIVO'));
      if (e.status === 'installed') f.append(mkEl('span', 'mk-b', 'INSTALADO'));
      if (e.status === 'update') f.append(mkEl('span', 'mk-b warn', 'ATUALIZAÇÃO'));
      f.append(mkActBtn(e, e.status === 'installed' ? 'ghost' : ''));
      c.append(f);
      c.onclick = () => { mkSel = e.id; renderMkt(); };
      c.onkeydown = ev => { if (ev.key === 'Enter') { mkSel = e.id; renderMkt(); } };
      grid.appendChild(c);
    });
    renderDrawer();
  }
  function renderDrawer() {
    const e = mkt && (mkt.list || []).find(x => x.id === mkSel);
    $('#mk').classList.toggle('drawer', !!e);
    if (!e) return;
    const b = $('#mk-dbody'); b.innerHTML = '';
    b.append(mkEl('b', '', e.name), mkEl('div', 'note', 'por ' + e.author), mkEl('p', 'note', e.desc));
    if (!e.verified) b.append(mkEl('div', 'riskbox mk-unver', 'Publicador não verificado: os mantenedores do catálogo não revisaram o código deste plugin.'));
    if (e.native) b.append(mkEl('div', 'riskbox mk-native', 'Executa código nativo no seu computador, com os mesmos poderes do HB Studio. Não existe isolamento (sandbox). Você ainda verá a tela de revisão e precisará consentir.'));
    if (!e.compatible) b.append(mkEl('div', 'mk-msg on', 'Indisponível: ' + (e.reason || 'incompatível') + '.'));
    if (mkErr[e.id]) b.append(mkEl('div', 'mk-msg on mk-err', mkErr[e.id]));
    const rows = mkEl('div', 'rows');
    [['Categoria', mkCats[e.cat] || e.cat], ['Versão', e.version || '—'], ['Tamanho', e.size ? mkSize(e.size) : '—'], ['Instalada', e.installedVersion || 'não'],
     ['Licença', e.license || '—'], ['Requer HB Studio', e.minApp || '—'], ['Permissões', e.permissions.length ? e.permissions.join(', ') : 'nenhuma'],
     ['Site', e.homepage || '—'], ['Código-fonte', e.repository || '—']].forEach(([k, v]) => rows.append(mkEl('span', '', k), mkEl('span', '', v)));
    b.append(rows);
    if (e.notes) b.append(mkEl('p', 'note', e.notes));
    if (e.screenshots.length) {
      const sh = mkEl('div', 'mk-shots');
      e.screenshots.filter(u => /^https:\/\//i.test(u)).forEach(u => { const i = document.createElement('img'); i.loading = 'lazy'; i.referrerPolicy = 'no-referrer'; i.alt = 'Captura de tela'; i.src = u; sh.append(i); });
      b.append(sh);
    }
    const pr = mkEl('div', 'mk-bar-prog'); const bar = document.createElement('i'); pr.append(bar); pr.id = 'mk-prog';
    if (mkProg && mkProg.id === e.id) { pr.classList.add('on'); bar.style.width = mkProg.pct + '%'; }
    b.append(pr);
    const row = mkEl('div', 'mk-bar');
    const act = mkActBtn(e); act.style.marginLeft = '0'; act.style.height = '34px'; row.append(act);
    if (mkProg && mkProg.id === e.id) { const cn = mkEl('button', 'btn sm', 'CANCELAR'); cn.onclick = () => send('mktcancel'); row.append(cn); }
    if (e.installedVersion) { const rm = mkEl('button', 'btn sm', 'REMOVER'); rm.onclick = () => { if (confirm('Remover o plugin “' + e.name + '”?')) send('pluginremove', e.id); }; row.append(rm); }
    b.append(row);
  }
  $('#mk-dx').onclick = () => { mkSel = null; renderMkt(); };
  $('#mk-q').oninput = $('#mk-cat').onchange = () => renderMkt();
  $('#mk-refresh').onclick = () => { send('mktrefresh'); toast('Atualizando o catálogo…'); };
  $('#mk-url-save').onclick = () => send('mktcfg', $('#mk-url').value.trim());
  $('#mk-url-reset').onclick = () => { $('#mk-url').value = ''; send('mktcfg', ''); };
  function mktMsg(d) {
    if (d.t === 'mkt_catalog') { mkt = d; renderMkt(); }
    else if (d.t === 'mkt_progress') {
      mkProg = {id: d.id, phase: d.phase, pct: d.pct};
      const bar = document.querySelector('#mk-prog i'); if (bar) { bar.style.width = d.pct + '%'; bar.parentNode.classList.add('on'); }
    } else if (d.t === 'mkt_done') {
      mkProg = null;
      if (!d.ok) { mkErr[d.id] = d.message || 'Falha na instalação.'; toast(d.message || 'Falha na instalação.'); }
      else if (d.message) toast(d.message);
      renderMkt();
    } else if (d.t === 'mkt_review') mkCtx[d.id] = d;
  }
  function mktAfterReview(d) {
    const ctx = d.ok && mkCtx[d.id];
    $('#rv-unver').style.display = ctx && !ctx.verified ? '' : 'none';
    if (d.ok) delete mkCtx[d.id];
  }
  // ---------- /FEATURE: marketplace ----------
