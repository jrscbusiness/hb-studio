  // ---------- frames ----------
  const imgs = {};
  function drawFrame(id, b64) {
    const ctx = canv[id]; if (!ctx) return;
    const im = imgs[id] || (imgs[id] = new Image());
    im.onload = () => { ctx.drawImage(im, 0, 0, ctx.canvas.width, ctx.canvas.height); if (window.__chrPrev) window.__chrPrev(id, im);   /* FEATURE: chroma: live preview hook */
      if (id === 'pgm') ovGfxCtx.drawImage(im, 0, 0, ovGfxCtx.canvas.width, ovGfxCtx.canvas.height); /* FEATURE: overlays */ };
    im.src = 'data:image/jpeg;base64,' + b64;
  }

  // ---------- state ----------
  const pad = n => String(n).padStart(2, '0');
  function toast(msg) { const t = $('#toast'); t.textContent = msg; t.classList.add('show'); clearTimeout(toast.h); toast.h = setTimeout(() => t.classList.remove('show'), 3200); }
  let lastMsg = -1;
  let lastLog = [];
  function render(s) {
    if (s.slots) {
      slots = s.slots;
      $$('.tile').forEach(t => {
        const x = s.slots[+t.dataset.i], b = t.querySelector('b'), em = t.querySelector('em');
        if (!x) return;   // FEATURE: scenes: scene tiles are drawn by renderSceneTiles()
        b.textContent = x.k === 'dev' ? x.name : 'CAM ' + (+t.dataset.i + 1);
        em.textContent = stTxt[x.st]; em.style.display = '';
        em.className = x.k === 'dev' && x.st !== 'online' && x.st !== 'off' ? 'bad' : '';
        t.classList.toggle('off', x.en === false);
      });
      const hud = i => i >= N ? 'CENA' : s.slots[i].k === 'dev' && s.slots[i].w ? s.slots[i].w + '×' + s.slots[i].h : '1920×1080';
      const nm = i => i >= N ? sceneLabel(i) : s.slots[i].k === 'dev' ? s.slots[i].name : 'CAM ' + (i + 1);
      $('#pgm-name').textContent = nm(s.pgm); $('#pvw-name').textContent = nm(s.pvw);
      $('#pvw-hud').textContent = 'PRÓXIMA · ' + hud(s.pvw);
      $('#pgm-res').textContent = hud(s.pgm) + ' → ' + (s.pw || 1920) + '×' + (s.ph || 1080) + ' · ' + s.target + ' fps';
      const nd = s.slots.filter(x => x.k === 'dev').length;
      $('#foot-src').textContent = nd ? nd + ' dispositivo(s) · ' + (N - nd) + ' teste' : 'TESTE';
      renderLists();
    }
    if (s.ver) $('#ver').textContent = s.ver;
    lastPvw = s.pvw;
    if ($('#ptz-follow').checked && ptzCam !== s.pvw && s.pvw < N) { ptzCam = s.pvw; renderPtz(); }
    $$('.tile').forEach(t => { const i = +t.dataset.i; t.classList.toggle('pgm', i === s.pgm); t.classList.toggle('pvw', i === s.pvw); t.querySelector('.tally').textContent = i === s.pgm ? 'PGM' : 'PVW'; });
    $$('#bus-pgm .key').forEach(k => k.classList.toggle('pgm', +k.dataset.i === s.pgm));
    $$('#bus-pvw .key').forEach(k => k.classList.toggle('pvw', +k.dataset.i === s.pvw));
    if (s.pgm >= N || s.pvw >= N) { $('#pgm-name').textContent = sceneLabel(s.pgm); $('#pvw-name').textContent = sceneLabel(s.pvw); }   // FEATURE: scenes
    if (!s.slots) { $('#pgm-name').textContent = 'CAM ' + (s.pgm + 1); $('#pvw-name').textContent = 'CAM ' + (s.pvw + 1); }
    if (!tbarDown) $('#tbar').value = Math.round((s.transition ? s.progress : 0) * 1000);
    $('#b-cut').disabled = $('#b-dis').disabled = !!s.transition;
    $('#c-fps').textContent = s.fps.toFixed(1) + ' fps';
    $('#c-tier').textContent = s.tier;
    $('#s-tier').textContent = s.tier; $('#g-tier').textContent = s.tier;
    $('#s-cores').textContent = s.cores + ' núcleos'; $('#g-cores').textContent = s.cores;
    $('#s-ram').textContent = s.ramGiB + ' GB'; $('#g-ram').textContent = s.ramGiB + ' GB';
    $('#g-src').textContent = s.srcRec + ' / ' + s.srcMax;
    $('#s-fps').textContent = s.fps.toFixed(1) + ' / ' + s.target + ' fps';
    $('#s-meter').style.width = Math.min(100, s.fps / s.target * 100) + '%';
    $('#s-skip').textContent = s.skipped;
    // Only REAL cameras count toward the performance warning: the slots still on the procedural
    // test pattern are effectively free to render, unlike decoding a real camera feed.
    const liveCount = (s.slots || []).filter(x => x.k === 'dev').length;
    const over = liveCount > s.srcRec;
    const ic = $('#in-count');
    ic.textContent = over ? liveCount + ' · acima do recomendado (' + s.srcRec + ')' : liveCount + ' / ' + s.srcRec;
    ic.className = 'cap mono' + (over ? ' warn' : '');
    ic.title = over ? 'AVISO DE DESEMPENHO: este computador (perfil ' + s.tier + ') recomenda até ' + s.srcRec + ' câmeras reais simultâneas (máximo ' + s.srcMax + '). Sugestão: reduzir a qualidade do Preview.' : (N + ' entradas disponíveis em 2 fileiras de 8; ' + liveCount + ' com câmera real conectada.');
    const f = s.frames, fps = s.target, sec = Math.floor(f / fps);
    $('#tc').textContent = pad(Math.floor(sec / 3600)) + ':' + pad(Math.floor(sec / 60) % 60) + ':' + pad(sec % 60) + ':' + pad(f % fps);

    const v = s.vcam;
    $('#c-vdot').className = 'dot' + (v.active ? ' on' : '');
    $('#c-vtxt').textContent = v.active ? (v.connected ? 'AO VIVO' : 'PRONTA') : 'OFF';
    $('#s-vsent').textContent = v.sent + ' quadros';
    $('#v-dot').className = 'dot' + (v.active ? ' on' : '');
    const main = $('#v-main'), sec2 = $('#v-sec');
    let title, note, mainTxt, mainCmd = null, secTxt = null, secCmd = null;
    if (!v.supported) { title = 'INDISPONÍVEL'; note = 'A câmera virtual exige Windows 11 (build 22000 ou superior).'; mainTxt = 'Indisponível'; }
    else if (v.busy) { title = 'AGUARDE…'; note = v.message || 'Processando…'; mainTxt = 'Trabalhando…'; }
    else if (!v.installed) { title = 'NÃO INSTALADA'; note = 'O componente da câmera virtual não está instalado. Execute o instalador do HB Studio novamente e marque “Instalar a câmera virtual”.'; mainTxt = 'Indisponível'; }
    else if (!v.active) { title = 'INSTALADA · DESATIVADA'; note = 'Ative para o "HB Studio Windows Virtual Camera" aparecer no OBS, Zoom, Teams e navegador.'; mainTxt = 'Ativar câmera virtual'; mainCmd = 'activate'; }
    else { title = v.connected ? 'AO VIVO · ENVIANDO' : 'ATIVA · AGUARDANDO APP'; note = v.connected ? 'O Program está sendo entregue ao aplicativo que abriu a câmera.' : 'No OBS: Fontes → + → Dispositivo de Captura de Vídeo → “HB Studio Windows Virtual Camera”.'; mainTxt = 'Desativar câmera virtual'; mainCmd = 'deactivate'; }
    $('#v-title').textContent = title;
    $('#v-note').textContent = note + (v.message && !v.busy ? '\n' + v.message : '');
    $('#v-note').className = 'note' + (v.error ? ' err' : '');
    main.textContent = mainTxt; main.disabled = !mainCmd; main.onclick = () => mainCmd && send('vcam', mainCmd);
    main.classList.toggle('primary', mainCmd !== 'deactivate');
    sec2.style.display = secTxt ? '' : 'none'; if (secTxt) { sec2.textContent = secTxt; sec2.onclick = () => send('vcam', secCmd); }
    if (v.busy && !$('#v-main .spin')) main.insertAdjacentHTML('afterbegin', '<span class="spin"></span>');

    const lg = $('#log'); lg.innerHTML = '';
    (s.log || []).slice(-9).forEach(l => { const d = document.createElement('div'); d.textContent = l.t; if (l.l === 'WARNING') d.className = 'w'; if (l.l === 'ERROR' || l.l === 'CRITICAL') d.className = 'e'; lg.appendChild(d); });
    lastLog = s.log || [];
    const lgc = $('#log-cfg');
    const wasAtBottom = lgc.scrollTop + lgc.clientHeight >= lgc.scrollHeight - 4;
    lgc.innerHTML = '';
    lastLog.forEach(l => { const d = document.createElement('div'); d.textContent = l.t; if (l.l === 'WARNING') d.className = 'w'; if (l.l === 'ERROR' || l.l === 'CRITICAL') d.className = 'e'; lgc.appendChild(d); });
    if (!lastLog.length) lgc.innerHTML = '<div>Nenhum registro ainda.</div>';
    if (wasAtBottom) lgc.scrollTop = lgc.scrollHeight;
    if (s.toast && s.toast.id !== lastMsg) { lastMsg = s.toast.id; toast(s.toast.text); }
  }

