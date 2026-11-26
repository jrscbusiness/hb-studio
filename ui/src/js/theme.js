  // ---------- theme (installed theme plugins) ----------
  let themeVars = [];
  function applyTheme(t) {
    const root = document.documentElement;
    themeVars.forEach(k => root.style.removeProperty(k)); themeVars = [];
    root.style.colorScheme = t.scheme === 'dark' || t.scheme === 'light' ? t.scheme : '';
    const colour = /^(#[0-9a-fA-F]{3,8}|rgba?\([0-9 ,.%\/]+\))$/;
    Object.entries(t.vars || {}).forEach(([k, v]) => { if (/^--[a-z0-9-]{1,24}$/.test(k) && colour.test(v)) { root.style.setProperty(k, v); themeVars.push(k); } });
  }

