(() => {
  'use strict';

  const $ = (s, root = document) => root.querySelector(s);
  const $$ = (s, root = document) => Array.from(root.querySelectorAll(s));
  const VALID_TABS = ['overview', 'wifi', 'tune', 'radar', 'network', 'system'];

  const state = {
    token: localStorage.getItem('ap_token') || '',
    tab: 'overview',
    scopeIface: 'lan',
    logSrc: 'syslog',
    rfRange: '6h',
    rfBand: '5g',
    rfMetric: 'air',
    trafficHist: [],
    lastOverview: null,
    lastWifi: null,
    lastOpt: null,
    lastRf: null,
    lastRadar: null,
    pollTimer: null,
    pollGen: 0,
    pollTicks: 0,
    tabAbort: null,
    radarFormHydrated: false,
    radarCandSig: '',
    stamonFormHydrated: false,
    stamonCandSig: '',
    stamonDraftTargets: [],
    stamonSavedSig: '',
    stamonLastStateMap: {},
    wtActive: false,
    wtSound: false,
    wtTimer: null,
    wtGen: 0,
    wtHistory: [],
    wtLast: null,
    wtPrevTx: null,
    wtSpots: [],
    wtAudioCtx: null,
    wtTargetSig: '',
    chartMeta: {
      traffic: null,
      rf: null,
      radar: null,
      walktest: null,
    },
  };

  /* ── Theme Tokens & Formatting Helpers ─────────────────────────────── */

  function cssVar(name, fallback) {
    const val = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return val || fallback;
  }

  function esc(str) {
    return String(str ?? '')
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  let toastTimer = null;
  function toast(msg, isErr = false) {
    const el = $('#toast');
    if (!el) return;
    el.textContent = msg;
    el.className = 'toast show' + (isErr ? ' err' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.className = 'toast'; }, 3200);
  }

  function fmtBytes(n) {
    n = Number(n) || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
    return (n / 1073741824).toFixed(2) + ' GB';
  }

  function fmtDur(sec) {
    sec = Math.max(0, Math.floor(Number(sec) || 0));
    const d = Math.floor(sec / 86400);
    const h = Math.floor((sec % 86400) / 3600);
    const m = Math.floor((sec % 3600) / 60);
    if (d > 0) return `${d}天${h}时`;
    if (h > 0) return `${h}时${m}分`;
    return `${m}分${sec % 60}秒`;
  }

  function fmtHM(ts) {
    const dt = new Date((Number(ts) || 0) * 1000);
    return `${String(dt.getHours()).padStart(2, '0')}:${String(dt.getMinutes()).padStart(2, '0')}`;
  }

  function fmtHMS(ts) {
    const dt = new Date((Number(ts) || 0) * 1000);
    return `${String(dt.getHours()).padStart(2, '0')}:${String(dt.getMinutes()).padStart(2, '0')}:${String(dt.getSeconds()).padStart(2, '0')}`;
  }

  async function withBusy(btn, fn, busyLabel) {
    if (!btn) return fn();
    if (btn.disabled) return undefined;
    const origText = btn.textContent;
    btn.disabled = true;
    btn.setAttribute('aria-busy', 'true');
    if (busyLabel) btn.textContent = busyLabel;
    try {
      return await fn();
    } finally {
      btn.disabled = false;
      btn.removeAttribute('aria-busy');
      if (busyLabel) btn.textContent = origText;
    }
  }

  function setSegActive(groupSelector, activeBtn) {
    $$(groupSelector + ' .seg-btn').forEach(btn => {
      const on = btn === activeBtn;
      btn.classList.toggle('active', on);
      btn.setAttribute('aria-pressed', on ? 'true' : 'false');
    });
  }

  function setSelectSafe(sel, val) {
    if (!sel) return;
    sel.value = [...sel.options].some(o => o.value === val) ? val : '';
  }

  /* ── Native Non-Blocking Modal Dialog (replaces confirm / prompt) ──── */

  let activeDialogCancel = null;

  function openDialog({ title, message, confirmText = '确定', danger = false, prompt = false, defaultValue = '', inputLabel = '', placeholder = '' }) {
    const dlg = $('#appDialog');
    if (!dlg || typeof dlg.showModal !== 'function') {
      if (prompt) return Promise.resolve(window.prompt(message, defaultValue));
      return Promise.resolve(window.confirm(message));
    }
    if (activeDialogCancel) {
      const cancelPrev = activeDialogCancel;
      activeDialogCancel = null;
      cancelPrev();
    }

    const prevFocus = document.activeElement;
    $('#dialogTitle').textContent = title || (prompt ? '编辑内容' : '确认操作');
    $('#dialogMsg').textContent = message || '';

    const inputWrap = $('#dialogInputWrap');
    const input = $('#dialogInput');
    const inputLbl = $('#dialogInputLabel');
    inputWrap.hidden = !prompt;
    if (prompt) {
      inputLbl.textContent = inputLabel || '输入内容';
      input.value = defaultValue || '';
      input.placeholder = placeholder || '';
    }

    const okBtn = $('#dialogConfirm');
    okBtn.textContent = confirmText;
    okBtn.className = danger ? 'btn-primary btn-danger-solid' : 'btn-primary';

    return new Promise(resolve => {
      let settled = false;
      const finish = val => {
        if (settled) return;
        settled = true;
        activeDialogCancel = null;
        dlg.removeEventListener('cancel', onCancel);
        dlg.removeEventListener('click', onBackdrop);
        $('#dialogCancel').removeEventListener('click', onCancelBtn);
        $('#dialogForm').removeEventListener('submit', onSubmit);
        if (dlg.open) dlg.close();
        if (prevFocus && typeof prevFocus.focus === 'function') prevFocus.focus();
        resolve(val);
      };

      const cancelVal = prompt ? null : false;
      const onCancel = e => { e.preventDefault(); finish(cancelVal); };
      const onCancelBtn = () => finish(cancelVal);
      const onBackdrop = e => {
        if (e.target === dlg) finish(cancelVal);
      };
      const onSubmit = e => {
        e.preventDefault();
        finish(prompt ? input.value : true);
      };

      activeDialogCancel = () => finish(cancelVal);
      dlg.addEventListener('cancel', onCancel);
      dlg.addEventListener('click', onBackdrop);
      $('#dialogCancel').addEventListener('click', onCancelBtn);
      $('#dialogForm').addEventListener('submit', onSubmit);

      dlg.showModal();
      if (prompt) {
        input.focus();
        input.select();
      } else {
        okBtn.focus();
      }
    });
  }

  function uiConfirm(message, opts = {}) {
    return openDialog(Object.assign({ title: '确认操作', message, confirmText: '确认', danger: false, prompt: false }, opts));
  }

  function uiPrompt(message, defaultValue = '', opts = {}) {
    return openDialog(Object.assign({ title: '编辑', message, defaultValue, confirmText: '保存', prompt: true }, opts));
  }

  /* ── HTTP API Client & Authenticated Blob Download ─────────────────── */

  function makeAbortError() {
    const err = new Error('aborted');
    err.name = 'AbortError';
    return err;
  }

  async function api(path, opts = {}) {
    const { timeoutMs = 15000, signal: extSignal, headers: optHeaders, body: optBody, ...rest } = opts;
    const headers = Object.assign({}, optHeaders || {});
    if (state.token) headers['X-Token'] = state.token;

    let body = optBody;
    if (body && typeof body === 'object' && !(body instanceof Blob) && !(body instanceof ArrayBuffer)) {
      headers['Content-Type'] = 'application/json';
      body = JSON.stringify(body);
    }

    const ctrl = new AbortController();
    let timedOut = false;
    const onExtAbort = () => ctrl.abort();
    if (extSignal) {
      if (extSignal.aborted) throw makeAbortError();
      extSignal.addEventListener('abort', onExtAbort, { once: true });
    }
    const timer = timeoutMs > 0
      ? setTimeout(() => { timedOut = true; ctrl.abort(); }, timeoutMs)
      : null;

    try {
      const res = await fetch(path, Object.assign({}, rest, { headers, body, signal: ctrl.signal }));
      if (res.status === 401) {
        state.token = '';
        localStorage.removeItem('ap_token');
        showLogin();
        const err = new Error('unauthorized');
        err.status = 401;
        throw err;
      }
      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        const err = new Error(data.error || `HTTP ${res.status}`);
        err.status = res.status;
        throw err;
      }
      return data;
    } catch (err) {
      if (timedOut) {
        const tErr = new Error('请求超时，请检查设备连接');
        tErr.isTimeout = true;
        throw tErr;
      }
      throw err;
    } finally {
      if (timer) clearTimeout(timer);
      if (extSignal) extSignal.removeEventListener('abort', onExtAbort);
    }
  }

  function isSilentErr(err) {
    return !err || err.name === 'AbortError' || err.message === 'unauthorized';
  }

  async function downloadFile(path, fallbackName) {
    const headers = {};
    if (state.token) headers['X-Token'] = state.token;
    const res = await fetch(path, { headers, cache: 'no-store' });
    if (res.status === 401) {
      state.token = '';
      localStorage.removeItem('ap_token');
      showLogin();
      return;
    }
    if (!res.ok) {
      const data = await res.json().catch(() => ({}));
      throw new Error(data.error || `导出失败 (HTTP ${res.status})`);
    }
    const cd = res.headers.get('Content-Disposition') || '';
    const m = cd.match(/filename="?([^";]+)"?/i);
    const filename = m ? m[1] : fallbackName;
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
  }

  /* ── Session, Routing & WAI-ARIA Tabs ──────────────────────────────── */

  function parseHashTab() {
    const raw = (location.hash || '').replace(/^#\/?/, '').trim();
    return VALID_TABS.includes(raw) ? raw : 'overview';
  }

  function showLogin(errMsg = '') {
    stopPolling();
    $('#topbar').hidden = true;
    $('#app').hidden = true;
    $('#loginView').hidden = false;
    $('#loginErr').textContent = errMsg;
    requestAnimationFrame(() => $('#loginPass')?.focus());
  }

  function showApp(initialOverview = null) {
    $('#loginView').hidden = true;
    $('#topbar').hidden = false;
    $('#app').hidden = false;
    const logoutBtn = $('#btnLogout');
    if (logoutBtn) logoutBtn.hidden = !state.token || state.token === 'open';
    const startTab = parseHashTab();
    if (initialOverview) renderOverview(initialOverview);
    switchTab(startTab, {
      pushHash: false,
      skipOverviewFetch: startTab === 'overview' && !!initialOverview,
    });
    startPolling();
  }

  function switchTab(tab, opts = {}) {
    const { pushHash = true, skipOverviewFetch = false } = opts;
    if (!VALID_TABS.includes(tab)) tab = 'overview';

    if (state.tabAbort) {
      state.tabAbort.abort();
    }
    state.tabAbort = new AbortController();
    const signal = state.tabAbort.signal;

    state.tab = tab;
    state.pollTicks = 0;
    if (tab !== 'wifi' && state.wtActive) {
      stopWalkTest();
    }

    $$('.nav-btn').forEach(b => {
      const on = b.dataset.tab === tab;
      b.classList.toggle('active', on);
      b.setAttribute('aria-selected', on ? 'true' : 'false');
      b.tabIndex = on ? 0 : -1;
    });
    $$('.view').forEach(v => {
      v.hidden = v.id !== 'view-' + tab;
    });

    if (pushHash && location.hash !== '#' + tab) {
      history.replaceState(null, '', '#' + tab);
    }

    if (tab === 'overview') {
      if (skipOverviewFetch) {
        refreshTrafficOnly(signal);
      } else {
        refreshOverview(signal);
      }
    } else if (tab === 'wifi') {
      refreshWifi(signal);
    } else if (tab === 'tune') {
      refreshTune(signal);
      refreshRFHistory(signal);
    } else if (tab === 'radar') {
      refreshRadar(true, signal);
      refreshStaMon(true, signal);
    } else if (tab === 'network') {
      refreshNetwork(signal);
    } else if (tab === 'system') {
      refreshSystem(signal);
      refreshLogs(signal);
    }
  }

  /* ── Responsive 1:1 SVG Chart Engine & Hover Inspector ─────────────── */

  function measureSvgBox(svg, fallbackH) {
    const rect = svg.getBoundingClientRect();
    const W = Math.max(300, Math.round(rect.width || svg.parentElement?.clientWidth || 760));
    const H = Math.max(120, Math.round(rect.height || fallbackH));
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    return { W, H };
  }

  function renderTraffic() {
    const svg = $('#trafficSvg');
    if (!svg) return;
    const tip = $('#trafficTip');
    if (tip) tip.hidden = true;
    const hist = state.trafficHist;
    const keyRx = state.scopeIface + '_rx';
    const keyTx = state.scopeIface + '_tx';
    const rxVals = hist.map(s => Number(s[keyRx]) || 0);
    const txVals = hist.map(s => Number(s[keyTx]) || 0);
    const latestRx = rxVals.length ? rxVals[rxVals.length - 1] : 0;
    const latestTx = txVals.length ? txVals[txVals.length - 1] : 0;
    const peak = Math.max(0, ...rxVals, ...txVals);

    $('#rateRx').textContent = latestRx.toFixed(2);
    $('#rateTx').textContent = latestTx.toFixed(2);
    $('#ratePeak').textContent = peak.toFixed(2);

    const { W, H } = measureSvgBox(svg, 170);
    const padT = 14, padB = 22, padL = 42, padR = 10;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;
    const maxY = Math.max(10, Math.ceil(peak * 1.2 / 10) * 10);

    const cAccent = cssVar('--accent', '#0B6E7E');
    const cWarn = cssVar('--warn', '#B86B14');
    const cInk3 = cssVar('--ink-3', '#5E6D7A');
    const cGrid = cssVar('--grid-line', '#E6ECEF');

    const yOf = v => padT + plotH - (Math.min(v, maxY) / maxY) * plotH;
    const n = Math.max(hist.length, 2);
    const xOf = i => padL + (i / (n - 1)) * plotW;

    let grid = '';
    for (let i = 0; i <= 3; i++) {
      const val = (maxY * (3 - i)) / 3;
      const y = padT + (plotH * i) / 3;
      grid += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}" stroke="${cGrid}" stroke-width="1"/>`;
      grid += `<text x="${padL - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" fill="${cInk3}" font-size="11" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${Math.round(val)}</text>`;
    }

    const buildPath = vals => vals.map((v, i) => `${i === 0 ? 'M' : 'L'}${xOf(i).toFixed(1)},${yOf(v).toFixed(1)}`).join(' ');
    let paths = '';
    if (hist.length >= 2) {
      const rxLine = buildPath(rxVals);
      const rxArea = `${rxLine} L${xOf(rxVals.length - 1).toFixed(1)},${(padT + plotH).toFixed(1)} L${xOf(0).toFixed(1)},${(padT + plotH).toFixed(1)} Z`;
      const txLine = buildPath(txVals);
      const t0 = fmtHMS(hist[0].ts);
      const t1 = fmtHMS(hist[hist.length - 1].ts);
      paths = `
        <path d="${rxArea}" fill="rgba(11,110,126,0.10)"/>
        <path d="${rxLine}" fill="none" stroke="${cAccent}" stroke-width="2" stroke-linejoin="round"/>
        <path d="${txLine}" fill="none" stroke="${cWarn}" stroke-width="1.6" stroke-linejoin="round" stroke-dasharray="4 3"/>
        <text x="${padL}" y="${H - 5}" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t0}</text>
        <text x="${W - padR}" y="${H - 5}" text-anchor="end" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t1}</text>
        <line class="chart-cursor" x1="0" y1="${padT}" x2="0" y2="${padT + plotH}" stroke="${cInk3}" stroke-width="1" stroke-dasharray="2 2" opacity="0"/>
      `;
      state.chartMeta.traffic = {
        W, padL, padR, plotW, count: hist.length,
        format: idx => `${fmtHMS(hist[idx].ts)} · 接收 ${rxVals[idx].toFixed(2)} Mbps · 发送 ${txVals[idx].toFixed(2)} Mbps`,
      };
    } else {
      paths = `<text x="${W / 2}" y="${H / 2}" text-anchor="middle" fill="${cInk3}" font-size="12">正在采样实时流量...</text>`;
      state.chartMeta.traffic = null;
    }
    svg.innerHTML = grid + paths;
  }

  function bindChartHover(chartKey, svgId, tipId) {
    const svg = $(svgId);
    const tip = $(tipId);
    if (!svg || !tip) return;

    const hide = () => {
      tip.hidden = true;
      const cur = svg.querySelector('.chart-cursor');
      if (cur) cur.setAttribute('opacity', '0');
    };

    svg.addEventListener('pointermove', e => {
      const meta = state.chartMeta[chartKey];
      if (!meta || meta.count < 2) return;
      const rect = svg.getBoundingClientRect();
      if (!rect.width) return;
      const xSvg = ((e.clientX - rect.left) / rect.width) * meta.W;
      const ratio = Math.max(0, Math.min(1, (xSvg - meta.padL) / meta.plotW));
      const idx = Math.round(ratio * (meta.count - 1));
      const snapX = meta.padL + (idx / (meta.count - 1)) * meta.plotW;
      const cur = svg.querySelector('.chart-cursor');
      if (cur) {
        cur.setAttribute('x1', snapX.toFixed(1));
        cur.setAttribute('x2', snapX.toFixed(1));
        cur.setAttribute('opacity', '0.7');
      }
      tip.textContent = meta.format(idx);
      tip.hidden = false;
    });

    svg.addEventListener('pointerleave', hide);
    svg.addEventListener('pointercancel', hide);
  }

  /* ── Overview ──────────────────────────────────────────────────────── */

  function renderAirCell(air) {
    if (!air) return '—';
    const self = Math.min(100, (air.tx_pct || 0) + (air.inbss_pct || 0));
    const obss = Math.min(100 - self, air.obss_pct || 0);
    const busy = air.busy_pct || 0;
    const other = Math.max(0, Math.min(100 - self - obss, busy - self - obss));
    return `
      <div class="air-meter">
        <div class="air-bar" title="本网 ${self}% / 邻居 OBSS ${obss}% / 总繁忙 ${busy}%">
          <div class="air-self" style="width:${self}%"></div>
          <div class="air-obss" style="width:${obss}%"></div>
          <div class="air-other" style="width:${other}%"></div>
        </div>
        <span class="air-lbl mono">忙 ${busy}% · 邻 ${obss}%</span>
      </div>`;
  }

  function renderOverview(ov) {
    if (!ov) return;
    state.lastOverview = ov;

    $('#liveDot').classList.remove('offline');
    $('#hostName').textContent = ov.hostname || 'hg6371f-ap';
    $('#slotBadge').textContent = `Slot ${ov.slots?.booted || '—'}`;
    $('#tlsPill').hidden = !ov.tls_enabled;

    const led = ov.led || 'off';
    $('#topLedDot').className = 'led-dot ' + led;
    $('#topLedText').textContent = led === 'blue' ? '蓝灯' : led === 'red' ? '红灯' : '熄灯';

    const w1t = ov.wl1_temp_c ? `${ov.wl1_temp_c}°C` : '—';
    const w0t = ov.wl0_temp_c ? `${ov.wl0_temp_c}°C` : '—';
    $('#mTemp').textContent = `${ov.temp_c}°C · ${ov.cpu_pct}%`;
    $('#mLoad').textContent = `5G ${w1t} / 2.4G ${w0t} · 负载 ${ov.load || '—'}`;

    $('#mMem').textContent = `${ov.mem_avail_mb} / ${ov.mem_total_mb} MiB`;
    $('#mMemSub').textContent = `已用 ${ov.mem_used_mb} MiB · 进程 ${ov.procs}`;

    const ports = ov.ports || [];
    const p0 = ports[0] || {};
    const p1 = ports[1] || {};
    const errSum = (p0.rx_errors || 0) + (p0.tx_errors || 0) + (p1.rx_errors || 0) + (p1.tx_errors || 0);
    $('#mUplink').textContent = p0.link ? p0.speed : '未连接';
    $('#mUplinkSub').textContent = `${ov.uplink?.br_ip || '无 IP'} · lan1 ${p1.link ? p1.speed : '闲置'}${errSum ? ` · 错包 ${errSum}` : ''}`;

    const fc = ov.fcache || {};
    $('#mFcache').innerHTML = fc.hw_enabled
      ? '<span class="status-tag ok">硬件直通已启用</span>'
      : '<span class="status-tag warn">未启用</span>';
    $('#mFcacheSub').textContent = `活跃流 ${fc.active_flows ?? 0} · 累计 ${fc.cumulative ?? 0}`;

    $('#sysMeta').textContent = `Alpine ${ov.alpine_version || ''} · Linux ${ov.kernel || ''} · 已运行 ${fmtDur(ov.uptime_sec)}`;

    const tbody = $('#overviewRadios');
    tbody.innerHTML = (ov.radios || []).map(r => {
      const gBadge = r.guest?.up ? `<span class="sub-pill">+访客:${esc(r.guest.ssid)}</span>` : '';
      const retCls = (r.air?.tx_retrans_pct || 0) > 20 ? 'warn' : '';
      return `
        <tr>
          <td data-label="频段"><span class="band-pill ${r.band === '5G' ? 'b5' : ''}">${esc(r.band)}</span></td>
          <td data-label="SSID"><strong>${esc(r.ssid || '—')}</strong>${r.hidden ? ' <span class="meta-line">(隐藏)</span>' : ''}${gBadge}</td>
          <td data-label="状态"><span class="status-tag ${r.up ? 'ok' : 'err'}">${r.up ? '运行中' : '已停止'}</span></td>
          <td data-label="信道 / 频宽" class="mono">${esc(r.chanspec || r.channel)} (${r.width_mhz} MHz)</td>
          <td data-label="发射功率" class="mono">${esc(r.txpower || '—')} (${r.txpwr_percent || 100}%)</td>
          <td data-label="底噪 / 温度" class="mono">${r.noise_dbm ? r.noise_dbm + ' dBm' : '—'} / ${r.air?.chip_temp_c ? r.air.chip_temp_c + '°C' : '—'}</td>
          <td data-label="空口 / 邻居干扰">${r.up ? renderAirCell(r.air) : '—'}</td>
          <td data-label="发包重传" class="mono">${r.up ? `<span class="${retCls ? 'status-tag warn' : ''}">${(r.air?.tx_retrans_pct || 0).toFixed(1)}%</span>` : '—'}</td>
          <td data-label="在线" class="mono">${r.clients} 台</td>
        </tr>`;
    }).join('');
  }

  async function refreshOverview(signal) {
    try {
      const [ov, tr] = await Promise.all([
        api('/api/overview', { signal }),
        api('/api/traffic', { signal }),
      ]);
      state.trafficHist = tr.history || [];
      renderOverview(ov);
      renderTraffic();
    } catch (e) {
      if (!isSilentErr(e)) $('#liveDot').classList.add('offline');
    }
  }

  async function refreshTrafficOnly(signal) {
    try {
      const tr = await api('/api/traffic', { signal });
      state.trafficHist = tr.history || [];
      renderTraffic();
      $('#liveDot').classList.remove('offline');
    } catch (e) {
      if (!isSilentErr(e)) $('#liveDot').classList.add('offline');
    }
  }

  /* ── Wi-Fi & Clients ───────────────────────────────────────────────── */

  async function refreshWifi(signal) {
    try {
      const [data, spotsRes] = await Promise.all([
        api('/api/wifi', { signal }),
        api('/api/wifi/walktest/spots', { signal }).catch(() => null),
      ]);
      state.lastWifi = data;
      renderClients(data.clients || []);
      for (const r of (data.radios || [])) fillRadioForms(r);
      if (spotsRes && Array.isArray(spotsRes.spots)) {
        state.wtSpots = spotsRes.spots;
        renderWtSpots();
      }
    } catch (e) {
      if (!isSilentErr(e)) toast(e.message, true);
    }
  }

  function sigClass(rssi) {
    if (rssi >= -60) return '';
    if (rssi >= -72) return 'mid';
    return 'low';
  }
  function sigPct(rssi) {
    return Math.max(8, Math.min(100, Math.round((rssi + 95) * 1.8)));
  }

  function renderClients(list) {
    $('#clientCount').textContent = `${list.length} 台设备`;
    syncWtTargetOptions(list);
    const tbody = $('#clientRows');
    if (!list.length) {
      tbody.innerHTML = '<tr><td colspan="9" class="empty-cell">当前没有在线无线设备</td></tr>';
      return;
    }
    tbody.innerHTML = list.map(c => {
      const antLine = c.ant_rssi ? c.ant_rssi.replace(/\s*\/\s*/, ' / ') : '';
      const specSub = [c.mode, c.width_nss].filter(Boolean).join(' · ');
      const displayLabel = c.note || c.hostname || c.mac;
      const showMacSub = !!(c.note || c.hostname);
      const autoBadge = (!c.note && c.hostname) ? '<span class="sub-pill" title="通过 mDNS / 局域网反向解析自动发现">自动</span>' : '';
      return `
      <tr>
        <td data-label="设备">
          <div class="cell-stack">
            <div><button type="button" class="client-name" data-mac="${esc(c.mac)}" data-note="${esc(c.note || '')}" title="点击修改备注">${esc(displayLabel)}</button>${autoBadge}</div>
            ${showMacSub ? `<span class="client-mac mono">${esc(c.mac)}</span>` : ''}
          </div>
        </td>
        <td data-label="IP 地址" class="mono">${esc(c.ip || '—')}</td>
        <td data-label="频段 / 规格">
          <div class="cell-stack">
            <div><span class="band-pill ${c.band === '5G' ? 'b5' : ''}">${esc(c.band)}</span>${c.is_guest ? '<span class="sub-pill">访客</span>' : ''}</div>
            <span class="cell-sub mono">${esc(specSub)}</span>
          </div>
        </td>
        <td data-label="信号强度">
          <div class="sig-cell">
            <div class="sig-wrap">
              <div class="sig-bar"><div class="sig-fill ${sigClass(c.rssi)}" style="width:${sigPct(c.rssi)}%"></div></div>
              <span class="sig-val mono">${c.rssi} dBm</span>
            </div>
            ${antLine ? `<span class="sig-sub mono">双天线 ${esc(antLine)}</span>` : ''}
          </div>
        </td>
        <td data-label="协商速率">
          <div class="cell-stack mono">
            <span>↑ ${c.tx_mbps} Mbps</span>
            <span class="cell-sub">↓ ${c.rx_mbps} Mbps</span>
          </div>
        </td>
        <td data-label="重传率" class="mono">${c.retry_pct > 25 ? `<span class="status-tag warn">${c.retry_pct}%</span>` : `${c.retry_pct}%`}</td>
        <td data-label="流量">
          <div class="cell-stack mono">
            <span>↓ ${fmtBytes(c.rx_bytes)}</span>
            <span class="cell-sub">↑ ${fmtBytes(c.tx_bytes)}</span>
          </div>
        </td>
        <td data-label="在线" class="mono">${fmtDur(c.in_net_sec)}</td>
        <td data-label="操作"><button type="button" class="btn-danger btn-sm" data-kick-mac="${esc(c.mac)}" data-kick-iface="${esc(c.iface)}">踢出</button></td>
      </tr>`;
    }).join('');
  }

  function fillRadioForms(r) {
    const form = $(`#form-${r.iface}`);
    if (form) {
      form.ssid.value = r.ssid || '';
      form.passphrase.value = '';
      if (r.channel) form.channel.value = String(r.channel);
      if (form.width_mhz && r.width_mhz) form.width_mhz.value = String(r.width_mhz);
      form.txpwr_percent.value = String(r.txpwr_percent || 100);
      form.assoc_rssi_th.value = String(r.assoc_rssi_th || 0);
      form.hidden.checked = !!r.hidden;
      form.isolate.checked = !!r.isolate;
    }
    const tag = $(`#tag-${r.iface}`);
    if (tag) {
      tag.className = 'status-tag ' + (r.up ? 'ok' : 'err');
      tag.textContent = r.up ? `${r.chanspec || r.channel} · ${r.txpower || ''}` : '已关闭';
    }
    const tBtn = $(r.iface === 'wl1' ? '#btnToggleWl1' : '#btnToggleWl0');
    if (tBtn) {
      tBtn.textContent = r.up ? '停用' : '启用';
      tBtn.dataset.next = r.up ? '0' : '1';
    }

    const gForm = $(`#form-guest-${r.iface}`);
    const g = r.guest || {};
    if (gForm) {
      gForm.ssid.value = g.ssid || '';
      gForm.passphrase.value = '';
      gForm.passphrase.placeholder = g.has_pass ? '已设密码，留空不修改' : '首次开启必填（8-63位）';
      gForm.vlan_id.value = String(g.vlan_id || 0);
      gForm.enabled.checked = !!g.enabled;
      gForm.isolate.checked = !!g.isolate;
      gForm.hidden.checked = !!g.hidden;
    }
    const gTag = $(`#tag-guest-${r.iface}`);
    if (gTag) {
      gTag.className = 'status-tag ' + (g.up ? 'ok' : '');
      gTag.textContent = g.up ? `运行中${g.vlan_id ? ` · VLAN ${g.vlan_id}` : ''} · ${g.clients || 0}台` : '未启用';
    }

    const aForm = $(`#form-adv-${r.iface}`);
    if (aForm) {
      const rm = r.roaming || {};
      aForm.dot11k.checked = !!rm.dot11k;
      aForm.dot11v.checked = !!rm.dot11v;
      aForm.dot11r.checked = !!rm.dot11r;
      aForm.mobility_domain.value = rm.mobility_domain || '6371';
      aForm.mac_mode.value = String(r.mac_mode || 0);
      aForm.mac_list.value = (r.mac_list || []).join('\n');
    }
  }

  async function saveRadio(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const iface = form.dataset.iface;
    const btn = form.querySelector('button[type="submit"]');
    await withBusy(btn, async () => {
      try {
        await api('/api/wifi/save', {
          method: 'POST',
          timeoutMs: 25000,
          body: {
            iface,
            ssid: form.ssid.value.trim(),
            passphrase: form.passphrase.value,
            channel: Number(form.channel.value),
            width_mhz: Number(form.width_mhz.value),
            txpwr_percent: Number(form.txpwr_percent.value),
            assoc_rssi_th: Number(form.assoc_rssi_th.value),
            hidden: form.hidden.checked,
            isolate: form.isolate.checked,
          },
        });
        toast(`${iface} 配置已生效并重启无线`);
        setTimeout(() => refreshWifi(), 1800);
      } catch (err) {
        if (!isSilentErr(err)) toast(err.message, true);
      }
    });
  }

  async function saveGuest(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const iface = form.dataset.iface;
    const btn = form.querySelector('button[type="submit"]');
    await withBusy(btn, async () => {
      try {
        await api('/api/wifi/guest', {
          method: 'POST',
          timeoutMs: 25000,
          body: {
            iface,
            guest: {
              enabled: form.enabled.checked,
              ssid: form.ssid.value.trim(),
              passphrase: form.passphrase.value,
              vlan_id: Number(form.vlan_id.value) || 0,
              isolate: form.isolate.checked,
              hidden: form.hidden.checked,
            },
          },
        });
        toast(`${iface}.1 访客网络已更新`);
        setTimeout(() => refreshWifi(), 1800);
      } catch (err) {
        if (!isSilentErr(err)) toast(err.message, true);
      }
    });
  }

  async function saveAdv(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const iface = form.dataset.iface;
    const mainForm = $(`#form-${iface}`);
    const btn = form.querySelector('button[type="submit"]');
    await withBusy(btn, async () => {
      try {
        const macList = form.mac_list.value.split(/[\r\n,; ]+/).map(s => s.trim()).filter(Boolean);
        await api('/api/wifi/save', {
          method: 'POST',
          timeoutMs: 25000,
          body: {
            iface,
            ssid: mainForm.ssid.value.trim(),
            passphrase: '',
            channel: Number(mainForm.channel.value),
            width_mhz: Number(mainForm.width_mhz.value),
            txpwr_percent: Number(mainForm.txpwr_percent.value),
            assoc_rssi_th: Number(mainForm.assoc_rssi_th.value),
            hidden: mainForm.hidden.checked,
            isolate: mainForm.isolate.checked,
            mac_mode: Number(form.mac_mode.value),
            mac_list: macList,
            roaming: {
              dot11k: form.dot11k.checked,
              dot11v: form.dot11v.checked,
              dot11r: form.dot11r.checked,
              mobility_domain: form.mobility_domain.value.trim(),
            },
          },
        });
        toast(`${iface} 漫游与访问控制已应用`);
        setTimeout(() => refreshWifi(), 1800);
      } catch (err) {
        if (!isSilentErr(err)) toast(err.message, true);
      }
    });
  }

  async function runScan(iface, btn) {
    const tbody = $('#scanRows');
    await withBusy(btn, async () => {
      tbody.innerHTML = `<tr><td colspan="5" class="empty-cell">正在扫描 ${esc(iface)} 周边无线信号...</td></tr>`;
      try {
        const d = await api('/api/wifi/scan?iface=' + encodeURIComponent(iface), { timeoutMs: 30000 });
        const aps = d.aps || [];
        if (!aps.length) {
          tbody.innerHTML = '<tr><td colspan="5" class="empty-cell">未扫描到周边 AP</td></tr>';
          return;
        }
        tbody.innerHTML = aps.map(a => `
          <tr>
            <td data-label="SSID">${esc(a.ssid || '<隐藏网络>')}</td>
            <td data-label="BSSID" class="mono">${esc(a.bssid)}</td>
            <td data-label="信道 / 频宽" class="mono">${esc(a.chanspec || a.channel)}</td>
            <td data-label="信号强度" class="mono">${a.rssi} dBm</td>
            <td data-label="底噪" class="mono">${a.noise} dBm</td>
          </tr>
        `).join('');
      } catch (e) {
        if (!isSilentErr(e)) {
          tbody.innerHTML = `<tr><td colspan="5" class="empty-cell">扫描失败：${esc(e.message)}</td></tr>`;
        }
      }
    });
  }

  /* ── Handheld Walk Test (Real-Time Mobile Signal Mapper) ───────────── */

  function syncWtTargetOptions(clients) {
    const sel = $('#wtTarget');
    if (!sel || document.activeElement === sel) return;
    const list = Array.isArray(clients) ? clients : [];
    const sig = list.map(c => `${c.mac}|${c.note || c.hostname || ''}|${c.band}|${c.rssi}`).join(';');
    if (sig === state.wtTargetSig) return;
    state.wtTargetSig = sig;
    const prev = sel.value;
    sel.innerHTML = '<option value="">自动识别本机</option>' + list.map(c => {
      const label = c.note || c.hostname || c.mac;
      return `<option value="${esc(c.mac)}">${esc(label)} (${esc(c.band)} · ${c.rssi} dBm)</option>`;
    }).join('');
    setSelectSafe(sel, prev);
  }

  function wtQuality(rssi, online) {
    if (!online || !rssi || rssi >= 0) return { text: '信号丢失', cls: 'err', bar: 'low', pct: 0 };
    if (rssi >= -55) return { text: '极佳', cls: 'ok', bar: '', pct: sigPct(rssi) };
    if (rssi >= -67) return { text: '良好', cls: 'ok', bar: '', pct: sigPct(rssi) };
    if (rssi >= -75) return { text: '一般', cls: 'warn', bar: 'mid', pct: sigPct(rssi) };
    return { text: '偏弱', cls: 'err', bar: 'low', pct: sigPct(rssi) };
  }

  function playWtTone(rssi) {
    if (!state.wtSound || !rssi || rssi >= 0) return;
    try {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return;
      if (!state.wtAudioCtx) state.wtAudioCtx = new Ctx();
      const ctx = state.wtAudioCtx;
      if (ctx.state === 'suspended') ctx.resume();
      const now = ctx.currentTime;
      const clamped = Math.max(-88, Math.min(-35, rssi));
      const ratio = (clamped + 88) / 53;
      const freq = 260 + ratio * 660;
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = 'sine';
      osc.frequency.setValueAtTime(freq, now);
      gain.gain.setValueAtTime(0.0001, now);
      gain.gain.exponentialRampToValueAtTime(0.06, now + 0.012);
      gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.085);
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.start(now);
      osc.stop(now + 0.09);
    } catch { /* ignore audio errors */ }
  }

  function renderWtSvg() {
    const svg = $('#wtSvg');
    if (!svg) return;
    const tip = $('#wtTip');
    if (tip) tip.hidden = true;
    const { W, H } = measureSvgBox(svg, 145);
    const padL = 42, padR = 12, padT = 12, padB = 22;
    const iw = W - padL - padR;
    const ih = H - padT - padB;

    const cAccent = cssVar('--accent', '#0B6E7E');
    const cOk = cssVar('--ok', '#1F7A4D');
    const cErr = cssVar('--err', '#C0392B');
    const cInk3 = cssVar('--ink-3', '#5E6D7A');
    const cGrid = cssVar('--grid-line', '#E6ECEF');

    const minR = -90, maxR = -25;
    const yOf = r => {
      const c = Math.max(minR, Math.min(maxR, r));
      return padT + ih - ((c - minR) / (maxR - minR)) * ih;
    };

    let grid = '';
    for (const v of [-30, -45, -60, -75, -90]) {
      const y = yOf(v);
      const isGood = v === -60;
      const isWeak = v === -75;
      const stroke = isGood ? cOk : (isWeak ? cErr : cGrid);
      const dash = (isGood || isWeak) ? ' stroke-dasharray="4 4" stroke-width="1.2"' : ' stroke-width="1"';
      grid += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}" stroke="${stroke}"${dash}/>`;
      grid += `<text x="${padL - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${v}</text>`;
    }

    const pts = state.wtHistory;
    if (pts.length < 2) {
      svg.innerHTML = grid + `<text x="${W / 2}" y="${H / 2}" text-anchor="middle" fill="${cInk3}" font-size="12">正在采样实时信号波形…</text>`;
      state.chartMeta.walktest = null;
      return;
    }

    const n = pts.length;
    const xOf = i => padL + (i / (n - 1)) * iw;
    let dLine = '';
    let dArea = '';
    let inSeg = false;
    let segStartX = 0;
    let lastX = 0;
    let dots = '';

    for (let i = 0; i < n; i++) {
      const p = pts[i];
      if (!p.online || p.rssi >= 0) {
        if (inSeg) {
          dArea += ` L ${lastX.toFixed(1)} ${(padT + ih).toFixed(1)} L ${segStartX.toFixed(1)} ${(padT + ih).toFixed(1)} Z `;
          inSeg = false;
        }
        continue;
      }
      const x = xOf(i);
      const y = yOf(p.rssi);
      if (!inSeg) {
        dLine += `M ${x.toFixed(1)} ${y.toFixed(1)}`;
        dArea += `M ${x.toFixed(1)} ${(padT + ih).toFixed(1)} L ${x.toFixed(1)} ${y.toFixed(1)}`;
        segStartX = x;
        inSeg = true;
      } else {
        dLine += ` L ${x.toFixed(1)} ${y.toFixed(1)}`;
        dArea += ` L ${x.toFixed(1)} ${y.toFixed(1)}`;
      }
      lastX = x;
      if (i === n - 1) {
        dots += `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="3.5" fill="${cAccent}"/>`;
      }
    }
    if (inSeg) {
      dArea += ` L ${lastX.toFixed(1)} ${(padT + ih).toFixed(1)} Z`;
    }

    const validRssi = pts.filter(p => p.online && p.rssi < 0).map(p => p.rssi);
    if (validRssi.length) {
      const mn = Math.min(...validRssi);
      const mx = Math.max(...validRssi);
      const avg = (validRssi.reduce((a, b) => a + b, 0) / validRssi.length).toFixed(1);
      $('#wtSummary').textContent = `最弱 ${mn} / 平均 ${avg} / 最强 ${mx} dBm`;
    }

    const t0 = fmtHMS(Math.floor(pts[0].ts_ms / 1000));
    const t1 = fmtHMS(Math.floor(pts[n - 1].ts_ms / 1000));
    svg.innerHTML = grid +
      (dArea ? `<path d="${dArea}" fill="rgba(11,110,126,0.12)"/>` : '') +
      (dLine ? `<path d="${dLine}" fill="none" stroke="${cAccent}" stroke-width="2" stroke-linejoin="round"/>` : '') +
      dots +
      `<text x="${padL}" y="${H - 5}" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t0}</text>` +
      `<text x="${W - padR}" y="${H - 5}" text-anchor="end" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t1}</text>` +
      `<line class="chart-cursor" x1="0" y1="${padT}" x2="0" y2="${padT + ih}" stroke="${cInk3}" stroke-width="1" stroke-dasharray="2 2" opacity="0"/>`;

    state.chartMeta.walktest = {
      W, padL, padR, plotW: iw, count: n,
      format: idx => {
        const p = pts[idx];
        const tm = fmtHMS(Math.floor(p.ts_ms / 1000));
        if (!p.online) return `${tm} · 信号丢失 / 离线`;
        return `${tm} · ${p.rssi} dBm (${p.band}) · ↑${p.tx_mbps}/↓${p.rx_mbps} Mbps · RTT ${p.rtt_ms} ms`;
      },
    };
  }

  function renderWtSpots() {
    const tbody = $('#wtSpotRows');
    if (!tbody) return;
    if (state.wtSpots.length && $('#wtBody')) {
      $('#wtBody').hidden = false;
    }
    if (!state.wtSpots.length) {
      tbody.innerHTML = '<tr><td colspan="7" class="empty-cell">走到目标位置后点击「记录当前点位」，即可对比各角落信号覆盖（保存在 AP /tmp，跨设备同步）</td></tr>';
      return;
    }
    tbody.innerHTML = state.wtSpots.map(s => {
      const q = wtQuality(s.rssi, true);
      const ant = s.ant_rssi ? ` (${s.ant_rssi})` : '';
      const sub = [s.device, s.time].filter(Boolean).join(' · ');
      return `<tr>
        <td data-label="点位"><strong>${esc(s.spot)}</strong><div class="cell-sub mono">${esc(sub)}</div></td>
        <td data-label="频段 / 信道"><span class="band-pill ${s.band === '5G' ? 'b5' : ''}">${esc(s.band || '—')}</span> <span class="mono">${esc(s.chanspec || '')}</span></td>
        <td data-label="信号强度" class="mono"><span class="status-tag ${q.cls}">${s.rssi} dBm</span><span class="cell-sub">${esc(ant)}</span></td>
        <td data-label="SNR" class="mono">${s.snr > 0 ? `${s.snr} dB` : '—'}</td>
        <td data-label="速率" class="mono">↑ ${s.tx_mbps} / ↓ ${s.rx_mbps} Mbps</td>
        <td data-label="RTT / 重传" class="mono">${s.rtt_ms} ms · ${s.retry_pct}%</td>
        <td data-label="操作"><button type="button" class="btn-danger btn-sm" data-wt-del="${esc(s.id)}">删除</button></td>
      </tr>`;
    }).join('');
  }

  function renderWtSample(d, rttMs, deltaRetry) {
    const q = wtQuality(d.rssi, d.online);
    $('#wtRssiVal').textContent = (d.online && d.rssi < 0) ? `${d.rssi} dBm` : '已断开';
    const qTag = $('#wtQualityTag');
    qTag.className = 'status-tag ' + q.cls;
    qTag.textContent = q.text;
    const fill = $('#wtMeterFill');
    fill.className = 'wt-meter-fill ' + q.bar;
    fill.style.width = `${q.pct}%`;

    const idLine = $('#wtIdentityLine');
    if (!d.mac) {
      idLine.textContent = `未识别到本机无线 MAC（来源 ${d.caller_ip || '有线'}），请在右上角手动选择一台无线设备`;
    } else if (!d.online) {
      idLine.textContent = `目标 ${d.name || d.mac} 当前未连入本 AP（走出覆盖范围或有线接入）`;
    } else {
      const tag = d.auto_detected ? '自动识别本机' : '手动目标';
      idLine.textContent = `${tag}：${d.name || d.mac} (${d.mac}${d.ip ? ' · ' + d.ip : ''})`;
    }

    if (d.online) {
      const spec = [d.mode, d.width_nss].filter(Boolean).join(' · ');
      $('#wtBandChan').textContent = `${d.band} · ${d.chanspec}${spec ? ' · ' + spec : ''}`;
      $('#wtAntSnr').textContent = `${d.ant_rssi ? d.ant_rssi + ' dBm' : `${d.rssi} dBm`} · SNR ${d.snr} dB`;
      $('#wtRates').textContent = `↑ ${d.tx_mbps} / ↓ ${d.rx_mbps} Mbps`;
      $('#wtRttRetry').textContent = `${rttMs} ms · ${deltaRetry}%`;
    } else {
      $('#wtBandChan').textContent = '—';
      $('#wtAntSnr').textContent = '—';
      $('#wtRates').textContent = '—';
      $('#wtRttRetry').textContent = `${rttMs} ms · —`;
    }
  }

  function stopWalkTest() {
    state.wtActive = false;
    state.wtGen++;
    clearTimeout(state.wtTimer);
    state.wtTimer = null;
    const btn = $('#btnWtToggle');
    if (btn) {
      btn.textContent = '开始测信号';
      btn.classList.remove('btn-danger');
      btn.classList.add('btn-primary');
    }
  }

  function startWalkTest() {
    stopWalkTest();
    state.wtActive = true;
    const gen = state.wtGen;
    $('#wtBody').hidden = false;
    const btn = $('#btnWtToggle');
    if (btn) {
      btn.textContent = '停止测信号';
      btn.classList.remove('btn-primary');
      btn.classList.add('btn-danger');
    }

    const schedule = () => {
      if (!state.wtActive || gen !== state.wtGen) return;
      state.wtTimer = setTimeout(tick, 500);
    };

    const tick = async () => {
      if (!state.wtActive || gen !== state.wtGen) return;
      if (document.hidden) {
        schedule();
        return;
      }
      const mac = $('#wtTarget')?.value || '';
      const q = mac ? '?mac=' + encodeURIComponent(mac) : '';
      const t0 = performance.now();
      try {
        const d = await api('/api/wifi/walktest' + q, { timeoutMs: 4000 });
        if (!state.wtActive || gen !== state.wtGen) return;
        const rttMs = Number((performance.now() - t0).toFixed(1));
        let deltaRetry = d.retry_pct ?? 0;
        if (d.online && d.mac) {
          const prev = state.wtPrevTx;
          if (prev && prev.mac === d.mac && d.tx_sent >= prev.sent && d.tx_retries >= prev.retries) {
            const ds = d.tx_sent - prev.sent;
            const dr = d.tx_retries - prev.retries;
            deltaRetry = (ds + dr) > 0 ? Number((dr * 100 / (ds + dr)).toFixed(1)) : 0;
          }
          state.wtPrevTx = { mac: d.mac, sent: d.tx_sent || 0, retries: d.tx_retries || 0 };
        }
        const sample = {
          ts_ms: d.ts_ms || Date.now(),
          online: !!d.online,
          band: d.band || '',
          chanspec: d.chanspec || '',
          rssi: d.rssi || 0,
          ant_rssi: d.ant_rssi || '',
          snr: d.snr || 0,
          tx_mbps: d.tx_mbps || 0,
          rx_mbps: d.rx_mbps || 0,
          rtt_ms: rttMs,
          retry_pct: deltaRetry,
        };
        state.wtLast = { ...d, rtt_ms: rttMs, retry_pct: deltaRetry };
        state.wtHistory.push(sample);
        if (state.wtHistory.length > 120) state.wtHistory.shift();
        renderWtSample(d, rttMs, deltaRetry);
        renderWtSvg();
        if (d.online) playWtTone(d.rssi);
      } catch (e) {
        if (!isSilentErr(e) && state.wtActive && gen === state.wtGen) {
          $('#wtQualityTag').className = 'status-tag err';
          $('#wtQualityTag').textContent = '请求超时';
        }
      } finally {
        schedule();
      }
    };

    tick();
  }

  /* ── 24h RF Environment History ────────────────────────────────────── */

  async function refreshRFHistory(signal) {
    try {
      const d = await api('/api/rf/history?range=' + encodeURIComponent(state.rfRange), { signal });
      state.lastRf = d;
      renderRFHistory(d);
    } catch (e) {
      if (!isSilentErr(e)) toast(e.message, true);
    }
  }

  function renderRFHistory(d) {
    if (!d) return;
    const st = d.stats || {};
    $('#rfStat5Obss').textContent = `${st.avg_5g_obss ?? 0}% / ${st.max_5g_obss ?? 0}%`;
    $('#rfStat5Noise').textContent = st.avg_5g_noise ? `${st.avg_5g_noise} dBm` : '—';
    $('#rfStat2Obss').textContent = `${st.avg_24g_obss ?? 0}% / ${st.max_24g_obss ?? 0}%`;
    $('#rfStatCount').textContent = `${d.total_samples ?? 0} 分钟`;

    const samples = d.samples || [];
    const svg = $('#rfChartSvg');
    const legend = $('#rfLegend');
    const is5 = state.rfBand === '5g';
    const colors = [
      cssVar('--accent', '#0B6E7E'),
      cssVar('--warn', '#B86B14'),
      cssVar('--ok', '#1B7F4B'),
      '#6D28D9',
      cssVar('--err', '#C0392B'),
    ];
    const cInk3 = cssVar('--ink-3', '#5E6D7A');
    const cGrid = cssVar('--grid-line', '#E6ECEF');

    let series = [];
    let unit = '%';
    let minY = 0;
    let maxY = 100;

    if (state.rfMetric === 'air') {
      series = [
        { name: `${is5 ? '5G' : '2.4G'} 总繁忙`, color: colors[0], vals: samples.map(s => is5 ? s.w5_busy : s.w2_busy) },
        { name: `${is5 ? '5G' : '2.4G'} 邻居干扰 OBSS`, color: colors[1], vals: samples.map(s => is5 ? s.w5_obss : s.w2_obss) },
      ];
      const peak = Math.max(20, ...series.flatMap(x => x.vals));
      maxY = Math.min(100, Math.ceil(peak * 1.25 / 10) * 10);
    } else if (state.rfMetric === 'noise') {
      series = [
        { name: `${is5 ? '5G' : '2.4G'} 重传率 (%)`, color: colors[1], vals: samples.map(s => is5 ? s.w5_retrans : s.w2_retrans) },
        { name: `${is5 ? '5G' : '2.4G'} 底噪偏移 (+100 dBm)`, color: colors[0], vals: samples.map(s => {
          const n = is5 ? s.w5_noise : s.w2_noise;
          return n ? Math.max(0, n + 100) : 0;
        }) },
      ];
      unit = '';
      const peak = Math.max(30, ...series.flatMap(x => x.vals));
      maxY = Math.ceil(peak * 1.2 / 10) * 10;
    } else if (state.rfMetric === 'temp') {
      series = [
        { name: 'SoC 温度', color: colors[0], vals: samples.map(s => s.soc_temp) },
        { name: '5G 射频温度', color: colors[1], vals: samples.map(s => s.w5_temp) },
        { name: '2.4G 射频温度', color: colors[2], vals: samples.map(s => s.w2_temp) },
      ];
      unit = '°C';
      minY = 30;
      maxY = Math.max(80, Math.ceil(Math.max(...series.flatMap(x => x.vals), 75) / 10) * 10);
    } else {
      const labels = d.client_labels || {};
      const macs = Object.keys(labels).slice(0, 5);
      series = macs.map((mac, idx) => ({
        name: labels[mac],
        color: colors[idx % colors.length],
        vals: samples.map(s => (s.client_rssi && s.client_rssi[mac] != null) ? s.client_rssi[mac] : null),
      }));
      unit = ' dBm';
      minY = -95;
      maxY = -20;
    }

    legend.innerHTML = series.map(sr => {
      const valid = sr.vals.filter(v => v != null);
      const last = valid.length ? valid[valid.length - 1] : '—';
      return `<span class="rf-legend-item"><span class="rf-dot" style="background:${sr.color}"></span>${esc(sr.name)} <strong class="mono">${last}${last === '—' ? '' : unit}</strong></span>`;
    }).join('');

    const { W, H } = measureSvgBox(svg, 178);
    const padT = 12, padB = 24, padL = 44, padR = 10;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;
    const spanY = Math.max(1, maxY - minY);
    const yOf = v => padT + plotH - ((Math.max(minY, Math.min(maxY, v)) - minY) / spanY) * plotH;
    const n = Math.max(samples.length, 2);
    const xOf = i => padL + (i / (n - 1)) * plotW;

    let grid = '';
    for (let i = 0; i <= 3; i++) {
      const val = maxY - (spanY * i) / 3;
      const y = padT + (plotH * i) / 3;
      grid += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}" stroke="${cGrid}" stroke-width="1"/>`;
      grid += `<text x="${padL - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" fill="${cInk3}" font-size="11" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${Math.round(val)}</text>`;
    }
    if (samples.length >= 2) {
      for (const idx of [0, Math.floor(samples.length / 2), samples.length - 1]) {
        const x = xOf(idx);
        const anchor = idx === 0 ? 'start' : idx === samples.length - 1 ? 'end' : 'middle';
        grid += `<text x="${x.toFixed(1)}" y="${H - 6}" text-anchor="${anchor}" fill="${cInk3}" font-size="11" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${fmtHM(samples[idx].ts)}</text>`;
      }
    }

    let lines = '';
    const rfTip = $('#rfTip');
    if (rfTip) rfTip.hidden = true;
    if (samples.length >= 2) {
      for (const sr of series) {
        let path = '';
        let dots = '';
        let pen = false;
        sr.vals.forEach((v, i) => {
          if (v == null) { pen = false; return; }
          const x = xOf(i).toFixed(1);
          const y = yOf(v).toFixed(1);
          path += `${pen ? 'L' : 'M'}${x},${y} `;
          const prevNull = i === 0 || sr.vals[i - 1] == null;
          const nextNull = i === sr.vals.length - 1 || sr.vals[i + 1] == null;
          if (prevNull && nextNull) {
            dots += `<circle cx="${x}" cy="${y}" r="2.8" fill="${sr.color}"/>`;
          }
          pen = true;
        });
        if (path) {
          lines += `<path d="${path}" fill="none" stroke="${sr.color}" stroke-width="2" stroke-linejoin="round"/>`;
        }
        if (dots) lines += dots;
      }
      lines += `<line class="chart-cursor" x1="0" y1="${padT}" x2="0" y2="${padT + plotH}" stroke="${cInk3}" stroke-width="1" stroke-dasharray="2 2" opacity="0"/>`;
      state.chartMeta.rf = {
        W, padL, padR, plotW, count: samples.length,
        format: idx => {
          const parts = series.map(sr => {
            const v = sr.vals[idx];
            return `${sr.name} ${v == null ? '—' : v + unit}`;
          });
          return `${fmtHM(samples[idx].ts)} · ${parts.join(' · ')}`;
        },
      };
    } else {
      lines = `<text x="${W / 2}" y="${H / 2 - 4}" text-anchor="middle" fill="${cInk3}" font-size="12">已采集 ${samples.length} 点（每分钟自动采样，可点右上角「立即采样」立刻成图）</text>`;
      state.chartMeta.rf = null;
    }
    svg.innerHTML = grid + lines;

    const events = d.events || [];
    $('#rfEventCount').textContent = `${events.length} 条记录`;
    const kindMap = {
      join: ['上线', 'ok'],
      leave: ['离线', ''],
      band_switch: ['切频', 'warn'],
    };
    const tbody = $('#rfEventRows');
    tbody.innerHTML = events.length ? events.slice(0, 40).map(ev => {
      const [kl, kc] = kindMap[ev.kind] || [ev.kind, ''];
      return `
        <tr>
          <td data-label="时间" class="mono">${fmtHM(ev.ts)}</td>
          <td data-label="设备"><strong>${esc(ev.label)}</strong></td>
          <td data-label="事件"><span class="status-tag ${kc}">${esc(kl)}</span></td>
          <td data-label="频段"><span class="band-pill ${ev.band === '5G' ? 'b5' : ''}">${esc(ev.band)}</span></td>
          <td data-label="详情" class="meta-line">${esc(ev.detail)}</td>
        </tr>`;
    }).join('') : '<tr><td colspan="5" class="empty-cell">暂无无线事件（设备上下线与双频切换会自动记录）</td></tr>';
  }

  /* ── RF Tuning & Auto-Optimize ─────────────────────────────────────── */

  async function refreshTune(signal) {
    try {
      renderTune(await api('/api/tune', { signal }));
    } catch (e) {
      if (!isSilentErr(e)) toast(e.message, true);
    }
  }

  function renderTune(d) {
    const w1 = d.wl1 || {};
    const w0 = d.wl0 || {};
    const sc = d.sysctl || {};
    $('#tFrameburst').value = String(w1.frameburst ?? 1);
    $('#tAmpduRts').value = String(w1.ampdu_rts ?? 0);
    $('#tRxdesens').value = String(w1.phy_rxdesens ?? 15);
    $('#tWl1Pwr').value = String(w1.txpwr_percent ?? 100);
    $('#tWl0Pwr').value = String(w0.txpwr_percent ?? 100);
    $('#tTwt').value = String(w1.twt ?? 1);
    $('#tAtf').value = String(w1.atf ?? 0);
    $('#tApsd').value = String(w1.wme_apsd ?? 1);
    $('#tWl0He').value = String(w0.he ?? 0);
    $('#tWl0Width').value = String(w0.width_mhz ?? 20);

    const a1 = w1.air || {};
    const a0 = w0.air || {};
    const rows = [
      ['wl1 Beamforming / MU-MIMO', `${w1.txbf} / ${w1.mu_features}`],
      ['wl1 组播转单播 (wmf)', w1.wmf_bss_enable],
      ['wl1 AC_BE 窗口', w1.wme_be || '—'],
      ['wl1 干扰脉冲 (glitch / badplcp)', `${a1.glitch ?? 0} / ${a1.badplcp ?? 0}`],
      ['wl1 可用空口 (TXOP / 空闲)', `${a1.txop_pct ?? 0}% / ${a1.idle_pct ?? 0}%`],
      ['wl0 干扰脉冲 (glitch / badplcp)', `${a0.glitch ?? 0} / ${a0.badplcp ?? 0}`],
      ['wl0 frameburst / txbf', `${w0.frameburst} / ${w0.txbf}`],
      ['net.core.rmem_max / wmem_max', `${sc.rmem_max || '—'} / ${sc.wmem_max || '—'}`],
      ['netdev_max_backlog', sc.netdev_max_backlog || '—'],
    ];
    $('#tuneMeta').innerHTML = rows.map(([k, v]) =>
      `<div class="kv-row"><span class="kv-k">${esc(k)}</span><span class="kv-v">${esc(v)}</span></div>`
    ).join('');
  }

  function renderOptChannels(el, rep) {
    const list = rep?.candidates || [];
    el.innerHTML = list.map(c => {
      const cls = c.score >= 70 ? 'good' : c.score >= 40 ? 'mid' : 'bad';
      const tags = [
        c.is_current ? '当前' : '',
        c.is_recommended ? '推荐' : '',
      ].filter(Boolean).join('/');
      const detail = c.primary_count > 0
        ? `同主信道 ${c.primary_count} 个${c.strongest_rssi ? ` · 最强 ${c.strongest_ssid} (${c.strongest_rssi} dBm)` : ''}`
        : `同主信道 0 个 · 邻/副道重叠 ${c.overlap_count} 个${c.strongest_rssi ? ` (${c.strongest_rssi} dBm)` : ''}`;
      return `
        <div class="opt-ch-row">
          <span class="mono"><strong>CH ${c.channel}</strong>${tags ? ` <span class="sub-pill">${tags}</span>` : ''}</span>
          <div class="opt-ch-bar"><div class="opt-ch-fill ${cls}" style="width:${c.score}%"></div></div>
          <span class="mono opt-ch-score">${c.score} 分</span>
          <div class="opt-ch-meta">${esc(detail)}</div>
        </div>`;
    }).join('');
  }

  function renderOptReport(d) {
    state.lastOpt = d;
    $('#optBody').hidden = false;
    const tag = $('#optScoreTag');
    tag.className = 'status-tag ' + (d.changes === 0 ? 'ok' : 'warn');
    tag.textContent = d.changes === 0
      ? `健康分 ${d.health_score} · 已是最佳状态`
      : `健康分 ${d.health_score} · ${d.changes} 项可优化`;

    $('#optSummary').textContent = d.changes === 0
      ? '双频信道与射频参数均已处于当前环境最优状态，无需调整。'
      : `体检完成，发现 ${d.changes} 项可优化项，点击「一键应用最优方案」即可生效。`;

    $('#opt5gSum').textContent = d.band5g?.summary || '';
    $('#opt24gSum').textContent = d.band24g?.summary || '';
    renderOptChannels($('#opt5gList'), d.band5g);
    renderOptChannels($('#opt24gList'), d.band24g);

    $('#optItems').innerHTML = (d.items || []).map(it => `
      <tr>
        <td data-label="体检项"><strong>${esc(it.label)}</strong></td>
        <td data-label="当前值" class="mono">${esc(it.current)}</td>
        <td data-label="建议值" class="mono">${esc(it.recommended)}</td>
        <td data-label="状态"><span class="status-tag ${it.changed ? 'warn' : 'ok'}">${it.changed ? (it.disruptive ? '可优化 (需重连)' : '可优化 (热生效)') : '已最优'}</span></td>
        <td data-label="依据" class="meta-line">${esc(it.reason)}</td>
      </tr>
    `).join('');

    $('#optMeta').textContent = `体检时间 ${d.analyzed_at || ''} · 扫描周边 5G ${d.band5g?.neighbor_count ?? 0} 个 / 2.4G ${d.band24g?.neighbor_count ?? 0} 个`;
  }

  async function runOptAnalyze() {
    const btn = $('#btnOptAnalyze');
    return withBusy(btn, async () => {
      $('#optScoreTag').className = 'status-tag';
      $('#optScoreTag').textContent = '扫描分析中...';
      try {
        const d = await api('/api/tune/analyze', { method: 'POST', timeoutMs: 30000 });
        renderOptReport(d);
        toast('智能体检完成');
        return d;
      } catch (e) {
        if (!isSilentErr(e)) toast(e.message, true);
        $('#optScoreTag').textContent = '体检失败';
        return null;
      }
    });
  }

  async function measureWirelessRTT() {
    const headers = state.token ? { 'X-Token': state.token } : {};
    const samples = [];
    for (let i = 0; i < 12; i++) {
      const ctrl = new AbortController();
      const timer = setTimeout(() => ctrl.abort(), 3000);
      const t0 = performance.now();
      let res;
      try {
        res = await fetch('/api/ping?_=' + Date.now() + '_' + i, { headers, cache: 'no-store', signal: ctrl.signal });
        if (res.status === 401) {
          state.token = '';
          localStorage.removeItem('ap_token');
          showLogin();
          const err = new Error('unauthorized');
          err.status = 401;
          throw err;
        }
        if (!res.ok) throw new Error(`延迟探测失败 (HTTP ${res.status})`);
        await res.text();
      } catch (err) {
        if (err && err.name === 'AbortError') {
          throw new Error('延迟探测超时，请等待无线重连稳定后重试');
        }
        throw err;
      } finally {
        clearTimeout(timer);
      }
      const dt = performance.now() - t0;
      if (i >= 2) samples.push(dt);
    }
    const sorted = samples.slice().sort((a, b) => a - b);
    const p50 = sorted[Math.floor(sorted.length * 0.5)] || 0;
    const p95 = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.9))] || 0;
    let jitSum = 0;
    for (let i = 1; i < samples.length; i++) jitSum += Math.abs(samples[i] - samples[i - 1]);
    const jitter = samples.length > 1 ? jitSum / (samples.length - 1) : 0;
    return {
      p50: Number(p50.toFixed(2)),
      p95: Number(p95.toFixed(2)),
      jitter: Number(jitter.toFixed(2)),
    };
  }

  async function captureOptBenchmark() {
    const [rtt, snap] = await Promise.all([measureWirelessRTT(), api('/api/tune/snapshot')]);
    return { rtt, snap };
  }

  function fmtDelta(before, after, unit, lowerBetter) {
    if (before == null || after == null) return '<span class="meta-line">—</span>';
    const diff = Number((after - before).toFixed(2));
    if (Math.abs(diff) < 0.05) return '<span class="status-tag">持平</span>';
    const improved = lowerBetter ? diff < 0 : diff > 0;
    const sign = diff > 0 ? '+' : '';
    return `<span class="status-tag ${improved ? 'ok' : 'warn'}">${sign}${diff}${unit}</span>`;
  }

  function renderOptCompare(before, after, title) {
    $('#optCompareWrap').hidden = false;
    $('#optCompareTitle').textContent = title || '优化前后实测对比（硬件空口快照 + 无线延迟）';
    const bR = before?.rtt || {}, aR = after?.rtt || {};
    const bS = before?.snap || {}, aS = after?.snap || {};
    const single = !after;
    const rows = [
      ['无线延迟 P50 (中位数)', `${bR.p50 ?? '—'} ms`, single ? '—' : `${aR.p50 ?? '—'} ms`, single ? '<span class="status-tag ok">实时基准</span>' : fmtDelta(bR.p50, aR.p50, ' ms', true)],
      ['无线延迟 P95 (尾延迟)', `${bR.p95 ?? '—'} ms`, single ? '—' : `${aR.p95 ?? '—'} ms`, single ? '—' : fmtDelta(bR.p95, aR.p95, ' ms', true)],
      ['无线抖动 (Jitter)', `${bR.jitter ?? '—'} ms`, single ? '—' : `${aR.jitter ?? '—'} ms`, single ? '—' : fmtDelta(bR.jitter, aR.jitter, ' ms', true)],
      ['5G 空口可用率 (TXOP)', `${bS.txop_5g ?? '—'}%`, single ? '—' : `${aS.txop_5g ?? '—'}%`, single ? '—' : fmtDelta(bS.txop_5g, aS.txop_5g, '%', false)],
      ['5G 邻居干扰 (OBSS)', `${bS.obss_5g ?? '—'}%`, single ? '—' : `${aS.obss_5g ?? '—'}%`, single ? '—' : fmtDelta(bS.obss_5g, aS.obss_5g, '%', true)],
      ['5G 实时窗口重传率 (Δ)', `${bS.delta_retrans_pct ?? '—'}%`, single ? '—' : `${aS.delta_retrans_pct ?? '—'}%`, single ? '—' : fmtDelta(bS.delta_retrans_pct, aS.delta_retrans_pct, '%', true)],
      ['5G 底噪 / 干扰脉冲', `${bS.noise_5g ?? '—'} dBm / ${bS.glitch_5g ?? 0}`, single ? '—' : `${aS.noise_5g ?? '—'} dBm / ${aS.glitch_5g ?? 0}`, single ? '—' : fmtDelta(bS.glitch_5g, aS.glitch_5g, ' glitch', true)],
      ['5G 终端平均协商速率', `${bS.avg_tx_mbps ?? 0} / ${bS.avg_rx_mbps ?? 0} Mbps`, single ? '—' : `${aS.avg_tx_mbps ?? 0} / ${aS.avg_rx_mbps ?? 0} Mbps`, single ? '—' : fmtDelta(bS.avg_tx_mbps, aS.avg_tx_mbps, ' Mbps', false)],
    ];
    $('#optCompareRows').innerHTML = rows.map(([k, b, a, d]) => `
      <tr>
        <td data-label="指标"><strong>${esc(k)}</strong></td>
        <td data-label="优化前" class="mono">${esc(b)}</td>
        <td data-label="优化后" class="mono">${esc(a)}</td>
        <td data-label="变化" class="mono">${d}</td>
      </tr>
    `).join('');
  }

  /* ── CSI Presence Radar ────────────────────────────────────────────── */

  function renderRadarSvg(points, microTh, motionTh) {
    const svg = $('#radarSvg');
    if (!svg) return;
    const tip = $('#radarTip');
    if (tip) tip.hidden = true;
    const { W, H } = measureSvgBox(svg, 160);
    const padL = 38, padR = 12, padT = 12, padB = 22;
    const iw = W - padL - padR;
    const ih = H - padT - padB;

    const cAccent = cssVar('--accent', '#0B6E7E');
    const cWarn = cssVar('--warn', '#B86B14');
    const cErr = cssVar('--err', '#C0392B');
    const cInk3 = cssVar('--ink-3', '#5E6D7A');
    const cGrid = cssVar('--grid-line', '#E6ECEF');

    const vals = (points || []).map(p => Number(p.score) || 0);
    const maxV = Math.max(motionTh * 1.4, 20, ...vals);
    const yOf = v => padT + ih - (Math.min(v, maxV) / maxV) * ih;

    let grid = '';
    for (let i = 0; i <= 4; i++) {
      const v = (maxV / 4) * i;
      const y = yOf(v);
      grid += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}" stroke="${cGrid}" stroke-width="1"/>`;
      grid += `<text x="${padL - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" fill="${cInk3}" font-size="11" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${v.toFixed(0)}</text>`;
    }
    const yMic = yOf(microTh);
    const yMot = yOf(motionTh);
    grid += `<line x1="${padL}" y1="${yMic.toFixed(1)}" x2="${W - padR}" y2="${yMic.toFixed(1)}" stroke="${cWarn}" stroke-dasharray="4 4" stroke-width="1.3"/>`;
    grid += `<line x1="${padL}" y1="${yMot.toFixed(1)}" x2="${W - padR}" y2="${yMot.toFixed(1)}" stroke="${cErr}" stroke-dasharray="4 4" stroke-width="1.3"/>`;

    if (!points || points.length < 2) {
      svg.innerHTML = grid + `<text x="${W / 2}" y="${H / 2}" text-anchor="middle" fill="${cInk3}" font-size="12">等待实时 CSI 数据帧（开启后约 2 秒出曲线）</text>`;
      state.chartMeta.radar = null;
      return;
    }

    const n = points.length;
    const xOf = i => padL + (i / (n - 1)) * iw;
    let dLine = '';
    let dArea = `M ${xOf(0).toFixed(1)} ${(padT + ih).toFixed(1)}`;
    for (let i = 0; i < n; i++) {
      const x = xOf(i).toFixed(1);
      const y = yOf(vals[i]).toFixed(1);
      dLine += (i === 0 ? 'M ' : ' L ') + x + ' ' + y;
      dArea += ' L ' + x + ' ' + y;
    }
    dArea += ` L ${xOf(n - 1).toFixed(1)} ${(padT + ih).toFixed(1)} Z`;

    const t0 = fmtHMS(points[0].ts);
    const t1 = fmtHMS(points[n - 1].ts);
    svg.innerHTML = grid +
      `<path d="${dArea}" fill="rgba(11,110,126,0.12)"/>` +
      `<path d="${dLine}" fill="none" stroke="${cAccent}" stroke-width="2" stroke-linejoin="round"/>` +
      `<text x="${padL}" y="${H - 5}" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t0}</text>` +
      `<text x="${W - padR}" y="${H - 5}" text-anchor="end" fill="${cInk3}" font-size="10" font-family="ui-monospace,SFMono-Regular,Menlo,monospace">${t1}</text>` +
      `<line class="chart-cursor" x1="0" y1="${padT}" x2="0" y2="${padT + ih}" stroke="${cInk3}" stroke-width="1" stroke-dasharray="2 2" opacity="0"/>`;

    state.chartMeta.radar = {
      W, padL, padR, plotW: iw, count: n,
      format: idx => `${fmtHMS(points[idx].ts)} · 运动指数 ${vals[idx].toFixed(2)} (微动 ${microTh} / 有人 ${motionTh})`,
    };
  }

  function candidateSignature(cands) {
    return cands.map(c => `${c.mac}|${c.iface}|${c.note || ''}|${c.rssi}`).join(';');
  }

  async function refreshRadar(syncForm, signal) {
    try {
      const d = await api('/api/radar', { signal });
      state.lastRadar = d;
      const cfg = d.config || {};
      const tag = $('#radarStateTag');
      if (!d.supported) {
        tag.className = 'status-tag err';
        tag.textContent = '驱动未打 CSIMON 补丁';
      } else if (!cfg.enabled) {
        tag.className = 'status-tag';
        tag.textContent = '未启用';
      } else if (d.calibrating) {
        tag.className = 'status-tag warn';
        tag.textContent = `空房标定中 ${d.cal_progress || 0}%`;
      } else if (d.state === 'dma_fault') {
        tag.className = 'status-tag err';
        tag.textContent = '硬件 DMA 异常';
      } else if (d.occupied) {
        tag.className = 'status-tag err';
        tag.textContent = d.state === 'motion' ? '有人活动' : '有人（保持中）';
      } else if (d.state === 'waiting') {
        tag.className = 'status-tag';
        tag.textContent = '等待目标上线';
      } else if (d.state === 'micro') {
        tag.className = 'status-tag warn';
        tag.textContent = '轻微扰动';
      } else {
        tag.className = 'status-tag ok';
        tag.textContent = '无人静止';
      }

      $('#radarScore').textContent = cfg.enabled ? Number(d.score || 0).toFixed(2) : '—';
      $('#radarPhase').textContent = cfg.enabled ? `${Number(d.phase_mad || 0).toFixed(2)}°` : '—';
      $('#radarRssi').textContent = (cfg.enabled && d.frames > 0) ? `${d.rssi0} / ${d.rssi1} dBm` : '—';
      $('#radarFrames').textContent = `${d.frames || 0} / NL ${d.netlink_proto > 0 ? d.netlink_proto : '—'}`;
      const staleNote = d.baseline_stale
        ? `当前信道 (${d.chanspec}) 与上次标定 (${cfg.baseline?.chanspec || '未知'}) 不一致，建议空房重新标定`
        : '';
      $('#radarCalNote').textContent = d.fault_message || d.cal_message || staleNote || (d.supported
        ? `硬件就绪：wl0 → Netlink ${d.proto_wl0}，wl1 → Netlink ${d.proto_wl1}`
        : '当前 wl.ko 尚未应用 3 字节 Netlink 补丁，刷入新版 Alpine 镜像即可启用。');

      const cands = Array.isArray(d.candidates) ? d.candidates : [];
      const sig = candidateSignature(cands);
      const pick = $('#radarTargetPick');
      if (sig !== state.radarCandSig && document.activeElement !== pick) {
        state.radarCandSig = sig;
        const dl = $('#radarCandidates');
        if (dl) {
          dl.innerHTML = cands.map(c => {
            const band = c.iface === 'wl1' ? '5G' : '2.4G';
            const namePart = c.note ? `${c.note} / ${c.mac}` : c.mac;
            return `<option value="${esc(c.mac)}">${esc(namePart)} (${band} · ${c.rssi} dBm)</option>`;
          }).join('');
        }
        if (pick) {
          const curMac = ($('#radarTarget')?.value || cfg.target_mac || '').toUpperCase();
          pick.innerHTML = '<option value="">选择当前已连接设备…</option>' + cands.map(c => {
            const band = c.iface === 'wl1' ? '5G' : '2.4G';
            const namePart = c.note ? `${c.note} / ${c.mac}` : c.mac;
            const sel = c.mac.toUpperCase() === curMac ? ' selected' : '';
            return `<option value="${esc(c.mac)}" data-iface="${esc(c.iface)}"${sel}>${esc(namePart)} (${band} · ${c.rssi} dBm)</option>`;
          }).join('');
        }
      }

      if (syncForm || !state.radarFormHydrated) {
        state.radarFormHydrated = true;
        $('#radarEnabled').checked = !!cfg.enabled;
        $('#radarIface').value = cfg.iface || 'wl0';
        $('#radarTarget').value = cfg.target_mac || '';
        if (pick && document.activeElement !== pick) {
          setSelectSafe(pick, (cfg.target_mac || '').toUpperCase());
        }
        $('#radarInterval').value = cfg.interval_ms || 200;
        $('#radarMicro').value = cfg.micro_thresh || 3.5;
        $('#radarMotion').value = cfg.motion_thresh || 12;
        $('#radarConfirm').value = cfg.confirm_sec || 2;
        $('#radarHold').value = cfg.hold_sec || 30;
        $('#radarWebhook').value = cfg.webhook_url || '';
      }

      renderRadarSvg(d.live_points || [], cfg.micro_thresh || 3.5, cfg.motion_thresh || 12);

      const evs = (d.events || []).slice().reverse();
      $('#radarEventCount').textContent = `${evs.length} 条记录`;
      $('#radarEventRows').innerHTML = evs.length ? evs.map(ev => {
        const tm = new Date(ev.ts * 1000).toLocaleString('zh-CN', { hour12: false });
        const cls = ev.to === 'occupied' ? 'err' : 'ok';
        const lbl = ev.to === 'occupied' ? '无人 → 有人' : '有人 → 无人';
        return `<tr>
          <td data-label="时间" class="mono">${esc(tm)}</td>
          <td data-label="状态变化"><span class="status-tag ${cls}">${lbl}</span></td>
          <td data-label="峰值分数" class="mono">${Number(ev.peak_score || 0).toFixed(2)}</td>
          <td data-label="详情" class="meta-line">${esc(ev.detail)}</td>
        </tr>`;
      }).join('') : '<tr><td colspan="4" class="empty-cell">暂无状态切换记录</td></tr>';
    } catch (e) {
      if (!isSilentErr(e)) console.warn('radar refresh failed:', e);
    }
  }

  /* ── STA Monitor (Unassociated MAC Proximity) ──────────────────────── */

  function targetsSig(list) {
    return (list || []).map(t => `${(t.mac || '').toUpperCase()}|${t.label || ''}`).join(';');
  }

  function updateStaMonDirtyIndicator() {
    const dirtyTag = $('#stamonDirtyTag');
    if (!dirtyTag) return;
    dirtyTag.hidden = targetsSig(state.stamonDraftTargets) === state.stamonSavedSig;
  }

  function renderStaMonTargets() {
    const tbody = $('#stamonRows');
    if (!tbody) return;
    updateStaMonDirtyIndicator();
    if (!state.stamonDraftTargets.length) {
      tbody.innerHTML = '<tr><td colspan="7" class="empty-cell">暂未添加监听设备（可在下方选择已连接设备或手动填入任意 MAC）</td></tr>';
      return;
    }
    tbody.innerHTML = state.stamonDraftTargets.map((t, idx) => {
      const mac = (t.mac || '').toUpperCase();
      const st = state.stamonLastStateMap[mac] || {};
      const r2 = st.rssi_24g < 0 ? `${st.rssi_24g} dBm` : '—';
      const r5 = st.rssi_5g < 0 ? `${st.rssi_5g} dBm` : '—';
      const seen = st.last_seen_ts ? new Date(st.last_seen_ts * 1000).toLocaleTimeString('zh-CN', { hour12: false }) : '尚未捕获';
      const tag = st.present
        ? '<span class="status-tag ok">在场</span>'
        : (st.last_seen_ts ? '<span class="status-tag">已离开</span>' : '<span class="status-tag">等待抓包</span>');
      return `<tr>
        <td data-label="设备名称"><strong>${esc(t.label || st.label || mac)}</strong></td>
        <td data-label="MAC 地址" class="mono">${esc(mac)}</td>
        <td data-label="状态">${tag}</td>
        <td data-label="2.4G RSSI" class="mono">${r2}</td>
        <td data-label="5G RSSI" class="mono">${r5}</td>
        <td data-label="最后捕获" class="mono">${esc(seen)}${st.seen_count ? ` (${st.seen_count}次)` : ''}</td>
        <td data-label="操作"><button type="button" class="btn-danger btn-sm" data-stamon-del="${idx}">移除</button></td>
      </tr>`;
    }).join('');
  }

  async function refreshStaMon(syncForm, signal) {
    try {
      const d = await api('/api/stamon', { signal });
      const cfg = d.config || {};
      const targets = Array.isArray(d.targets) ? d.targets : [];
      state.stamonLastStateMap = {};
      for (const t of targets) state.stamonLastStateMap[(t.mac || '').toUpperCase()] = t;

      const tag = $('#stamonStateTag');
      if (tag) {
        if (!cfg.enabled) {
          tag.className = 'status-tag';
          tag.textContent = '未启用';
        } else if (!targets.length) {
          tag.className = 'status-tag warn';
          tag.textContent = '已启用（未设目标）';
        } else if (d.present_count > 0) {
          tag.className = 'status-tag ok';
          tag.textContent = `监听中 · ${d.present_count} 台在场`;
        } else {
          tag.className = 'status-tag';
          tag.textContent = '监听中 · 均离开';
        }
      }
      const bandMap = { both: '双频 (wl0+wl1)', wl1: '仅 5G (wl1)', wl0: '仅 2.4G (wl0)' };
      $('#stamonCount').textContent = `${d.present_count || 0} / ${targets.length}`;
      $('#stamonBandLabel').textContent = bandMap[cfg.band] || '双频 (wl0+wl1)';
      $('#stamonThreshLabel').textContent = `${cfg.rssi_thresh || -75} dBm`;
      $('#stamonAwayLabel').textContent = `${cfg.away_sec || 180} s`;

      const pick = $('#stamonTargetPick');
      const cands = Array.isArray(d.candidates) ? d.candidates : [];
      const sig = candidateSignature(cands);
      if (pick && sig !== state.stamonCandSig && document.activeElement !== pick) {
        state.stamonCandSig = sig;
        const prev = pick.value;
        pick.innerHTML = '<option value="">从当前已连接设备快捷填入…</option>' + cands.map(c => {
          const band = c.iface === 'wl1' ? '5G' : '2.4G';
          const namePart = c.note ? `${c.note} / ${c.mac}` : c.mac;
          return `<option value="${esc(c.mac)}" data-note="${esc(c.note || '')}">${esc(namePart)} (${band} · ${c.rssi} dBm)</option>`;
        }).join('');
        setSelectSafe(pick, prev);
      }

      if (syncForm || !state.stamonFormHydrated) {
        state.stamonFormHydrated = true;
        state.stamonDraftTargets = (cfg.targets || []).map(t => ({ mac: (t.mac || '').toUpperCase(), label: t.label || '' }));
        state.stamonSavedSig = targetsSig(state.stamonDraftTargets);
        $('#stamonEnabled').checked = !!cfg.enabled;
        $('#stamonBand').value = cfg.band || 'both';
        $('#stamonThresh').value = cfg.rssi_thresh || -75;
        $('#stamonAway').value = cfg.away_sec || 180;
        $('#stamonWebhook').value = cfg.webhook_url || '';
      }
      renderStaMonTargets();

      const evs = (d.events || []).slice().reverse();
      $('#stamonEventCount').textContent = `${evs.length} 条记录`;
      $('#stamonEventRows').innerHTML = evs.length ? evs.map(ev => {
        const tm = new Date(ev.ts * 1000).toLocaleString('zh-CN', { hour12: false });
        return `<tr>
          <td data-label="时间" class="mono">${esc(tm)}</td>
          <td data-label="设备"><strong>${esc(ev.label || ev.mac)}</strong></td>
          <td data-label="状态"><span class="status-tag ${ev.present ? 'ok' : ''}">${ev.present ? '到场' : '离开'}</span></td>
          <td data-label="详情" class="meta-line">${esc(ev.detail)}</td>
        </tr>`;
      }).join('') : '<tr><td colspan="4" class="empty-cell">暂无到场 / 离开记录</td></tr>';
    } catch (e) {
      if (!isSilentErr(e)) console.warn('stamon refresh failed:', e);
    }
  }

  /* ── Network & Diagnostics ─────────────────────────────────────────── */

  function toggleStaticFields() {
    const isStatic = $('#netMode').value === 'static';
    $$('.net-static-field').forEach(el => { el.hidden = !isStatic; });
  }

  async function refreshNetwork(signal) {
    try {
      const d = await api('/api/network', { signal });
      const ports = d.ports || [];
      $('#portRows').innerHTML = ports.map(p => `
        <tr>
          <td data-label="网口" class="mono"><strong>${esc(p.iface)}</strong></td>
          <td data-label="角色">${esc(p.role)}</td>
          <td data-label="链路"><span class="status-tag ${p.link ? 'ok' : ''}">${p.link ? '已连接' : '未插线'}</span></td>
          <td data-label="速率 / 双工" class="mono">${esc(p.speed)}${p.duplex ? ' ' + esc(p.duplex) : ''}</td>
          <td data-label="MAC" class="mono">${esc(p.mac)}</td>
          <td data-label="收 / 发流量" class="mono">${fmtBytes(p.rx_bytes)} / ${fmtBytes(p.tx_bytes)}</td>
          <td data-label="包数" class="mono">${p.rx_packets} / ${p.tx_packets}</td>
          <td data-label="错包" class="mono">${(p.rx_errors || p.tx_errors) ? `<span class="status-tag err">${p.rx_errors} / ${p.tx_errors}</span>` : '0 / 0'}</td>
        </tr>
      `).join('');

      const nc = d.config || {};
      const f = $('#formNetwork');
      f.mode.value = nc.mode || 'dhcp';
      f.hostname.value = nc.hostname || 'hg6371f-ap';
      f.ip.value = nc.ip || '';
      f.gateway.value = nc.gateway || '';
      f.dns.value = nc.dns || '';
      $('#netCurIp').textContent = `${nc.cur_ip || '无 IP'} · GW ${nc.cur_gw || '—'}`;
      toggleStaticFields();

      const fdb = d.fdb || [];
      $('#fdbCount').textContent = `${fdb.length} 条动态记录`;
      $('#fdbRows').innerHTML = fdb.length ? fdb.map(x => `
        <tr>
          <td data-label="端口"><span class="band-pill ${x.port === 'wl1' ? 'b5' : ''}">${esc(x.port)}</span></td>
          <td data-label="MAC" class="mono">${esc(x.mac)}</td>
          <td data-label="IP" class="mono">${esc(x.ip || '—')}</td>
          <td data-label="备注">${esc(x.note || '—')}</td>
        </tr>
      `).join('') : '<tr><td colspan="4" class="empty-cell">暂无转发表项</td></tr>';
    } catch (e) {
      if (!isSilentErr(e)) toast(e.message, true);
    }
  }

  /* ── System & Maintenance ──────────────────────────────────────────── */

  async function refreshSystem(signal) {
    try {
      const d = await api('/api/system', { signal });
      const sl = d.slots || {};
      $('#slotSummary').textContent = `当前 Slot ${sl.booted || '—'} / 下次启动 Slot ${sl.reboot || '—'}`;
      $('#slotCards').innerHTML = [1, 2].map(num => {
        const isBooted = sl.booted === num;
        const isReboot = sl.reboot === num;
        const isCommitted = sl.committed === num;
        const commitDone = isCommitted && isReboot;
        return `
          <div class="slot-box ${isBooted ? 'active' : ''}">
            <div class="slot-title">
              <span>Slot ${num} (${num === 1 ? 'A' : 'B'})</span>
              <span class="slot-badges">
                ${isBooted ? '<span class="status-tag ok">当前运行</span>' : '<span class="status-tag">备用</span>'}
                ${isCommitted ? '<span class="status-tag ok">已固化</span>' : ''}
              </span>
            </div>
            <div class="meta-line">下次重启目标：${isReboot ? '是' : '否'}</div>
            <div class="slot-actions">
              <button type="button" class="btn-secondary btn-sm" data-slot="${num}" data-mode="once">下次临时启动</button>
              <button type="button" class="${commitDone ? 'btn-secondary' : 'btn-primary'} btn-sm" data-slot="${num}" data-mode="commit"${commitDone ? ' disabled' : ''}>${commitDone ? '当前已固化' : '设为永久默认'}</button>
            </div>
          </div>`;
      }).join('');

      const pf = $('#formPrefs');
      const led = d.led || {};
      const btn = d.button || {};
      const ws = d.wifi_schedule || {};
      pf.led_mode.value = led.mode || 'auto';
      pf.night_start.value = led.night_start || '23:00';
      pf.night_end.value = led.night_end || '07:00';
      pf.btn_short.value = btn.short_action || 'toggle_led';
      pf.btn_long.value = btn.long_action || 'toggle_guest';
      pf.ws_band.value = ws.band || 'wl0';
      pf.ws_start.value = ws.off_start || '01:00';
      pf.ws_end.value = ws.off_end || '06:30';
      pf.ws_enabled.checked = !!ws.enabled;
      const lt = $('#ledCurTag');
      lt.className = 'status-tag ' + (d.led_state === 'blue' ? 'ok' : d.led_state === 'red' ? 'err' : '');
      lt.textContent = d.led_state === 'blue' ? '当前：蓝灯' : d.led_state === 'red' ? '当前：红灯' : '当前：熄灯';

      const hs = $('#httpsState');
      hs.className = 'status-tag ' + (d.tls_enabled ? 'ok' : 'warn');
      hs.textContent = d.tls_enabled ? 'HTTPS :443 运行中' : '未配置证书（仅 HTTP :80）';
      $('#tlsRedirect').checked = !!d.tls_redirect;

      $('#svcRows').innerHTML = (d.services || []).map(s => `
        <tr>
          <td data-label="服务" class="mono">${esc(s.name)}</td>
          <td data-label="状态"><span class="status-tag ${s.status === 'started' ? 'ok' : 'err'}">${esc(s.status)}</span></td>
          <td data-label="操作">${s.can_action
            ? `<button type="button" class="btn-secondary btn-sm" data-svc="${esc(s.name)}">重启</button>`
            : '<span class="hint">当前进程</span>'}</td>
        </tr>
      `).join('');
      await refreshMqtt(signal);
    } catch (e) {
      if (!isSilentErr(e)) toast(e.message, true);
    }
  }

  async function refreshMqtt(signal) {
    const f = $('#formMqtt');
    if (!f) return;
    try {
      const m = await api('/api/mqtt', { signal });
      const cfg = m.config || {};
      const tag = $('#mqttStateTag');
      if (!cfg.enabled) {
        tag.className = 'status-tag';
        tag.textContent = '未启用';
      } else if (m.connected) {
        tag.className = 'status-tag ok';
        tag.textContent = '已连接';
      } else {
        tag.className = 'status-tag err';
        tag.textContent = '未连接';
      }

      if (document.activeElement?.closest('#formMqtt') !== f) {
        $('#mqttEnabled').checked = !!cfg.enabled;
        $('#mqttBroker').value = cfg.broker || '';
        $('#mqttUser').value = cfg.username || '';
        $('#mqttPass').value = '';
        $('#mqttPass').placeholder = m.has_password ? '已保存（留空保持不变）' : '留空表示无密码';
        $('#mqttClearPass').checked = false;
        $('#mqttPrefix').value = cfg.topic_prefix || 'hg6371f-ap';
        $('#mqttDiscPrefix').value = cfg.discovery_prefix || 'homeassistant';
        $('#mqttClientId').value = cfg.client_id || 'hg6371f-ap';
        $('#mqttInterval').value = cfg.interval_sec || 30;
        $('#mqttDiscovery').checked = cfg.discovery !== false;
        $('#mqttAllowCtrl').checked = cfg.allow_control !== false;
        $('#mqttTlsInsecure').checked = !!cfg.tls_insecure;
      }

      const hint = $('#mqttHint');
      if (m.connected) {
        const lastStr = m.last_pub_sec >= 0 ? `${m.last_pub_sec}s 前上报` : '等待首次上报';
        hint.textContent = `在线 ${fmtDur(m.connected_sec || 0)} · 已注册 ${m.discovery_count || 0} 实体 · 发 ${m.pub_count || 0} / 收 ${m.rx_count || 0} · ${lastStr}`;
      } else if (m.last_error) {
        hint.textContent = `错误：${m.last_error}`;
      } else {
        hint.textContent = `设备 ID：${m.device_id || 'hg6371f'} · 保存并启用后立即连接 Broker`;
      }
    } catch (e) {
      if (!isSilentErr(e)) $('#mqttHint').textContent = '状态获取失败：' + e.message;
    }
  }

  async function refreshLogs(signal) {
    try {
      const d = await api('/api/system/logs?src=' + encodeURIComponent(state.logSrc), { signal });
      const box = $('#logBox');
      box.textContent = (d.lines || []).join('\n') || '(暂无日志)';
      box.scrollTop = box.scrollHeight;
    } catch (e) {
      if (!isSilentErr(e)) $('#logBox').textContent = '日志读取失败：' + e.message;
    }
  }

  /* ── Back-Pressure Polling Scheduler (no overlapping requests) ─────── */

  function stopPolling() {
    state.pollGen++;
    clearTimeout(state.pollTimer);
    state.pollTimer = null;
  }

  function startPolling() {
    stopPolling();
    const gen = state.pollGen;

    const scheduleNext = () => {
      if (gen !== state.pollGen) return;
      state.pollTimer = setTimeout(tick, 2500);
    };

    const tick = async () => {
      if (gen !== state.pollGen) return;
      if (document.hidden) {
        scheduleNext();
        return;
      }
      state.pollTicks++;
      const signal = state.tabAbort?.signal;
      try {
        if (state.tab === 'radar') {
          await refreshRadar(false, signal);
          if (gen === state.pollGen && state.pollTicks % 2 === 0) {
            await refreshStaMon(false, signal);
          }
        } else if (state.tab === 'overview') {
          if (state.pollTicks % 4 === 0) {
            await refreshOverview(signal);
          } else {
            await refreshTrafficOnly(signal);
          }
        }
      } finally {
        scheduleNext();
      }
    };

    scheduleNext();
  }

  /* ── ResizeObserver for 1:1 Crisp SVG Re-rendering ─────────────────── */

  function setupChartResizeObserver() {
    if (typeof ResizeObserver === 'undefined') return;
    let rafId = 0;
    const ro = new ResizeObserver(() => {
      cancelAnimationFrame(rafId);
      rafId = requestAnimationFrame(() => {
        if (state.tab === 'overview' && state.trafficHist.length) renderTraffic();
        if (state.tab === 'wifi' && state.wtHistory.length) renderWtSvg();
        if (state.tab === 'tune' && state.lastRf) renderRFHistory(state.lastRf);
        if (state.tab === 'radar' && state.lastRadar) {
          const cfg = state.lastRadar.config || {};
          renderRadarSvg(state.lastRadar.live_points || [], cfg.micro_thresh || 3.5, cfg.motion_thresh || 12);
        }
      });
    });
    const app = $('#app');
    if (app) ro.observe(app);
  }

  /* ── Event Bindings ────────────────────────────────────────────────── */

  function bindEvents() {
    /* Main Tablist click + WAI-ARIA Arrow/Home/End keyboard navigation */
    const navButtons = $$('.nav-btn');
    navButtons.forEach((b, idx) => {
      b.addEventListener('click', () => switchTab(b.dataset.tab));
      b.addEventListener('keydown', e => {
        let targetIdx = -1;
        if (e.key === 'ArrowRight') targetIdx = (idx + 1) % navButtons.length;
        else if (e.key === 'ArrowLeft') targetIdx = (idx - 1 + navButtons.length) % navButtons.length;
        else if (e.key === 'Home') targetIdx = 0;
        else if (e.key === 'End') targetIdx = navButtons.length - 1;
        if (targetIdx >= 0) {
          e.preventDefault();
          navButtons[targetIdx].focus();
          switchTab(navButtons[targetIdx].dataset.tab);
        }
      });
    });

    window.addEventListener('hashchange', () => {
      if ($('#app').hidden) return;
      const target = parseHashTab();
      if (target !== state.tab) switchTab(target, { pushHash: false });
    });

    document.addEventListener('visibilitychange', () => {
      if (document.hidden || $('#app').hidden) return;
      const signal = state.tabAbort?.signal;
      if (state.tab === 'overview') refreshOverview(signal);
      else if (state.tab === 'radar') { refreshRadar(false, signal); refreshStaMon(false, signal); }
    });

    /* Chart hover inspectors */
    bindChartHover('traffic', '#trafficSvg', '#trafficTip');
    bindChartHover('rf', '#rfChartSvg', '#rfTip');
    bindChartHover('radar', '#radarSvg', '#radarTip');
    bindChartHover('walktest', '#wtSvg', '#wtTip');

    /* Topbar Quick LED Toggle (preserves custom night schedule) */
    $('#btnQuickLed').addEventListener('click', async e => {
      const btn = e.currentTarget;
      const curMode = state.lastOverview?.led_mode || 'auto';
      const next = curMode === 'off' ? 'auto' : 'off';
      const ledCfg = state.lastOverview?.led_cfg || {};
      await withBusy(btn, async () => {
        try {
          await api('/api/system/prefs', {
            method: 'POST',
            body: {
              led: {
                mode: next,
                night_start: ledCfg.night_start || '',
                night_end: ledCfg.night_end || '',
              },
            },
          });
          toast(next === 'off' ? '已切换为熄灯模式' : '已恢复自动状态灯');
          refreshOverview();
          if (state.tab === 'system') refreshSystem();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    /* Login & Logout */
    $('#loginForm').addEventListener('submit', async e => {
      e.preventDefault();
      const btn = e.currentTarget.querySelector('button[type="submit"]');
      $('#loginErr').textContent = '';
      await withBusy(btn, async () => {
        try {
          const d = await api('/api/login', { method: 'POST', body: { pass: $('#loginPass').value } });
          state.token = d.token || '';
          localStorage.setItem('ap_token', state.token);
          $('#loginPass').value = '';
          showApp();
        } catch (err) {
          $('#loginErr').textContent = err.message === 'unauthorized' ? '密码错误' : err.message;
        }
      }, '登录中...');
    });

    $('#btnLogout').addEventListener('click', () => {
      stopWalkTest();
      state.token = '';
      localStorage.removeItem('ap_token');
      showLogin();
    });

    /* Overview Scope Interface Switcher */
    $$('#scopeIfaceGroup .seg-btn').forEach(b => {
      b.addEventListener('click', () => {
        state.scopeIface = b.dataset.iface;
        setSegActive('#scopeIfaceGroup', b);
        renderTraffic();
      });
    });

    /* Wi-Fi & Clients */
    $('#btnRefreshWifi').addEventListener('click', () => refreshWifi());

    /* Handheld Walk Test */
    renderWtSpots();
    $('#btnWtToggle')?.addEventListener('click', () => {
      if (state.wtActive) stopWalkTest();
      else startWalkTest();
    });
    $('#wtTarget')?.addEventListener('change', () => {
      state.wtHistory = [];
      state.wtPrevTx = null;
      if (state.wtActive) startWalkTest();
    });
    $('#btnWtSound')?.addEventListener('click', e => {
      state.wtSound = !state.wtSound;
      const btn = e.currentTarget;
      btn.setAttribute('aria-pressed', state.wtSound ? 'true' : 'false');
      btn.textContent = state.wtSound ? '提示音：开' : '提示音：关';
      if (state.wtSound) {
        playWtTone(state.wtLast?.online ? state.wtLast.rssi : -55);
      }
    });
    $$('.wt-chip').forEach(ch => {
      ch.addEventListener('click', () => {
        const inp = $('#wtSpotName');
        if (inp) {
          inp.value = ch.dataset.spot || '';
          inp.focus();
        }
      });
    });
    $('#btnWtPin')?.addEventListener('click', async e => {
      const cur = state.wtLast;
      if (!cur || !cur.online) {
        toast('请先开始测信号并等待获取到在线信号数据', true);
        return;
      }
      const inp = $('#wtSpotName');
      const spot = (inp?.value || '').trim() || `点位 ${state.wtSpots.length + 1}`;
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/wifi/walktest/spots', {
            method: 'POST',
            body: {
              spot,
              time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
              device: cur.name || cur.mac || '',
              mac: cur.mac || '',
              band: cur.band || '',
              chanspec: cur.chanspec || '',
              rssi: cur.rssi,
              ant_rssi: cur.ant_rssi || '',
              snr: cur.snr || 0,
              tx_mbps: cur.tx_mbps || 0,
              rx_mbps: cur.rx_mbps || 0,
              rtt_ms: cur.rtt_ms || 0,
              retry_pct: cur.retry_pct || 0,
            },
          });
          state.wtSpots = d.spots || [];
          renderWtSpots();
          if (inp) inp.value = '';
          toast(`已记录「${spot}」(${cur.rssi} dBm)`);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#btnWtClearSpots')?.addEventListener('click', async e => {
      if (!state.wtSpots.length) return;
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/wifi/walktest/spots', { method: 'DELETE' });
          state.wtSpots = d.spots || [];
          renderWtSpots();
          toast('已清空打点记录');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#wtSpotRows')?.addEventListener('click', async e => {
      const btn = e.target.closest('button[data-wt-del]');
      if (!btn) return;
      const id = btn.dataset.wtDel;
      if (!id) return;
      await withBusy(btn, async () => {
        try {
          const d = await api('/api/wifi/walktest/spots?id=' + encodeURIComponent(id), { method: 'DELETE' });
          state.wtSpots = d.spots || [];
          renderWtSpots();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#form-wl1').addEventListener('submit', saveRadio);
    $('#form-wl0').addEventListener('submit', saveRadio);
    $('#form-guest-wl1').addEventListener('submit', saveGuest);
    $('#form-guest-wl0').addEventListener('submit', saveGuest);
    $('#form-adv-wl1').addEventListener('submit', saveAdv);
    $('#form-adv-wl0').addEventListener('submit', saveAdv);

    for (const [id, iface] of [['#btnToggleWl1', 'wl1'], ['#btnToggleWl0', 'wl0']]) {
      $(id).addEventListener('click', async e => {
        const btn = e.currentTarget;
        const enable = btn.dataset.next === '1';
        if (!enable) {
          const ok = await uiConfirm(`确认关闭 ${iface} 无线射频？已连接该频段的设备将断开。`, {
            title: `停用 ${iface} 无线`,
            confirmText: '停用',
            danger: true,
          });
          if (!ok) return;
        }
        await withBusy(btn, async () => {
          try {
            await api('/api/wifi/toggle', { method: 'POST', body: { iface, enabled: enable } });
            toast(`${iface} 已${enable ? '启用' : '关闭'}`);
            setTimeout(() => refreshWifi(), 1200);
          } catch (err) {
            if (!isSilentErr(err)) toast(err.message, true);
          }
        });
      });
    }

    $('#btnScan5g').addEventListener('click', e => runScan('wl1', e.currentTarget));
    $('#btnScan24g').addEventListener('click', e => runScan('wl0', e.currentTarget));

    $('#clientRows').addEventListener('click', async e => {
      const nameEl = e.target.closest('.client-name');
      if (nameEl) {
        const mac = nameEl.dataset.mac;
        const cur = nameEl.dataset.note || '';
        const next = await uiPrompt(`设置设备 ${mac} 的备注名称（留空则清除备注）：`, cur, {
          title: '修改设备备注',
          inputLabel: '设备备注名称',
          placeholder: '例如：MacBook Pro / 客厅空调',
        });
        if (next === null) return;
        try {
          await api('/api/wifi/note', { method: 'POST', body: { mac, note: next.trim() } });
          toast('备注已保存');
          refreshWifi();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
        return;
      }
      const kickBtn = e.target.closest('[data-kick-mac]');
      if (kickBtn) {
        const mac = kickBtn.dataset.kickMac;
        const iface = kickBtn.dataset.kickIface;
        const ok = await uiConfirm(`确认将 ${mac} 从 ${iface} 断开？`, {
          title: '踢出无线终端',
          confirmText: '断开连接',
          danger: true,
        });
        if (!ok) return;
        await withBusy(kickBtn, async () => {
          try {
            await api('/api/wifi/kick', { method: 'POST', body: { iface, mac } });
            toast(`已断开 ${mac}`);
            setTimeout(() => refreshWifi(), 800);
          } catch (err) {
            if (!isSilentErr(err)) toast(err.message, true);
          }
        });
      }
    });

    /* 24h RF History Controls */
    $$('#rfRangeGroup .seg-btn').forEach(b => {
      b.addEventListener('click', () => {
        state.rfRange = b.dataset.range;
        setSegActive('#rfRangeGroup', b);
        refreshRFHistory();
      });
    });
    $$('#rfBandGroup .seg-btn').forEach(b => {
      b.addEventListener('click', () => {
        state.rfBand = b.dataset.band;
        setSegActive('#rfBandGroup', b);
        if (state.lastRf) renderRFHistory(state.lastRf);
      });
    });
    $('#rfMetricSel').addEventListener('change', e => {
      state.rfMetric = e.currentTarget.value;
      if (state.lastRf) renderRFHistory(state.lastRf);
    });
    $('#btnRfSample').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          await api('/api/rf/sample', { method: 'POST' });
          await refreshRFHistory();
          toast('已新增一次空口采样');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#btnRfExport').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          await downloadFile('/api/rf/export', 'hg6371f-rf-history.csv');
        } catch (err) {
          toast(err.message, true);
        }
      });
    });

    /* Tune & Auto-Optimize */
    $('#btnOptAnalyze').addEventListener('click', runOptAnalyze);

    $('#btnQuickRtt').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          const snap = await captureOptBenchmark();
          renderOptCompare(snap, null, '当前无线延迟与空口快照（基准读数）');
          toast(`无线延迟 P50 ${snap.rtt.p50} ms · 抖动 ${snap.rtt.jitter} ms`);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      }, '探测中...');
    });

    $('#btnOptApply').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          let rep = state.lastOpt;
          if (!rep) {
            rep = await runOptAnalyze();
            if (!rep) return;
          }
          const applyCh = $('#optApplyCh').checked;
          const willRestart = applyCh && (rep.plan?.wl1_need_switch || rep.plan?.wl0_need_switch || rep.plan?.wl1_spect_off);
          if (willRestart) {
            const ok = await uiConfirm('本次调优包含信道或射频重载，相关无线将短暂闪断约 2 秒，是否继续？', {
              title: '应用射频调优方案',
              confirmText: '继续应用',
            });
            if (!ok) return;
          }

          $('#optSummary').textContent = '正在采集优化前空口快照与无线延迟基准...';
          const before = await captureOptBenchmark();

          $('#optSummary').textContent = '正在应用最优方案...';
          const d = await api('/api/tune/auto-apply', {
            method: 'POST',
            timeoutMs: 30000,
            body: { apply_channels: applyCh, plan: rep.plan },
          });
          renderTune(d.tune || {});

          $('#optSummary').textContent = willRestart ? '等待无线重连稳定后采集优化后对比数据...' : '正在采集优化后空口快照与无线延迟...';
          await new Promise(r => setTimeout(r, willRestart ? 4000 : 800));
          const after = await captureOptBenchmark();
          renderOptCompare(before, after, '优化前后实测对比（硬件空口快照 + 无线延迟）');
          toast((d.applied || []).join('；') || '一键调优已应用');
          await runOptAnalyze();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnApplyTune').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/tune/apply', {
            method: 'POST',
            timeoutMs: 25000,
            body: {
              frameburst: Number($('#tFrameburst').value),
              ampdu_rts: Number($('#tAmpduRts').value),
              phy_rxdesens: Number($('#tRxdesens').value),
              wl1_txpwr_pct: Number($('#tWl1Pwr').value),
              wl0_txpwr_pct: Number($('#tWl0Pwr').value),
              wl1_twt: Number($('#tTwt').value),
              wl1_atf: Number($('#tAtf').value),
              wl1_apsd: Number($('#tApsd').value),
              wl0_he: Number($('#tWl0He').value),
              wl0_width_mhz: Number($('#tWl0Width').value),
            },
          });
          renderTune(d.tune || {});
          toast('射频参数已生效并持久化');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    /* Radar & STA Monitor */
    $('#radarTargetPick')?.addEventListener('change', e => {
      const sel = e.currentTarget;
      const mac = sel.value;
      if (!mac) return;
      $('#radarTarget').value = mac;
      const opt = sel.options[sel.selectedIndex];
      const ifc = opt?.dataset?.iface;
      if (ifc && (ifc === 'wl0' || ifc === 'wl1')) {
        $('#radarIface').value = ifc;
      }
    });
    $('#radarTarget')?.addEventListener('input', e => {
      const v = e.currentTarget.value.trim().toUpperCase();
      const pick = $('#radarTargetPick');
      if (pick) pick.value = [...pick.options].some(o => o.value === v) ? v : '';
    });
    $('#btnRadarRefresh')?.addEventListener('click', () => refreshRadar(true));
    $('#btnRadarCal')?.addEventListener('click', async e => {
      const ok = await uiConfirm('确认执行 30 秒空房底噪标定？请确保房间内无人走动。', {
        title: 'CSI 空房底噪标定',
        confirmText: '开始标定',
      });
      if (!ok) return;
      await withBusy(e.currentTarget, async () => {
        try {
          await api('/api/radar/calibrate', { method: 'POST', body: { duration_sec: 30 } });
          toast('已开始 30 秒空房标定，完成后将自动更新阈值');
          refreshRadar(false);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#formRadar')?.addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          await api('/api/radar/config', {
            method: 'POST',
            body: {
              enabled: f.enabled.checked,
              iface: f.iface.value,
              target_mac: f.target_mac.value.trim().toUpperCase(),
              interval_ms: Number(f.interval_ms.value) || 200,
              micro_thresh: Number(f.micro_thresh.value) || 3.5,
              motion_thresh: Number(f.motion_thresh.value) || 12,
              confirm_sec: Number(f.confirm_sec.value) || 2,
              hold_sec: Number(f.hold_sec.value) || 30,
              webhook_url: f.webhook_url.value.trim(),
            },
          });
          toast('CSI 雷达配置已保存并生效');
          refreshRadar(true);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#btnRadarTestHook')?.addEventListener('click', async e => {
      const url = ($('#radarWebhook')?.value || '').trim();
      if (!url) { toast('请先填写 Webhook 地址', true); return; }
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/radar/test-webhook', { method: 'POST', body: { url } });
          toast(`Webhook 测试完成：${d.message || '已发送'}`, !d.ok);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnStamonRefresh')?.addEventListener('click', () => refreshStaMon(true));
    $('#stamonTargetPick')?.addEventListener('change', e => {
      const sel = e.currentTarget;
      const mac = sel.value;
      if (!mac) return;
      $('#stamonAddMac').value = mac;
      const note = sel.options[sel.selectedIndex]?.dataset?.note || '';
      if (note && !$('#stamonAddLabel').value.trim()) {
        $('#stamonAddLabel').value = note;
      }
    });
    $('#btnStamonAdd')?.addEventListener('click', () => {
      const mac = ($('#stamonAddMac')?.value || '').trim().toUpperCase();
      const label = ($('#stamonAddLabel')?.value || '').trim();
      if (!/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac)) {
        toast('请输入有效的 MAC 地址（格式 AA:BB:CC:DD:EE:FF）', true);
        return;
      }
      if (state.stamonDraftTargets.some(t => t.mac === mac)) {
        toast('该 MAC 已在监听列表中', true);
        return;
      }
      if (state.stamonDraftTargets.length >= 4) {
        toast('受硬件 AMT 槽位限制，最多同时监听 4 个 MAC', true);
        return;
      }
      state.stamonDraftTargets.push({ mac, label });
      $('#stamonAddMac').value = '';
      $('#stamonAddLabel').value = '';
      if ($('#stamonTargetPick')) $('#stamonTargetPick').value = '';
      renderStaMonTargets();
      toast('已加入列表，请点击「保存监测配置」下发到驱动');
    });
    $('#stamonRows')?.addEventListener('click', e => {
      const btn = e.target.closest('[data-stamon-del]');
      if (!btn) return;
      const idx = Number(btn.dataset.stamonDel);
      if (idx >= 0 && idx < state.stamonDraftTargets.length) {
        state.stamonDraftTargets.splice(idx, 1);
        renderStaMonTargets();
        toast('已移出列表，请点击「保存监测配置」生效');
      }
    });
    $('#formStamon')?.addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          await api('/api/stamon/config', {
            method: 'POST',
            body: {
              enabled: f.enabled.checked,
              band: f.band.value,
              rssi_thresh: Number(f.rssi_thresh.value) || -75,
              away_sec: Number(f.away_sec.value) || 180,
              webhook_url: f.webhook_url.value.trim(),
              targets: state.stamonDraftTargets,
            },
          });
          toast('STA Monitor 配置已保存并生效');
          refreshStaMon(true);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
    $('#btnStamonTestHook')?.addEventListener('click', async e => {
      const url = ($('#stamonWebhook')?.value || '').trim();
      if (!url) { toast('请先填写 Webhook 地址', true); return; }
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/stamon/test-webhook', { method: 'POST', body: { url } });
          toast(`Webhook 测试完成：${d.message || '已发送'}`, !d.ok);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    /* Network & Diagnostics */
    $('#btnRefreshNet').addEventListener('click', () => refreshNetwork());
    $('#netMode').addEventListener('change', toggleStaticFields);

    $('#formNetwork').addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      const ok = await uiConfirm('修改管理网络会短暂重启 networking，如果更改了 IP 需使用新地址重新访问，是否继续？', {
        title: '保存管理网络配置',
        confirmText: '确认修改',
        danger: true,
      });
      if (!ok) return;
      await withBusy(btn, async () => {
        try {
          await api('/api/network', {
            method: 'POST',
            body: {
              mode: f.mode.value,
              hostname: f.hostname.value.trim(),
              ip: f.ip.value.trim(),
              gateway: f.gateway.value.trim(),
              dns: f.dns.value.trim(),
            },
          });
          toast('网络配置已写入，正在应用...');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#formPing').addEventListener('submit', async e => {
      e.preventDefault();
      const target = e.currentTarget.target.value.trim();
      const btn = $('#btnPing');
      await withBusy(btn, async () => {
        $('#pingOut').textContent = '正在执行 Ping...';
        try {
          const d = await api('/api/diag/ping', { method: 'POST', timeoutMs: 20000, body: { target, count: 4 } });
          $('#pingOut').textContent = d.output || '(无输出)';
        } catch (err) {
          if (!isSilentErr(err)) $('#pingOut').textContent = '执行失败：' + err.message;
        }
      });
    });

    /* System & Maintenance */
    $('#btnRefreshSys').addEventListener('click', () => { refreshSystem(); refreshLogs(); });

    $('#slotCards').addEventListener('click', async e => {
      const btn = e.target.closest('[data-slot]');
      if (!btn || btn.disabled) return;
      const slot = Number(btn.dataset.slot);
      const mode = btn.dataset.mode;
      const label = mode === 'commit' ? `永久设为默认 Slot ${slot}` : `下次临时启动 Slot ${slot}`;
      const ok = await uiConfirm(`确认执行：${label}？`, {
        title: 'A/B 启动槽位切换',
        confirmText: '确认执行',
        danger: mode === 'commit',
      });
      if (!ok) return;
      await withBusy(btn, async () => {
        try {
          await api('/api/system/slot', { method: 'POST', body: { slot, mode } });
          toast(`${label} 已生效`);
          refreshSystem();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnOtaUpload').addEventListener('click', async e => {
      const file = $('#otaFile').files[0];
      if (!file) { toast('请先选择 .sqfs 根镜像文件', true); return; }
      const ok = await uiConfirm(`确认上传 ${file.name} (${fmtBytes(file.size)}) 并刷入非活动槽？\n刷写期间请勿断电，完成后自动设为一次性试跑。`, {
        title: '网页 OTA 刷机',
        confirmText: '开始刷写',
        danger: true,
      });
      if (!ok) return;
      await withBusy(e.currentTarget, async () => {
        $('#otaNote').textContent = '正在上传并校验刷写非活动槽，请勿断电...';
        try {
          const d = await api('/api/system/ota', { method: 'POST', timeoutMs: 180000, body: file });
          $('#otaNote').textContent = d.message || '刷写成功，请重启生效';
          toast('备用槽刷写完成');
          refreshSystem();
        } catch (err) {
          if (!isSilentErr(err)) {
            $('#otaNote').textContent = '刷写失败：' + err.message;
            toast(err.message, true);
          }
        }
      }, '刷写中...');
    });

    $('#formPrefs').addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          await api('/api/system/prefs', {
            method: 'POST',
            body: {
              led: {
                mode: f.led_mode.value,
                night_start: f.night_start.value,
                night_end: f.night_end.value,
              },
              button: {
                short_action: f.btn_short.value,
                long_action: f.btn_long.value,
              },
              wifi_schedule: {
                enabled: f.ws_enabled.checked,
                band: f.ws_band.value,
                off_start: f.ws_start.value,
                off_end: f.ws_end.value,
              },
            },
          });
          toast('指示灯、按键与计划已保存');
          refreshSystem();
          refreshOverview();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    const readMqttForm = () => {
      const f = $('#formMqtt');
      return {
        enabled: f.enabled.checked,
        broker: f.broker.value.trim(),
        username: f.username.value.trim(),
        password: f.password.value,
        clear_password: f.clear_password.checked,
        client_id: f.client_id.value.trim() || 'hg6371f-ap',
        topic_prefix: f.topic_prefix.value.trim() || 'hg6371f-ap',
        discovery: f.discovery.checked,
        discovery_prefix: f.discovery_prefix.value.trim() || 'homeassistant',
        interval_sec: Number(f.interval_sec.value) || 30,
        allow_control: f.allow_control.checked,
        tls_insecure: f.tls_insecure.checked,
      };
    };

    $('#formMqtt')?.addEventListener('submit', async e => {
      e.preventDefault();
      const btn = e.currentTarget.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          await api('/api/mqtt/config', { method: 'POST', body: readMqttForm() });
          $('#mqttPass').value = '';
          $('#mqttClearPass').checked = false;
          toast('MQTT / Home Assistant 配置已保存');
          setTimeout(() => refreshMqtt(), 600);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnMqttTest')?.addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          const res = await api('/api/mqtt/test', { method: 'POST', body: readMqttForm() });
          toast(res.message || 'MQTT 连接成功');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      }, '测试中...');
    });

    $('#btnMqttPush')?.addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          const res = await api('/api/mqtt/publish', { method: 'POST' });
          toast(res.message || '已触发同步');
          setTimeout(() => refreshMqtt(), 500);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#formTls').addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          await api('/api/system/tls', {
            method: 'POST',
            body: {
              cert_pem: f.cert_pem.value,
              key_pem: f.key_pem.value,
              redirect: f.redirect.checked,
            },
          });
          f.cert_pem.value = '';
          f.key_pem.value = '';
          toast('HTTPS 设置已更新');
          refreshSystem();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnBackup').addEventListener('click', async e => {
      await withBusy(e.currentTarget, async () => {
        try {
          await downloadFile('/api/system/backup', 'hg6371f-backup.tar.gz');
        } catch (err) {
          toast(err.message, true);
        }
      });
    });

    $('#btnRestore').addEventListener('click', async e => {
      const file = $('#restoreFile').files[0];
      if (!file) { toast('请先选择 .tar.gz 备份文件', true); return; }
      const ok = await uiConfirm(`确认从 ${file.name} 恢复配置？将覆盖当前无线与网络配置。`, {
        title: '恢复配置备份',
        confirmText: '确认恢复',
        danger: true,
      });
      if (!ok) return;
      await withBusy(e.currentTarget, async () => {
        try {
          const d = await api('/api/system/restore', { method: 'POST', timeoutMs: 30000, body: file });
          toast(d.message || '配置已恢复');
          refreshSystem();
          refreshOverview();
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#svcRows').addEventListener('click', async e => {
      const btn = e.target.closest('[data-svc]');
      if (!btn || btn.disabled) return;
      const name = btn.dataset.svc;
      const ok = await uiConfirm(`确认重启服务 ${name}？`, {
        title: '重启系统服务',
        confirmText: '重启服务',
      });
      if (!ok) return;
      await withBusy(btn, async () => {
        try {
          await api('/api/system/service', { method: 'POST', timeoutMs: 20000, body: { name, action: 'restart' } });
          toast(`服务 ${name} 已重启`);
          setTimeout(() => refreshSystem(), 1000);
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $$('#logSrcGroup .seg-btn').forEach(b => {
      b.addEventListener('click', () => {
        state.logSrc = b.dataset.src;
        setSegActive('#logSrcGroup', b);
        refreshLogs();
      });
    });

    $('#formPasswd').addEventListener('submit', async e => {
      e.preventDefault();
      const f = e.currentTarget;
      const btn = f.querySelector('button[type="submit"]');
      await withBusy(btn, async () => {
        try {
          const d = await api('/api/system/passwd', {
            method: 'POST',
            body: { old_pass: f.old_pass.value, new_pass: f.new_pass.value },
          });
          if (d.token) {
            state.token = d.token;
            localStorage.setItem('ap_token', state.token);
          }
          f.reset();
          toast('面板密码已更新');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });

    $('#btnReboot').addEventListener('click', async e => {
      const ok = await uiConfirm('确认重启 AP？所有无线与有线桥接设备将短暂断网约 30 秒。', {
        title: '重启设备',
        confirmText: '立即重启',
        danger: true,
      });
      if (!ok) return;
      await withBusy(e.currentTarget, async () => {
        try {
          await api('/api/system/reboot', { method: 'POST' });
          toast('设备正在重启，约 30 秒后恢复在线...');
        } catch (err) {
          if (!isSilentErr(err)) toast(err.message, true);
        }
      });
    });
  }

  /* ── Bootstrap ─────────────────────────────────────────────────────── */

  async function init() {
    bindEvents();
    setupChartResizeObserver();
    try {
      const ov = await api('/api/overview');
      showApp(ov);
    } catch (err) {
      if (err && err.status === 401) {
        showLogin();
      } else if (state.token) {
        showApp();
        $('#liveDot').classList.add('offline');
        toast(err?.message || '无法连接设备，正在自动重试...', true);
      } else {
        showLogin(err?.message || '无法连接设备，请检查网络后重试');
      }
    }
  }

  init();
})();
