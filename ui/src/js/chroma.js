  // ---- FEATURE: chroma ---- Fundo (background removal) section of the camera-settings dialog
  let chrSt = null;
  const chr = id => $('#chr-' + id);
  const chrSlot = () => camcfgSlot;
  const chrCur = () => (chrSt && chrSt.slots && chrSt.slots[camcfgSlot]) || null;
  let chrSentAt = {};
  const chrSend = (key, val, throttle) => {
    if (camcfgSlot < 0) return;
    const now = Date.now();
    if (throttle && now - (chrSentAt[key] || 0) < 120) { clearTimeout(chrSend['t' + key]); chrSend['t' + key] = setTimeout(() => chrSend(key, val, false), 130); return; }
    chrSentAt[key] = now;
    send('chroma', camcfgSlot, key, val);
  };
  const fmtMb = n => (n / 1048576).toFixed(2).replace('.', ',');
  function syncChroma(force) {
    const d = chrCur();
    if (!d || !$('#modal5').classList.contains('on')) return;
    const active = document.activeElement;
    const setVal = (el, v) => { if (force || active !== el) el.value = v; };
    setVal(chr('mode'), d.mode); setVal(chr('repl'), d.repl); setVal(chr('tol'), d.tol); setVal(chr('soft'), d.soft);
    setVal(chr('spill'), d.spill); setVal(chr('q'), d.q); setVal(chr('color'), d.color); setVal(chr('blur'), d.blur);
    chr('ai-opt').disabled = !chrSt.runtime;
    chr('ai-opt').textContent = chrSt.runtime ? 'IA (sem tela verde)' : 'IA (indisponível neste build)';
    const m = d.mode, classic = m === 'green' || m === 'blue';
    const vis = (id, on) => { chr(id).style.display = on ? '' : 'none'; };
    vis('tol-l', m !== 'off'); vis('soft-l', m !== 'off'); vis('spill-l', classic); vis('q-l', m === 'ai');
    vis('color-l', m !== 'off' && d.repl === 'color'); vis('blur-l', m !== 'off' && d.repl === 'blur');
    vis('img-l', m !== 'off' && d.repl === 'image');
    chr('repl').disabled = m === 'off';
    chr('imgname').textContent = d.img || 'nenhuma';
    chr('tol-l').firstChild.textContent = m === 'ai' ? 'Sensibilidade (limiar da máscara)' : 'Tolerância';
    // model box
    const md = chrSt.model || {};
    const bar = chr('bar'), dl = chr('dl'), cn = chr('cancel');
    let txt = '', showBar = false;
    if (!chrSt.runtime) txt = 'Este build não inclui o ONNX Runtime: só o chroma verde/azul está disponível.';
    else if (md.state === 'ready') txt = 'Modelo de IA instalado (' + md.license + ').' + (chrSt.ai && chrSt.ai.state === 'loading' ? ' Carregando…' : '');
    else if (md.state === 'downloading') { txt = 'Baixando o modelo de IA… ' + Math.round(100 * md.received / Math.max(1, md.total)) + '%'; showBar = true; }
    else if (md.state === 'error') txt = md.msg;
    else if (md.state === 'unavailable') txt = md.msg;
    else txt = 'A IA precisa de um modelo (' + fmtMb(md.mb * 1048576) + ' MB, ' + md.license + ') que é baixado uma vez e verificado por SHA-256.';
    chr('model').textContent = txt;
    bar.style.display = showBar ? '' : 'none';
    chr('bari').style.width = showBar ? Math.round(100 * md.received / Math.max(1, md.total)) + '%' : '0';
    dl.style.display = chrSt.runtime && (md.state === 'none' || md.state === 'error') ? '' : 'none';
    dl.textContent = md.state === 'error' ? 'TENTAR DE NOVO' : 'BAIXAR MODELO';
    cn.style.display = md.state === 'downloading' ? '' : 'none';
    // warnings
    const w = chr('warn'); let wt = '';
    if (d.imgErr) wt = d.imgErr;
    else if (m === 'ai' && chrSt.ai && chrSt.ai.state === 'error') wt = chrSt.ai.msg;
    else if (d.aiFailed) wt = d.aiMsg;
    else if (d.slowAi) wt = 'Seu computador está lento para IA (' + Math.round(d.inferMs) + ' ms por análise): reduza a qualidade.';
    else if (d.slowApply) wt = 'O efeito está pesado neste computador (' + d.applyMs.toFixed(0) + ' ms por quadro): reduza a qualidade ou use o chroma verde/azul.';
    else if (m === 'ai' && md.state !== 'ready') wt = 'Baixe o modelo para ativar a IA; até lá a câmera aparece sem alteração.';
    w.textContent = wt; w.style.display = wt ? '' : 'none';
  }
  window.__chrPrev = (id, im) => {
    if (id !== 's' + camcfgSlot || !$('#modal5').classList.contains('on')) return;
    const c = chr('prev'); c.getContext('2d').clearRect(0, 0, c.width, c.height); c.getContext('2d').drawImage(im, 0, 0, c.width, c.height);
  };
  for (const [id, key, thr] of [['mode', 'mode'], ['repl', 'repl'], ['q', 'q'], ['tol', 'tol', 1], ['soft', 'soft', 1], ['spill', 'spill', 1], ['blur', 'blur', 1], ['color', 'color', 1]])
    chr(id).addEventListener(thr ? 'input' : 'change', ev => chrSend(key, ev.target.value, !!thr));
  chr('img').addEventListener('click', () => send('chromaimg', camcfgSlot));
  chr('dl').addEventListener('click', () => send('chromamodel', 'download'));
  chr('cancel').addEventListener('click', () => send('chromamodel', 'cancel'));
  // ---- /FEATURE: chroma ----

