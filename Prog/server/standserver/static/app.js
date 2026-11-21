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
  { key: 'rpm', color: '#4da3ff', label: 'rpm', floor: 1000, format: (v) => Math.round(v).toLocaleString('en-GB') },
  { key: 'thrust', color: '#37c978', label: 'g', floor: 100, format: (v) => Math.round(v).toLocaleString('en-GB') },
  { key: 'thr', color: '#8b98a9', label: 'throttle', fixedMax: 100, format: (v) => v.toFixed(0) + '%' },
]);

const chartPower = new Strip($('chartPower'), [
  { key: 'power', color: '#f6a935', label: 'W', floor: 50, format: (v) => v.toFixed(0) },
  { key: 'current', color: '#e8433d', label: 'A', floor: 5, format: (v) => v.toFixed(2) },
]);

// The largest values of a run. There are two of them per quantity, because
// they answer different questions: the peak (instantaneous) is what the
// hardware has to survive, the working value (the largest one-second average)
// is what the motor actually runs at. A single spike lifts the first and
// barely moves the second.
class Peaks {
  constructor() {
    this.reset();
  }

  reset() {
    this.peak = new Map();
    this.work = new Map();
  }

  // `value` is the instantaneous value, `average` is the same thing smoothed
  // over a second.
  put(key, value, average) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      this.peak.set(key, Math.max(this.peak.get(key) ?? value, value));
    }
    if (typeof average === 'number' && Number.isFinite(average)) {
      this.work.set(key, Math.max(this.work.get(key) ?? average, average));
    }
  }

  // A line of the form "Rpm: peak 12,345, working 11,980 rpm", or a dash
  // while the quantity has never been measured.
  line(key, name, unit, format) {
    const peak = this.peak.get(key);
    const work = this.work.get(key);
    if (peak === undefined) {
      return `${name}: —`;
    }
    return `${name}: peak ${format(peak)}, working ${format(work ?? peak)} ${unit}`;
  }
}

const peaks = new Peaks();
const asInt = (v) => Math.round(v).toLocaleString('en-GB');

