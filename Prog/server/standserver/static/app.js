'use strict';

// ---------------------------------------------------------------------------
// Deadman chain
//
// The firmware stops the motor if the host goes quiet for 300 ms, and the
// server only forwards heartbeats while this page keeps sending them. So a
// crashed tab, a dead Wi-Fi link or a sleeping laptop all end with the motor
// stopped. Hiding the tab throttles timers below the heartbeat rate, so we
// disarm deliberately on the way out instead of letting it time out.
// ---------------------------------------------------------------------------

const HEARTBEAT_MS = 100;
const THROTTLE_SEND_MS = 50;
const CHART_SECONDS = 30;

const $ = (id) => document.getElementById(id);

// Green marks the state the stand is in right now, so the grey button of a
// pair is always the one left to press.
const setActive = (id, on) => $(id).classList.toggle('active', !!on);

let ws = null;
let connected = false;
let boardState = 0;
let lastBoardState = 0;
let lastSettings = null;
let pendingThrottle = null;
let lastThrottleSent = 0;

// ---------------------------------------------------------------------------
// Charts
// ---------------------------------------------------------------------------

class Strip {
  constructor(canvas, series) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.series = series;
    this.samples = [];
    this.maxAge = CHART_SECONDS * 1000;
  }

  push(now, values) {
    this.samples.push({ t: now, v: values });
    const cutoff = now - this.maxAge;
    while (this.samples.length && this.samples[0].t < cutoff) this.samples.shift();
  }

  render() {
    const { canvas, ctx } = this;
    const dpr = window.devicePixelRatio || 1;
    const width = canvas.clientWidth;
    const height = canvas.clientHeight || 120;
    if (canvas.width !== width * dpr || canvas.height !== height * dpr) {
      canvas.width = width * dpr;
      canvas.height = height * dpr;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);

    const padTop = 22;
    const padBottom = 6;
    const plotH = height - padTop - padBottom;

    ctx.strokeStyle = '#1e2731';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = Math.round(padTop + (plotH * i) / 4) + 0.5;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(width, y);
      ctx.stroke();
    }

    if (this.samples.length < 2) return;

    // Anchor to a fixed window so the trace scrolls at a constant speed
    // instead of stretching while the buffer fills.
    const t1 = this.samples[this.samples.length - 1].t;
    const span = this.maxAge;

    let legendX = 8;
    for (const s of this.series) {
      let peak = 0;
      for (const sample of this.samples) peak = Math.max(peak, sample.v[s.key] || 0);
      const scale = s.fixedMax || Math.max(peak * 1.15, s.floor || 1);

      ctx.beginPath();
      ctx.strokeStyle = s.color;
      ctx.lineWidth = 2;
      this.samples.forEach((sample, index) => {
        const x = width - ((t1 - sample.t) / span) * width;
        const y = padTop + plotH - ((sample.v[s.key] || 0) / scale) * plotH;
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.stroke();

      const current = this.samples[this.samples.length - 1].v[s.key] || 0;
      const text = `${s.label} ${s.format(current)}`;
      ctx.fillStyle = s.color;
      ctx.font = '600 11px ui-monospace, Menlo, monospace';
      ctx.fillText(text, legendX, 14);
      legendX += ctx.measureText(text).width + 18;
    }
  }
}

const chartRpm = new Strip($('chartRpm'), [
  { key: 'rpm', color: '#4da3ff', label: 'об/мин', floor: 1000, format: (v) => Math.round(v).toLocaleString('ru-RU') },
  { key: 'thrust', color: '#37c978', label: 'г', floor: 100, format: (v) => Math.round(v).toLocaleString('ru-RU') },
  { key: 'thr', color: '#8b98a9', label: 'газ', fixedMax: 100, format: (v) => v.toFixed(0) + '%' },
]);

const chartPower = new Strip($('chartPower'), [
  { key: 'power', color: '#f6a935', label: 'Вт', floor: 50, format: (v) => v.toFixed(0) },
  { key: 'current', color: '#e8433d', label: 'А', floor: 5, format: (v) => v.toFixed(2) },
]);

