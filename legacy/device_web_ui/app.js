/*
 * app.js — 环境监测控制系统 前端逻辑
 */

/* ════════════════════════════════════════════
   目录切换
   ════════════════════════════════════════════ */
var PAGE_TITLES = ['实时数据', '历史趋势', '水肥控制', 'AI农艺助手'];

function sw(n) {
  for (var i = 0; i < PAGE_TITLES.length; i++) {
    var tab = document.getElementById('tab-' + i);
    var nav = document.getElementById('nav-' + i);
    if (tab) tab.className = i === n ? '' : 'hidden';
    if (nav) nav.className = i === n ? 'nav-item nav-active' : 'nav-item';
  }
  var pt = document.getElementById('page-title');
  if (pt) pt.textContent = PAGE_TITLES[n];
  if (n === 1) hOnShow();
}

/* ════════════════════════════════════════════
   传感器卡片初始化
   ════════════════════════════════════════════ */
var sc = document.getElementById('scards');
[
  { id: 'ws',  ic: 'wind',            n: '风速', u: 'm/s',  c: '#06b6d4', mn: 0,   mx: 20   },
  { id: 'at',  ic: 'temperature-half', n: '气温', u: '°C',  c: '#f97316', mn: -10, mx: 50   },
  { id: 'ah',  ic: 'droplet',         n: '湿度', u: '%RH', c: '#3b82f6', mn: 0,   mx: 100  },
  { id: 'lux', ic: 'sun',             n: '光照', u: 'lux', c: '#f59e0b', mn: 0,   mx: 5000 },
  { id: 'co2', ic: 'smog',            n: 'CO2',  u: 'ppm', c: '#64748b', mn: 300, mx: 2000 },
  { id: 'sm',  ic: 'leaf',            n: '土湿', u: '%',   c: '#22c55e', mn: 0,   mx: 100  },
  { id: 'st',  ic: 'temperature-half', n: '土温', u: '°C',  c: '#a855f7', mn: -10, mx: 50   },
  { id: 'o2',  ic: 'flask',           n: '氧气', u: '%',   c: '#06b6d4', mn: 0,   mx: 30   }
].forEach(function(s) {
  var d = document.createElement('div');
  d.className = 'bg-white rounded-xl shadow-sm p-3 text-center border';
  d.id = 'sc-' + s.id;
  d.innerHTML = '<i class="fas fa-' + s.ic + ' text-xl" style="color:' + s.c + '"></i>' +
    '<div class="text-lg font-bold mt-1" style="color:' + s.c + '">--</div>' +
    '<div class="text-xs text-gray-400">' + s.n + ' <span class="text-gray-300">' + s.u + '</span></div>';
  sc.appendChild(d);
});

/* ════════════════════════════════════════════
   Chart.js 折线图初始化
   ════════════════════════════════════════════ */
var rdf = [
  { id: 'ws',  lb: '风速 (m/s)',    co: '#06b6d4' },
  { id: 'at',  lb: '气温 (°C)',     co: '#f97316' },
  { id: 'ah',  lb: '湿度 (%RH)',    co: '#3b82f6' },
  { id: 'lux', lb: '光照 (lux)',    co: '#f59e0b' },
  { id: 'co2', lb: 'CO2 (ppm)',     co: '#64748b' },
  { id: 'sm',  lb: '土壤湿度 (%)',  co: '#22c55e' }
];
var cls = {}, clen = 60;
var cw = document.getElementById('charts');

rdf.forEach(function(c) {
  var b = document.createElement('div');
  b.className = 'bg-white rounded-xl shadow-sm p-3 border';
  b.innerHTML = '<div class="text-xs text-gray-500 mb-1">' + c.lb + '</div>' +
    '<div class="h-16"><canvas id="ch-' + c.id + '"></canvas></div>';
  cw.appendChild(b);
});

rdf.forEach(function(c) {
  var cv = document.getElementById('ch-' + c.id);
  cv.getContext('2d');
  var ch = new Chart(cv, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: c.lb,
        borderColor: c.co,
        backgroundColor: c.co + '22',
        tension: 0.3,
        fill: true,
        data: []
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      scales: { x: { display: false }, y: { beginAtZero: true } },
      plugins: { legend: { display: false } }
    }
  });
  cls[c.id] = ch;
});

/* ════════════════════════════════════════════
   时钟更新
   ════════════════════════════════════════════ */
setInterval(function() {
  var n = new Date();
  document.getElementById('hdr-time').textContent = n.toLocaleTimeString('zh-CN', { hour12: false });
}, 1000);

/* ════════════════════════════════════════════
   继电器网格初始化
   ════════════════════════════════════════════ */
