  // ---------- native dark/light mode (built in; no plugin needed) ----------
  (() => {
    let pref = 'system';
    try { const s = localStorage.getItem('hb.theme'); if (s === 'light' || s === 'dark' || s === 'gray' || s === 'obs' || s === 'system') pref = s; } catch (e) {}
    const apply = p => {
      if (p === 'system') document.documentElement.removeAttribute('data-theme');
      else document.documentElement.setAttribute('data-theme', p);
      $$('#theme-seg button').forEach(b => b.classList.toggle('on', b.dataset.th === p));
    };
    apply(pref);
    $$('#theme-seg button').forEach(b => b.addEventListener('click', () => {
      pref = b.dataset.th;
      apply(pref);
      try { localStorage.setItem('hb.theme', pref); } catch (e) {}
      send('uitheme', pref);   // keeps the native title bar in sync and remembers it for next launch
    }));
  })();