// Наибольшие значения за прогон. Их два на каждую величину, потому что они
// отвечают на разные вопросы: пик (мгновенное) — что должно выдержать железо,
// рабочее (наибольшее среднее за секунду) — на чём мотор реально работает.
// Одиночный выброс поднимает первое и почти не двигает второе.
class Peaks {
  constructor() {
    this.reset();
  }

  reset() {
    this.peak = new Map();
    this.work = new Map();
  }

  // `value` — мгновенное значение, `average` — оно же, сглаженное за секунду.
  put(key, value, average) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      this.peak.set(key, Math.max(this.peak.get(key) ?? value, value));
    }
    if (typeof average === 'number' && Number.isFinite(average)) {
      this.work.set(key, Math.max(this.work.get(key) ?? average, average));
    }
  }

  // Строка вида «Обороты: пик 12 345, рабочие 11 980 об/мин», или прочерк,
  // пока величина не измерялась ни разу.
  line(key, name, unit, format) {
    const peak = this.peak.get(key);
    const work = this.work.get(key);
    if (peak === undefined) {
      return `${name}: —`;
    }
    return `${name}: пик ${format(peak)}, рабочее ${format(work ?? peak)} ${unit}`;
  }
}

const peaks = new Peaks();
const asInt = (v) => Math.round(v).toLocaleString('ru-RU');

function renderPeaks() {
  $('peaksRpm').textContent = [
    peaks.line('rpm', 'Обороты', 'об/мин', asInt),
    peaks.line('thrust', 'Тяга', 'г', asInt),
  ].join(' · ');
  $('peaksPower').textContent = [
    peaks.line('power', 'Мощность', 'Вт', asInt),
    peaks.line('current', 'Ток', 'А', (v) => v.toFixed(2)),
  ].join(' · ');
}

function renderCharts() {
  chartRpm.render();
  chartPower.render();
  requestAnimationFrame(renderCharts);
}
requestAnimationFrame(renderCharts);

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

function send(message) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
}

function connect() {
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${scheme}://${location.host}/ws`);

  ws.onopen = () => {
    connected = true;
    setBanner('');
  };
  ws.onclose = () => {
    connected = false;
    setBanner('Связь с сервером потеряна. Переподключение…');
    setTimeout(connect, 1000);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.type === 'state') applyState(message);
  };
}

setInterval(() => send({ type: 'hb' }), HEARTBEAT_MS);

// Flush a pending slider value at a fixed rate instead of on every pixel.
setInterval(() => {
  if (pendingThrottle === null) return;
  const now = Date.now();
  if (now - lastThrottleSent < THROTTLE_SEND_MS) return;
  send({ type: 'throttle', motor: 1, value: pendingThrottle });
  lastThrottleSent = now;
  pendingThrottle = null;
}, THROTTLE_SEND_MS);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

const STATE_NAMES = ['БЕЗОПАСНО', 'ГАЗ ПОДАН', 'АВАРИЯ'];
const STATE_CLASSES = ['safe', 'armed', 'fault'];
const FAULT_NAMES = [
  [1, 'потеря связи с компьютером'],
  [2, 'нажат аварийный стоп'],
  [4, 'превышен ток'],
  [8, 'перегрев ESC'],
];

function setBanner(text, kind) {
  const banner = $('banner');
  banner.textContent = text;
  banner.classList.toggle('hidden', !text);
  banner.style.background = kind === 'error' ? 'var(--danger)' : 'var(--warn)';
  banner.style.color = kind === 'error' ? '#fff' : '#1a1206';
}