var rg = document.getElementById('rgrid');
for (var i = 1; i <= 12; i++) {
  (function(id) {
    var c = document.createElement('div');
    c.className = 'bg-white rounded-xl shadow-sm p-3 text-center border';
    c.innerHTML =
      '<div class="text-xs text-gray-400 mb-1">通道 ' + id + '</div>' +
      '<div class="w-3 h-3 rounded-full mx-auto mb-1 bg-gray-300" id="rd' + id + '"></div>' +
      '<div class="text-sm font-semibold mb-2" id="rl' + id + '">OFF</div>' +
      '<div class="flex gap-1 justify-center">' +
        '<button class="px-2 py-1 rounded text-xs text-white font-semibold bg-green-500" data-cmd="on" data-id="' + id + '">ON</button>' +
        '<button class="px-2 py-1 rounded text-xs text-white font-semibold bg-red-500" data-cmd="off" data-id="' + id + '">OFF</button>' +
      '</div>';
    rg.appendChild(c);
    c.querySelectorAll('button').forEach(function(btn) {
      btn.onclick = function() { rCmd(id, btn.getAttribute('data-cmd')); };
    });
  })(i);
}

/* ════════════════════════════════════════════
   继电器控制
   ════════════════════════════════════════════ */
function rCmd(id, cmd) {
  fetch('/api/' + cmd + '?id=' + id)
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.ok) { updateR(id, cmd); ts('通道 ' + id + ': ' + cmd.toUpperCase(), 'bg-green-500'); }
    });
}

function updateR(id, cmd) {
  var d = document.getElementById('rd' + id),
      l = document.getElementById('rl' + id);
  d.className = 'w-3 h-3 rounded-full mx-auto mb-1 ' + (cmd === 'on' ? 'bg-green-500' : 'bg-gray-300');
  l.textContent = cmd === 'on' ? 'ON' : 'OFF';
  l.style.color = cmd === 'on' ? '#22c55e' : '#6b7280';
}

/* ════════════════════════════════════════════
   Toast 提示
   ════════════════════════════════════════════ */
function ts(m, c) {
  var e = document.getElementById('toast');
  e.textContent = m;
  e.className = 'fixed top-4 right-4 px-4 py-2 rounded-lg text-white text-sm z-50 opacity-100 transition-opacity pointer-events-none ' + c;
  clearTimeout(ts._t);
  ts._t = setTimeout(function() {
    e.className = 'fixed top-4 right-4 px-4 py-2 rounded-lg text-white text-sm z-50 opacity-0 transition-opacity pointer-events-none';
  }, 1500);
}

/* ════════════════════════════════════════════
   辅助函数
   ════════════════════════════════════════════ */
function uc(d) {
  var el = document.getElementById('wd-arrow');
  if (el) el.style.transform = 'translate(-50%,-100%) rotate(' + d + 'deg)';
}

function setBar(el, v, mn, mx) {
  var p = Math.max(0, Math.min(100, (v - mn) / (mx - mn) * 100));
  el.style.width = p + '%';
}

function sv(id, v) {
  var el = document.getElementById('sc-' + id);
  if (el) el.querySelector('.font-bold').textContent = v;
}

/* ════════════════════════════════════════════
   数据刷新 (每 2 秒)
   ════════════════════════════════════════════ */
function refresh() {
  fetch('/api/sensors')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      // 顶部信息
      document.getElementById('hdr-ip').textContent = 'IP: ' + d.ip;

      // 传感器卡片
      sv('ws', d.ws.toFixed(1));
      sv('at', d.at.toFixed(1));
      sv('ah', d.ah.toFixed(1));
      sv('lux', Math.round(d.lux));
      sv('co2', Math.round(d.co2));
      sv('sm', d.sm.toFixed(1));
      sv('st', d.st.toFixed(1));
      sv('o2', d.o2.toFixed(1));

      // 风速 & 风向
      document.getElementById('ws-val').textContent = d.ws.toFixed(1);
      setBar(document.getElementById('ws-bar'), d.ws, 0, 20);
      document.getElementById('wd-val').textContent = d.wd.toFixed(1);
      document.getElementById('wd-text').textContent = '风向: ' + d.wdn;
      uc(d.wd);

      // 土壤环境
      document.getElementById('sm-val').textContent = d.sm.toFixed(1);
      setBar(document.getElementById('sm-bar'), d.sm, 0, 100);
      document.getElementById('st-val').textContent = d.st.toFixed(1);
      setBar(document.getElementById('st-bar'), d.st, -10, 50);
      document.getElementById('sec-val').textContent = Math.round(d.sec);
      setBar(document.getElementById('sec-bar'), d.sec, 0, 5000);
      document.getElementById('o2-val').textContent = d.o2.toFixed(1);
      setBar(document.getElementById('o2-bar'), d.o2, 0, 30);

      // 综合评分
      var sc = Math.min(100, Math.max(0,
        100 - (d.ws > 15 ? 15 : 0) - (d.at > 35 || d.at < 5 ? 20 : 0) -
        (d.ah > 85 || d.ah < 20 ? 15 : 0) - (d.sm < 20 || d.sm > 85 ? 20 : 0)
      ));
      document.getElementById('score-val').textContent = Math.round(sc);
      setBar(document.getElementById('score-bar'), sc, 0, 100);
      document.getElementById('score-bar').style.backgroundColor =
        sc >= 80 ? '#22c55e' : sc >= 60 ? '#facc15' : '#f97373';
      document.getElementById('score-text').textContent =
        sc >= 85 ? '环境良好' : sc >= 60 ? '基本适宜' : '需关注';

      // 整体状态标签
      var tag = document.getElementById('overall-tag');
      tag.textContent = sc >= 80 ? '环境正常' : sc >= 60 ? '轻微异常' : '需关注';
      tag.className = 'text-xs px-2.5 py-1 ' +
        (sc >= 80 ? 'badge-ok' : sc >= 60 ? 'badge-warn' : 'badge-err');

      // 折线图更新
      var f = { ws: d.ws, at: d.at, ah: d.ah, lux: d.lux, co2: d.co2, sm: d.sm };
      Object.keys(f).forEach(function(k) {
        var ch = cls[k];
        if (!ch) return;
        ch.data.labels.push('');
        ch.data.datasets[0].data.push(f[k]);
        if (ch.data.labels.length > clen) {
          ch.data.labels.shift();
          ch.data.datasets[0].data.shift();
        }
        ch.update();
      });

      // 继电器状态更新
      for (var i = 1; i <= 12; i++) {
        updateR(i, d.rly[i - 1] === 1 ? 'on' : 'off');
      }
    });
}

