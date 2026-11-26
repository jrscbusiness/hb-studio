  // ---- FEATURE: updates ---- CONFIG card "Atualizações" (the host owns every decision: download path, SHA-256, the
  // native confirmation dialog and running the installer; this page only sends intents and renders the state)
  let updSt = null;
  const updFmtSize = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
  function updRender(d) {
    updSt = d;
    $('#upd-cur').textContent = d.current || '—';
    $('#upd-last').textContent = d.lastCheck ? new Date(d.lastCheck * 1000).toLocaleString('pt-BR') : 'nunca';
    if (document.activeElement !== $('#upd-auto')) $('#upd-auto').checked = !!d.enabled;
    const u = $('#upd-url');
    if (document.activeElement !== u) u.value = d.releaseUrl === d.defaultUrl ? '' : d.releaseUrl;
    u.placeholder = d.defaultUrl || '';
    const dl = d.download || {}, busy = dl.phase === 'download' || dl.phase === 'verify';
    const avail = d.state === 'available' && d.release;
    $('#upd-status').textContent = d.checking ? 'Verificando…' : avail ? 'Há uma versão nova' : d.state === 'uptodate' ? 'Você está na versão mais recente' : 'Ainda não verificado';
    const msg = $('#upd-msg');
    msg.textContent = d.error || '';
    msg.classList.toggle('on', !!d.error);
    $('#upd-new').style.display = avail ? '' : 'none';
    $('#upd-dot').hidden = !(avail && !d.dismissed);
    if (avail) {
      $('#upd-newv').textContent = '(' + (d.release.commit || '') + ')';
      $('#upd-notes').textContent = d.release.notes || '';
      $('#upd-notes').style.display = d.release.notes ? '' : 'none';
      $('#upd-size').textContent = 'Instalador: ' + updFmtSize(d.release.size || 0) + (d.release.hashPublished ? ' · SHA-256 publicado' : ' · SEM SHA-256 publicado (a atualização automática será recusada)');
    }
    $('#upd-bar').style.display = busy ? '' : 'none';
    $('#upd-barfill').style.width = (dl.pct || 0) + '%';
    $('#upd-cancel').style.display = dl.phase === 'download' ? '' : 'none';
    $('#upd-install').disabled = busy;
    $('#upd-install').textContent = dl.phase === 'download' ? 'BAIXANDO… ' + (dl.pct || 0) + '%' : dl.phase === 'verify' ? 'VERIFICANDO…' : 'BAIXAR E INSTALAR';
    $('#upd-later').disabled = busy;
    $('#upd-check').disabled = !!d.checking || busy;
  }
  $('#upd-check').onclick = () => send('updcheck');
  $('#upd-install').onclick = () => send('updinstall');
  $('#upd-later').onclick = () => send('updlater');
  $('#upd-cancel').onclick = () => send('updcancel');
  $('#upd-auto').onchange = ev => send('updauto', ev.target.checked ? '1' : '0');
  $('#upd-url-save').onclick = () => send('updcfg', $('#upd-url').value.trim());
  $('#upd-url-reset').onclick = () => { $('#upd-url').value = ''; send('updcfg', ''); };
  if (host) {
    send('updget');   // the host decides whether a check is due (at most once per 6 h)
    setInterval(() => send('updget'), 30 * 60 * 1000);   // long sessions: re-ask; the host throttles
  }