function applyState(message) {
  const telemetry = message.telemetry || {};
  const link = message.link || {};
  const esc = telemetry.esc || {};

  // Connection indicator
  const dot = $('linkDot');
  dot.classList.toggle('online', !!link.streaming);
  dot.classList.toggle('stale', !!link.connected && !link.streaming);
  const board = link.board || {};
  const released = link.heldOffFor > 0;
  $('btnReleasePort').classList.toggle('hidden', released);
  $('btnAcquirePort').classList.toggle('hidden', !released);

  if (released) {
    $('linkInfo').textContent = `порт отпущен ещё на ${Math.ceil(link.heldOffFor)} с`;
  } else if (!link.connected) {
    $('linkInfo').textContent = 'плата не найдена';
  } else if (!link.identified) {
    $('linkInfo').textContent = `${link.port} · устройство не отвечает как плата стенда`;
  } else {
    $('linkInfo').textContent = `${link.port} · ${board.version || '?'}${link.simulated ? ' · симуляция' : ''}`;
  }

  // State badge
  boardState = telemetry.state || 0;
  const badge = $('stateBadge');
  badge.textContent = STATE_NAMES[boardState] || '—';
  badge.className = 'badge ' + (STATE_CLASSES[boardState] || '');

  const faults = telemetry.faults || 0;
  $('btnReset').classList.toggle('hidden', boardState !== 2);
  if (faults) {
    const names = FAULT_NAMES.filter(([bit]) => faults & bit).map(([, name]) => name);
    setBanner('Авария: ' + names.join(', ') + '. Устраните причину и нажмите «Сброс аварии».', 'error');
  } else if (link.simulated) {
    // Симуляцию нельзя опознать по самим цифрам: они выглядят правдоподобно.
    // Спутать её с настоящим прогоном — значит записать выдуманный результат,
    // поэтому предупреждение висит постоянно, а не строчкой в углу.
    setBanner('Работает модель платы: все показания вымышленные, мотор ничем не управляется.');
  } else if (connected && !link.connected) {
    setBanner('Плата не подключена — проверьте USB-кабель.');
  } else if (connected && link.heldOffFor > 0) {
    setBanner('Порт отдан другой программе. Нажмите «Забрать порт», когда закончите с конфигуратором.');
  } else if (connected && !link.identified) {
    setBanner(`На порту ${link.port} что-то есть, но это не прошивка стенда. Прошейте плату: ./scripts/flash.sh`);
  } else if (connected) {
    setBanner('');
  }

  // Throttle
  const armed = boardState === 1;
  // Only a live board can light anything up: with no telemetry arriving, a
  // green DISARM would be claiming to know a state nobody reported.
  const live = !!link.streaming;
  setActive('btnArm', live && armed);
  setActive('btnDisarm', live && boardState === 0);
  // Arming a board that has not identified itself cannot work, and a dead
  // button says so more clearly than a command that quietly goes nowhere.
  $('btnArm').disabled = !link.identified;
  const slider = $('throttle');
  slider.disabled = !armed;
  if (!armed && slider.value !== '0') slider.value = '0';
  if (message.autotest && message.autotest.running) {
    slider.value = String(message.autotest.throttle);
  }
  $('throttleCmd').textContent = (Number(slider.value) / 10).toFixed(1);
  markPresets(Number(slider.value));
  const applied = (telemetry.thr || [0])[0] / 10;
  $('throttleActual').textContent = applied.toFixed(1);
  // Само число в кадре DShot. Если два разных положения газа дают одинаковые
  // обороты, первый вопрос — уходят ли в регулятор разные числа; «факт» на это
  // не отвечает, потому что его режет предел газа и он же показывает рампу.
  $('throttleRaw').textContent = String((telemetry.out || [0])[0]);

  // Tiles
  // Voltage and current are measured in two independent places, and the board's
  // own sense pins win. They are what the operator calibrated against a meter,
  // whereas the figures inside the packet are the regulator's own guess: the
  // AM32 targets for this ESC never set MILLIVOLT_PER_AMP, so its current is a
  // project-wide default applied to whatever its ADC happens to see. The ESC is
  // used only when the board reports no sense pins at all.
  const sense = telemetry.board || {};
  const escOk = esc.ok === 1;
  const haveBoard = sense.mv !== undefined;
  const volts = ((haveBoard ? sense.mv : esc.mv) || 0) / 1000;
  const amps = ((haveBoard ? sense.ma : esc.ma) || 0) / 1000;
  const haveSense = escOk || haveBoard;

  // Мощность считается из усреднённых слагаемых, иначе она дёргалась бы
  // сильнее их обоих, перемножая два разных шума.
  const voltsAvg = smooth.put('volts', haveSense ? volts : null);
  const ampsAvg = smooth.put('amps', haveSense ? amps : null);
  const rpmAvg = smooth.put('rpm', escOk ? esc.rpm || 0 : null);

  $('mRpm').textContent = rpmAvg === null ? '—' : Math.round(rpmAvg).toLocaleString('ru-RU');
  $('mCurrent').textContent = ampsAvg === null ? '—' : ampsAvg.toFixed(2);
  $('mVoltage').textContent = voltsAvg === null ? '—' : voltsAvg.toFixed(2);
  $('mPower').textContent = voltsAvg === null ? '—' : (voltsAvg * ampsAvg).toFixed(0);
  $('mTemp').textContent = escOk ? String(esc.tempC ?? '—') : '—';
  // Расход считается от последнего сброса, а не от подачи питания: счётчик
  // внутри регулятора не обнуляется ничем и между прогонами только растёт.
  $('mMah').textContent =
    typeof telemetry.mahUsed === 'number' ? String(telemetry.mahUsed) : '—';

  // Thrust comes from the board as raw counts and only becomes grams once the
  // dialog has been through a calibration, so there are three states to tell
  // apart: no sensor, a sensor nobody has calibrated, and a real reading.
  const calibration = message.calibration || {};
  const thrust = telemetry.thrustG;
  const haveLoad = !!telemetry.load;
  const thrustAvg = smooth.put('thrust', typeof thrust === 'number' ? thrust : null);
  $('mThrust').textContent =
    thrustAvg === null ? '—' : Math.round(thrustAvg).toLocaleString('ru-RU');

  $('senseSource').textContent = [
    haveBoard
      ? 'ток и напряжение — с датчиков платы' +
        (escOk ? ', обороты и температура — из телеметрии ESC' : ', телеметрия ESC не идёт')
      : escOk
        ? 'плата не сообщает свои датчики, всё берётся из телеметрии ESC'
        : 'нет данных ни от ESC, ни с датчиков платы',
    esc.foreign > 0
      ? 'в линию телеметрии говорит больше одного регулятора — их пакеты ' +
        `отброшены (${esc.foreign}); выключите телеметрию у незадействованных ` +
        'в конфигураторе AM32'
      : null,
    !haveLoad
      ? 'датчик тяги плата пока не передаёт'
      : calibration.loadCalibrated
        ? 'тяга — с датчика стенда'
        : 'датчик тяги не откалиброван, нажмите ⚙',
    'в плитках среднее за секунду, на графиках и в файле — мгновенные значения',
  ].filter(Boolean).join(' · ');

  if (link.streaming) {
    const now = Date.now();
    chartRpm.push(now, { rpm: esc.rpm || 0, thr: applied, thrust: thrust || 0 });
    chartPower.push(now, { power: volts * amps, current: amps });

    // Каждый ARM — это новый прогон, и цифры прошлого к нему отношения не
    // имеют. Накопление продолжается и после DISARM: на остановленном моторе
    // максимумы уже не меняются, зато их можно спокойно прочитать.
    if (boardState === 1 && lastBoardState !== 1) {
      peaks.reset();
    }
    lastBoardState = boardState;

    if (escOk) {
      peaks.put('rpm', esc.rpm || 0, rpmAvg);
    }
    if (typeof thrust === 'number') {
      peaks.put('thrust', thrust, thrustAvg);
    }
    if (haveSense) {
      peaks.put('current', amps, ampsAvg);
      peaks.put('power', volts * amps, voltsAvg === null ? null : voltsAvg * ampsAvg);
    }
    renderPeaks();
  }

  updateCalibrationDialog(telemetry, link.settings || {}, calibration, message.log || []);

  // Settings, filled once from the board then left to the operator
  if (link.settings && JSON.stringify(link.settings) !== JSON.stringify(lastSettings)) {
    lastSettings = link.settings;
    $('setMaxThrottle').value = Math.round(link.settings.maxThrottle / 10);
    $('setRamp').value = Math.round(link.settings.rampPerS / 10);
    $('setPoles').value = link.settings.motorPoles;
    $('setCurrentLimit').value = Math.round(link.settings.currentLimitMa / 1000);
    $('setTempLimit').value = link.settings.tempLimitC;
  }

  // Read-only now: the protocol is fixed to DShot on both sides. Showing it
  // still matters, because a board that came up in servo PWM would be unable
  // to drive the ESC and the reason would otherwise be invisible.
  const proto = telemetry.proto;
  const protoBadge = $('protoBadge');
  if (proto) {
    protoBadge.textContent = proto;
    const dshot = proto.startsWith('DSHOT');
    protoBadge.className = 'badge ' + (dshot ? 'ok' : 'warn');
    if (!dshot) {
      setBanner(`Плата выдаёт ${proto} вместо DShot — регулятор такой сигнал не поймёт.`, 'error');
    }
  }

  // Recorder
  const recorder = message.recorder || {};
  setActive('btnRecStart', recorder.active);
  $('recStatus').textContent = recorder.active
    ? `пишется ${recorder.file} · строк: ${recorder.rows}`
    : recorder.file
      ? `сохранено: ${recorder.file}`
      : 'не пишется';

  // Autotest
  const autotest = message.autotest || {};
  setActive('btnAutotest', autotest.running);
  $('atProgress').classList.toggle('hidden', !autotest.running);
  if (autotest.running) {
    const percent = (autotest.step / autotest.total) * 100;
    $('atBar').style.width = percent + '%';
    $('atLabel').textContent = `ступень ${autotest.step}/${autotest.total} — ${(autotest.throttle / 10).toFixed(0)}%`;
  }

  // Log
  const list = $('log');
  list.innerHTML = '';
  for (const entry of (message.log || []).slice().reverse()) {
    const item = document.createElement('li');
    item.className = entry.level || 'info';
    const time = new Date((entry.at || 0) * 1000).toLocaleTimeString('ru-RU');
    item.textContent = `${time}  ${entry.msg}`;
    list.appendChild(item);
  }
  // The raw byte count is what tells the two failure modes apart, so it is
  // shown even when nothing decodes: zero bytes means the telemetry wire is
  // silent (in practice: telemetry_on_interval is off in the ESC), while bytes
  // without packets means data arrives but never forms a valid frame.
  if (escOk) {
    $('rawInfo').textContent =
      `пакетов ESC: ${esc.pkts}, ошибок CRC: ${esc.crcErr}, байт: ${esc.bytes ?? '—'}`;
  } else if (esc.bytes > 2 || esc.edges > 10) {
    // Что-то на линии есть, но кадр не складывается. Одиночные байты тут не в
    // счёт: проба подтяжками сама качает вывод и тем рождает пару штук.
    $('rawInfo').textContent =
      `на линии есть сигнал (байт: ${esc.bytes}, фронтов: ${esc.edges}), но кадр ` +
      `не собирается — не та скорость или не тот формат. Ошибок CRC: ${esc.crcErr}, ` +
      `ошибок кадра: ${esc.frameErr}`;
  } else {
    // Вердикт пробы показываем всегда, когда телеметрии нет: именно он
    // отвечает на вопрос «а что вообще на том конце провода».
    $('rawInfo').textContent = `телеметрии нет · ${padVerdict(esc)}`;
  }
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

$('throttle').addEventListener('input', (event) => {
  pendingThrottle = Number(event.target.value);
  $('throttleCmd').textContent = (pendingThrottle / 10).toFixed(1);
  markPresets(pendingThrottle);
});

const presets = $('presets');
const presetButtons = [];
for (const percent of [0, 10, 20, 30, 40, 50, 60, 70, 85, 100]) {
  const button = document.createElement('button');
  button.textContent = percent + '%';
  button.addEventListener('click', () => {
    $('throttle').value = String(percent * 10);
    pendingThrottle = percent * 10;
    $('throttleCmd').textContent = percent.toFixed(1);
    markPresets(percent * 10);
  });
  presets.appendChild(button);
  presetButtons.push({ perMille: percent * 10, button });
}

function markPresets(perMille) {
  for (const preset of presetButtons) {
    preset.button.classList.toggle('active', preset.perMille === perMille);
  }
}

$('btnArm').addEventListener('click', () => {
  $('throttle').value = '0';
  pendingThrottle = 0;
  send({ type: 'arm' });
});
$('btnMahReset').addEventListener('click', () => send({ type: 'mahReset' }));
$('btnDisarm').addEventListener('click', () => send({ type: 'disarm' }));
$('btnReset').addEventListener('click', () => send({ type: 'reset' }));
$('btnEstop').addEventListener('click', () => send({ type: 'estop' }));

$('btnBeep').addEventListener('click', () => send({ type: 'dshot', motor: 1, command: 1, repeat: 10 }));

$('btnReleasePort').addEventListener('click', () => {
  if (confirm('Отдать USB-порт другой программе? Мотор будет остановлен.')) {
    fetch('/api/port/release', { method: 'POST' });
  }
});
$('btnAcquirePort').addEventListener('click', () => fetch('/api/port/acquire', { method: 'POST' }));
const SETTING_INPUTS = [
  ['setMaxThrottle', 'maxThrottle', 10],
  ['setRamp', 'rampPerS', 10],
  ['setPoles', 'motorPoles', 1],
  ['setCurrentLimit', 'currentLimitMa', 1000],
  ['setTempLimit', 'tempLimitC', 1],
];
for (const [id, key, factor] of SETTING_INPUTS) {
  $(id).addEventListener('change', (event) => {
    send({ type: 'setting', key, value: Math.round(Number(event.target.value) * factor) });
  });
}

// ---------------------------------------------------------------------------
// Calibration dialog
// ---------------------------------------------------------------------------

// Only messages logged after the dialog was opened can be about this session's
// calibration, so the operator never sees an old error as a fresh answer.
let calOpenedAt = 0;

// Цифры в плитках скачут не от шума в проводке, а от разрешения АЦП: один его
// отсчёт на токе — это около 75 мА, так что последняя цифра дёргается сама по
// себе и прочитать её на глаз невозможно. Усреднение за секунду делает число
// читаемым и ничего не прячет: графики и запись в файл берут сырые значения,
// поэтому и переходный процесс, и выброс останутся видны там.
const SMOOTH_WINDOW_MS = 1000;

class Smoothed {
  constructor(windowMs = SMOOTH_WINDOW_MS) {
    this.windowMs = windowMs;
    this.series = new Map();
  }

  // Кладёт значение и возвращает среднее за окно. null означает «показывать
  // нечего»: источник пропал, и накопленное среднее было бы враньём.
  put(key, value) {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      this.series.delete(key);
      return null;
    }

    const now = performance.now();
    let samples = this.series.get(key);

    if (!samples) {
      samples = [];
      this.series.set(key, samples);
    }
    samples.push([now, value]);
    while (samples.length > 1 && now - samples[0][0] > this.windowMs) {
      samples.shift();
    }

    let sum = 0;
    for (const sample of samples) {
      sum += sample[1];
    }
    return sum / samples.length;
  }
}