setInterval(refresh, 2000);
refresh();

/* ════════════════════════════════════════════
   历史趋势模块
   ════════════════════════════════════════════ */

/* 通道元数据 (id 与固件 HIST_CH_ID 一致) */
var HCH = [
  { id: 'ws',  n: '风速', u: 'm/s',   c: '#06b6d4', dec: 1 },
  { id: 'wd',  n: '风向', u: '°',     c: '#f59e0b', dec: 1 },
  { id: 'at',  n: '气温', u: '°C',    c: '#f97316', dec: 1 },
  { id: 'ah',  n: '湿度', u: '%RH',   c: '#3b82f6', dec: 1 },
  { id: 'lux', n: '光照', u: 'lux',   c: '#eab308', dec: 0 },
  { id: 'sm',  n: '土湿', u: '%',     c: '#22c55e', dec: 1 },
  { id: 'st',  n: '土温', u: '°C',    c: '#a855f7', dec: 1 },
  { id: 'sec', n: 'EC',   u: 'μS/cm', c: '#14b8a6', dec: 0 },
  { id: 'co2', n: 'CO2',  u: 'ppm',   c: '#64748b', dec: 0 },
  { id: 'o2',  n: '氧气', u: '%',     c: '#0ea5e9', dec: 1 }
];

/* 派生农艺指标 (由气温 + 湿度换算) */
var HDERIV = [
  { id: 'dew', n: '露点', u: '°C',  c: '#ec4899', dec: 1 },
  { id: 'vpd', n: 'VPD',  u: 'kPa', c: '#8b5cf6', dec: 2 }
];

/* 数据源 */
var HSRC = [
  { k: 'ram',   l: '内存缓冲', def: 900   },
  { k: 'flash', l: 'Flash 日志', def: 21600 }
];

/* 可选时间范围 (按数据源区分; s = 0 表示全部) */
var HRANGE = {
  ram: [
    { s: 300,  l: '5分钟'  },
    { s: 900,  l: '15分钟' },
    { s: 1800, l: '30分钟' }
  ],
  flash: [
    { s: 3600,   l: '1小时' },
    { s: 21600,  l: '6小时' },
    { s: 86400,  l: '1天'   },
    { s: 259200, l: '3天'   },
    { s: 604800, l: '7天'   },
    { s: 0,      l: '全部'  }
  ]
};

/* 可选采样间隔 (内存缓冲, 秒) */
var HINTERVAL = [2, 5, 10, 30, 60, 120, 300];

/* 可选落盘间隔 (秒) */
var LOGINTERVAL = [5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600];

var hS = {
  src: 'ram',                    /* ram | flash */
  range: 900,                    /* 当前窗口 (秒), 0 = 全部 */
  mode: 'norm',                  /* norm | raw */
  smooth: false,                 /* 3点滑动平均 */
  deriv: false,                  /* 是否显示派生指标 */
  chans: ['ws', 'at', 'ah', 'sm'],/* 选中的通道 */
  paused: false,
  res: 'raw',                    /* raw | min (仅内存源) */
  meta: null,
  log: null,                     /* Flash 日志状态 */
  clockValid: false,
  data: null,
  recvAt: 0,
  busy: false
};

var hCmp = null;
var hGrid = {};

/* ── 小工具 ── */
function hNum(x) {
  return (x === null || x === undefined || isNaN(x)) ? null : x;
}

function hMetaOf(id) {
  var i;
  for (i = 0; i < HCH.length; i++) if (HCH[i].id === id) return HCH[i];
  for (i = 0; i < HDERIV.length; i++) if (HDERIV[i].id === id) return HDERIV[i];
  return { id: id, n: id, u: '', c: '#9ca3af', dec: 1 };
}

function hPad2(n) { return n < 10 ? '0' + n : '' + n; }

function hDur(sec) {
  sec = Math.round(sec);
  if (sec < 60) return sec + '秒';
  if (sec < 3600) return Math.floor(sec / 60) + '分' + (sec % 60 ? (sec % 60) + '秒' : '');
  return (sec / 3600).toFixed(1) + '小时';
}

function hFmtTime(ms, long) {
  var d = new Date(ms);
  if (long) return (d.getMonth() + 1) + '-' + hPad2(d.getDate()) + ' ' + hPad2(d.getHours()) + ':' + hPad2(d.getMinutes());
  return hPad2(d.getHours()) + ':' + hPad2(d.getMinutes()) + ':' + hPad2(d.getSeconds());
}

