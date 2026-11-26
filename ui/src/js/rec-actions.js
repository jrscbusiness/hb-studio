  const recToggle = () => send('rec', (recst.state === 'recording') ? 'stop' : 'start');
  $('#rec-btn').onclick = $('#rec-btn2').onclick = recToggle;
  $('#rc-pick').onclick = () => send('rec', 'pickfolder');
  for (const k of ['template', 'videoKbps', 'videoQuality', 'keyframeSec', 'audioKbps', 'audioRate', 'splitMin', 'splitMB', 'minFreeMB'])
    $('#rc-' + k).addEventListener('change', ev => send('rec', 'cfg', k, ev.target.value));

