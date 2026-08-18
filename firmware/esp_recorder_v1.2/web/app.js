/* 维特智能 WiFi 数据记录仪 - 前端逻辑 */
/* 支持真实设备模式和本地模拟模式 */

(function() {
  'use strict';

  /* ============= 模式检测 ============= */
  const IS_LOCAL = (window.location.hostname === '127.0.0.1' ||
                    window.location.hostname === 'localhost' ||
                    window.location.port === '8080');
  const API_BASE = IS_LOCAL ? '' : '';

  /* ============= 全局状态 ============= */
  let sinceSeq = 0;
  let streamGapMs = 100;
  let pollCount = 0;
  let activeLines = [];
  const MAX_LINES = 500;
  let runningSec = 0;
  let rxBytes = 0;
  let txBytes = 0;
  let simTimer = null;
  let lastRecvTs = 0;
  let recStartTime = 0;
  let recBytes = 0;

  /* 模拟数据生成器 */
  const SIM_SCENARIOS = [
    { dir: 'rx', hex: '68 04 00 04 00 E8 5A 00 00 00 01 5E 16', desc: 'IMU 姿态数据' },
    { dir: 'rx', hex: '68 0D 00 04 00 E8 0D 01 02 03 FC 18 00 01 00 08 4C 16', desc: 'IMU 加速度+角速度' },
    { dir: 'tx', hex: '01 03 00 00 00 0A C5 CD', desc: '读取寄存器请求' },
    { dir: 'info', text: 'SD 卡写入 256 字节，剩余 7.2 GB' },
    { dir: 'rx', hex: '68 04 00 84 00 E8 C2 01 00 00 00 83 16', desc: 'IMU 欧拉角数据' },
    { dir: 'tx', hex: '01 06 00 10 00 01 09 CF', desc: '写入寄存器' },
    { dir: 'rx', hex: '68 0D 00 E4 00 E8 1E 02 9A FF 01 00 07 00 01 9B AB 16', desc: 'IMU 四元数' },
    { dir: 'info', text: 'WiFi Station 信号强度: -52 dBm' },
  ];

  function simHexLine() {
    const s = SIM_SCENARIOS[Math.floor(Math.random() * SIM_SCENARIOS.length)];
    const ts = new Date().toTimeString().slice(0, 8) + '.' + String(Math.floor(Math.random() * 1000)).padStart(3, '0');
    if (s.dir === 'info') {
      return { dir: 'info', ts: ts, text: s.text };
    }
    return { dir: s.dir, ts: ts, hex: s.hex, desc: s.desc };
  }

  /* ============= API 调用 ============= */
  async function api(path, opts) {
    if (IS_LOCAL) return simulateApi(path, opts);
    try {
      const r = await fetch(API_BASE + path, opts || {});
      return await r.json();
    } catch(e) {
      console.error('[API]', path, e);
      return null;
    }
  }

  /* ============= 模拟 API ============= */
  function simulateApi(path, opts) {
    return new Promise(resolve => {
      setTimeout(() => {
        let resp = {};
        switch (path) {
          case '/api/status':
            resp = {
              ap: true, ap_ssid: 'WIT-LOGGER-D4F2', ap_ip: '192.168.4.1',
              sta_connected: false, sta_ssid: '-', sta_ip: '-',
              sd: true, sd_capacity: '8 GB', sd_free: '7.2 GB',
              recording: recStartTime > 0,
              file_open: recStartTime > 0,
              current_file: recStartTime > 0 ? 'REC_20260724_143022.bin' : '-',
              rec_drop_count: 0, rec_total_bytes: recBytes,
              uart_baud: 115200, uart_bits: 8, uart_stop: 1, uart_parity: 'None',
              usb_mode: 'CDC Serial', server_connected: false,
              uptime: runningSec + Math.floor(Math.random() * 5),
              free_heap: 245760 + Math.floor(Math.random() * 50000),
              fw_version: 'v1.1.0', serial: 'WIT-ESP32S3-0001'
            };
            break;
          case '/api/stream':
            resp = { items: [], max_seq: sinceSeq };
            break;
          case '/api/files':
            resp = { files: [
              { name: 'REC_20260724_140000.bin', size: 1048576 },
              { name: 'REC_20260724_141500.bin', size: 2097152 },
              { name: 'REC_20260724_143022.bin', size: 524288 },
            ]};
            break;
          case '/api/wifi/status':
            resp = { sta_connected: false, sta_ssid: '-', sta_ip: '-', rssi: -55 };
            break;
          case '/api/wifi/scan':
            resp = { networks: [
              { ssid: 'WiFi-Office', rssi: -45, auth: 3 },
              { ssid: 'WiFi-Lab', rssi: -62, auth: 3 },
              { ssid: 'Guest-Network', rssi: -78, auth: 0 },
            ]};
            break;
          default: resp = { ok: true };
        }
        resolve(resp);
      }, 50 + Math.random() * 100);
    });
  }

  /* ============= Toast 提示 ============= */
  function toast(msg, duration) {
    duration = duration || 2000;
    let el = document.getElementById('toast');
    if (!el) {
      el = document.createElement('div');
      el.id = 'toast';
      el.className = 'toast';
      document.body.appendChild(el);
    }
    el.textContent = msg;
    el.classList.add('show');
    clearTimeout(el._tid);
    el._tid = setTimeout(() => el.classList.remove('show'), duration);
  }

  /* ============= 标签切换 ============= */
  function initTabs() {
    document.querySelectorAll('.tab-btn').forEach(btn => {
      btn.addEventListener('click', function() {
        document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
        document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
        this.classList.add('active');
        const target = document.getElementById('panel-' + this.dataset.tab);
        if (target) target.classList.add('active');
      });
    });
  }

  /* ============= 状态刷新 ============= */
  async function refreshStatus() {
    const s = await api('/api/status');
    if (!s) return;
    /* AP 状态 */
    setStatusEl('st-ap', s.ap ? '已开启' : '未开启', s.ap ? 'on' : 'off');
    setStatusEl('st-apssid', s.ap_ssid || '-');
    setStatusEl('st-apip', s.ap_ip || '-');
    /* STA 状态 */
    setStatusEl('st-sta', s.sta_connected ? '已连接' : '未连接', s.sta_connected ? 'on' : 'off');
    setStatusEl('st-staip', s.sta_ip || '-');
    if (s.sta_connected) {
      document.getElementById('wifi-status').textContent = '已连接 - ' + (s.sta_ssid || '');
      document.getElementById('wifi-status').className = 'value on';
    } else {
      document.getElementById('wifi-status').textContent = '未连接';
      document.getElementById('wifi-status').className = 'value off';
    }
    setStatusEl('st-baud', (s.uart_baud || 115200) + ' bps');
    setStatusEl('st-sd', s.sd ? '已挂载' : '未挂载', s.sd ? 'on' : 'off');
    setStatusEl('st-sdcap', s.sd_free || '-');
    setStatusEl('st-rec', s.recording ? '记录中' : '空闲', s.recording ? 'on' : 'off');
    setStatusEl('st-file', s.current_file || '-');
    setStatusEl('st-usb', s.usb_mode || 'CDC');
    setStatusEl('st-srv', s.server_connected ? '已连接' : '未连接', s.server_connected ? 'on' : 'off');
    setStatusEl('st-uptime', formatUptime(s.uptime || runningSec));
    setStatusEl('st-heap', formatBytes(s.free_heap || 0));
    setStatusEl('st-fw', s.fw_version || 'v1.1.0');
    /* 更新记录控制区 */
    const recInd = document.getElementById('rec-indicator');
    if (recInd) {
      recInd.className = 'rec-indicator ' + (s.recording ? 'running' : 'stopped');
    }
    document.getElementById('rec-btn-start').disabled = s.recording;
    document.getElementById('rec-btn-stop').disabled = !s.recording;
    document.getElementById('rec-filename').textContent = s.current_file || '-';
    document.getElementById('rec-bytes').textContent = formatBytes(s.rec_total_bytes || 0);
    document.getElementById('rec-drops').textContent = s.rec_drop_count || 0;
    if (!IS_LOCAL && typeof s.stream_gap_ms === 'number' && s.stream_gap_ms !== streamGapMs) {
      streamGapMs = s.stream_gap_ms;
      document.getElementById('streamgap').value = streamGapMs;
    }
    /* 保存真实运行时间 */
    if (s.uptime) runningSec = s.uptime;
  }

  function setStatusEl(id, text, cls) {
    const el = document.getElementById(id);
    if (!el) return;
    el.textContent = text;
    el.className = 'value';
    if (cls) el.classList.add(cls);
  }

  function formatUptime(sec) {
    var h = Math.floor(sec / 3600);
    var m = Math.floor((sec % 3600) / 60);
    var s = sec % 60;
    return h + 'h ' + String(m).padStart(2, '0') + 'm ' + String(s).padStart(2, '0') + 's';
  }

  function formatBytes(bytes) {
    if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + ' GB';
    if (bytes >= 1048576) return (bytes / 1048576).toFixed(2) + ' MB';
    if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return bytes + ' B';
  }

  /* ============= 实时数据面板 ============= */
  function initTerminal() {
    var autoScroll = true;
    var showHex = true;
    var showTS = true;
    var paused = false;

    document.getElementById('chk-autoscroll').addEventListener('change', function() { autoScroll = this.checked; });
    document.getElementById('chk-hex').addEventListener('change', function() { showHex = this.checked; });
    document.getElementById('chk-ts').addEventListener('change', function() { showTS = this.checked; });
    document.getElementById('btn-pause').addEventListener('click', function() {
      paused = !paused;
      this.textContent = paused ? '▶ 继续' : '⏸ 暂停';
    });
    document.getElementById('btn-clear').addEventListener('click', function() {
      clearTerminal();
      if (!IS_LOCAL) api('/api/stream/clear', { method: 'POST' });
    });

    function appendLine(item) {
      if (paused) return;
      var el = document.getElementById('terminal');
      if (!el) return;
      var div = document.createElement('div');
      var prefix = '';
      if (showTS) prefix += '[' + item.ts + '] ';
      if (item.dir === 'rx') {
        div.className = 'rx';
        rxBytes += (item.hex || '').length;
        prefix += '[RX] ';
        div.textContent = prefix + (showHex ? (item.hex || '') : (item.desc || ''));
      } else if (item.dir === 'tx') {
        div.className = 'tx';
        txBytes += (item.hex || '').length;
        prefix += '[TX] ';
        div.textContent = prefix + (showHex ? (item.hex || '') : (item.desc || ''));
      } else if (item.dir === 'info') {
        div.className = 'info';
        div.textContent = prefix + (item.text || '');
      } else if (item.dir === 'gap') {
        div.className = 'gap';
        div.textContent = item.text || '---';
      }
      activeLines.push({ node: div, ts: item.ts || '' });
      el.appendChild(div);
      if (activeLines.length > MAX_LINES) {
        var old = activeLines.shift();
        if (old && old.node.parentNode) old.node.parentNode.removeChild(old.node);
      }
      if (autoScroll) el.scrollTop = el.scrollHeight;
      updateTerminalStats();
    }

    function updateTerminalStats() {
      document.getElementById('stat-rxbytes').textContent = formatBytes(rxBytes);
      document.getElementById('stat-txbytes').textContent = formatBytes(txBytes);
      document.getElementById('stat-lines').textContent = activeLines.length;
    }

    window._appendTermLine = appendLine;
    window._updateTermStats = updateTerminalStats;
  }

  function clearTerminal() {
    var el = document.getElementById('terminal');
    if (el) el.innerHTML = '';
    activeLines = [];
    window._updateTermStats && window._updateTermStats();
  }

  /* ============= 串口数据发送 ============= */
  function initSendPanel() {
    var sendHistory = [];
    document.getElementById('btn-send').addEventListener('click', async function() {
      var hex = document.getElementById('tx-input').value.trim();
      if (!hex) { toast('请输入要发送的数据'); return; }
      if (IS_LOCAL) {
        var d = new Date();
        var ts = d.toTimeString().slice(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
        window._appendTermLine({ dir: 'tx', ts: ts, hex: hex, desc: '本地模拟发送' });
        sendHistory.push({ ts: ts, hex: hex });
        toast('模拟发送 ' + hex.length + ' 字符');
        return;
      }
      var r = await api('/api/send', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ hex: hex })
      });
      if (r && r.ok) {
        sendHistory.push({ ts: new Date().toISOString(), hex: hex });
        toast('发送成功 ' + (r.sent || 0) + ' 字节');
      } else {
        toast('发送失败');
      }
    });
    document.getElementById('btn-send-clear').addEventListener('click', function() {
      document.getElementById('tx-input').value = '';
    });
    /* HEX模式提示 */
    document.getElementById('tx-input').addEventListener('input', function() {
      var isHex = /^[0-9a-fA-F\s]+$/.test(this.value);
      document.getElementById('tx-hint').textContent = isHex ? '✓ HEX 格式' : '请输入十六进制字符串';
      document.getElementById('tx-hint').style.color = isHex ? 'var(--success)' : 'var(--gray-400)';
    });
  }

  /* ============= 记录控制 ============= */
  function initRecordPanel() {
    document.getElementById('rec-btn-start').addEventListener('click', async function() {
      if (IS_LOCAL) {
        recStartTime = Date.now();
        recBytes = 0;
        toast('模拟开始记录');
        refreshStatus();
        return;
      }
      var r = await api('/api/recorder/start', { method: 'POST' });
      if (r && r.ok) { toast('开始记录'); refreshStatus(); } else toast('开始记录失败');
    });
    document.getElementById('rec-btn-stop').addEventListener('click', async function() {
      if (IS_LOCAL) {
        recStartTime = 0;
        toast('模拟停止记录');
        refreshStatus();
        return;
      }
      var r = await api('/api/recorder/stop', { method: 'POST' });
      if (r && r.ok) { toast('停止记录'); refreshStatus(); } else toast('停止记录失败');
    });
  }

  /* ============= UART 配置 ============= */
  function initUartConfig() {
    document.getElementById('btn-uart-save').addEventListener('click', async function() {
      var baud = parseInt(document.getElementById('uart-baud').value, 10);
      var bits = parseInt(document.getElementById('uart-bits').value, 10);
      var stop = parseInt(document.getElementById('uart-stop').value, 10);
      var parity = document.getElementById('uart-parity').value;
      if (IS_LOCAL) {
        toast('UART 配置已保存（模拟）: ' + baud + ' ' + bits + bitsToStr(bits) + stopToStr(stop) + '-' + parity);
        return;
      }
      var r = await api('/api/uart/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ baudrate: baud, databits: bits, stopbits: stop, parity: parity })
      });
      if (r && r.ok) toast('UART 配置已保存'); else toast('保存失败');
    });
  }

  function bitsToStr(b) { return b === 8 ? 'N' : (b === 7 ? '7' : String(b)); }
  function stopToStr(s) { return s === 1 ? '1' : (s === 2 ? '2' : String(s)); }

  /* ============= WiFi 配置 ============= */
  function initWifiPanel() {
    /* 扫描 */
    document.getElementById('btn-scan').addEventListener('click', async function() {
      var el = document.getElementById('scan-results');
      el.innerHTML = '<div style="padding:8px;color:var(--gray-500);">扫描中...</div>';
      if (IS_LOCAL) {
        var nets = [
          { ssid: 'WiFi-Office', rssi: -45, auth: 3 },
          { ssid: 'WiFi-Lab', rssi: -62, auth: 3 },
          { ssid: 'Guest-Network', rssi: -78, auth: 0 },
          { ssid: 'IoT-Net', rssi: -55, auth: 3 },
        ];
        setTimeout(() => { renderScanResults(el, nets); }, 800);
        return;
      }
      var r = await api('/api/wifi/scan');
      if (r && r.networks) renderScanResults(el, r.networks);
      else el.innerHTML = '<div style="padding:8px;color:var(--danger);">扫描失败</div>';
    });

    function renderScanResults(el, nets) {
      if (!nets.length) { el.innerHTML = '<div style="padding:8px;color:var(--gray-500);">未发现网络</div>'; return; }
      el.innerHTML = nets.map(function(n) {
        var dbClass = n.rssi > -50 ? 'on' : (n.rssi > -70 ? 'warn' : 'off');
        return '<div class="item" onclick="document.getElementById(\'wifi-ssid\').value=\'' +
               n.ssid.replace(/'/g, "\\'") + '\';document.getElementById(\'wifi-auth\').value=' + n.auth + '">' +
               '<span>' + n.ssid + '</span>' +
               '<span class="sig">' + n.rssi + ' dBm</span></div>';
      }).join('');
    }

    /* 连接 */
    document.getElementById('btn-connect').addEventListener('click', async function() {
      var ssid = document.getElementById('wifi-ssid').value.trim();
      var pass = document.getElementById('wifi-pass').value;
      var auth = parseInt(document.getElementById('wifi-auth').value, 10);
      if (!ssid) { toast('请输入 WiFi SSID'); return; }
      if (auth !== 0 && !pass) { toast('请输入 WiFi 密码'); return; }
      if (IS_LOCAL) { toast('模拟连接 WiFi: ' + ssid); refreshStatus(); return; }
      var r = await api('/api/wifi/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: ssid, pass: pass || '', auth: auth })
      });
      if (r && r.ok) { toast('正在连接 ' + ssid + '...'); setTimeout(refreshStatus, 3000); }
      else toast('连接失败');
    });

    /* 断开 */
    document.getElementById('btn-disconnect').addEventListener('click', async function() {
      if (IS_LOCAL) { toast('模拟断开 WiFi'); refreshStatus(); return; }
      var r = await api('/api/wifi/disconnect', { method: 'POST' });
      if (r && r.ok) { toast('已断开连接'); refreshStatus(); } else toast('断开失败');
    });
  }

  /* ============= 文件管理 ============= */
  async function refreshFiles() {
    var el = document.getElementById('file-list');
    if (!el) return;
    var r = await api('/api/files');
    if (!r || !r.files || !r.files.length) {
      el.innerHTML = '<tr><td colspan="3" style="text-align:center;padding:16px;color:var(--gray-400);">暂无文件</td></tr>';
      document.getElementById('file-count').textContent = '0 个文件';
      return;
    }
    document.getElementById('file-count').textContent = r.files.length + ' 个文件';
    var totalSize = 0;
    el.innerHTML = r.files.map(function(f) {
      totalSize += f.size;
      var dt = f.time || '-';
      var sz = formatBytes(f.size);
      var dlUrl = IS_LOCAL ? '#' : ('/api/file?name=' + encodeURIComponent(f.name));
      return '<tr>' +
        '<td>' + f.name + '</td>' +
        '<td>' + sz + '</td>' +
        '<td>' + dt + '</td>' +
        '<td>' +
          '<button class="btn btn-sm btn-outline" onclick="window._downloadFile(\'' + f.name.replace(/'/g, "\\'") + '\')">下载</button>' +
          '<button class="btn btn-sm btn-danger" onclick="window._deleteFile(\'' + f.name.replace(/'/g, "\\'") + '\')" style="margin-left:4px">删除</button>' +
        '</td></tr>';
    }).join('');
    document.getElementById('file-total').textContent = formatBytes(totalSize);
  }

  window._downloadFile = function(name) {
    if (IS_LOCAL) { toast('本地预览模式，无法下载文件: ' + name); return; }
    var a = document.createElement('a');
    a.href = '/api/file?name=' + encodeURIComponent(name);
    a.download = name;
    a.click();
  };

  window._deleteFile = async function(name) {
    if (!confirm('确认删除 ' + name + '？此操作不可恢复。')) return;
    if (IS_LOCAL) { toast('模拟删除: ' + name); refreshFiles(); return; }
    var r = await api('/api/file/delete?name=' + encodeURIComponent(name), { method: 'POST' });
    if (r && r.ok) { toast('已删除 ' + name); refreshFiles(); } else toast('删除失败');
  };

  /* ============= 模拟数据流 ============= */
  function startSimStream() {
    if (!IS_LOCAL) return;
    if (simTimer) clearInterval(simTimer);
    simTimer = setInterval(function() {
      var item = simHexLine();
      window._appendTermLine && window._appendTermLine(item);
      if (recStartTime > 0) {
        recBytes += Math.floor(Math.random() * 128) + 16;
        document.getElementById('rec-bytes').textContent = formatBytes(recBytes);
        var dur = Math.floor((Date.now() - recStartTime) / 1000);
        document.getElementById('rec-dur').textContent = formatUptime(dur);
      }
    }, 200 + Math.random() * 600);
  }

  /* ============= 运行计时器 ============= */
  function startUptimeCounter() {
    setInterval(function() {
      runningSec++;
      var el = document.getElementById('st-uptime');
      if (el) el.textContent = formatUptime(runningSec);
      var rt = document.getElementById('runtime');
      if (rt) rt.textContent = '运行 ' + formatUptime(runningSec);
    }, 1000);
  }

  /* ============= 初始化 ============= */
  function init() {
    /* 模拟模式横幅 */
    if (IS_LOCAL) {
      document.getElementById('sim-banner').classList.add('visible');
    }

    initTabs();
    initTerminal();
    initSendPanel();
    initRecordPanel();
    initUartConfig();
    initWifiPanel();

    /* 刷新按钮 */
    document.getElementById('btn-refresh-status').addEventListener('click', refreshStatus);
    document.getElementById('btn-refresh-files').addEventListener('click', refreshFiles);

    /* 初始加载 */
    refreshStatus();
    refreshFiles();
    startUptimeCounter();
    startSimStream();

    /* 定时刷新 */
    setInterval(refreshStatus, 3000);
    if (!IS_LOCAL) setInterval(pollStream, 300);
  }

  async function pollStream() {
    if (IS_LOCAL) return;
    var el = document.getElementById('terminal');
    if (!el) return;
    try {
      var r = await fetch(API_BASE + '/api/stream?since=' + sinceSeq);
      if (r.ok) {
        var j = await r.json();
        var items = j.items || [];
        if (items.length) {
          lastRecvTs = performance.now();
          for (var i = 0; i < items.length; i++) {
            var it = items[i];
            window._appendTermLine && window._appendTermLine({ dir: 'rx', ts: String(it.ts), hex: it.line, desc: '' });
            sinceSeq = Math.max(sinceSeq, it.seq);
          }
          if (j.max_seq) sinceSeq = Math.max(sinceSeq, j.max_seq);
        }
      }
    } catch(e) {}
  }

  document.addEventListener('DOMContentLoaded', init);
})();