function hTag(id, text, cls) {
  var el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className = 'text-xs px-2.5 py-1 ' + cls;
}

/* 3 点滑动平均 (跳过缺失点) */
function hSmooth(arr) {
  var out = [];
  for (var i = 0; i < arr.length; i++) {
    var sum = 0, cnt = 0;
    for (var k = i - 1; k <= i + 1; k++) {
      if (k < 0 || k >= arr.length) continue;
      var v = hNum(arr[k]);
      if (v === null) continue;
      sum += v; cnt++;
    }
    out.push(cnt ? sum / cnt : null);
  }
  return out;
}

/* 按窗口内极值归一化到 0~100 */
function hNorm(arr) {
  var mn = null, mx = null, i, v;
  for (i = 0; i < arr.length; i++) {
    v = hNum(arr[i]);
    if (v === null) continue;
    if (mn === null || v < mn) mn = v;
    if (mx === null || v > mx) mx = v;
  }
  if (mn === null) return arr.slice();
  var rng = mx - mn;
  if (rng <= 0) rng = 1;
  var out = [];
  for (i = 0; i < arr.length; i++) {
    v = hNum(arr[i]);
    out.push(v === null ? null : (v - mn) / rng * 100);
  }
  return out;
}

/* 统计: 当前/最小/最大/平均/极差/均变化 */
function hStats(arr) {
  var mn = null, mx = null, sum = 0, cnt = 0, cur = null, prev = null, diff = 0, dn = 0;
  for (var i = 0; i < arr.length; i++) {
    var v = hNum(arr[i]);
    if (v === null) continue;
    if (mn === null || v < mn) mn = v;
    if (mx === null || v > mx) mx = v;
    sum += v;
    cnt++;
    if (prev !== null) { diff += Math.abs(v - prev); dn++; }
    prev = v;
    cur = v;
  }
  return {
    cur: cur, mn: mn, mx: mx, n: cnt, total: arr.length,
    avg: cnt ? sum / cnt : null,
    range: (mn === null) ? null : mx - mn,
    drift: dn ? diff / dn : null
  };
}

/* ── 工具栏构建 ── */
function hBuildSrc() {
  var box = document.getElementById('h-src');
  if (!box) return;
  box.innerHTML = '';
  HSRC.forEach(function(s) {
    var b = document.createElement('button');
    b.className = 'seg-btn' + (hS.src === s.k ? ' active' : '');
    b.textContent = s.l;
    b.onclick = function() {
      if (hS.src === s.k) return;
      hS.src = s.k;
      hS.range = s.def;
      hS.data = null;
      hBuildSrc();
      hBuildRange();
      hFetch();
    };
    box.appendChild(b);
  });
}

function hBuildRange() {
  var box = document.getElementById('h-range');
  if (!box) return;
  box.innerHTML = '';
  HRANGE[hS.src].forEach(function(r) {
    var b = document.createElement('button');
    b.className = 'seg-btn' + (r.s === hS.range ? ' active' : '');
    b.textContent = r.l;
    b.onclick = function() {
      hS.range = r.s;
      hBuildRange();
      hSetRes();
      hFetch();
    };
    box.appendChild(b);
  });
}

function hBuildMode() {
  var box = document.getElementById('h-mode');
  if (!box) return;
  box.innerHTML = '';
  [{ k: 'norm', l: '归一化对比' }, { k: 'raw', l: '原值叠加' }].forEach(function(m) {
    var b = document.createElement('button');
    b.className = 'seg-btn' + (hS.mode === m.k ? ' active' : '');
    b.textContent = m.l;
    b.onclick = function() {
      hS.mode = m.k;
      hBuildMode();
      hRender();
    };
    box.appendChild(b);
  });
}

function hBuildChips() {
  var box = document.getElementById('h-chips');
  if (!box) return;
  box.innerHTML = '';
  var list = hS.deriv ? HCH.concat(HDERIV) : HCH;
  list.forEach(function(c) {
    var on = hS.chans.indexOf(c.id) >= 0;
    var b = document.createElement('button');
    b.className = 'chip' + (on ? ' active' : '');
    b.innerHTML = '<span class="dot" style="background:' + (on ? '#ffffff' : c.c) + '"></span>' + c.n;
    if (on) b.style.background = c.c;
    b.title = c.n + ' (' + c.u + ')';
    b.onclick = function() { hToggleChan(c.id); };
    box.appendChild(b);
  });
}

function hBuildInterval() {
  var sel = document.getElementById('h-interval');
  if (!sel) return;
  sel.innerHTML = '';
  HINTERVAL.forEach(function(v) {
    var o = document.createElement('option');
    o.value = String(v);
    o.textContent = v + ' 秒';
    sel.appendChild(o);
  });
  sel.value = String(hS.meta ? hS.meta.interval : HINTERVAL[0]);
  sel.onchange = function() {
    fetch('/api/history/config?interval=' + sel.value)
      .then(function(r) { return r.json(); })
      .then(function(d) {
        hS.meta = null;
        ts('采样间隔已设为 ' + sel.value + ' 秒', 'bg-emerald-500');
        hFetchMeta(function() { hFetch(); });
      });
  };
}