const smooth = new Smoothed();

// Плата при загрузке читает вывод телеметрии под внутренней подтяжкой вверх,
// потом вниз. Слабая подтяжка (около 40 кОм) проигрывает всему, что линию
// действительно ведёт, поэтому пара уровней говорит, что на другом конце.
function padVerdict(esc) {
  if (esc.padUp === undefined) return 'плата не сообщает пробу вывода';
  if (esc.padUp === 1 && esc.padDown === 0) {
    return 'вывод TEL ни к чему не подключён: он просто идёт за внутренней ' +
      'подтяжкой. Провод телеметрии не доходит до полётника';
  }
  if (esc.padUp === 0 && esc.padDown === 0) {
    return 'вывод TEL прижат к нулю чем-то внешним — обычно это неисправный ' +
      'регулятор либо замыкание в жгуте';
  }
  if (esc.padUp === 1 && esc.padDown === 1) {
    return 'вывод TEL удерживается высоким, то есть передатчик подключён и ' +
      'молчит — телеметрия в регуляторе выключена';
  }
  return `проба вывода: вверх ${esc.padUp}, вниз ${esc.padDown}`;
}

// Ноль ампер и отсутствие датчика выглядят на экране одинаково, и разница
// между ними решает, что чинить. Различить их можно только по сырому сигналу
// с вывода АЦП вместе с тем, крутится ли сейчас мотор.
function currentSourceNote(telemetry) {
  const esc = telemetry.esc || {};
  const sense = telemetry.board || {};
  const spinning = telemetry.state === 1 && (telemetry.thr || [0])[0] > 0;

  if (sense.iPinMv === undefined) return 'плата не сообщает сигнал датчика тока';
  if (sense.iPinMv > 0) return 'датчик тока платы выдаёт сигнал, калибровка возможна';
  if (!spinning) return 'мотор стоит, поэтому 0 мВ — это норма. Дайте газ и посмотрите сюда снова';
  return 'мотор крутится, а на входе тока 0 мВ — сигнала нет, калибровка тут не поможет';
}

