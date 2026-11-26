  // ---------- camera settings (resolution/fps) ----------
  let camcfgSlot = -1, camcfgId = '';
  function openCamSettings(i, x) {
    camcfgSlot = i; camcfgId = x.id || '';
    $('#camcfg-title').textContent = 'CAM ' + (i + 1);
    $('#camcfg-res').innerHTML = '<option value="">Automática (padrão da câmera)</option>';
    $('#camcfg-note').textContent = 'Buscando formatos suportados…';
    $('#modal5').classList.add('on');
    syncChroma(true);   // FEATURE: chroma
    if (camcfgId) send('devformats', camcfgId);
  }
  function renderCamFormats(d) {
    if (d.id !== camcfgId) return;   // a stale reply for a dialog the operator already closed/changed
    const sel = $('#camcfg-res');
    sel.innerHTML = '<option value="">Automática (padrão da câmera)</option>';
    (d.list || []).forEach(f => {
      const o = document.createElement('option');
      o.value = [f.w, f.h, f.fpsNum, f.fpsDen].join(',');
      const fps = f.fpsDen ? (f.fpsNum / f.fpsDen) : f.fpsNum;
      o.textContent = f.w + '×' + f.h + ' · ' + fps.toFixed(fps % 1 ? 2 : 0) + ' fps';
      sel.appendChild(o);
    });
    $('#camcfg-note').textContent = (d.list && d.list.length)
      ? 'Formatos informados pela própria câmera.'
      : 'Esta câmera não informa uma lista de formatos; será usado o padrão automático.';
  }
  $('#camcfg-x').addEventListener('click', closeModals);
  $('#camcfg-cancel').addEventListener('click', closeModals);
  $('#camcfg-save').addEventListener('click', () => {
    const v = $('#camcfg-res').value;
    const parts = v ? v.split(',').map(Number) : [0, 0, 0, 0];
    send('devformat', camcfgSlot, parts[0], parts[1], parts[2], parts[3]);
    closeModals();
  });

