  // ---- FEATURE: ai ----
  // CONFIG > Inteligência Artificial. The key is write-only: the host only ever sends keySet/keyHint.
  const AI_DEFAULT_URL = { openrouter: 'https://openrouter.ai/api/v1', '9router': 'http://localhost:20128/v1', custom: '' };
  let aiSt = { list: [], active: '', configured: false }, aiModels = [], aiCur = '';
  const aiRes = (id, text, ok) => { const e = $(id); e.textContent = text || ''; e.classList.toggle('ok', !!ok); };
  const aiDraft = () => JSON.stringify({ id: aiCur, kind: $('#ai-kind').value, baseUrl: $('#ai-url').value.trim(), model: $('#ai-model').value.trim(), apiKey: $('#ai-key').value });
  function aiFreeBadge() { const m = aiModels.find(x => x.id === $('#ai-model').value.trim()); $('#ai-free').classList.toggle('on', !!(m && m.free)); }
  function aiFill(id) {
    aiCur = id || '';
    const p = aiSt.list.find(x => x.id === aiCur);
    $('#ai-kind').value = p ? p.kind : 'openrouter';
    $('#ai-url').value = p ? p.baseUrl : AI_DEFAULT_URL[$('#ai-kind').value];
    $('#ai-model').value = p ? p.model : '';
    $('#ai-key').value = '';
    $('#ai-key').placeholder = p && p.keySet ? 'chave salva ' + p.keyHint + ' — deixe em branco para manter' : 'cole sua chave aqui';
    $('#ai-act-btn').disabled = !p || p.id === aiSt.active; $('#ai-del-btn').disabled = !p;
    $('#ai-sel').value = aiCur; aiFreeBadge();
  }
  function aiRenderModels() {
    const only = $('#ai-onlyfree').checked, dl = $('#ai-models-dl'); dl.innerHTML = '';
    aiModels.filter(m => !only || m.free).slice(0, 600).forEach(m => { const o = document.createElement('option'); o.value = m.id; o.label = (m.free ? '[GRÁTIS] ' : '') + (m.name || m.id); dl.appendChild(o); });
  }
  function aiRenderState(d) {
    aiSt = d; const sel = $('#ai-sel'); sel.innerHTML = '';
    const n = document.createElement('option'); n.value = ''; n.textContent = '+ Novo provedor'; sel.appendChild(n);
    d.list.forEach(p => { const o = document.createElement('option'); o.value = p.id; o.textContent = p.name + ' · ' + p.kind + (p.id === d.active ? ' (ativo)' : ''); sel.appendChild(o); });
    const want = d.savedId || (d.list.some(p => p.id === aiCur) ? aiCur : d.active);
    aiFill(want || '');
    const a = d.list.find(p => p.id === d.active);
    $('#ai-active-note').textContent = a ? (d.configured ? 'Ativo: ' + a.name + ' / ' + a.model : 'Ativo: ' + a.name + ' — falta escolher o modelo' + (a.keySet ? '.' : ' e colar a chave.')) : 'Nenhum provedor ativo.';
    if (d.keysLost) aiRes('#ai-res', 'Não foi possível ler a chave salva (outro usuário/perfil do Windows?). Cole a chave de novo.', false);
    else if (d.canPersistKeys === false) aiRes('#ai-res', 'Este sistema não oferece armazenamento criptografado: a chave vale só até fechar o programa.', false);
  }
  $('#ai-sel').onchange = e => { aiModels = []; aiRenderModels(); aiFill(e.target.value); };
  $('#ai-kind').onchange = () => { $('#ai-url').value = AI_DEFAULT_URL[$('#ai-kind').value]; };
  $('#ai-model').addEventListener('input', aiFreeBadge);
  $('#ai-onlyfree').onchange = aiRenderModels;
  $('#ai-models-btn').onclick = () => { aiRes('#ai-res', 'Buscando modelos…'); send('aimodels', aiDraft()); };
  $('#ai-test-btn').onclick = () => { aiRes('#ai-res', 'Testando…'); send('aitest', aiDraft()); };
  $('#ai-save-btn').onclick = () => send('aisave', aiDraft());
  $('#ai-act-btn').onclick = () => { if (aiCur) send('aiactivate', aiCur); };
  $('#ai-del-btn').onclick = () => { if (aiCur && confirm('Remover este provedor e apagar a chave salva?')) send('airemove', aiCur); };
  $('#ai-sug-btn').onclick = () => { aiRes('#ai-sug-res', 'Pensando…'); send('aisuggest', JSON.stringify({ topic: $('#ai-topic').value.trim() })); };
  const aiCopy = id => { const v = $(id).value; if (!v) return; try { navigator.clipboard.writeText(v); toast('Copiado.'); } catch (e) { $(id).select(); } };
  $('#ai-copy-title').onclick = () => aiCopy('#ai-sug-title'); $('#ai-copy-desc').onclick = () => aiCopy('#ai-sug-desc');
  function aiMsg(d) {
    if (d.t === 'ai_state') aiRenderState(d);
    else if (d.t === 'ai_test') aiRes('#ai-res', (d.ok ? '✔ ' : '✖ ') + d.message + (d.ok ? ' (' + d.latencyMs + ' ms)' : ''), d.ok);
    else if (d.t === 'ai_models') {
      if (d.ok) { aiModels = d.models || []; aiRenderModels(); aiFreeBadge(); const f = aiModels.filter(m => m.free).length; aiRes('#ai-res', aiModels.length + ' modelos' + (f ? ' (' + f + ' grátis)' : '') + '. Comece a digitar no campo Modelo para escolher.', true); }
      else aiRes('#ai-res', '✖ ' + d.error, false);
    } else if (d.t === 'ai_suggest') {
      if (d.ok) { $('#ai-sug-title').value = d.title; $('#ai-sug-desc').value = d.description; aiRes('#ai-sug-res', 'Pronto. Revise antes de usar.', true); }
      else aiRes('#ai-sug-res', '✖ ' + d.error, false);
    }
  }
  aiFill('');
  // ---- /FEATURE ----