/* 落盘间隔选择器 */
function hBuildLogInterval() {
  var sel = document.getElementById('lg-interval');
  if (!sel) return;

  if (sel.options.length === 0) {
    LOGINTERVAL.forEach(function(v) {
      var o = document.createElement('option');
      o.value = String(v);
      o.textContent = v + ' 秒';
      sel.appendChild(o);
    });
    sel.onchange = function() {
      fetch('/api/history/log/config?interval=' + sel.value)
        .then(function(r) { return r.json(); })
        .then(function() {
          ts('落盘间隔已设为 ' + sel.value + ' 秒', 'bg-emerald-500');
          hFetchMeta(function() { hFetch(); });
        });
    };
  }

  var cur = (hS.log && hS.log.interval) ? hS.log.interval : 30;
  sel.value = String(cur);
}

/* Flash 持久化状态卡 */
function hUpdateLogCard() {
  var L = hS.log;
  if (!L) return;

  var pct = L.capacity ? (L.records / L.capacity * 100) : 0;
  setBar(document.getElementById('lg-bar'), pct, 0, 100);

  document.getElementById('lg-rec-txt').textContent = L.records + ' / ' + L.capacity + ' 条';
  document.getElementById('lg-size-txt').textContent =
    (L.bytes / 1024).toFixed(1) + ' KB · ' + L.segs + ' 段';
  document.getElementById('lg-span-txt').textContent = L.records
    ? (hDur(L.records * L.interval) + ' @ ' + L.interval + 's')
    : '暂无数据';

  var tag = document.getElementById('lg-clock-tag');
  if (!L.ready) {
    tag.textContent = '未启用';
    tag.className = 'text-xs px-2.5 py-1 badge-err';
  } else if (L.wall) {
    tag.textContent = '已对时 · 绝对时间';
    tag.className = 'text-xs px-2.5 py-1 badge-ok';
  } else {
    tag.textContent = '未对时 · 开机时间';
    tag.className = 'text-xs px-2.5 py-1 badge-warn';
  }
}

function hToggleChan(id) {
  var i = hS.chans.indexOf(id);
  if (i >= 0) {
    if (hS.chans.length <= 1) { ts('至少保留一个通道', 'bg-amber-500'); return; }
    hS.chans.splice(i, 1);
  } else {
    hS.chans.push(id);
  }
  hBuildChips();
  hRender();
}

/* ── 数据请求 ── */
function hNowSec() { return Math.floor(Date.now() / 1000); }

function hSetRes() {
  var cover = hS.meta ? hS.meta.rawSpan : 900;
  hS.res = (hS.range <= cover) ? 'raw' : 'min';
}

function hFetchMeta(cb) {
  fetch('/api/history/meta?t0=' + hNowSec()).then(function(r) { return r.json(); }).then(function(m) {
    hS.meta = m;
    hS.log = m.log || hS.log;
    hS.clockValid = m.clock ? !!m.clock.valid : hS.clockValid;
    hSetRes();
    hBuildInterval();
    hBuildLogInterval();
    hUpdateLogCard();
    if (cb) cb();
  }).catch(function() { if (cb) cb(); });
}

function hFetch() {
  if (hS.busy) return;
  hS.busy = true;

  fetch('/api/history?src=' + hS.src + '&res=' + hS.res + '&span=' + hS.range +
        '&points=180&t0=' + hNowSec())
    .then(function(r) { return r.json(); })
    .then(function(d) {
      hS.busy = false;
      hS.data = d;
      hS.recvAt = Date.now();

      if (d.log) hS.log = d.log;
      if (d.clock) hS.clockValid = !!d.clock.valid;

      /* 用响应内的缓冲信息同步元数据, 后续刷新只需 1 个请求 */
      if (d.buffers) {
        hS.meta = {
          now: d.now,
          interval: d.buffers.raw.interval,
          rawCount: d.buffers.raw.count, rawCap: d.buffers.raw.cap,
          rawSpan: d.buffers.raw.cap * d.buffers.raw.interval,
          minCount: d.buffers.min.count, minCap: d.buffers.min.cap,
          minSpan: d.buffers.min.cap * d.buffers.min.interval,
          latest: d.t.length ? d.t[d.t.length - 1] : 0
        };
      }
      hRender();
    })
    .catch(function() {
      hS.busy = false;
      hTag('h-res-tag', '数据读取失败', 'badge-err');
    });
}

/* ── 数据拆解 ── */
function hExtract(d) {
  var n = d.t.length, cols = {};
  HCH.forEach(function(c) { cols[c.id] = d.v[c.id] || []; });

  /* 露点 (Magnus 公式) 与 VPD */
  var at = cols.at, ah = cols.ah;
  cols.dew = [];
  cols.vpd = [];
  for (var i = 0; i < n; i++) {
    var t = hNum(at[i]), r = hNum(ah[i]);
    if (t === null || r === null || r <= 0.5 || r > 100) {
      cols.dew.push(null);
      cols.vpd.push(null);
      continue;
    }
    var g = Math.log(r / 100) + (17.27 * t) / (237.7 + t);
    cols.dew.push(237.7 * g / (17.27 - g));
    cols.vpd.push(0.6108 * Math.exp(17.27 * t / (t + 237.3)) * (1 - r / 100));
  }
  return cols;
}

