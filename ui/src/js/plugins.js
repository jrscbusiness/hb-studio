  // ---------- plugins ----------
  let plugins = null, reviewToken = null;
  const typeInfo = {theme: ['TEMA', '<path d="M12 3a9 9 0 100 18c1.7 0 2-1.2 1.4-2.2-.7-1.2.1-2.3 1.4-2.3H17a4 4 0 004-4c0-5-4-9.5-9-9.5z"/>'],
                    source: ['FONTE DE VÍDEO', '<rect x="3" y="7" width="12" height="10" rx="2.5"/><path d="M15 11l6-3v8l-6-3"/>'],
                    effect: ['EFEITO DE ÁUDIO', '<path d="M4 14V10M8 17V7M12 20V4M16 16V8M20 13v-2"/>']};
  const svg = p => '<svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">' + p + '</svg>';
  function renderPlugins() {
    const box = $('#plg-list'); box.innerHTML = '';
    if (!plugins) { box.innerHTML = '<div class="devempty">Carregando…</div>'; return; }
    $('#plg-dir').textContent = plugins.dir || '—';
    if (!plugins.list.length) { box.innerHTML = '<div class="devempty"><b>Nenhum plugin instalado.</b><br>Clique em “Instalar plugin…” e escolha um arquivo <b>.hbplugin</b>. Plugins de exemplo estão disponíveis na página de downloads do HB Studio.</div>'; return; }
    plugins.list.forEach(p => {
      const ti = typeInfo[p.type] || typeInfo.theme;
      const row = document.createElement('div'); row.className = 'plg';
      row.innerHTML = '<div class="ico"><svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">' + ti[1] + '</svg></div><div class="info"><b></b><div class="meta"></div><p></p></div><div class="acts"></div>';
      const b = row.querySelector('b'); b.textContent = p.name;
      const tg = document.createElement('span'); tg.className = 'tg' + (p.type !== 'theme' ? ' native' : ''); tg.textContent = ti[0]; b.appendChild(tg);
      row.querySelector('.meta').textContent = 'v' + p.version + (p.author ? ' · ' + p.author : '') + (p.license ? ' · ' + p.license : '');
      row.querySelector('p').textContent = p.description || '';
      const err = p.error || (p.removed ? 'Removido: os arquivos são apagados ao reiniciar o HB Studio.' : '');
      if (err) { const e = document.createElement('div'); e.className = 'perr'; e.textContent = err; row.querySelector('.info').appendChild(e); }
      const acts = row.querySelector('.acts');
      if (p.valid && p.supported && !p.removed) {
        const t = document.createElement('button'); t.className = p.enabled ? 'on' : ''; t.textContent = p.enabled ? 'ATIVO' : 'ATIVAR';
        t.title = p.enabled ? 'Clique para desativar' : 'Clique para ativar';
        t.onclick = () => send('pluginenable', p.id, p.enabled ? 0 : 1); acts.appendChild(t);
      }
      if (!p.removed) { const r = document.createElement('button'); r.className = 'rm'; r.textContent = 'Remover'; r.onclick = () => { if (confirm('Remover o plugin “' + p.name + '”?')) send('pluginremove', p.id); }; acts.appendChild(r); }
      box.appendChild(row);
    });
  }
  $('#plg-install').onclick = () => send('pluginpick');
  function showReview(d) {
    const body = $('#rv-body'); body.innerHTML = '';
    $('#rv-err').style.display = 'none';
    if (!d.ok) {
      reviewToken = null;
      $('#rv-name').textContent = 'Não foi possível ler o pacote';
      $('#rv-risk').style.display = 'none'; $('#rv-ok').style.display = 'none';
      const e = $('#rv-err'); e.textContent = d.error || 'Pacote inválido.'; e.style.display = '';
      $('#modal3').classList.add('on'); return;
    }
    reviewToken = d.token;
    $('#rv-ok').style.display = '';
    $('#rv-name').textContent = d.name;
    const g = document.createElement('div'); g.className = 'rvgrid';
    const rows = [['Tipo', (typeInfo[d.type] || typeInfo.theme)[0]], ['Versão', d.version + (d.replaces ? ' (substitui a ' + d.installedVersion + ')' : '')], ['Autor', d.author || '—'],
                  ['Licença', d.license || '—'], ['Arquivos', d.files + ' · ' + (d.size / 1024).toFixed(0) + ' KB'], ['Impressão', d.sha + '…'], ['Descrição', d.description || '—']];
    rows.forEach(([k, v]) => { const a = document.createElement('span'); a.textContent = k; const b = document.createElement('span'); b.textContent = v; g.append(a, b); });
    body.appendChild(g);
    $('#rv-risk').style.display = d.native ? '' : 'none';
    $('#rv-ack').checked = false;
    $('#rv-ok').disabled = !!d.native;
    $('#modal3').classList.add('on');
  }
  $('#rv-ack').onchange = () => { $('#rv-ok').disabled = !$('#rv-ack').checked; };
  $('#rv-ok').onclick = () => { if (reviewToken) send('pluginconfirm', reviewToken); reviewToken = null; closeModals(); };
  const cancelReview = () => { if (reviewToken) send('plugincancel', reviewToken); reviewToken = null; closeModals(); };
  $('#rv-cancel').onclick = $('#rv-x').onclick = cancelReview;
  $$('#tabs button').forEach(b => b.addEventListener('click', () => { if (b.dataset.v === 'plg') renderPlugins(); }));

  // remember the chosen transition time between runs
  try { const d = +localStorage.getItem('hb.duration'); if ([250, 500, 1000, 2000].includes(d)) { duration = d; $$('#dur button').forEach(x => x.classList.toggle('on', +x.dataset.ms === d)); } } catch (e) {}

