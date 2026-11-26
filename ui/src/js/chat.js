  // ---------- FEATURE: chat ---------- unified live chat (YouTube / Facebook / Instagram / Twitch), docs/CHAT.md
  const chatPlats = [
    {p: 'youtube', name: 'YouTube'}, {p: 'facebook', name: 'Facebook'},
    {p: 'instagram', name: 'Instagram'}, {p: 'twitch', name: 'Twitch'},
  ];
  const chatStateLabel = {connected: 'CONECTADO', connecting: 'CONECTANDO', backoff: 'RECONECTANDO', needs_auth: 'PRECISA AUTORIZAR',
    failed: 'ERRO', ended: 'ENCERRADO', unavailable: 'LIMITADO / INDISPONÍVEL', disconnected: 'DESCONECTADO'};
  const chat = {msgs: [], max: 500, off: {}, hideCmd: false, hideBot: false, q: '', paused: false, compact: false, lastBatch: 0,
    muted: [], pinned: 0, status: null, cfg: null, open: {}};
  const chatFeed = $('#chat-feed');
  const chatPass = m => !chat.off[m.p] && !(chat.hideCmd && m.cmd) && !(chat.hideBot && m.bot) && !chat.muted.includes(m.uk) &&
    (!chat.q || (m.a + ' ' + m.x).toLowerCase().includes(chat.q));
  const chatTime = ts => { const d = new Date(ts); return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0'); };
  // Message text is only ever assigned through textContent: chat content is hostile input.
  function chatNode(m) {
    const row = document.createElement('div'); row.className = 'cm' + (chat.pinned === m.n ? ' pinned' : ''); row.dataset.n = m.n;
    const t = document.createElement('span'); t.className = 'ct'; t.textContent = chatTime(m.ts);
    const c = document.createElement('span'); c.className = 'cchip p-' + m.p; c.textContent = (chatPlats.find(x => x.p === m.p) || {name: m.p}).name.toUpperCase();
    const a = document.createElement('span'); a.className = 'ca'; a.textContent = m.a;
    row.append(t, c);
    if (m.b & 3) { const b = document.createElement('span'); b.className = 'cb'; b.textContent = (m.b & 2) ? 'DONO' : 'MOD'; row.append(b); }
    const x = document.createElement('span'); x.className = 'cx'; x.textContent = m.x;
    const mu = document.createElement('span'); mu.className = 'cmute'; mu.textContent = '⊘'; mu.title = 'Silenciar este usuário (só aqui)';
    mu.dataset.mute = m.n;
    row.append(a, x, mu);
    return row;
  }
  const chatNearBottom = () => chatFeed.scrollHeight - chatFeed.scrollTop - chatFeed.clientHeight < 48;
  const chatCount = () => { $('#chat-count').textContent = chat.msgs.length + (chat.msgs.length === 1 ? ' mensagem' : ' mensagens'); };
  function chatRenderAll() {
    chatFeed.textContent = '';
    const frag = document.createDocumentFragment();
    chat.msgs.filter(chatPass).slice(-300).forEach(m => frag.appendChild(chatNode(m)));
    chatFeed.appendChild(frag);
    chatFeed.scrollTop = chatFeed.scrollHeight;
    chatCount();
  }
  function chatAppend(list) {
    const stick = !chat.paused && chatNearBottom();
    list.filter(chatPass).forEach(m => chatFeed.appendChild(chatNode(m)));
    while (chatFeed.childElementCount > 300) chatFeed.removeChild(chatFeed.firstChild);
    if (stick) chatFeed.scrollTop = chatFeed.scrollHeight;
    chatCount();
  }
  let chatResync = false;
  function chatBatch(d) {
    const gap = !d.reset && chat.lastBatch && d.seq !== chat.lastBatch + 1;
    chat.lastBatch = d.seq;
    if (d.reset) chat.msgs = [];
    const list = d.msgs || [];
    chat.msgs = chat.msgs.concat(list).slice(-chat.max);
    if (d.reset) chatRenderAll(); else chatAppend(list);
    if (gap && !chatResync) { chatResync = true; send('chatget'); setTimeout(() => { chatResync = false; }, 2000); }   // missed a batch: ask for the history again
  }
  function chatRenderPlats() {
    const box = $('#chat-plats');
    if (box.contains(document.activeElement) && document.activeElement.matches('input[type=text],input[type=password]')) return;   // never rebuild a form being typed in
    box.textContent = '';
    const st = chat.status || {platforms: []}, cfg = chat.cfg || {};
    chatPlats.forEach(pl => {
      const s = (st.platforms || []).find(x => x.p === pl.p) || {state: 'disconnected', detail: '', error: ''};
      const card = document.createElement('div'); card.className = 'chatplat' + (chat.open[pl.p] ? ' open' : ''); card.dataset.p = pl.p;
      const hd = document.createElement('div'); hd.className = 'hd';
      const chip = document.createElement('span'); chip.className = 'cchip p-' + pl.p; chip.textContent = pl.name.toUpperCase();
      const dot = document.createElement('span'); dot.className = 'dot s-' + s.state;
      const lab = document.createElement('span'); lab.className = 'st'; lab.textContent = chatStateLabel[s.state] || s.state;
      hd.append(chip, dot, lab); card.appendChild(hd);
      const dt = document.createElement('div'); dt.className = 'dt'; dt.textContent = s.detail || ''; card.appendChild(dt);
      if (s.error && s.state !== 'connected') { const er = document.createElement('div'); er.className = 'er'; er.textContent = s.error; card.appendChild(er); }
      if (pl.p === 'youtube' && s.quota) { const q = document.createElement('div'); q.className = 'dt'; q.textContent = '≈ ' + s.quota + ' unidades de cota usadas nesta sessão (limite padrão: 10.000/dia)'; card.appendChild(q); }
      const row = document.createElement('div'); row.className = 'row';
      const mk = (txt, act, cls) => { const b = document.createElement('button'); b.className = 'btn sm' + (cls ? ' ' + cls : ''); b.textContent = txt; b.dataset.act = act; row.appendChild(b); return b; };
      mk(s.want ? 'DESCONECTAR' : 'CONECTAR', s.want ? 'disconnect' : 'connect', s.want ? 'on' : '');
      if (pl.p !== 'instagram') mk(chat.open[pl.p] ? 'FECHAR' : 'CONFIGURAR', 'cfg');
      card.appendChild(row);
      const f = document.createElement('div'); f.className = 'cfg';
      const field = (id, label, type, val, ph) => { const l = document.createElement('label'); l.textContent = label; const i = document.createElement('input'); i.type = type; i.id = id; i.value = val || ''; i.placeholder = ph || ''; i.autocomplete = 'off'; i.maxLength = 300; l.appendChild(i); f.appendChild(l); };
      const note = txt => { const n = document.createElement('div'); n.className = 'dt'; n.textContent = txt; f.appendChild(n); };
      if (pl.p === 'youtube') {
        const c = cfg.youtube || {};
        field('chat-yt-video', 'ID ou link do vídeo ao vivo', 'text', c.videoId, 'ex.: dQw4w9WgXcQ');
        field('chat-yt-cid', 'Client ID do Google (tipo “App para computador”)', 'text', c.clientId, 'xxxx.apps.googleusercontent.com');
        field('chat-yt-sec', 'Client secret (opcional)', 'password', '', c.hasSecret ? c.secretHint + ' (salvo)' : '');
        note(c.authorized ? 'Conta Google autorizada.' : 'Conta Google ainda não autorizada.');
        if (st.auth && st.auth.message) note(st.auth.message);
      } else if (pl.p === 'facebook') {
        const c = cfg.facebook || {};
        field('chat-fb-video', 'ID do vídeo ao vivo', 'text', c.liveVideoId, 'ex.: 1234567890');
        field('chat-fb-tok', 'Token de acesso da Página', 'password', '', c.hasToken ? c.tokenHint + ' (salvo)' : '');
      } else if (pl.p === 'twitch') {
        field('chat-tw-ch', 'Canal', 'text', (cfg.twitch || {}).channel, 'nome do canal');
        note('Leitura anônima: não precisa de login nem token.');
      }
      if (pl.p !== 'instagram') {
        const r2 = document.createElement('div'); r2.className = 'row';
        const add = (txt, act, cls) => { const b = document.createElement('button'); b.className = 'btn sm' + (cls ? ' ' + cls : ''); b.textContent = txt; b.dataset.act = act; r2.appendChild(b); };
        add('SALVAR', 'save', 'primary');
        if (pl.p === 'youtube') add('AUTORIZAR', 'auth');
        if (pl.p !== 'twitch') add('REMOVER CREDENCIAIS', 'clear');
        f.appendChild(r2);
      }
      card.appendChild(f);
      box.appendChild(card);
    });
    const extra = document.createElement('div');
    const mh = document.createElement('div'); mh.className = 'dt'; mh.textContent = chat.muted.length ? 'Silenciados (clique para liberar):' : 'Nenhum usuário silenciado.';
    extra.appendChild(mh);
    const mm = document.createElement('div'); mm.className = 'chat-muted';
    chat.muted.forEach(k => { const b = document.createElement('button'); b.textContent = k; b.dataset.unmute = k; mm.appendChild(b); });
    extra.appendChild(mm);
    const pl2 = document.createElement('label'); pl2.className = 'dt'; pl2.style.display = 'flex'; pl2.style.gap = '6px'; pl2.style.marginTop = '8px';
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.id = 'chat-persist'; cb.checked = !!cfg.persist;
    pl2.append(cb, document.createTextNode('Salvar mensagens em arquivo (chat_log.jsonl). Desligado por padrão, por privacidade.'));
    extra.appendChild(pl2);
    if (cfg.net === false) { const n = document.createElement('div'); n.className = 'er'; n.textContent = 'Rede indisponível nesta plataforma.'; extra.appendChild(n); }
    box.appendChild(extra);
  }
  $('#chat-plats').addEventListener('click', ev => {
    const un = ev.target.closest('[data-unmute]'); if (un) { send('chatunmute', un.dataset.unmute); return; }
    const b = ev.target.closest('[data-act]'); if (!b) return;
    const p = b.closest('.chatplat').dataset.p, v = id => ($('#' + id) || {value: ''}).value.trim();
    const act = b.dataset.act;
    if (act === 'connect') send('chatconnect', p);
    else if (act === 'disconnect') send('chatdisconnect', p);
    else if (act === 'cfg') { chat.open[p] = !chat.open[p]; chatRenderPlats(); }
    else if (act === 'auth') send('chatauth', 'youtube');
    else if (act === 'clear') { if (confirm('Remover as credenciais salvas de ' + p + '?')) send('chatclear', p); }
    else if (act === 'save') {
      const payload = p === 'youtube' ? {videoId: v('chat-yt-video'), clientId: v('chat-yt-cid'), clientSecret: v('chat-yt-sec')}
        : p === 'facebook' ? {liveVideoId: v('chat-fb-video'), token: v('chat-fb-tok')} : {channel: v('chat-tw-ch')};
      send('chatsave', p, JSON.stringify(payload));
    }
  });
  $('#chat-plats').addEventListener('change', ev => { if (ev.target.id === 'chat-persist') send('chatopt', 'persist', ev.target.checked ? 1 : 0); });
  chatFeed.addEventListener('click', ev => {
    const mu = ev.target.closest('[data-mute]');
    if (mu) { ev.stopPropagation(); send('chatmute', mu.dataset.mute); return; }
    const row = ev.target.closest('.cm'); if (!row) return;
    chat.pinned = +row.dataset.n;
    chatFeed.querySelectorAll('.cm.pinned').forEach(e => e.classList.remove('pinned'));
    row.classList.add('pinned');
    send('chatpin', row.dataset.n);
  });
  $('#chat-unpin').addEventListener('click', () => { chat.pinned = 0; chatFeed.querySelectorAll('.cm.pinned').forEach(e => e.classList.remove('pinned')); send('chatunpin'); });
  $('#chat-clear').addEventListener('click', () => { chat.msgs = []; chatRenderAll(); send('chatclearfeed'); });
  $('#chat-pause').addEventListener('click', () => {
    chat.paused = !chat.paused; $('#chat-pause').classList.toggle('on', chat.paused);
    $('#chat-pause').textContent = chat.paused ? 'RETOMAR ROLAGEM' : 'PAUSAR ROLAGEM';
    if (!chat.paused) chatFeed.scrollTop = chatFeed.scrollHeight;
  });
  $('#chat-compact').addEventListener('click', () => { chat.compact = !chat.compact; $('#chat-compact').classList.toggle('on', chat.compact); $('#chat-wrap').classList.toggle('compact', chat.compact); });
  $$('#chat-filters .cchip').forEach(b => b.addEventListener('click', () => { chat.off[b.dataset.p] = !chat.off[b.dataset.p]; b.classList.toggle('off', !!chat.off[b.dataset.p]); chatRenderAll(); }));
  $('#chat-hidecmd').addEventListener('change', ev => { chat.hideCmd = ev.target.checked; chatRenderAll(); });
  $('#chat-hidebot').addEventListener('change', ev => { chat.hideBot = ev.target.checked; chatRenderAll(); });
  $('#chat-search').addEventListener('input', ev => { chat.q = ev.target.value.trim().toLowerCase(); chatRenderAll(); });
  function chatMsg(d) {
    if (d.t === 'chat_msgs') chatBatch(d);
    else if (d.t === 'chat_status') { chat.status = d; const before = chat.muted.join('|'); chat.muted = d.muted || []; chatRenderPlats(); if (before !== chat.muted.join('|')) chatRenderAll(); }
    else if (d.t === 'chat_settings') { chat.cfg = d; chatRenderPlats(); }
  }
  chatRenderPlats();   // shows the four platform cards immediately, before any chat_status arrives
  // ---------- /FEATURE: chat ----------

