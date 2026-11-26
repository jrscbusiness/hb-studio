  // ---------- TRANSMISSÃO ----------
  let xmitList = [], xmitStatus = {};
  // Only the two most stable, publicly documented ingest URLs are pre-filled - a wrong prefilled
  // server is worse than an empty one with a hint, so every other platform leaves it blank for the
  // operator to paste from that platform's own broadcast page (this app does no OAuth/login with
  // any of them - "platform" only changes the preset/label, see the note in the TRANSMISSÃO tab).
  // RTMPS (TLS) variants: YouTube's rtmps ingest and Twitch's rtmps ingest are documented by those platforms
  // (rtmps://a.rtmps.youtube.com:443/live2, rtmps://live.twitch.tv:443/app). Facebook requires RTMPS
  // (rtmps://live-api-s.facebook.com:443/rtmp/) but that address was NOT verified here, so it is only a hint.
  const xmitPresetByProto = {
    rtmp: {youtube: 'rtmp://a.rtmp.youtube.com/live2', twitch: 'rtmp://live.twitch.tv/app'},
    rtmps: {youtube: 'rtmps://a.rtmps.youtube.com:443/live2', twitch: 'rtmps://live.twitch.tv:443/app'},
  };
  const xmitPresetServer = xmitPresetByProto.rtmp;
  const xmitPresetFor = (platform, proto) => (xmitPresetByProto[proto] || {})[platform] || '';
  const xmitIsPreset = url => Object.values(xmitPresetByProto).some(m => Object.values(m).includes(url));
  function xmitHint() {
    const proto = $('#xm-protocol').value, plat = $('#xm-platform').value, srv = $('#xm-server').value.trim();
    let t = '';
    if (proto === 'srt') t = 'SRT ainda não é suportado nesta versão (veja docs/SRT.md): use RTMP ou RTMPS.';
    else if (proto === 'rtmps') t = 'RTMPS = RTMP criptografado (TLS, porta 443 por padrão); o certificado do servidor é sempre verificado.';
    if (plat === 'facebook') t += ' O Facebook exige RTMPS; copie o endereço da sua página de transmissão (rtmps://live-api-s.facebook.com:443/rtmp/ — não verificado).';
    if (/^rtmps:\/\//i.test(srv) && proto === 'rtmp') t += ' O endereço começa com rtmps:// — o protocolo será tratado como RTMPS.';
    $('#xm-hint').textContent = t.trim();
  }
  function xmitSpark(canvas, hist, target) {
    const w = canvas.width, h = canvas.height, c = canvas.getContext('2d');
    c.clearRect(0, 0, w, h);
    if (!hist || !hist.length) return;
    const max = Math.max(target || 1, ...hist, 1) * 1.1, n = hist.length, bw = w / Math.max(n, 30);
    const css = getComputedStyle(document.documentElement);
    hist.forEach((v, i) => {
      const bh = Math.max(1, Math.round(h * v / max));
      c.fillStyle = v < (target || 0) * 0.6 ? (css.getPropertyValue('--warn-ink').trim() || '#c07000') : (css.getPropertyValue('--blue').trim() || '#1f6bff');
      c.fillRect(w - (n - i) * bw, h - bh, Math.max(1, bw - 1), bh);
    });
  }
  const fmtMbps = k => (k / 1000).toFixed(k >= 10000 ? 0 : 1);
  const xmitPlatformLabel = {youtube: 'YouTube', twitch: 'Twitch', facebook: 'Facebook', kick: 'Kick', vimeo: 'Vimeo',
                             linkedin: 'LinkedIn', x: 'X (Twitter)', custom: 'Personalizado'};
  function renderXmit() {
    const box = $('#xmit-list'); box.innerHTML = '';
    if (!xmitList.length) {
      box.innerHTML = '<div class="devempty">Nenhum destino cadastrado ainda. Clique em “+ Adicionar destino”.</div>';
      return;
    }
    xmitList.forEach(d => {
      const st = xmitStatus[d.id] || {};
      const live = d.enabled && st.publishing, connecting = d.enabled && st.running && !st.publishing;
      const cls = live ? 'live' : connecting ? 'connecting' : '';
      const label = !d.enabled ? 'DESATIVADO' : live ? 'AO VIVO' : st.reconnecting ? 'RECONECTANDO' : connecting ? 'CONECTANDO' : 'PARADO';
      const row = document.createElement('div'); row.className = 'xmitcard';
      row.innerHTML = `<div class="xc-top"><span class="xc-dot ${cls}"></span><b></b>
          <span class="badge"></span><span class="xc-st ${cls}">${label}</span></div>
        <div class="xc-info"></div><div class="xc-info err" style="display:none"></div>
        <div class="xc-health" style="display:none"><canvas class="xc-spark" width="120" height="26" aria-label="Bitrate dos últimos 30 s"></canvas>
          <div class="xc-stats"></div></div>
        <div class="xc-actions">
          <button class="btn sm" data-a="enable"></button>
          <button class="btn sm primary" data-a="run"></button>
          <button class="icon" data-a="edit" title="Editar">⚙</button>
          <button class="icon" data-a="remove" title="Remover">✕</button>
        </div>`;
      row.querySelector('b').textContent = d.name || '(sem nome)';
      row.querySelector('.badge').textContent = xmitPlatformLabel[d.platform] || d.platform;
      row.querySelector('.xc-info').textContent = (d.serverUrl || '(sem servidor)') + (d.keySet ? ' · chave ' + d.keyHint : ' · sem chave definida');
      const errEl = row.querySelector('.xc-info.err');
      if (st.message && ((connecting === false && live === false && d.enabled) || st.reconnecting)) { errEl.textContent = st.message; errEl.style.display = ''; }
      const hs = st.stats;
      if (hs && (live || connecting || st.reconnecting)) {
        const hb = row.querySelector('.xc-health'); hb.style.display = '';
        xmitSpark(hb.querySelector('canvas'), hs.hist, hs.targetKbps);
        const bits = [fmtMbps(hs.actualKbps) + ' / ' + fmtMbps(hs.targetKbps) + ' Mbps'];
        if (st.adaptive && hs.encoderKbps && hs.encoderKbps < hs.targetKbps) bits.push('adaptativo: ' + fmtMbps(hs.encoderKbps) + ' Mbps');
        bits.push('descartes ' + (hs.droppedV + hs.droppedA));
        bits.push('reconexões ' + hs.reconnects);
        bits.push('fila ' + hs.qV + '/' + hs.qA);
        if (hs.encoder) bits.push(hs.encoder);
        const sEl = hb.querySelector('.xc-stats'); sEl.textContent = bits.join(' · ');
        if (hs.droppedRecent > 0 || st.reconnecting) sEl.classList.add('warn');
      }
      const eb = row.querySelector('[data-a=enable]');
      eb.textContent = d.enabled ? 'DESATIVAR' : 'ATIVAR';
      eb.onclick = () => send('xmitenable', d.id, d.enabled ? 0 : 1);
      const rb = row.querySelector('[data-a=run]');
      rb.textContent = st.running ? 'PARAR' : 'INICIAR';
      rb.disabled = !d.enabled;
      rb.onclick = () => send(st.running ? 'xmitstop' : 'xmitstart', d.id);
      row.querySelector('[data-a=edit]').onclick = () => openXmitEditor(d);
      row.querySelector('[data-a=remove]').onclick = () => { if (confirm('Remover o destino “' + (d.name || '') + '”?')) send('xmitremove', d.id); };
      box.appendChild(row);
    });
  }
  let xmitEditId = '';
  function openXmitEditor(d) {
    d = d || {};
    xmitEditId = d.id || '';
    $('#xm-title').textContent = d.id ? (d.name || 'Editar destino') : 'Novo destino';
    $('#xm-name').value = d.name || '';
    $('#xm-platform').value = d.platform || 'youtube';
    $('#xm-protocol').value = d.protocol || 'rtmp';
    $('#xm-server').value = d.serverUrl || xmitPresetFor(d.platform || 'youtube', d.protocol || 'rtmp') || '';
    $('#xm-abr').checked = !!d.adaptiveBitrate;
    xmitHint();
    $('#xm-key').value = '';
    $('#xm-key').placeholder = d.keySet ? 'Chave salva (' + d.keyHint + ') - deixe em branco para manter' : 'Cole aqui a chave da sua conta nesta plataforma';
    $('#xm-vkbps').value = d.videoKbps || 6000;
    $('#xm-akbps').value = d.audioKbps || 160;
    $('#xm-encoder').value = d.encoder || 'auto';
    $('#xm-ownformat').checked = !!d.useOwnFormat;
    $('#xm-ownformat-fields').style.display = d.useOwnFormat ? '' : 'none';
    $('#xm-w').value = d.width || 1920; $('#xm-h').value = d.height || 1080; $('#xm-fps').value = d.fpsNum || 30;
    $('#xm-note').textContent = '';
    $('#modal4').classList.add('on');
  }
  renderXmit();   // shows the empty-state hint immediately, before any xmit_dest message arrives
  renderOutFmt(null);   // shows the preset list immediately, before any outfmt message arrives
  $('#xmit-add').addEventListener('click', () => openXmitEditor(null));
  $('#xmit-startall').addEventListener('click', () => send('xmitstartall'));
  $('#xmit-stopall').addEventListener('click', () => send('xmitstopall'));
  $('#xm-platform').addEventListener('change', () => {
    const preset = xmitPresetFor($('#xm-platform').value, $('#xm-protocol').value);
    if (preset && !$('#xm-server').value.trim()) $('#xm-server').value = preset;
    xmitHint();
  });
  $('#xm-protocol').addEventListener('change', () => {
    // swap a preset address for its twin in the other protocol, but never overwrite a hand-typed one
    const cur = $('#xm-server').value.trim(), preset = xmitPresetFor($('#xm-platform').value, $('#xm-protocol').value);
    if (preset && (!cur || xmitIsPreset(cur))) $('#xm-server').value = preset;
    xmitHint();
  });
  $('#xm-server').addEventListener('input', xmitHint);
  $('#xm-ownformat').addEventListener('change', () => { $('#xm-ownformat-fields').style.display = $('#xm-ownformat').checked ? '' : 'none'; });
  $('#xm-x').addEventListener('click', closeModals);
  $('#xm-cancel').addEventListener('click', closeModals);
  $('#xm-save').addEventListener('click', () => {
    const name = $('#xm-name').value.trim(), server = $('#xm-server').value.trim();
    if (!name || !server) { $('#xm-note').textContent = 'Informe pelo menos o nome e o endereço do servidor.'; return; }
    const payload = {
      id: xmitEditId, name, platform: $('#xm-platform').value, protocol: $('#xm-protocol').value, serverUrl: server,
      streamKey: $('#xm-key').value, encoder: $('#xm-encoder').value, adaptiveBitrate: $('#xm-abr').checked,
      videoKbps: +$('#xm-vkbps').value || 6000, audioKbps: +$('#xm-akbps').value || 160,
      useOwnFormat: $('#xm-ownformat').checked, width: +$('#xm-w').value || 1920, height: +$('#xm-h').value || 1080,
      fpsNum: +$('#xm-fps').value || 30, fpsDen: 1,
    };
    if (/^rtmps:\/\//i.test(server)) payload.protocol = 'rtmps';   // the address wins over the dropdown
    send('xmitsave', JSON.stringify(payload));
    closeModals();
  });