function renderPeaks() {
  $('peaksRpm').textContent = [
    peaks.line('rpm', 'Rpm', 'rpm', asInt),
    peaks.line('thrust', 'Thrust', 'g', asInt),
  ].join(' · ');
  $('peaksPower').textContent = [
    peaks.line('power', 'Power', 'W', asInt),
    peaks.line('current', 'Current', 'A', (v) => v.toFixed(2)),
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
    setBanner('Lost the link to the server. Reconnecting…');
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

const STATE_NAMES = ['SAFE', 'THROTTLE LIVE', 'FAULT'];
const STATE_CLASSES = ['safe', 'armed', 'fault'];
const FAULT_NAMES = [
  [1, 'lost the link to the computer'],
  [2, 'emergency stop pressed'],
  [4, 'current exceeded'],
  [8, 'ESC overheated'],
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
    $('linkInfo').textContent = `port stays released for another ${Math.ceil(link.heldOffFor)} s`;
  } else if (!link.connected) {
    $('linkInfo').textContent = 'board not found';
  } else if (!link.identified) {
    $('linkInfo').textContent = `${link.port} · the device does not answer as a stand board`;
  } else {
    $('linkInfo').textContent = `${link.port} · ${board.version || '?'}${link.simulated ? ' · simulation' : ''}`;
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
    setBanner('Fault: ' + names.join(', ') + '. Remove the cause and press “Clear fault”.', 'error');
  } else if (link.simulated) {
    // A simulation cannot be recognised from the numbers themselves: they look
    // plausible. Mistaking it for a real run means recording an invented
    // result, so the warning stays up instead of being a line in the corner.
    setBanner('A model of the board is running: every reading is invented and nothing is driving the motor.');
  } else if (connected && !link.connected) {
    setBanner('The board is not connected — check the USB cable.');
  } else if (connected && link.heldOffFor > 0) {
    setBanner('The port has been given to another program. Press “Take port” when you are done with the configurator.');
  } else if (connected && !link.identified) {
    setBanner(`There is something on port ${link.port}, but it is not the stand firmware. Flash the board: ./scripts/flash.sh`);
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
  // The number inside the DShot frame itself. If two different throttle
  // positions give the same rpm, the first question is whether different
  // numbers reach the ESC at all; "actual" does not answer that, because the
  // throttle limit cuts it and it also shows the ramp.
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

  // Power is computed from the averaged factors, otherwise it would twitch
  // more than either of them, multiplying two different noises together.
  const voltsAvg = smooth.put('volts', haveSense ? volts : null);
  const ampsAvg = smooth.put('amps', haveSense ? amps : null);
  const rpmAvg = smooth.put('rpm', escOk ? esc.rpm || 0 : null);

  $('mRpm').textContent = rpmAvg === null ? '—' : Math.round(rpmAvg).toLocaleString('en-GB');
  $('mCurrent').textContent = ampsAvg === null ? '—' : ampsAvg.toFixed(2);
  $('mVoltage').textContent = voltsAvg === null ? '—' : voltsAvg.toFixed(2);
  $('mPower').textContent = voltsAvg === null ? '—' : (voltsAvg * ampsAvg).toFixed(0);
  $('mTemp').textContent = escOk ? String(esc.tempC ?? '—') : '—';
  // Consumption is counted from the last reset, not from power-up: the counter
  // inside the ESC is zeroed by nothing and only grows between runs.
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
    thrustAvg === null ? '—' : Math.round(thrustAvg).toLocaleString('en-GB');

  $('senseSource').textContent = [
    haveBoard
      ? 'current and voltage come from the board sensors' +
        (escOk ? ', rpm and temperature from the ESC telemetry' : ', no ESC telemetry is arriving')
      : escOk
        ? 'the board does not report its own sensors, everything is taken from the ESC telemetry'
        : 'no data from the ESC and none from the board sensors',
    esc.foreign > 0
      ? 'more than one ESC is talking on the telemetry line — their packets ' +
        `were dropped (${esc.foreign}); turn telemetry off on the unused ones ` +
        'in the AM32 configurator'
      : null,
    !haveLoad
      ? 'the board does not send a thrust sensor yet'
      : calibration.loadCalibrated
        ? 'thrust comes from the stand sensor'
        : 'the thrust sensor is not calibrated, press ⚙',
    'the tiles show the average over a second, the charts and the file hold instantaneous values',
  ].filter(Boolean).join(' · ');

  if (link.streaming) {
    const now = Date.now();
    chartRpm.push(now, { rpm: esc.rpm || 0, thr: applied, thrust: thrust || 0 });
    chartPower.push(now, { power: volts * amps, current: amps });

    // Every ARM is a new run, and the numbers of the previous one have nothing
    // to do with it. Accumulation carries on after DISARM as well: on a
    // stopped motor the maxima no longer change, but they can be read calmly.
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
      setBanner(`The board puts out ${proto} instead of DShot — the ESC will not understand such a signal.`, 'error');
    }
  }

  // Recorder
  const recorder = message.recorder || {};
  setActive('btnRecStart', recorder.active);
  $('recStatus').textContent = recorder.active
    ? `recording ${recorder.file} · rows: ${recorder.rows}`
    : recorder.file
      ? `saved: ${recorder.file}`
      : 'not recording';

  // Autotest
  const autotest = message.autotest || {};
  setActive('btnAutotest', autotest.running);
  $('atProgress').classList.toggle('hidden', !autotest.running);
  if (autotest.running) {
    const percent = (autotest.step / autotest.total) * 100;
    $('atBar').style.width = percent + '%';
    $('atLabel').textContent = `step ${autotest.step}/${autotest.total} — ${(autotest.throttle / 10).toFixed(0)}%`;
  }

  // Log
  const list = $('log');
  list.innerHTML = '';
  for (const entry of (message.log || []).slice().reverse()) {
    const item = document.createElement('li');
    item.className = entry.level || 'info';
    const time = new Date((entry.at || 0) * 1000).toLocaleTimeString('en-GB');
    item.textContent = `${time}  ${entry.msg}`;
    list.appendChild(item);
  }
  // The raw byte count is what tells the two failure modes apart, so it is
  // shown even when nothing decodes: zero bytes means the telemetry wire is
  // silent (in practice: telemetry_on_interval is off in the ESC), while bytes
  // without packets means data arrives but never forms a valid frame.
  if (escOk) {
    $('rawInfo').textContent =
      `ESC packets: ${esc.pkts}, CRC errors: ${esc.crcErr}, bytes: ${esc.bytes ?? '—'}`;
  } else if (esc.bytes > 2 || esc.edges > 10) {
    // Something is on the line, but no frame comes together. Single bytes do
    // not count here: the pull-up probe drives the pin itself and so produces
    // a couple of them.
    $('rawInfo').textContent =
      `there is a signal on the line (bytes: ${esc.bytes}, edges: ${esc.edges}), but no ` +
      `frame comes together — wrong rate or wrong format. CRC errors: ${esc.crcErr}, ` +
      `frame errors: ${esc.frameErr}`;
  } else {
    // The verdict of the probe is always shown when there is no telemetry: it
    // is what answers the question of what is on the other end of the wire.
    $('rawInfo').textContent = `no telemetry · ${padVerdict(esc)}`;
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
  if (confirm('Give the USB port to another program? The motor will be stopped.')) {
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

// The numbers in the tiles jump not from noise in the wiring but from the
// resolution of the ADC: one of its steps on current is about 75 mA, so the
// last digit twitches on its own and cannot be read by eye. Averaging over a
// second makes the number readable and hides nothing: the charts and the file
// take the raw values, so a transient and a spike both stay visible there.
const SMOOTH_WINDOW_MS = 1000;

class Smoothed {
  constructor(windowMs = SMOOTH_WINDOW_MS) {
    this.windowMs = windowMs;
    this.series = new Map();
  }

  // Takes a value and returns the average over the window. null means there is
  // nothing to show: the source is gone, and the average accumulated so far
  // would be a lie.
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

// At startup the board reads the telemetry pin under an internal pull-up, then
// under a pull-down. A weak pull (about 40 kΩ) loses to anything that really
// drives the line, so the pair of levels tells what is on the other end.
function padVerdict(esc) {
  if (esc.padUp === undefined) return 'the board does not report the pin probe';
  if (esc.padUp === 1 && esc.padDown === 0) {
    return 'the TEL pin is connected to nothing: it merely follows the ' +
      'internal pull. The telemetry wire does not reach the flight controller';
  }
  if (esc.padUp === 0 && esc.padDown === 0) {
    return 'the TEL pin is held at zero by something external — usually a ' +
      'faulty ESC or a short in the harness';
  }
  if (esc.padUp === 1 && esc.padDown === 1) {
    return 'the TEL pin is held high, which means the transmitter is ' +
      'connected and silent — telemetry is off in the ESC';
  }
  return `pin probe: up ${esc.padUp}, down ${esc.padDown}`;
}

// Zero amps and a missing sensor look the same on screen, and the difference
// between them decides what to repair. They can only be told apart from the
// raw signal on the ADC pin together with whether the motor is spinning.
function currentSourceNote(telemetry) {
  const esc = telemetry.esc || {};
  const sense = telemetry.board || {};
  const spinning = telemetry.state === 1 && (telemetry.thr || [0])[0] > 0;

  if (sense.iPinMv === undefined) return 'the board does not report the current sensor signal';
  if (sense.iPinMv > 0) return 'the board current sensor puts out a signal, calibration is possible';
  if (!spinning) return 'the motor is stopped, so 0 mV is normal. Give it throttle and look here again';
  return 'the motor is spinning and the current input reads 0 mV — there is no signal, calibration will not help here';
}

function updateCalibrationDialog(telemetry, settings, calibration, log) {
  if (!$('calDialog').open) return;

  const sense = telemetry.board || {};
  const load = telemetry.load || {};
  // Averaging matters most here: these are the numbers the operator uses to
  // decide whether the reading matches the external meter, and a twitching
  // last digit makes that impossible. The calibration itself is computed on
  // the server from the board's data and does not depend on what is drawn
  // here.
  const nowV = smooth.put('calV', sense.mv === undefined ? null : sense.mv / 1000);
  const nowA = smooth.put('calA', sense.ma === undefined ? null : sense.ma / 1000);
  const nowG = smooth.put('calG', typeof telemetry.thrustG === 'number' ? telemetry.thrustG : null);

  $('calNowVoltage').textContent = nowV === null ? '—' : nowV.toFixed(2);
  $('calNowCurrent').textContent = nowA === null ? '—' : nowA.toFixed(2);
  $('calNowThrust').textContent = nowG === null ? '—' : Math.round(nowG);

  // The raw millivolts on the ADC pins. Without them "there is no current"
  // cannot be told from "the sensor puts out nothing": the converted current
  // is zero in both cases. The fractional digit here is not redundant — the
  // ADC step is 0.8 mV, and whole millivolts would look just as twitchy after
  // averaging as before it.
  const pinV = smooth.put('calPinV', sense.vPinMv ?? null);
  const pinA = smooth.put('calPinA', sense.iPinMv ?? null);

  $('calPinVoltage').textContent = pinV === null ? '—' : pinV.toFixed(1);
  $('calPinCurrent').textContent = pinA === null ? '—' : pinA.toFixed(1);
  // Scales in milli-units tell the eye nothing. In volts and amps it is
  // immediately visible if a calibration has taken the coefficient far from
  // the datasheet one — ArduPilot has 8.3 V/V and 92.6 A/V for this board.
  $('calScaleVoltage').textContent =
    settings.vbatScaleM === undefined ? '—' : (settings.vbatScaleM / 1000).toFixed(2) + ' V/V';
  $('calScaleCurrent').textContent =
    settings.currentScaleM === undefined ? '—' : (settings.currentScaleM / 1000).toFixed(1) + ' A/V';
  $('calZeroCurrent').textContent = settings.currentOffsetMv ?? '—';
  $('calCurrentSource').textContent = currentSourceNote(telemetry);

  // The board itself reports whether what is in its memory matches what it is
  // currently computing with. Until they match, a calibration lives only until
  // the first power-down.
  const saved = settings.saved;
  const savedLine = $('calSaved');
  savedLine.textContent =
    saved === undefined
      ? 'the board does not report whether the settings are saved'
      : saved
        ? 'the settings are written to the board memory'
        : 'the settings are not written to the board memory — they will be lost on power-down';
  savedLine.classList.toggle('ok', saved === true);
  savedLine.classList.toggle('warn', saved === false);
  $('calRawThrust').textContent = load.raw ?? '—';
  $('calScaleThrust').textContent = calibration.loadScale
    ? calibration.loadScale.toFixed(5)
    : 'not set';

  const answers = log.filter(
    (entry) => (entry.at || 0) * 1000 > calOpenedAt && /calibrat|current zero|thrust zero/i.test(entry.msg || '')
  );
  const last = answers[answers.length - 1];
  $('calStatus').textContent = last ? last.msg : 'changes apply as soon as the button is pressed';
}

$('btnCalibrate').addEventListener('click', () => {
  calOpenedAt = Date.now();
  $('calStatus').textContent = 'changes apply as soon as the button is pressed';
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
        $('calStatus').textContent = 'enter a measured value greater than zero';
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
    setBanner('Press ARM first.');
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
      item.appendChild(document.createTextNode(`  ${(file.bytes / 1024).toFixed(0)} kB`));
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
