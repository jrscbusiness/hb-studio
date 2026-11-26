  const onMsg = m => {
    const d = typeof m === 'string' ? JSON.parse(m) : m;
    if (d.t === 'f') drawFrame(d.id, d.d); else if (d.t === 's') { lastState = d; render(d); } else if (d.t === 'ptz') { ptz = d; renderPtz(); } else if (d.t === 'devices') { devs = d.list; renderLists(true); const nn = $('#ndi-note'); if (d.ndiMsg) { nn.textContent = d.ndiMsg; nn.style.display = ''; } else nn.style.display = 'none'; } else if (d.t === 'theme') applyTheme(d); else if (d.t === 'plugins') { plugins = d; renderPlugins(); if (mkAsked) send('mktlist'); } else if (d.t === 'plugin_review') { showReview(d); mktAfterReview(d); } else if (typeof d.t === 'string' && d.t.startsWith('mkt')) mktMsg(d); else if (d.t === 'plugin_result') { toast(d.message); } else if (d.t === 'outputs') renderOutputs(d);
    else if (d.t === 'scenes_state') onScenesState(d);   // FEATURE: scenes
    else if (d.t === 'devformats') renderCamFormats(d);
    else if (d.t === 'chroma') { chrSt = d; syncChroma(false); }   // FEATURE: chroma
    else if (d.t === 'remote') renderRemote(d);   /* FEATURE: remote */
    else if (d.t === 'outfmt') renderOutFmt(d);
    else if (d.t === 'previewq') renderPreviewQ(d.id);
    else if (typeof d.t === 'string' && d.t.startsWith('chat_')) chatMsg(d);   // FEATURE: chat
    else if (d.t === 'overlay_state') ovState(d);   // FEATURE: overlays
    else if (d.t === 'cap') renderCap(d);   // FEATURE: captions
    else if (typeof d.t === 'string' && d.t.startsWith('ai_')) aiMsg(d);   // FEATURE: ai
    else if (d.t === 'xmit_dest') { xmitList = d.list || []; renderXmit(); }
    else if (d.t === 'cloud_state') cbRender(d);   // FEATURE: cloudbackup
    else if (d.t === 'upd_state') updRender(d);   // FEATURE: updates
    else if (d.t === 'checklist') chkRender(d);   // FEATURE: diagnostics
    else if (d.t === 'chkcfg') chkCfg(d);   // FEATURE: diagnostics
    else if (d.t === 'xmit_status') { xmitStatus = {}; (d.list || []).forEach(s => { xmitStatus[s.id] = s; }); renderXmit(); }
    else if (d.t === 'audio') { if (audBusy()) audPending = d; else renderAudio(d); } else if (d.t === 'audiolv') audioLevels(d); else if (d.t === 'recprofiles') renderRecProfiles(d); else if (d.t === 'rec') { recst = d; renderRec(); } else if (d.t === 'rec_iso') renderMultitrack(d);   /* FEATURE: multitrack */
  };
  if (host) { host.addEventListener('message', e => onMsg(e.data)); send('ready'); }
  if (host) send('aiget');   // FEATURE: ai
  window.__hbMessage = onMsg;   // used by the design preview harness