/* ── 对比图 ── */
function hCompareChart() {
  if (hCmp) return hCmp;
  var el = document.getElementById('h-compare');
  if (!el) return null;

  hCmp = new Chart(el, {
    type: 'line',
    data: { labels: [], datasets: [] },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: 'index', intersect: false },
      scales: {
        x: { ticks: { maxTicksLimit: 8, font: { size: 10 }, color: '#9ca3af' }, grid: { display: false } },
        y: { beginAtZero: false, ticks: { font: { size: 10 }, color: '#9ca3af' }, grid: { color: '#f3f4f6' } }
      },
      plugins: {
        legend: { position: 'bottom', labels: { boxWidth: 8, usePointStyle: true, font: { size: 10 } } },
        tooltip: {
          callbacks: {
            label: function(ctx) {
              var ds = ctx.dataset;
              var v = ds._raw ? ds._raw[ctx.dataIndex] : null;
              if (v === null || v === undefined) return ds.label + ': 无数据';
              return ds.label + ': ' + v.toFixed(ds._dec) + ' ' + ds._unit;
            }
          }
        }
      }
    }
  });
  return hCmp;
}

function hUpdateCompare(cols, labels) {
  var ch = hCompareChart();
  if (!ch) return;

  var ds = [];
  hS.chans.forEach(function(id) {
    var m = hMetaOf(id);
    var raw = cols[id] || [];
    var vals = hS.smooth ? hSmooth(raw) : raw;
    ds.push({
      label: m.n,
      borderColor: m.c,
      backgroundColor: m.c + '22',
      data: (hS.mode === 'norm') ? hNorm(vals) : vals,
      _raw: vals,
      _unit: m.u,
      _dec: m.dec,
      borderWidth: 1.8,
      pointRadius: 0,
      pointHoverRadius: 3,
      tension: 0.3,
      fill: false,
      spanGaps: false
    });
  });

  ch.data.labels = labels;
  ch.data.datasets = ds;
  ch.options.scales.y.beginAtZero = (hS.mode === 'norm');
  ch.options.scales.y.min = (hS.mode === 'norm') ? 0 : undefined;
  ch.options.scales.y.max = (hS.mode === 'norm') ? 100 : undefined;
  ch.options.scales.y.title = {
    display: true,
    text: (hS.mode === 'norm') ? '归一化 (%)' : '实际数值',
    font: { size: 10 },
    color: '#9ca3af'
  };
  ch.update('none');
}

