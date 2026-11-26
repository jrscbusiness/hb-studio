  // ---------- PRODUÇÃO: right-click menu on an input tile ----------
  let tmSlot = -1;
  const tileMenu = $('#tilemenu');
  const closeTileMenu = () => tileMenu.classList.remove('on');
  function openTileMenu(e, i) {
    tmSlot = i;
    const x = slots[i] || {k: 'test'};
    const isDev = x.k === 'dev', isOff = x.en === false;
    $('#tm-activate').disabled = !isDev || !isOff;
    $('#tm-deactivate').disabled = !isDev || isOff;
    $('#tm-cfg').disabled = !isDev;
    const vw = innerWidth, vh = innerHeight;
    tileMenu.classList.add('on');   // measure after it's visible, so offsetWidth/Height are real
    tileMenu.style.left = Math.min(e.clientX, vw - tileMenu.offsetWidth - 8) + 'px';
    tileMenu.style.top = Math.min(e.clientY, vh - tileMenu.offsetHeight - 8) + 'px';
  }
  $('#tm-activate').addEventListener('click', () => { send('devenable', tmSlot); closeTileMenu(); });
  $('#tm-deactivate').addEventListener('click', () => { send('devdisable', tmSlot); closeTileMenu(); });
  $('#tm-cfg').addEventListener('click', () => { closeTileMenu(); if (slots[tmSlot]) openCamSettings(tmSlot, slots[tmSlot]); });
  document.addEventListener('pointerdown', e => { if (!e.target.closest('#tilemenu')) closeTileMenu(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') closeTileMenu(); });
  addEventListener('blur', closeTileMenu);


