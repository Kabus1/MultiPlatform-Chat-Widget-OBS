/* Unified Stream Chat - dock / overlay front-end.
 * Talks to the local C++ backend: GET/POST /api/settings, SSE /api/events,
 * GET /api/tts for Google speech audio. No external dependencies. */
(() => {
  'use strict';

  const params = new URLSearchParams(location.search);
  const isOverlay = location.pathname.startsWith('/overlay') || params.get('overlay') === '1';
  // The dock speaks by default; an overlay only when ?tts=1 (avoids double audio).
  const speakHere = params.has('tts') ? params.get('tts') === '1' : !isOverlay;

  const LOGOS = {
    twitch: '<svg viewBox="0 0 24 24" aria-label="Twitch"><path fill="currentColor" d="M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0 1.714 4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 11.143-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714Z"/></svg>',
    youtube: '<svg viewBox="0 0 24 24" aria-label="YouTube"><path fill="currentColor" d="M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432L15.818 12z"/></svg>',
    kick: '<svg viewBox="0 0 24 24" aria-label="Kick"><path fill="currentColor" d="M1.333 0h8v5.333H12V2.667h2.667V0h8v8H20v2.667h-2.667v2.666H20V16h2.667v8h-8v-2.667H12v-2.666H9.333V24h-8Z"/></svg>',
  };
  const PLATFORMS = ['twitch', 'youtube', 'kick'];
  const LANGS = [
    ['ar', 'Arabic - العربية'], ['en', 'English'], ['fr', 'French'], ['es', 'Spanish'], ['de', 'German'],
    ['tr', 'Turkish'], ['fa', 'Persian'], ['ur', 'Urdu'], ['hi', 'Hindi'], ['ru', 'Russian'], ['pt', 'Portuguese'],
    ['it', 'Italian'], ['id', 'Indonesian'], ['ja', 'Japanese'], ['ko', 'Korean'], ['zh-CN', 'Chinese (Simplified)'],
  ];

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const chatEl = $('#chat');

  let settings = null;
  let status = { platforms: {} };

  // ------------------------------------------------------------------ api
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

  // ------------------------------------------------------------------ helpers
  const get = (obj, path) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
  function patchFor(path, value) {
    const keys = path.split('.');
    const root = {};
    let o = root;
    keys.slice(0, -1).forEach((k) => { o = o[k] = {}; });
    o[keys[keys.length - 1]] = value;
    return root;
  }
  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  // Direction of the first strong character (Arabic/Hebrew/Persian => RTL).
  const isRtl = (text) => {
    const m = /[A-Za-z\u00C0-\u024F\u0370-\u03FF\u0400-\u04FF\u0590-\u08FF\uFB1D-\uFDFF\uFE70-\uFEFC]/.exec(text || '');
    return !!m && /[\u0590-\u08FF\uFB1D-\uFDFF\uFE70-\uFEFC]/.test(m[0]);
  };
  const hexAlpha = (hex, a) => (/^#[0-9a-f]{6}$/i.test(hex) ? hex + Math.round(a * 255).toString(16).padStart(2, '0') : 'transparent');

  // ------------------------------------------------------------------ appearance
  function applyAppearance() {
    const ui = settings.ui;
    document.documentElement.dataset.theme = isOverlay ? 'transparent' : ui.theme;
    document.documentElement.style.setProperty('--font-size', ui.fontSize + 'px');
    PLATFORMS.forEach((p) => document.documentElement.style.setProperty('--c-' + p, ui.colors[p]));
    chatEl.classList.toggle('show-pcolors', !!ui.showPlatformColors);
    chatEl.classList.toggle('animate', !!ui.animate);
    $$('.msg', chatEl).forEach(styleMessage);
    trimChat();
  }

  function styleMessage(node) {
    const ui = settings.ui;
    const p = node.dataset.platform;
    const pc = ui.colors[p] || '#888888';
    node.style.setProperty('--pc', pc);
    node.style.setProperty('--pc-soft', hexAlpha(pc, 0.09));
    const name = $('.name', node);
    name.style.color = ui.useUserColors && node.dataset.userColor ? node.dataset.userColor : pc;
    $('.plogo', node).style.display = ui.showLogos ? '' : 'none';
    $('.plogo', node).style.color = pc;
    $('.time', node).style.display = ui.showTimestamps ? '' : 'none';
    $$('.badge', node).forEach((b) => { b.style.display = ui.showBadges ? '' : 'none'; });
  }

  // ------------------------------------------------------------------ chat rendering
  let stickToBottom = true;
  chatEl.addEventListener('scroll', () => {
    stickToBottom = chatEl.scrollHeight - chatEl.scrollTop - chatEl.clientHeight < 40;
    $('#scroll-resume').hidden = stickToBottom;
  });
  $('#scroll-resume').addEventListener('click', () => {
    chatEl.scrollTop = chatEl.scrollHeight;
  });

  const seen = new Set();

  function renderMessage(m) {
    if (seen.has(m.id)) return;
    seen.add(m.id);
    $('.empty-hint', chatEl)?.remove();

    const node = el('div', 'msg');
    node.dataset.platform = m.platform;
    node.dataset.id = m.id;
    if (m.userColor) node.dataset.userColor = m.userColor;
    if (m.history) node.classList.add('history');
    if (m.test) node.classList.add('test');

    const line = el('div', 'line');
    const t = new Date(m.timestamp || Date.now());
    line.append(el('span', 'time', t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })));
    const logo = el('span', 'plogo');
    logo.innerHTML = LOGOS[m.platform] || ''; // static markup, never user data
    logo.title = m.platform;
    line.append(logo);
    for (const r of m.roles || []) line.append(el('span', 'badge ' + r, r === 'broadcaster' ? 'host' : r === 'moderator' ? 'mod' : r === 'subscriber' ? 'sub' : r));
    // <bdi> isolates each user-supplied run so mixed Arabic/English lines
    // keep name, colon and message in the right visual order.
    const name = el('bdi', 'name', m.displayName);
    line.append(name, el('span', 'sep', ':'));

    const text = el('bdi', 'text');
    const plain = (m.parts || []).map((p) => (p.type === 'text' ? p.text : '')).join('');
    line.dir = isRtl(plain) ? 'rtl' : 'ltr';
    for (const part of m.parts || []) {
      if (part.type === 'emote' && /^https:\/\//.test(part.url)) {
        const img = el('img', 'emote');
        img.src = part.url;
        img.alt = img.title = part.name;
        img.loading = 'lazy';
        img.onerror = () => img.replaceWith(document.createTextNode(part.name));
        text.append(img);
      } else {
        text.append(document.createTextNode(part.text || part.name || ''));
      }
    }
    line.append(text);
    node.append(line);

    if (m.translation) {
      const tr = el('div', 'translation', m.translation);
      tr.dir = 'auto';
      tr.lang = settings.translation.targetLang;
      node.append(tr);
    }

    styleMessage(node);
    chatEl.append(node);
    trimChat();
    if (stickToBottom || isOverlay) chatEl.scrollTop = chatEl.scrollHeight;

    if (m.tts && speakHere) tts.enqueue(m.tts);
  }

  function trimChat() {
    const max = Math.max(20, settings.ui.maxMessages | 0);
    const nodes = chatEl.querySelectorAll('.msg');
    for (let i = 0; i < nodes.length - max; i++) {
      seen.delete(nodes[i].dataset.id);
      nodes[i].remove();
    }
  }

  function showEmptyHint() {
    if (chatEl.querySelector('.msg') || isOverlay) return;
    const hint = el('div', 'empty-hint', 'Waiting for chat messages… Open ⚙ Settings to connect Twitch, YouTube and Kick.');
    const b = el('button', null, 'Send test messages');
    b.onclick = () => api('/api/test', { method: 'POST' });
    hint.append(el('br'), b);
    chatEl.append(hint);
  }

  // ------------------------------------------------------------------ TTS player
  const tts = (() => {
    const queue = [];
    let playing = false;
    let skipRequested = false;
    let finishCurrent = null; // resolves the segment that is playing now
    let muted = false;
    try { muted = localStorage.getItem('usc.muted') === '1'; } catch (_) { /* storage unavailable */ }

    function updateButton() {
      const b = $('#tts-toggle');
      b.textContent = muted ? '🔇' : '🔊';
      b.classList.toggle('muted', muted);
      b.setAttribute('aria-pressed', String(muted));
      b.hidden = !speakHere;
      $('#tts-skip').hidden = !speakHere;
    }

    function enqueue(segments) {
      if (muted || !Array.isArray(segments) || !segments.length) return;
      const max = Math.max(1, settings.tts.maxQueue | 0);
      while (queue.length >= max) queue.shift(); // stay close to live chat
      queue.push(segments);
      run();
    }

    function playSegment(seg) {
      return new Promise((resolve) => {
        const q = new URLSearchParams({ text: seg.text, lang: seg.lang || 'en' });
        const audio = new Audio('/api/tts?' + q);
        audio.volume = Math.min(1, Math.max(0, +settings.tts.volume));
        audio.playbackRate = Math.min(4, Math.max(0.25, +settings.tts.rate));
        audio.preservesPitch = true;
        const done = () => {
          audio.onended = audio.onerror = null;
          audio.pause();
          finishCurrent = null;
          resolve();
        };
        finishCurrent = done;
        audio.onended = done;
        audio.onerror = done;
        audio.play().catch((err) => {
          if (err && err.name === 'NotAllowedError') $('#audio-unlock').hidden = false;
          done();
        });
      });
    }

    async function run() {
      if (playing) return;
      playing = true;
      while (queue.length) {
        const segments = queue.shift();
        skipRequested = false;
        for (const seg of segments) {
          if (muted || skipRequested) break;
          await playSegment(seg);
        }
      }
      playing = false;
    }

    function skip() {
      skipRequested = true;
      if (finishCurrent) finishCurrent();
    }

    function setMuted(v) {
      muted = v;
      try { localStorage.setItem('usc.muted', v ? '1' : '0'); } catch (_) { /* ignore */ }
      if (v) { queue.length = 0; skip(); }
      updateButton();
    }

    return { enqueue, skip, setMuted, isMuted: () => muted, updateButton };
  })();

  $('#tts-toggle').addEventListener('click', () => tts.setMuted(!tts.isMuted()));
  $('#tts-skip').addEventListener('click', () => tts.skip());
  $('#audio-unlock').addEventListener('click', () => {
    $('#audio-unlock').hidden = true;
    new Audio().play().catch(() => {});
  });

  // ------------------------------------------------------------------ status
  function renderStatus() {
    const bar = $('#platform-status');
    bar.textContent = '';
    for (const p of PLATFORMS) {
      const st = status.platforms[p] || { state: 'disabled' };
      const dot = el('span', 'status-dot');
      dot.innerHTML = LOGOS[p];
      dot.style.color = settings ? settings.ui.colors[p] : '';
      dot.dataset.state = st.state;
      dot.title = `${p}: ${st.state}${st.detail ? ' - ' + st.detail : ''}`;
      bar.append(dot);

      const pill = $(`[data-status="${p}"]`);
      if (pill) {
        pill.textContent = st.state;
        pill.dataset.state = st.state;
        pill.title = st.detail || '';
      }
      const acc = $(`[data-account="${p}"]`);
      if (acc) {
        acc.textContent = st.loggedIn ? `Logged in as ${st.account || '(unknown)'}` : 'Not logged in';
        if (st.detail && st.state !== 'connected') acc.textContent += ` — ${st.detail}`;
      }
      const redirect = $(`[data-redirect="${p}"]`);
      if (redirect && st.redirectUri) redirect.textContent = st.redirectUri;
    }
    $('#config-path').textContent = status.configPath || '';
    $('#overlay-url').textContent = `${location.origin}/overlay`;
    $('#version').textContent = status.version ? `Unified Stream Chat v${status.version}` : '';
  }

  // ------------------------------------------------------------------ settings UI
  const sidebar = $('#settings');
  function openSettings(open) {
    sidebar.classList.toggle('open', open);
    sidebar.setAttribute('aria-hidden', String(!open));
  }
  $('#settings-btn').addEventListener('click', () => openSettings(!sidebar.classList.contains('open')));
  $('#settings-close').addEventListener('click', () => openSettings(false));
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') openSettings(false); });

  $$('.tabs button').forEach((b) => b.addEventListener('click', () => {
    $$('.tabs button').forEach((x) => x.classList.toggle('active', x === b));
    $$('.panel').forEach((p) => p.classList.toggle('active', p.dataset.panel === b.dataset.tab));
    try { localStorage.setItem('usc.tab', b.dataset.tab); } catch (_) { /* ignore */ }
  }));
  try {
    const tab = localStorage.getItem('usc.tab');
    if (tab) $(`.tabs button[data-tab="${tab}"]`)?.click();
  } catch (_) { /* ignore */ }

  $$('[data-langs]').forEach((sel) => LANGS.forEach(([code, label]) => sel.append(new Option(label, code))));
  $$('[data-logo]').forEach((n) => {
    n.innerHTML = LOGOS[n.dataset.logo];
    n.style.color = `var(--c-${n.dataset.logo})`;
  });

  function fillForm() {
    for (const input of $$('[data-key]')) {
      const v = get(settings, input.dataset.key);
      if (v === undefined) continue;
      if (input.type === 'checkbox') input.checked = !!v;
      else if (input.dataset.type === 'list') input.value = (v || []).join('\n');
      else if (document.activeElement !== input) input.value = v;
      const out = $(`[data-out="${input.dataset.key}"]`);
      if (out) out.textContent = input.value;
    }
  }

  function readInput(input) {
    if (input.type === 'checkbox') return input.checked;
    if (input.type === 'number' || input.type === 'range') return input.value === '' ? 0 : Number(input.value);
    if (input.dataset.type === 'list') return input.value.split(/[\n,]/).map((s) => s.trim()).filter(Boolean);
    return input.value;
  }

  const pending = {};
  let saveTimer = null;
  function queueSave(input) {
    const key = input.dataset.key;
    pending[key] = readInput(input);
    const out = $(`[data-out="${key}"]`);
    if (out) out.textContent = input.value;
    $('#save-state').textContent = 'Saving…';
    clearTimeout(saveTimer);
    // Text fields wait for the user to pause typing; toggles save immediately.
    const delay = input.type === 'text' || input.type === 'password' || input.tagName === 'TEXTAREA' ? 600 : 150;
    saveTimer = setTimeout(flushSave, delay);
  }

  async function flushSave() {
    const keys = Object.keys(pending);
    if (!keys.length) return;
    let patch = {};
    for (const k of keys) {
      patch = deepMerge(patch, patchFor(k, pending[k]));
      delete pending[k];
    }
    try {
      settings = await api('/api/settings', { method: 'POST', body: patch });
      applyAppearance();
      $('#save-state').textContent = 'Saved';
      setTimeout(() => { if ($('#save-state').textContent === 'Saved') $('#save-state').textContent = ''; }, 1500);
    } catch (e) {
      $('#save-state').textContent = 'Save failed: ' + e.message;
    }
  }

  function deepMerge(a, b) {
    for (const [k, v] of Object.entries(b)) {
      a[k] = v && typeof v === 'object' && !Array.isArray(v) ? deepMerge(a[k] || {}, v) : v;
    }
    return a;
  }

  for (const input of $$('[data-key]')) {
    const live = input.type === 'range' || input.type === 'text' || input.type === 'password' || input.tagName === 'TEXTAREA';
    input.addEventListener(live ? 'input' : 'change', () => queueSave(input));
  }

  $$('[data-login]').forEach((b) => b.addEventListener('click', async () => {
    const p = b.dataset.login;
    await flushSave();
    try {
      const { url } = await api(`/api/auth/${p}/login`, { method: 'POST' });
      $('[data-detail]').textContent = 'A browser window was opened to log in. If nothing opened, visit: ' + url;
    } catch (e) {
      $('[data-detail]').textContent = e.message;
      alert(e.message);
    }
  }));
  $$('[data-logout]').forEach((b) => b.addEventListener('click', async () => {
    status = await api(`/api/auth/${b.dataset.logout}/logout`, { method: 'POST' });
    renderStatus();
  }));
  $$('[data-reconnect]').forEach((b) => b.addEventListener('click', () =>
    api('/api/reconnect?platform=' + encodeURIComponent(b.dataset.reconnect), { method: 'POST' })));
  $$('[data-copy]').forEach((b) => b.addEventListener('click', () => {
    const text = $(`[data-redirect="${b.dataset.copy}"]`).textContent;
    navigator.clipboard?.writeText(text).then(() => { b.textContent = 'Copied'; setTimeout(() => { b.textContent = 'Copy'; }, 1200); });
  }));

  $('#send-test').addEventListener('click', () => api('/api/test', { method: 'POST' }));
  $('#clear-chat').addEventListener('click', () => api('/api/clear', { method: 'POST' }));
  $('#reset-settings').addEventListener('click', async () => {
    if (!confirm('Reset all settings (except API credentials) to defaults?')) return;
    settings = await api('/api/settings/reset', { method: 'POST' });
    fillForm();
    applyAppearance();
  });
  $('#tts-test').addEventListener('click', () => {
    const lang = settings.translation.targetLang;
    const sample = lang === 'ar' ? 'مرحبا! هذا اختبار لصوت القراءة.' : 'Hello! This is a text to speech test.';
    const segs = [];
    if (settings.tts.readUsername) segs.push({ text: settings.tts.usernameTemplate.replace('{user}', 'Tester'), lang });
    segs.push({ text: sample, lang });
    tts.enqueue(segs);
  });

  // ------------------------------------------------------------------ live events
  function connectEvents() {
    const es = new EventSource('/api/events');
    es.addEventListener('hello', (e) => {
      status = JSON.parse(e.data);
      renderStatus();
    });
    es.addEventListener('chat', (e) => renderMessage(JSON.parse(e.data)));
    es.addEventListener('status', (e) => {
      const s = JSON.parse(e.data);
      status.platforms[s.platform] = Object.assign(status.platforms[s.platform] || {}, { state: s.state, detail: s.detail });
      // Login state may have changed; refresh the full picture.
      if (s.state === 'connected' || s.state === 'disabled' || s.state === 'error')
        api('/api/status').then((st) => { status = st; renderStatus(); }).catch(() => {});
      renderStatus();
    });
    es.addEventListener('settings', (e) => {
      settings = JSON.parse(e.data);
      fillForm();
      applyAppearance();
    });
    es.addEventListener('clear', () => {
      chatEl.textContent = '';
      seen.clear();
      showEmptyHint();
    });
    es.onerror = () => {
      for (const p of PLATFORMS) status.platforms[p] = Object.assign(status.platforms[p] || {}, { state: 'error', detail: 'App not running' });
      renderStatus();
    };
  }

  // ------------------------------------------------------------------ boot
  async function boot() {
    if (isOverlay) document.body.classList.add('overlay');
    try {
      settings = await api('/api/settings');
    } catch (e) {
      chatEl.append(el('div', 'empty-hint', 'Cannot reach the Unified Stream Chat app. Is it running?'));
      setTimeout(boot, 3000);
      return;
    }
    fillForm();
    applyAppearance();
    tts.updateButton();
    showEmptyHint();
    connectEvents();
  }
  boot();
})();
