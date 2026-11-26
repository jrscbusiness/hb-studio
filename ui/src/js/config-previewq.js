  // ---------- CONFIG: Qualidade do Preview ----------
  // Mirrors Engine::previewQualityPresets() (src/app/engine.cpp) - keep both lists in sync.
  const previewQPresets = [
    { id: 'baixa', label: 'Baixa (mais leve)', desc: '320×180' },
    { id: 'media', label: 'Média', desc: '480×270' },
    { id: 'alta', label: 'Alta (padrão)', desc: '640×360' },
    { id: 'maxima', label: 'Máxima (computadores potentes)', desc: '960×540' },
  ];
  function renderPreviewQ(id) {
    const box = $('#previewq-list'); box.innerHTML = '';
    previewQPresets.forEach(p => {
      const f = document.createElement('div'); f.className = 'fmt' + (id === p.id ? ' on' : ''); f.setAttribute('role', 'radio'); f.tabIndex = 0;
      f.innerHTML = `<span class="rd"></span><div><b>${p.label}</b><em>${p.desc}</em></div>`;
      const pick = () => send('previewq', p.id);
      f.onclick = pick; f.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } };
      box.appendChild(f);
    });
  }
  renderPreviewQ('alta');   // shows the preset list immediately, before any previewq message arrives
  $('#log-cfg-copy').addEventListener('click', () => {
    const text = lastLog.map(l => l.t).join('\n');
    const note = $('#log-cfg-note');
    const ok = () => { note.textContent = 'Registro copiado.'; setTimeout(() => { if (note.textContent === 'Registro copiado.') note.textContent = ''; }, 2500); };
    const fail = () => { note.textContent = 'Não foi possível copiar automaticamente; selecione o texto manualmente.'; };
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(ok, fail);
    else fail();
  });
  $('#out-ndi-name').addEventListener('change', () => { if (ndiOutEnabled) send('ndiout', 'on', $('#out-ndi-name').value.trim() || 'HB Studio Program'); });