function updateCalibrationDialog(telemetry, settings, calibration, log) {
  if (!$('calDialog').open) return;

  const sense = telemetry.board || {};
  const load = telemetry.load || {};
  // Здесь усреднение нужнее всего: по этим числам оператор решает, совпало ли
  // показание с внешним прибором, а по дёргающейся последней цифре решить
  // нельзя. Сама калибровка считается на сервере по данным платы и от того,
  // что нарисовано тут, не зависит.
  const nowV = smooth.put('calV', sense.mv === undefined ? null : sense.mv / 1000);
  const nowA = smooth.put('calA', sense.ma === undefined ? null : sense.ma / 1000);
  const nowG = smooth.put('calG', typeof telemetry.thrustG === 'number' ? telemetry.thrustG : null);

  $('calNowVoltage').textContent = nowV === null ? '—' : nowV.toFixed(2);
  $('calNowCurrent').textContent = nowA === null ? '—' : nowA.toFixed(2);
  $('calNowThrust').textContent = nowG === null ? '—' : Math.round(nowG);

  // Сырые милливольты на выводах АЦП. Без них нельзя отличить «тока нет» от
  // «датчик ничего не выдаёт»: пересчитанный ток в обоих случаях ноль.
  // Дробная цифра здесь не лишняя — шаг АЦП 0.8 мВ, и целые милливольты
  // после усреднения выглядели бы так же дёргано, как до него.
  const pinV = smooth.put('calPinV', sense.vPinMv ?? null);
  const pinA = smooth.put('calPinA', sense.iPinMv ?? null);

  $('calPinVoltage').textContent = pinV === null ? '—' : pinV.toFixed(1);
  $('calPinCurrent').textContent = pinA === null ? '—' : pinA.toFixed(1);
  // Масштабы в милли-единицах ничего не говорят глазу. В вольтах и амперах
  // сразу видно, если калибровка увела коэффициент далеко от паспортного —
  // у ArduPilot для этой платы 8.3 В/В и 92.6 А/В.
  $('calScaleVoltage').textContent =
    settings.vbatScaleM === undefined ? '—' : (settings.vbatScaleM / 1000).toFixed(2) + ' В/В';
  $('calScaleCurrent').textContent =
    settings.currentScaleM === undefined ? '—' : (settings.currentScaleM / 1000).toFixed(1) + ' А/В';
  $('calZeroCurrent').textContent = settings.currentOffsetMv ?? '—';
  $('calCurrentSource').textContent = currentSourceNote(telemetry);

  // Плата сама сообщает, совпадает ли то, что в её памяти, с тем, чем она
  // сейчас считает. Пока не совпало, калибровка живёт до первого выключения.
  const saved = settings.saved;
  const savedLine = $('calSaved');
  savedLine.textContent =
    saved === undefined
      ? 'плата не сообщает, сохранены ли настройки'
      : saved
        ? 'настройки записаны в память платы'
        : 'настройки не записаны в память платы — при выключении питания потеряются';
  savedLine.classList.toggle('ok', saved === true);
  savedLine.classList.toggle('warn', saved === false);
  $('calRawThrust').textContent = load.raw ?? '—';
  $('calScaleThrust').textContent = calibration.loadScale
    ? calibration.loadScale.toFixed(5)
    : 'не задан';

  const answers = log.filter(
    (entry) => (entry.at || 0) * 1000 > calOpenedAt && /калибров|ноль |тар/i.test(entry.msg || '')
  );
  const last = answers[answers.length - 1];
  $('calStatus').textContent = last ? last.msg : 'изменения применяются сразу после нажатия';
}