/* ── 单通道小图 ── */
function hEnsureGrid() {
  var wrap = document.getElementById('h-grid');
  if (!wrap) return;

  /* 移除取消选中的图表 */
  Object.keys(hGrid).forEach(function(id) {
    if (hS.chans.indexOf(id) < 0) {
      hGrid[id].destroy();
      delete hGrid[id];
    }
  });

  /* 移除多余容器 */
  Array.prototype.slice.call(wrap.children).forEach(function(c) {
    var id = c.id.replace('hgw-', '');
    if (!hGrid[id]) wrap.removeChild(c);
  });

  /* 创建缺失的容器与图表 */
  hS.chans.forEach(function(id) {
    var m = hMetaOf(id);
    if (!document.getElementById('hgw-' + id)) {
      var box = document.createElement('div');
      box.id = 'hgw-' + id;
      box.className = 'border rounded-xl p-3';
      box.innerHTML =
        '<div class="flex items-center justify-between mb-1">' +
          '<span class="text-xs font-semibold" style="color:' + m.c + '">' + m.n +
            ' <span class="text-gray-400">' + m.u + '</span></span>' +
          '<span class="text-xs text-gray-400 mono" id="hgv-' + id + '">--</span>' +
        '</div>' +
        '<div class="h-28"><canvas id="hg-' + id + '"></canvas></div>';
      wrap.appendChild(box);
    }

    if (!hGrid[id]) {
      hGrid[id] = new Chart(document.getElementById('hg-' + id), {
        type: 'line',
        data: {
          labels: [],
          datasets: [{
            label: m.n, borderColor: m.c, backgroundColor: m.c + '22',
            data: [], fill: true, borderWidth: 1.6, pointRadius: 0, tension: 0.3, spanGaps: false
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          interaction: { mode: 'index', intersect: false },
          scales: {
            x: { display: false },
            y: { ticks: { maxTicksLimit: 4, font: { size: 9 }, color: '#9ca3af' }, grid: { color: '#f3f4f6' } }
          },
          plugins: {
            legend: { display: false },
            tooltip: {
              callbacks: {
                title: function(it) { return it.length ? it[0].label : ''; },
                label: function(ctx) {
                  var v = ctx.parsed.y;
                  return (v === null || v === undefined) ? m.n + ': 无数据'
                    : m.n + ': ' + v.toFixed(m.dec) + ' ' + m.u;
                }
              }
            }
          }
        }
      });
    }
  });
}

function hUpdateGrid(cols, labels) {
  hS.chans.forEach(function(id) {
    var m = hMetaOf(id);
    var raw = cols[id] || [];
    var vals = hS.smooth ? hSmooth(raw) : raw;

    var st = hStats(raw);
    var el = document.getElementById('hgv-' + id);
    if (el) el.textContent = (st.cur === null) ? '--' : st.cur.toFixed(m.dec) + ' ' + m.u;

    var c = hGrid[id];
    if (!c) return;
    c.data.labels = labels;
    c.data.datasets[0].data = vals;
    c.update('none');
  });
}

/* ── 统计表 ── */
function hUpdateStats(cols) {
  var tb = document.getElementById('h-stats');
  if (!tb) return;

  var list = hS.deriv ? HCH.concat(HDERIV) : HCH;
  var html = '<thead><tr><th>通道</th><th>当前</th><th>最小</th><th>最大</th>' +
             '<th>平均</th><th>极差</th><th>均变化</th><th>有效点</th></tr></thead><tbody>';

  list.forEach(function(c) {
    var st = hStats(cols[c.id] || []);
    var f = function(v) {
      return (v === null || v === undefined)
        ? '<span class="text-gray-300">--</span>'
        : '<span class="mono">' + v.toFixed(c.dec) + '</span>';
    };
    html += '<tr>' +
      '<td><span class="cdot" style="background:' + c.c + '"></span>' + c.n +
        ' <span class="text-gray-400">' + c.u + '</span></td>' +
      '<td>' + f(st.cur) + '</td>' +
      '<td>' + f(st.mn) + '</td>' +
      '<td>' + f(st.mx) + '</td>' +
      '<td>' + f(st.avg) + '</td>' +
      '<td>' + f(st.range) + '</td>' +
      '<td>' + f(st.drift) + '</td>' +
      '<td><span class="mono text-gray-500">' + st.n + '/' + st.total + '</span></td>' +
      '</tr>';
  });

  html += '</tbody>';
  tb.innerHTML = html;
}

/* ── 数据明细表 ── */
function hUpdateTable(cols, labels) {
  var tb = document.getElementById('h-table');
  if (!tb) return;

  var list = hS.deriv ? HCH.concat(HDERIV) : HCH;
  var html = '<thead><tr><th>时间</th>';
  list.forEach(function(c) {
    html += '<th style="color:' + c.c + '">' + c.n + '</th>';
  });
  html += '</tr></thead><tbody>';

  var n = labels.length;
  var rows = Math.min(15, n);
  for (var i = n - 1; i >= n - rows; i--) {
    html += '<tr><td class="mono text-gray-500">' + labels[i] + '</td>';
    list.forEach(function(c) {
      var v = hNum((cols[c.id] || [])[i]);
      html += '<td>' + (v === null
        ? '<span class="text-gray-300">--</span>'
        : '<span class="mono">' + v.toFixed(c.dec) + '</span>') + '</td>';
    });
    html += '</tr>';
  }
  html += '</tbody>';
  tb.innerHTML = html;

  document.getElementById('h-table-hint').textContent =
    '最新 ' + rows + ' 条 / 当前窗口 ' + n + ' 点';
}

/* ── 记录状态 ── */
function hUpdateStatus(d) {
  var m = hS.meta;
  if (!m) return;

  document.getElementById('h-raw-txt').textContent =
    m.rawCount + ' / ' + m.rawCap + ' 点 · ' + hDur(m.rawCount * m.interval);
  setBar(document.getElementById('h-raw-bar'), m.rawCount, 0, m.rawCap);

  document.getElementById('h-min-txt').textContent =
    m.minCount + ' / ' + m.minCap + ' 点 · ' + hDur(m.minCount * 60);
  setBar(document.getElementById('h-min-bar'), m.minCount, 0, m.minCap);

  var nowU = (d && d.now !== undefined) ? d.now : m.now;
  var latest = (d && d.t && d.t.length) ? d.t[d.t.length - 1] : m.latest;
  var lag = (nowU > latest) ? (nowU - latest) : 0;
  document.getElementById('h-last-txt').textContent = lag + ' 秒前 · 间隔 ' + m.interval + ' 秒';
  setBar(document.getElementById('h-last-bar'), Math.min(lag, m.interval * 2), 0, Math.max(1, m.interval * 2));
}

/* ── 总渲染 ── */
function hRender() {
  var d = hS.data;
  if (!d) return;

  var cols = hExtract(d);

  /* Flash 已对时时时间戳就是绝对 Unix 秒, 直接用; 否则按开机时间换算 */
  var absolute = (d.src === 'flash' && d.wall);
  var long = absolute || hS.range > 3600;
  var ms = [], labels = [];
  for (var i = 0; i < d.t.length; i++) {
    ms.push(absolute ? (d.t[i] * 1000)
                     : (hS.recvAt - (d.now - d.t[i]) * 1000));
    labels.push(hFmtTime(ms[i], long));
  }

  hEnsureGrid();
  hUpdateCompare(cols, labels);
  hUpdateGrid(cols, labels);
  hUpdateStats(cols);
  hUpdateTable(cols, labels);
  hUpdateStatus(d);
  hUpdateLogCard();

  var empty = (labels.length === 0);
  var tagText, tagCls;
  if (empty) {
    tagText = '等待采样数据';
    tagCls = 'badge-warn';
  } else if (d.src === 'flash') {
    tagText = 'Flash 日志 · ' + d.interval + 's/点' + (d.wall ? ' · 绝对时间' : ' · 开机时间');
    tagCls = 'badge-ok';
  } else {
    tagText = (d.res === 'min' ? '分钟聚合 · 1点/分' : '原始采样 · ' + d.interval + 's/点');
    tagCls = 'badge-ok';
  }
  hTag('h-res-tag', tagText, tagCls);

  document.getElementById('h-stat-range').textContent = empty
    ? '暂无历史数据'
    : (labels[0] + ' ~ ' + labels[labels.length - 1] + ' · ' + labels.length + ' 点');

  document.getElementById('h-compare-hint').textContent =
    (hS.mode === 'norm'
      ? '归一化模式: 每条曲线按窗口内自身极值映射到 0~100%, 便于对比不同量级的通道'
      : '原值模式: 直接显示真实数值, 适合量级接近的通道') +
    (d.stride > 1 ? ' · 每 ' + d.stride + ' 点抽稀 1 点' : '') +
    (hS.smooth ? ' · 已启用平滑' : '') +
    (d.src === 'flash' ? ' · 数据来自 Flash 持久化日志' : '');

  document.getElementById('h-grid-hint').textContent =
    '跟随上方选中的 ' + hS.chans.length + ' 个通道';
}

/* ── 交互 ── */
function hPause() {
  hS.paused = !hS.paused;
  var b = document.getElementById('h-pause');
  b.className = 'tool-btn' + (hS.paused ? ' active' : '');
  b.innerHTML = hS.paused
    ? '<i class="fas fa-play"></i><span>继续</span>'
    : '<i class="fas fa-pause"></i><span>暂停</span>';
  ts(hS.paused ? '已暂停自动刷新' : '已恢复自动刷新', 'bg-emerald-500');
}

function hExport() {
  window.location.href = '/api/history.csv?src=' + hS.src + '&res=' + hS.res +
                         '&span=' + hS.range + '&t0=' + hNowSec();
  ts('正在导出 CSV…', 'bg-emerald-500');
}

function hClear() {
  var isLog = (hS.src === 'flash');
  var msg = isLog ? '确定清空 Flash 持久化日志吗？此操作不可恢复。'
                  : '确定清空内存历史缓冲吗？';
  if (!window.confirm(msg)) return;

  fetch(isLog ? '/api/history/log/clear' : '/api/history/clear')
    .then(function(r) { return r.json(); })
    .then(function() {
      hS.meta = null;
      hS.log = null;
      ts(isLog ? '持久化日志已清空' : '内存缓冲已清空', 'bg-emerald-500');
      hFetchMeta(function() { hFetch(); });
    });
}

/* 开机后主动与浏览器对时 */
function hSyncClock() {
  fetch('/api/time?t0=' + hNowSec())
    .then(function(r) { return r.json(); })
    .then(function(d) { hS.clockValid = !!d.valid; })
    .catch(function() {});
}

function hOnShow() {
  if (!document.getElementById('h-grid')) return;
  setTimeout(function() { if (hCmp) hCmp.resize(); }, 60);
  if (!hS.meta) {
    hFetchMeta(function() { hFetch(); });
  } else {
    hFetch();
  }
}

function hInit() {
  if (!document.getElementById('h-grid')) return;

  hBuildSrc();
  hBuildRange();
  hBuildMode();
  hBuildChips();
  hBuildInterval();
  hBuildLogInterval();
  hSyncClock();

  document.getElementById('lg-clear').onclick = function() {
    if (!window.confirm('确定清空 Flash 持久化日志吗？此操作不可恢复。')) return;
    fetch('/api/history/log/clear')
      .then(function(r) { return r.json(); })
      .then(function() {
        hS.log = null;
        ts('持久化日志已清空', 'bg-emerald-500');
        hFetchMeta(function() { hFetch(); });
      });
  };

  document.getElementById('h-pause').onclick = hPause;
  document.getElementById('h-now').onclick = function() {
    hFetch();
    ts('已刷新', 'bg-emerald-500');
  };
  document.getElementById('h-export').onclick = hExport;
  document.getElementById('h-clear').onclick = hClear;

  document.getElementById('h-smooth').onclick = function() {
    hS.smooth = !hS.smooth;
    this.className = 'tool-btn' + (hS.smooth ? ' active' : '');
    hRender();
  };

  document.getElementById('h-deriv').onclick = function() {
    hS.deriv = !hS.deriv;
    this.className = 'tool-btn' + (hS.deriv ? ' active' : '');
    if (!hS.deriv) {
      hS.chans = hS.chans.filter(function(id) { return id !== 'dew' && id !== 'vpd'; });
    }
    hBuildChips();
    hRender();
  };

  /* 后台自动刷新 (仅在历史页可见时请求) */
  setInterval(function() {
    if (hS.paused) return;
    var t1 = document.getElementById('tab-1');
    if (!t1 || t1.className.indexOf('hidden') >= 0) return;
    hFetch();
  }, 5000);
}

hInit();
