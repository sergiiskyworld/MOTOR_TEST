// Checks for the run maxima shown under the charts, taken from the shipped
// app.js rather than from a copy.
//
//   osascript -l JavaScript Prog/server/tests/peaks_test.js
//
// osascript is used because this machine has no node, and the class is plain
// ES2020 with no DOM access, so it runs anywhere with a JS engine.
//
// What needs guarding here is the distinction between the two numbers. A peak
// and a one-second average look interchangeable until a spike arrives: the
// spike is the whole reason both are shown, and a change that quietly fed the
// same value to both would leave the display looking perfectly normal.

ObjC.import('Foundation');

function readText(path) {
  const data = $.NSString.stringWithContentsOfFileEncodingError(
    path, $.NSUTF8StringEncoding, null);
  if (!data) {
    throw new Error('could not read file ' + path);
  }
  return ObjC.unwrap(data);
}

// Run from the project root: the current directory is all there is to go on,
// because osascript does not tell the script its own path.
function loadPeaksClass() {
  const root = ObjC.unwrap($.NSFileManager.defaultManager.currentDirectoryPath);
  const source = readText(root + '/Prog/server/standserver/static/app.js');

  const begin = source.indexOf('class Peaks {');
  const end = source.indexOf('const peaks = new Peaks();');
  if (begin < 0 || end < 0 || end < begin) {
    throw new Error('class Peaks not found in app.js — the test has fallen behind the code');
  }
  return new Function(source.slice(begin, end) + '; return Peaks;')();
}

const Peaks = loadPeaksClass();

const lines = [];
let failures = 0;

function expect(name, ok) {
  lines.push((ok ? 'PASS  ' : 'FAIL  ') + name);
  if (!ok) {
    failures++;
  }
}

const asInt = (v) => Math.round(v).toString();
const as2 = (v) => v.toFixed(2);

let p = new Peaks();
expect('a dash is shown while nothing has been measured',
       p.line('rpm', 'Rpm', 'rpm', asInt) === 'Rpm: —');

// A spike lifts the peak and barely moves the average. That is the very
// reason there are two numbers under the chart instead of one.
p.put('rpm', 1000, 1000);
p.put('rpm', 9000, 1200);
p.put('rpm', 1100, 1150);
expect('the peak takes the instantaneous spike', p.peak.get('rpm') === 9000);
expect('the working value is counted from the averages only', p.work.get('rpm') === 1200);
expect('the line comes together with its unit',
       p.line('rpm', 'Rpm', 'rpm', asInt)
       === 'Rpm: peak 9000, working 1200 rpm');

// Zero is a measurement, not the absence of one, and the two must not be
// confused: the motor may honestly be standing still.
const zero = new Peaks();
zero.put('current', 0, 0);
expect('zero does not look like a missing measurement',
       zero.line('current', 'Current', 'A', as2) === 'Current: peak 0.00, working 0.00 A');

// There may be no average: smoothing returns null while the quantity has not
// been measured.
const noAvg = new Peaks();
noAvg.put('power', 50, null);
expect('without an average the working value is shown equal to the peak',
       noAvg.line('power', 'Power', 'W', asInt) === 'Power: peak 50, working 50 W');
noAvg.put('power', 70, undefined);
expect('an unreliable average does not spoil what was accumulated',
       noAvg.work.get('power') === undefined && noAvg.peak.get('power') === 70);
noAvg.put('power', NaN, NaN);
expect('NaN does not wipe out the maxima', noAvg.peak.get('power') === 70);

// A maximum by definition does not decrease until the run is started again.
p.put('rpm', 10, 10);
expect('the maximum does not fall along with the readings',
       p.peak.get('rpm') === 9000 && p.work.get('rpm') === 1200);

// Every ARM starts a new run, and the numbers of the previous one have
// nothing to do with it.
p.reset();
expect('a reset erases both quantities',
       p.line('rpm', 'Rpm', 'rpm', asInt) === 'Rpm: —');
p.put('rpm', 5, 5);
expect('after a reset the count goes from the start', p.peak.get('rpm') === 5);

// The quantities must not interfere with one another.
const many = new Peaks();
many.put('rpm', 100, 100);
many.put('current', 7, 7);
expect('the quantities are counted independently',
       many.peak.get('rpm') === 100 && many.peak.get('current') === 7);

lines.push(failures ? failures + ' checks failed' : 'all checks passed');
lines.join('\n');
