  // ---------- CONFIG: Formatos de saída comuns ----------
  const outFmtPresets = [
    { w: 1280, h: 720, fpsNum: 30, fpsDen: 1, label: '720p30', desc: '1280×720 · 30 fps' },
    { w: 1280, h: 720, fpsNum: 60, fpsDen: 1, label: '720p60', desc: '1280×720 · 60 fps' },
    { w: 1920, h: 1080, fpsNum: 25, fpsDen: 1, label: '1080p25', desc: '1920×1080 · 25 fps' },
    { w: 1920, h: 1080, fpsNum: 30, fpsDen: 1, label: '1080p30', desc: '1920×1080 · 30 fps' },
    { w: 1920, h: 1080, fpsNum: 60, fpsDen: 1, label: '1080p60', desc: '1920×1080 · 60 fps' },
    { w: 3840, h: 2160, fpsNum: 30, fpsDen: 1, label: '2160p30 (4K)', desc: '3840×2160 · 30 fps' },
  ];
  function renderOutFmt(d) {
    const box = $('#outfmt-list'); box.innerHTML = '';
    outFmtPresets.forEach(p => {
      const isActive = d && d.activeW === p.w && d.activeH === p.h && d.activeFpsNum === p.fpsNum && d.activeFpsDen === p.fpsDen;
      const isPending = d && d.w === p.w && d.h === p.h && d.fpsNum === p.fpsNum && d.fpsDen === p.fpsDen;
      const f = document.createElement('div'); f.className = 'fmt' + (isPending ? ' on' : ''); f.setAttribute('role', 'radio'); f.tabIndex = 0;
      f.innerHTML = `<span class="rd"></span><div><b>${p.label}</b><em>${p.desc}</em></div>` +
        (isActive ? '<span class="ext">ativo agora</span>' : isPending ? '<span class="ext">aplica ao reiniciar</span>' : '');
      const pick = () => send('outfmt', p.w, p.h, p.fpsNum, p.fpsDen);
      f.onclick = pick; f.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } };
      box.appendChild(f);
    });
    const note = $('#outfmt-note');
    note.textContent = (d && (d.w !== d.activeW || d.h !== d.activeH || d.fpsNum !== d.activeFpsNum || d.fpsDen !== d.activeFpsDen))
      ? 'Formato escolhido: será aplicado na próxima vez que o HB Studio for aberto.'
      : 'Escolha o formato de saída do Program (afeta câmera virtual, gravação e transmissão) — a mudança só vale depois de reiniciar o HB Studio.';
  }
  $('#out-ndi-toggle').addEventListener('click', () => {
    const name = $('#out-ndi-name').value.trim() || 'HB Studio Program';
    send('ndiout', ndiOutEnabled ? 'off' : 'on', name);
  });