$('btnCalibrate').addEventListener('click', () => {
  calOpenedAt = Date.now();
  $('calStatus').textContent = 'изменения применяются сразу после нажатия';
  $('calDialog').showModal();
});
$('calClose').addEventListener('click', () => $('calDialog').close());

for (const button of document.querySelectorAll('[data-cal]')) {
  button.addEventListener('click', () => {
    const payload = { type: 'calibrate', target: button.dataset.cal };
    const inputId = button.dataset.input;
    if (inputId) {
      const measured = Number($(inputId).value);
      if (!Number.isFinite(measured) || measured <= 0) {
        $('calStatus').textContent = 'впишите измеренное значение больше нуля';
        return;
      }
      payload.value = measured;
    }
    send(payload);
  });
}

$('btnRecStart').addEventListener('click', () => send({ type: 'record', action: 'start', name: $('recName').value }));
$('btnRecStop').addEventListener('click', () => {
  send({ type: 'record', action: 'stop' });
  setTimeout(refreshLogs, 400);
});

$('btnAutotest').addEventListener('click', () => {
  if (boardState !== 1) {
    setBanner('Сначала нажмите ARM.');
    return;
  }
  const hold = Number($('atHold').value) || 3;
  const steps = $('atSteps').value
    .split(',')
    .map((part) => Number(part.trim()))
    .filter((value) => Number.isFinite(value) && value >= 0 && value <= 100)
    .map((percent) => ({ throttle: Math.round(percent * 10), hold }));
  if (!steps.length) return;
  send({ type: 'autotest', action: 'start', motor: 1, steps });
});
$('btnAutotestStop').addEventListener('click', () => send({ type: 'autotest', action: 'stop' }));

