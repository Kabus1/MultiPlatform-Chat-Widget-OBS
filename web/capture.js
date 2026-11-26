// Video capture relay page: its own settings sidebar (saved to settings.json
// "capture") plus live status and an on-demand preview. Every call returns
// immediately; the app does device work on its own worker thread.
(() => {
  'use strict';
  const $ = (s) => document.querySelector(s);

  async function api(path, opts = {}) {
    const init = { method: opts.method || 'GET', headers: { 'X-USC': '1' } };
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(opts.body);
    }
    const res = await fetch(path, init);
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || res.statusText);
    return data;
  }

  let cfg = {};        // settings.capture
  let st = null;       // last relay status
  let modes = [];      // modes of the device shown in the sidebar
  let modesFor = '';   // device id `modes` belongs to
  let modesError = '';

  const fmtFps = (f) => (Math.abs(f - Math.round(f)) < 0.01 ? String(Math.round(f)) : f.toFixed(2));
  const fmtMode = (m) => `${m.width}×${m.height} @ ${fmtFps(m.fps)} fps · ${m.subtype}` +
    (m.passThrough ? '' : ' (CPU decode)');

  async function save(patch) {
    Object.assign(cfg, patch);
    try {
      const s = await api('/api/settings', { method: 'POST', body: { capture: patch } });
      cfg = s.capture;
    } catch (e) {
      banner('error', 'Could not save: ' + e.message);
    }
  }

  // ------------------------------------------------------------- sidebar
  function renderDevices() {
    const sel = $('#device');
    const devs = (st && st.devices) || [];
    const opts = ['<option value="">— choose a device —</option>'];
    let found = false;
    for (const d of devs) {
      const selected = d.id === cfg.deviceId || (!found && cfg.deviceName && d.name === cfg.deviceName);
      if (selected) found = true;
      opts.push(`<option value="${esc(d.id)}"${selected ? ' selected' : ''}>${esc(d.name)}</option>`);
    }
    if (!found && cfg.deviceId)
      opts.push(`<option value="${esc(cfg.deviceId)}" selected>${esc(cfg.deviceName || 'Saved device')} (not connected)</option>`);
    if (document.activeElement !== sel) sel.innerHTML = opts.join('');

    const p = $('#device-presence');
    if (st && st.enumerating) { p.textContent = 'Scanning…'; p.className = 'hint'; }
    else if (st && st.devicesError) { p.textContent = st.devicesError; p.className = 'hint bad'; }
    else if (!cfg.deviceId) { p.textContent = `${devs.length} device(s) found`; p.className = 'hint'; }
    else if (st && st.devicePresent) { p.textContent = '● Connected, ready'; p.className = 'hint ok'; }
    else { p.textContent = '● Not connected'; p.className = 'hint bad'; }
  }

  function selectedDeviceId() {
    const devs = (st && st.devices) || [];
    const d = devs.find((x) => x.id === cfg.deviceId) || devs.find((x) => cfg.deviceName && x.name === cfg.deviceName);
    return d ? d.id : '';
  }

  let probeTimer = null;
  async function loadModes(force) {
    const id = selectedDeviceId();
    if (!id) { modes = []; modesFor = ''; modesError = ''; renderModes(); return; }
    if (!force && id === modesFor && (modes.length || modesError)) return;
    clearTimeout(probeTimer);
    try {
      const r = await api('/api/capture/modes?device=' + encodeURIComponent(id));
      if (r.probing) {
        modesFor = id; modes = []; modesError = '';
        renderModes('Reading the modes the device supports…');
        probeTimer = setTimeout(() => loadModes(true), 700);
        return;
      }
      modesFor = id; modes = r.modes || []; modesError = r.error || '';
    } catch (e) {
      modesFor = id; modes = []; modesError = e.message;
    }
    renderModes();
  }

  function renderModes(busy) {
    const sel = $('#mode');
    const opts = [`<option value="auto"${cfg.mode === 'auto' ? ' selected' : ''}>Auto (best match for the output settings)</option>`];
    let have = cfg.mode === 'auto';
    for (const m of modes) {
      const s = m.key === cfg.mode;
      if (s) have = true;
      opts.push(`<option value="${esc(m.key)}"${s ? ' selected' : ''}>${esc(fmtMode(m))}</option>`);
    }
    if (!have) opts.push(`<option value="${esc(cfg.mode)}" selected>${esc(cfg.mode)} (not offered now)</option>`);
    if (document.activeElement !== sel) sel.innerHTML = opts.join('');
    const h = $('#mode-hint');
    if (busy) { h.textContent = busy; h.className = 'hint'; }
    else if (modesError) { h.textContent = modesError; h.className = 'hint bad'; }
    else if (modes.length) {
      const maxFps = Math.max(...modes.map((m) => m.fps));
      const maxH = Math.max(...modes.map((m) => m.height));
      h.textContent = `${modes.length} modes · up to ${maxH}p and ${fmtFps(maxFps)} fps. Only these are used - nothing unsupported is forced.`;
      h.className = 'hint';
    } else {
      h.textContent = 'Only modes the device reports are listed - nothing unsupported is forced.';
      h.className = 'hint';
    }
    renderMismatch();
  }

  function renderSidebar() {
    renderDevices();
    if (document.activeElement !== $('#out-res')) $('#out-res').value = cfg.outputResolution || 'source';
    if (document.activeElement !== $('#out-fps')) $('#out-fps').value = String(cfg.outputFps || 0);
    $('#keep-aspect').checked = cfg.keepAspect !== false;
    $('#auto-start').checked = cfg.autoStart === true;
    if (document.activeElement !== $('#title')) $('#title').value = cfg.windowTitle || 'USC Capture Output';
    $('#title-code').textContent = `[UnifiedStreamChat.exe]: ${cfg.windowTitle || 'USC Capture Output'}`;
    loadModes(false);
    renderModes();
  }

  // Hints where the selection asks for more than the device provides.
  function renderMismatch() {
    if (!st || !st.selectedMode) return;
    const m = st.selectedMode;
    const notes = [];
    const cap = Number(cfg.outputFps || 0);
    if (cap > 0 && cap > m.fps + 0.5)
      notes.push(`The device sends ${fmtFps(m.fps)} fps in this mode, so the ${cap} fps cap has no effect.`);
    const targetH = { '2160p': 2160, '1440p': 1440, '1080p': 1080, '720p': 720 }[cfg.outputResolution];
    if (targetH && targetH > m.height)
      notes.push(`The source is ${m.height}p; the ${targetH}p output is upscaled on the GPU (no extra detail).`);
    if (st.note) notes.unshift(st.note);
    if (notes.length && st.state === 'running') banner('info', notes.join(' '));
  }

  // ------------------------------------------------------------- status
  function banner(kind, text) {
    const b = $('#banner');
    if (!text) { b.hidden = true; return; }
    b.hidden = false;
    b.className = 'banner ' + kind;
    b.textContent = text;
  }

  const STATE_LABEL = { off: 'Off', starting: 'Starting…', running: 'Running', 'no-signal': 'No signal', error: 'Error' };

  function renderStatus() {
    if (!st) return;
    const pill = $('#state-pill');
    pill.textContent = STATE_LABEL[st.state] || st.state;
    pill.className = 'pill ' + st.state;
    $('#start').hidden = st.enabled;
    $('#stop').hidden = !st.enabled;
    $('#start').disabled = !st.supported;

    if (!st.supported) banner('error', st.devicesError || 'The capture relay needs Windows 10/11 (Media Foundation + Direct3D 11).');
    else if (st.state === 'error') banner('error', st.detail);
    else if (st.window && st.window.error) banner('error', 'Output window: ' + st.window.error);
    else if (st.state === 'no-signal') banner('warn', st.detail);
    else if (st.state === 'running' && st.window && st.window.minimized)
      banner('warn', 'The output window is minimized - OBS Game Capture shows nothing while it is. Restore it (it can stay behind other windows).');
    else if (st.state === 'starting') banner('info', st.detail || 'Starting…');
    else if (st.state === 'off' && st.detail) banner('info', st.detail);
    else banner('', '');
    renderMismatch();

    const m = st.selectedMode;
    const meas = st.measured || {};
    const live = st.state === 'running' || st.state === 'no-signal';
    $('#m-selected').textContent = m ? fmtMode(m) : '–';
    $('#m-delivered').textContent = st.deliveredFormat
      ? st.deliveredFormat + (m && m.subtype !== st.deliveredFormat ? ` (converted from ${m.subtype})` : ' (direct, no conversion)')
      : '–';
    $('#m-capfps').textContent = live ? fmtFps(meas.captureFps || 0) : '–';
    const o = st.output || {};
    $('#m-output').textContent = o.width ? `${o.width}×${o.height}` : '–';
    $('#m-cap').textContent = live ? (o.fpsCap ? `${fmtFps(o.fpsCap)} fps` : 'none (source rate)') : '–';
    $('#m-outfps').textContent = live ? fmtFps(meas.outputFps || 0) : '–';
    const w = st.window || {};
    $('#w-state').textContent = !w.open ? 'closed' : w.minimized ? 'minimized ⚠' : w.visible ? 'open, capturable' : 'hidden ⚠';
    $('#w-frames').textContent = live ? `${meas.presented || 0} shown / ${meas.frames || 0} received` : '–';
    $('#w-dropped').textContent = live
      ? `${meas.failed || 0} failed · ${meas.skipped || 0} replaced by a newer frame · ${meas.paced || 0} by FPS cap`
      : '–';
    renderDevices();
    updatePreviewLoop();
    updatePreviewText();
  }

  // ------------------------------------------------------------- preview
  // Polled only while the page is visible, the box is ticked and frames flow;
  // the app stops making previews 3 s after the last request.
  let previewTimer = null;
  let previewUrl = '';
  function previewWanted() {
    return $('#preview-on').checked && document.visibilityState === 'visible' && st &&
      (st.state === 'running' || st.state === 'no-signal');
  }
  function updatePreviewLoop() {
    if (previewWanted()) {
      if (!previewTimer) previewTimer = setTimeout(pullPreview, 0);
    } else {
      clearTimeout(previewTimer);
      previewTimer = null;
      if (!st || st.state === 'off' || st.state === 'error') showPreview('');
    }
  }
  async function pullPreview() {
    try {
      const res = await fetch('/api/capture/preview.jpg', { cache: 'no-store' });
      if (res.status === 200) showPreview(URL.createObjectURL(await res.blob()));
    } catch (_) { /* app restarting */ }
    previewTimer = previewWanted() ? setTimeout(pullPreview, 250) : null;
  }
  function showPreview(url) {
    if (previewUrl) URL.revokeObjectURL(previewUrl);
    previewUrl = url;
    $('#preview').src = url || '';
    $('#preview').hidden = !url;
    $('#preview-empty').hidden = !!url;
    updatePreviewText();
  }
  // Kept in sync with every status update (it used to stay at "relay is off").
  function updatePreviewText() {
    const el = $('#preview-empty');
    const s = st ? st.state : 'off';
    const shown = st && st.measured ? st.measured.presented || 0 : 0;
    el.textContent =
      s === 'off' ? 'No picture (relay is off)'
      : s === 'error' ? 'No picture (see the error above)'
      : s === 'starting' ? 'Starting…'
      : s === 'no-signal' ? 'No signal from the device'
      : st.window && st.window.error ? 'Output problem (see above)'
      : !$('#preview-on').checked ? 'Preview is off'
      : shown === 0 ? 'Waiting for the first frame…'
      : 'Loading preview…';
  }

  // ------------------------------------------------------------- wiring
  function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  $('#device').addEventListener('change', (e) => {
    const id = e.target.value;
    const d = ((st && st.devices) || []).find((x) => x.id === id);
    save({ deviceId: id, deviceName: d ? d.name : '', mode: 'auto' }).then(() => { modesFor = ''; renderSidebar(); });
  });
  $('#refresh').addEventListener('click', () => {
    modesFor = '';
    api('/api/capture/devices/refresh', { method: 'POST' }).catch(() => {});
  });
  $('#mode').addEventListener('change', (e) => save({ mode: e.target.value }));
  $('#out-res').addEventListener('change', (e) => save({ outputResolution: e.target.value }));
  $('#out-fps').addEventListener('change', (e) => save({ outputFps: Number(e.target.value) }));
  $('#keep-aspect').addEventListener('change', (e) => save({ keepAspect: e.target.checked }));
  $('#auto-start').addEventListener('change', (e) => save({ autoStart: e.target.checked }));
  $('#title').addEventListener('change', (e) => {
    const t = e.target.value.trim() || 'USC Capture Output';
    save({ windowTitle: t }).then(renderSidebar);
  });
  $('#start').addEventListener('click', () => api('/api/capture/start', { method: 'POST' }).then((c) => { cfg = c; }).catch((e) => banner('error', e.message)));
  $('#stop').addEventListener('click', () => api('/api/capture/stop', { method: 'POST' }).then((c) => { cfg = c; }).catch((e) => banner('error', e.message)));
  $('#preview-on').addEventListener('change', updatePreviewLoop);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') api('/api/capture/devices/refresh', { method: 'POST' }).catch(() => {});
    updatePreviewLoop();
  });

  // ------------------------------------------------------------- TikTok feed (OBS)
  let feed = null;
  function renderFeed() {
    if (!feed) return;
    const pill = $('#feed-pill');
    const ready = feed.obsConnected && feed.sceneReady;
    pill.textContent = !feed.obsConnected ? 'OBS not connected' : feed.virtualCamActive ? 'Virtual Camera on' : ready ? 'Ready' : 'Not set up';
    pill.className = 'pill ' + (!feed.obsConnected ? 'error' : feed.virtualCamActive ? 'ok' : ready ? 'running' : 'warn');
    $('#f-obs').textContent = feed.obsConnected ? 'connected' : 'not connected - start OBS and enable its WebSocket server';
    const sel = $('#f-source');
    const opts = ['<option value="">Automatic</option>'];
    for (const i of feed.inputs || [])
      opts.push(`<option value="${esc(i.name)}"${i.name === feed.sourcePreferred ? ' selected' : ''}>${esc(i.name)}</option>`);
    if (document.activeElement !== sel) sel.innerHTML = opts.join('');
    $('#f-device').textContent = feed.source
      ? `using "${feed.source}"${feed.sourceDevice ? ' (' + feed.sourceDevice + ')' : ''}`
      : feed.obsConnected ? 'no Video Capture Device source in OBS yet' : '';
    $('#f-scene').textContent = feed.sceneReady ? `"${feed.sceneName}" ready` : feed.enabled ? 'not ready (see message)' : 'not created yet';
    $('#f-vcam').textContent = feed.virtualCamActive ? 'running - pick "OBS Virtual Camera" in TikTok Studio' : 'stopped';
    $('#f-vcam-start').hidden = !!feed.virtualCamActive;
    $('#f-vcam-stop').hidden = !feed.virtualCamActive;
    $('#f-vcam-start').disabled = $('#f-setup').disabled = !feed.obsConnected;
    $('#f-auto').checked = !!feed.autoStartVirtualCam;
    $('#f-scene-name').textContent = $('#f-scene-name2').textContent = feed.sceneName || 'USC TikTok Feed';
    const err = $('#f-error');
    err.hidden = !feed.error;
    err.textContent = feed.error || '';
  }
  $('#f-setup').addEventListener('click', () => api('/api/camfeed/setup', { method: 'POST' }).catch((e) => banner('error', e.message)));
  $('#f-vcam-start').addEventListener('click', () => api('/api/camfeed/virtualcam?on=1', { method: 'POST' }).catch(() => {}));
  $('#f-vcam-stop').addEventListener('click', () => api('/api/camfeed/virtualcam?on=0', { method: 'POST' }).catch(() => {}));
  $('#f-auto').addEventListener('change', (e) =>
    api('/api/settings', { method: 'POST', body: { camFeed: { autoStartVirtualCam: e.target.checked } } }).catch(() => {}));
  $('#f-source').addEventListener('change', (e) =>
    api('/api/settings', { method: 'POST', body: { camFeed: { sourceName: e.target.value } } }).catch(() => {}));

  async function init() {
    try {
      const s = await api('/api/settings');
      cfg = s.capture || {};
      if (s.ui && s.ui.theme && s.ui.theme !== 'transparent') document.documentElement.dataset.theme = s.ui.theme;
      st = await api('/api/capture/status');
      feed = await api('/api/camfeed/status');
    } catch (e) {
      banner('error', 'The app is not reachable: ' + e.message);
    }
    renderSidebar();
    renderStatus();
    renderFeed();
    showPreview('');

    const es = new EventSource('/api/events');
    es.addEventListener('capture', (e) => {
      const prevDevices = JSON.stringify((st && st.devices) || []);
      st = JSON.parse(e.data);
      if (JSON.stringify(st.devices || []) !== prevDevices) loadModes(true);
      renderStatus();
    });
    es.addEventListener('camfeed', (e) => {
      feed = JSON.parse(e.data);
      renderFeed();
    });
    es.addEventListener('settings', (e) => {
      const c = JSON.parse(e.data).capture;
      if (c && JSON.stringify(c) !== JSON.stringify(cfg)) { cfg = c; renderSidebar(); }
    });
  }
  init();
})();
