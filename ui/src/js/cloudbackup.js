  // ---- FEATURE: cloudbackup ---- CONFIG card "Backup em nuvem" (credentials are write-only: the host only sends masked hints)
  let cbSt = null, cbDirty = false, cbProv = 's3', cbSig = '';
  const cbFmt = n => n >= 1073741824 ? (n / 1073741824).toFixed(1) + ' GB' : n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
  const cbKind = { program: 'PROGRAM', 'iso-video': 'ISO VÍDEO', 'iso-audio': 'ISO ÁUDIO' };
  const cbStateTxt = { queued: 'Na fila', uploading: 'Enviando', done: 'Concluído', error: 'Erro' };
  function cbSetProv(p) {
    cbProv = p;
    $$('#cb-prov button').forEach(b => b.classList.toggle('on', b.dataset.p === p));
    $('#cb-s3').style.display = p === 's3' ? '' : 'none';
    $('#cb-gd').style.display = p === 'gdrive' ? '' : 'none';
  }
  function cbForm() {
    const o = {
      enabled: $('#cb-enabled').checked, provider: cbProv, s3Endpoint: $('#cb-endpoint').value.trim(), s3Region: $('#cb-region').value.trim(),
      s3Bucket: $('#cb-bucket').value.trim(), s3Prefix: $('#cb-prefix').value.trim(), s3Addressing: $('#cb-addr').value,
      gClientId: $('#cb-gid').value.trim(), gFolderName: $('#cb-gfolder').value.trim(),
      bandwidthMbps: Math.max(0, parseFloat($('#cb-bw').value) || 0), pauseWhileStreaming: $('#cb-pause').checked,
      upProgram: $('#cb-up-prog').checked, upIsoVideo: $('#cb-up-isov').checked, upIsoAudio: $('#cb-up-isoa').checked,
      deleteAfterUpload: $('#cb-del').checked, watchFolder: $('#cb-watch').checked,
    };
    // Secrets are only included when typed; blank means "keep the stored one".
    if ($('#cb-akey').value) o.s3AccessKey = $('#cb-akey').value;
    if ($('#cb-skey').value) o.s3SecretKey = $('#cb-skey').value;
    if ($('#cb-gsec').value) o.gClientSecret = $('#cb-gsec').value;
    return o;
  }
  function cbSave() { send('cloudsave', JSON.stringify(cbForm())); ['#cb-akey', '#cb-skey', '#cb-gsec'].forEach(s => { $(s).value = ''; }); cbDirty = false; }
  function cbRender(d) {
    cbSt = d;
    const c = d.cfg || {}, sec = d.secrets || {}, t = d.totals || {};
    if (!cbDirty) {
      $('#cb-enabled').checked = !!c.enabled; cbSetProv(c.provider || 's3');
      $('#cb-endpoint').value = c.s3Endpoint || ''; $('#cb-region').value = c.s3Region || ''; $('#cb-bucket').value = c.s3Bucket || '';
      $('#cb-prefix').value = c.s3Prefix || ''; $('#cb-addr').value = c.s3Addressing || 'auto';
      $('#cb-gid').value = c.gClientId || ''; $('#cb-gfolder').value = c.gFolderName || '';
      $('#cb-bw').value = c.bandwidthMbps || 0; $('#cb-pause').checked = !!c.pauseWhileStreaming;
      $('#cb-up-prog').checked = !!c.upProgram; $('#cb-up-isov').checked = !!c.upIsoVideo; $('#cb-up-isoa').checked = !!c.upIsoAudio;
      $('#cb-del').checked = !!c.deleteAfterUpload; $('#cb-watch').checked = !!c.watchFolder;
    }
    $('#cb-akey').placeholder = sec.s3Access ? sec.s3Access + ' (guardada; deixe em branco para manter)' : 'não definida';
    $('#cb-skey').placeholder = sec.s3Secret ? sec.s3Secret + ' (guardada; deixe em branco para manter)' : 'não definida';
    $('#cb-gsec').placeholder = sec.gClientSecret ? sec.gClientSecret + ' (guardado)' : 'opcional / não definido';
    const au = d.auth || {};
    $('#cb-gstate').textContent = au.state === 'waiting' || au.state === 'error' ? (au.msg || '') : (sec.gConnected ? 'Conta conectada' + (c.gAccount ? ' (' + c.gAccount + ')' : '') : 'Conta não conectada');
    const badge = $('#cb-badge');
    badge.textContent = !c.enabled ? 'DESLIGADO' : t.paused ? 'PAUSADO' : (t.uploading ? 'ENVIANDO' : 'ATIVO');
    const te = d.test || {};
    $('#cb-msg').textContent = te.state === 'running' ? 'Testando a conexão…' : te.msg ? (te.state === 'ok' ? '✓ ' : '✗ ') + te.msg : (d.status || '');
    const total = t.bytesTotal || 0;
    $('#cb-gbar').style.width = (total ? Math.min(100, 100 * (t.bytesDone || 0) / total) : 0) + '%';
    const parts = [];
    if (t.uploading) parts.push(t.uploading + ' enviando'); if (t.queued) parts.push(t.queued + ' na fila');
    if (t.done) parts.push(t.done + ' concluído(s)'); if (t.error) parts.push(t.error + ' com erro');
    let sum = parts.length ? parts.join(' · ') : 'Nada na fila.';
    if (t.bps > 0) sum += ' · ' + cbFmt(t.bps) + '/s';
    if (t.paused && t.pauseReason) sum += ' · ' + t.pauseReason;
    $('#cb-sum').textContent = sum;
    const jobs = d.jobs || [];
    const sig = jobs.map(j => j.id + j.state + j.error + j.note + j.attempts).join('|');
    const box = $('#cb-jobs');
    if (sig !== cbSig) {   // rebuild only when something other than progress changed (keeps buttons clickable at 4 Hz)
      cbSig = sig; box.innerHTML = '';
      jobs.forEach(j => {
        const row = document.createElement('div'); row.className = 'cb-job ' + j.state; row.dataset.id = j.id;
        const top = document.createElement('div'); top.className = 't';
        const nm = document.createElement('b'); nm.textContent = j.name; nm.title = j.name;
        const bd = document.createElement('span'); bd.className = 'badge'; bd.textContent = (cbKind[j.kind] || j.kind) + ' · ' + (cbStateTxt[j.state] || j.state);
        const act = document.createElement('span'); act.style.display = 'flex'; act.style.gap = '6px';
        if (j.state === 'error') { const b = document.createElement('button'); b.textContent = 'REPETIR'; b.dataset.a = 'retry'; act.appendChild(b); }
        if (j.state !== 'done') { const b = document.createElement('button'); b.textContent = 'CANCELAR'; b.dataset.a = 'cancel'; act.appendChild(b); }
        top.append(nm, bd, act); row.appendChild(top);
        const msg = j.error || j.note;
        if (msg) { const e = document.createElement('div'); e.className = 'e'; e.textContent = (j.state === 'queued' && j.attempts ? 'Nova tentativa em breve: ' : '') + msg; row.appendChild(e); }
        const bar = document.createElement('div'); bar.className = 'bar'; bar.appendChild(document.createElement('i')); row.appendChild(bar);
        box.appendChild(row);
      });
    }
    jobs.forEach(j => {
      const row = box.querySelector('[data-id="' + j.id + '"] .bar i');
      if (row) row.style.width = (j.state === 'done' ? 100 : j.size ? Math.min(100, 100 * j.done / j.size) : 0) + '%';
    });
  }
  $('#cb-jobs').addEventListener('click', e => {
    const b = e.target.closest('button[data-a]'); if (!b) return;
    const id = b.closest('.cb-job').dataset.id;
    send(b.dataset.a === 'retry' ? 'cloudretry' : 'cloudcancel', id);
  });
  $$('#cb-prov button').forEach(b => { b.onclick = () => { cbDirty = true; cbSetProv(b.dataset.p); }; });
  $$('#cb-card input, #cb-card select').forEach(el => el.addEventListener('input', () => { cbDirty = true; }));
  $('#cb-save').onclick = cbSave;
  $('#cb-test').onclick = () => { cbSave(); send('cloudtest'); };
  $('#cb-glogin').onclick = () => { cbSave(); send('cloudgoogle', 'login'); };
  $('#cb-clear').onclick = () => { if (confirm('Remover as credenciais guardadas deste destino?')) send('cloudclear', cbProv === 'gdrive' ? 'gdrive' : 's3'); };
  $('#cb-retryall').onclick = () => send('cloudretry', 'all');
  $('#cb-clearfin').onclick = () => send('cloudclearfinished');
  // ---- /FEATURE: cloudbackup ----