async function refreshLogs() {
  try {
    const response = await fetch('/api/logs');
    const files = await response.json();
    const list = $('logList');
    list.innerHTML = '';
    for (const file of files) {
      const item = document.createElement('li');
      const link = document.createElement('a');
      link.href = '/api/logs/' + encodeURIComponent(file.name);
      link.textContent = file.name;
      link.download = file.name;
      item.appendChild(link);
      item.appendChild(document.createTextNode(`  ${(file.bytes / 1024).toFixed(0)} КБ`));
      list.appendChild(item);
    }
  } catch (error) {
    /* the list is a convenience, not worth surfacing */
  }
}
refreshLogs();
setInterval(refreshLogs, 30000);

// ---------------------------------------------------------------------------
// Keyboard and visibility
// ---------------------------------------------------------------------------

document.addEventListener('keydown', (event) => {
  const typing = ['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement.tagName);
  if (event.code === 'Space') {
    event.preventDefault();
    send({ type: 'estop' });
    return;
  }
  if (typing || boardState !== 1) return;
  const step = event.shiftKey ? 50 : 10;
  if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
    event.preventDefault();
    const slider = $('throttle');
    const next = Math.max(0, Math.min(1000, Number(slider.value) + (event.key === 'ArrowRight' ? step : -step)));
    slider.value = String(next);
    pendingThrottle = next;
    $('throttleCmd').textContent = (next / 10).toFixed(1);
  }
});

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    // Background tabs get their timers throttled below the heartbeat rate, so
    // stop the motor on purpose rather than tripping the watchdog.
    send({ type: 'disarm' });
  }
});

// Leaving the page must free the USB port, and the only signal the server can
// trust is the socket actually closing. So close it deliberately rather than
// hoping the browser gets round to it: `beforeunload` never fires on iOS, and
// a page frozen into the back/forward cache stops its timers while keeping the
// socket open, which would look like a live session holding the board.
function endSession() {
  send({ type: 'disarm' });
  if (!ws) return;
  ws.onclose = null; // the page is going away, so no reconnect
  ws.onerror = null;
  try {
    ws.close();
  } catch (error) {
    /* already gone */
  }
}

window.addEventListener('pagehide', endSession);
window.addEventListener('beforeunload', endSession);

// A page restored from the back/forward cache comes back with a dead socket
// and no reconnect timer, because endSession removed it on the way out.
window.addEventListener('pageshow', (event) => {
  if (event.persisted || !ws || ws.readyState === WebSocket.CLOSED) connect();
});

connect();
