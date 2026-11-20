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
    throw new Error('не прочитался файл ' + path);
  }
  return ObjC.unwrap(data);
}

// Запускать из корня проекта: осталось только текущий каталог, потому что
// osascript не сообщает сценарию собственный путь.
function loadPeaksClass() {
  const root = ObjC.unwrap($.NSFileManager.defaultManager.currentDirectoryPath);
  const source = readText(root + '/Prog/server/standserver/static/app.js');

  const begin = source.indexOf('class Peaks {');
  const end = source.indexOf('const peaks = new Peaks();');
  if (begin < 0 || end < 0 || end < begin) {
    throw new Error('класс Peaks не найден в app.js — тест отстал от кода');
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
expect('пока ничего не измерено, показан прочерк',
       p.line('rpm', 'Обороты', 'об/мин', asInt) === 'Обороты: —');

// Выброс поднимает пик и почти не двигает среднее. Это и есть причина, по
// которой под графиком стоят два числа, а не одно.
p.put('rpm', 1000, 1000);
p.put('rpm', 9000, 1200);
p.put('rpm', 1100, 1150);
expect('пик берёт мгновенный выброс', p.peak.get('rpm') === 9000);
expect('рабочее значение считается только по средним', p.work.get('rpm') === 1200);
expect('строка собирается с размерностью',
       p.line('rpm', 'Обороты', 'об/мин', asInt)
       === 'Обороты: пик 9000, рабочее 1200 об/мин');

// Ноль — это измерение, а не его отсутствие, и путать их нельзя: мотор может
// честно стоять.
const zero = new Peaks();
zero.put('current', 0, 0);
expect('ноль не выглядит как отсутствие измерения',
       zero.line('current', 'Ток', 'А', as2) === 'Ток: пик 0.00, рабочее 0.00 А');

// Среднего может не быть: сглаживание отдаёт null, пока величину не измеряли.
const noAvg = new Peaks();
noAvg.put('power', 50, null);
expect('без среднего рабочее показывается равным пику',
       noAvg.line('power', 'Мощность', 'Вт', asInt) === 'Мощность: пик 50, рабочее 50 Вт');
noAvg.put('power', 70, undefined);
expect('недостоверное среднее не портит накопленное',
       noAvg.work.get('power') === undefined && noAvg.peak.get('power') === 70);
noAvg.put('power', NaN, NaN);
expect('NaN не затирает максимумы', noAvg.peak.get('power') === 70);

// Максимум по определению не убывает, пока прогон не начат заново.
p.put('rpm', 10, 10);
expect('максимум не падает вслед за показаниями',
       p.peak.get('rpm') === 9000 && p.work.get('rpm') === 1200);

// Каждый ARM начинает новый прогон, и цифры прошлого к нему отношения не имеют.
p.reset();
expect('сброс стирает обе величины',
       p.line('rpm', 'Обороты', 'об/мин', asInt) === 'Обороты: —');
p.put('rpm', 5, 5);
expect('после сброса отсчёт идёт заново', p.peak.get('rpm') === 5);

// Величины не должны мешать друг другу.
const many = new Peaks();
many.put('rpm', 100, 100);
many.put('current', 7, 7);
expect('величины считаются независимо',
       many.peak.get('rpm') === 100 && many.peak.get('current') === 7);

lines.push(failures ? failures + ' проверок не прошло' : 'all checks passed');
lines.join('\n');
