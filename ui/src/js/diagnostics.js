  // ---- FEATURE: diagnostics ---- CONFIG card "Diagnóstico e checklist" (docs/DIAGNOSTICO.md)
  // Host strings are written with textContent only. The export path is chosen by the host's native Save dialog.
  const chkIcons = { ok: '✔', warn: '⚠', fail: '✖', skip: '–' };
  const chkStatusTxt = { ok: 'OK', warn: 'Atenção', fail: 'Problema', skip: 'Não verificado' };
  let chkSpeedDirty = false;
  function chkRender(d) {
    const btn = $('#chk-run'), list = $('#chk-list'), sum = $('#chk-sum');
    if (d.busy) {
      btn.disabled = true; btn.textContent = 'VERIFICANDO…';
      sum.textContent = 'Verificando… o teste de internet pode levar alguns segundos.';
      return;
    }
    btn.disabled = false; btn.textContent = 'RODAR CHECKLIST';
    list.innerHTML = '';
    (d.results || []).forEach(r => {
      const li = document.createElement('li');
      const st = chkIcons[r.s] ? r.s : 'skip';
      li.className = 'chk-item ' + st; li.dataset.id = r.id;
      const ic = document.createElement('span'); ic.className = 'ico'; ic.textContent = chkIcons[st]; ic.setAttribute('aria-label', chkStatusTxt[st]);
      const t = document.createElement('div'); t.className = 't'; t.textContent = r.title;
      const dd = document.createElement('div'); dd.className = 'd'; dd.textContent = r.detail || '';
      li.append(ic, t, dd);
      if (r.hint) { const h = document.createElement('div'); h.className = 'h'; h.textContent = 'Como resolver: ' + r.hint; li.appendChild(h); }
      list.appendChild(li);
    });
    if (d.error) sum.textContent = 'Não foi possível concluir a verificação.';
    else if ((d.results || []).length) {
      sum.textContent = d.fail ? d.fail + ' problema(s) a resolver antes de entrar ao vivo.' + (d.warn ? ' ' + d.warn + ' atenção(ões).' : '')
        : d.warn ? 'Sem problemas graves; ' + d.warn + ' ponto(s) de atenção.' : 'Tudo certo para entrar ao vivo.';
    }
  }
  function chkCfg(d) {
    if (chkSpeedDirty) return;   // never wipe what the user is typing
    $('#chk-speed-on').checked = !!d.enabled;
    $('#chk-speed-url').value = d.url || '';
  }
  $('#dg-export').addEventListener('click', () => {
    send('diagexport');
    const n = $('#dg-note'); n.textContent = 'Escolha onde salvar o arquivo na janela que abriu.';
  });
  $('#chk-run').addEventListener('click', () => { send('chkrun'); });
  $('#chk-speed-save').addEventListener('click', () => {
    send('chkcfg', $('#chk-speed-on').checked ? '1' : '0', $('#chk-speed-url').value.trim());
    chkSpeedDirty = false;
  });
  ['#chk-speed-on', '#chk-speed-url'].forEach(s => $(s).addEventListener('input', () => { chkSpeedDirty = true; }));
  // ---- /FEATURE: diagnostics ----
