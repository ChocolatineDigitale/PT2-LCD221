// Runs inside the Clay settings page (injected via .toString(), so it must be
// self-contained). It hides settings that don't apply to the current choices and
// wires up the "Reset to defaults" button.
module.exports = function () {
  var clayConfig = this;

  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function () {
    function item(key) { return clayConfig.getItemByMessageKey(key); }

    // A corner's text only matters while that corner shows "Custom text".
    var corners = [['TopLeft', 'TopLeftText'], ['TopRight', 'TopRightText'],
                   ['BottomLeft', 'BottomLeftText'], ['BottomRight', 'BottomRightText']];
    function syncCorners() {
      corners.forEach(function (pair) {
        var choice = item(pair[0]), text = item(pair[1]);
        if (choice.get() === 'text') text.show(); else text.hide();
      });
    }
    corners.forEach(function (pair) { item(pair[0]).on('change', syncCorners); });
    syncCorners();

    // The hour's leading zero only matters in the 24-hour format ("Follow watch" may be either).
    var timeFormat = item('TimeFormat'), hourZero = item('TimeZero');
    function syncHourZero() { if (timeFormat.get() === '12') hourZero.hide(); else hourZero.show(); }
    timeFormat.on('change', syncHourZero);
    syncHourZero();

    // The seconds mode only matters while the right box shows the seconds, and the duration
    // only for "After a wrist shake".
    var rightBox = item('RightBox'), secondsMode = item('SecondsMode');
    var duration = item('SecondsDuration');
    function syncRightBox() {
      var seconds = rightBox.get() === 'seconds';
      if (seconds) secondsMode.show(); else secondsMode.hide();
      if (seconds && secondsMode.get() === 'shake') duration.show(); else duration.hide();
    }
    rightBox.on('change', syncRightBox);
    secondsMode.on('change', syncRightBox);
    syncRightBox();

    // The colour picker only matters while "Custom color..." is the backlight choice.
    var backlight = item('BacklightColor'), custom = item('BacklightCustom');
    function syncCustom() { if (backlight.get() === 'custom') custom.show(); else custom.hide(); }
    backlight.on('change', syncCustom);
    syncCustom();

    // "Respect Quiet Time" and the sample button only matter while the hourly chime is on.
    var chime = item('HourlyChime'), chimeQuiet = item('ChimeQuiet'), volume = item('ChimeVolume');
    var playButton = clayConfig.getItemById('playChime'), test = item('ChimeTest');
    function syncChime() {
      var choice = chime.get();  // "0" off, "4" vibration only: no volume for those
      if (choice === '0') { chimeQuiet.hide(); playButton.hide(); } else { chimeQuiet.show(); playButton.show(); }
      if (choice === '0' || choice === '4') volume.hide(); else volume.show();
    }
    chime.on('change', syncChime);
    syncChime();

    // The sample button can't play anything itself (this page is not talking to the watch),
    // so it asks the watch to play the chime when Save is tapped. The request is a hidden
    // switch that is cleared every time the page opens, so it never sticks.
    test.hide();
    test.set(false);
    function syncTest() { playButton.set(test.get() ? 'Chime plays when you tap Save' : 'Play chime'); }
    test.on('change', syncTest);
    playButton.on('click', function () { test.set(!test.get()); });
    syncTest();


    // ---- Preview ------------------------------------------------------------------------
    // An approximate picture of the watch face that follows the settings on this page. It is
    // drawn here (not on the watch) with sample data: the phone's time and date, a battery at
    // 100%, 8,432 steps, 4.0 mi / 6.4 km, a heart rate of 72 and 17 degrees C.
    (function () {
      var form = document.getElementById('main-form');
      if (!form) return;
      var box = document.createElement('div');
      box.className = 'component';
      box.style.cssText = 'position:sticky;top:0;z-index:20;background:#333;padding:8px 0 2px;text-align:center';
      var canvas = document.createElement('canvas');
      canvas.width = 400; canvas.height = 456;
      canvas.style.cssText = 'width:130px;height:148px;border-radius:3px;display:block;margin:0 auto';
      var note = document.createElement('div');
      note.style.cssText = 'font-size:.7rem;color:#a4a4a4;padding-top:2px';
      note.textContent = 'Preview (approximate). Tap to hide.';
      box.appendChild(canvas); box.appendChild(note);
      form.insertBefore(box, form.children[1] || null);
      var shown = true;
      box.addEventListener('click', function () {
        shown = !shown;
        canvas.style.display = shown ? 'block' : 'none';
        note.textContent = shown ? 'Preview (approximate). Tap to hide.' : 'Preview hidden. Tap to show.';
        if (shown) draw();
      });

      var POLYS = [
        [720,125,276,125,88,29,135,13,187,3,243,0,758,0,806,3],
        [758,145,852,10,933,38,983,76,1000,125,999,451,917,483,756,421],
        [758,581,916,518,999,551,1000,875,984,925,935,964,853,991,756,857],
        [276,875,720,875,806,999,758,1000,243,1000,187,997,136,988,89,973],
        [241,858,54,954,24,931,6,904,1,875,0,551,82,518,243,581],
        [241,421,83,483,0,451,1,125,7,97,25,71,55,47,243,143],
        [117,500,276,438,723,438,881,500,723,562,276,562]
      ];
      var DIGITS = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F];
      var GLYPHS = {
        A: '.###.#...#######...##...#', D: '####.#...##...##...#####.', E: '######....####.#....#####',
        F: '######....####.#....#....', H: '#...##...#######...##...#', I: '.###...#....#....#...###.',
        M: '#...###.###.#.##...##...#', N: '#...###..##.#.##..###...#', O: '.###.#...##...##...#.###.',
        R: '####.#...#####.#..#.#...#', S: '.#####.....###.....#####.', T: '#####..#....#....#....#..',
        U: '#...##...##...##...#.###.', W: '#...##...##.#.###.###...#'
      };
      var DAYS = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];
      var PM_BITS = '#########..' + '###########' + '###.....###' + '###.....###' + '###.....###' +
        '###.....###' + '###########' + '#########..' + '###........' + '###........' + '###........' +
        '###........' + '###........' + '###........';

      function get(key, fallback) {
        var it = clayConfig.getItemByMessageKey(key);
        var v = it ? it.get() : fallback;
        return (v === undefined || v === null) ? fallback : v;
      }
      // What the watch itself follows, if it has told the phone; otherwise a guess from the phone.
      var prefs = (clayConfig.meta && clayConfig.meta.userData) || {};
      function imperialRegion() {
        if (prefs.imperial !== undefined) return !!prefs.imperial;
        return /-(US|GB|LR|MM)$/i.test(navigator.language || '');
      }
      function usesDst(d) {
        var jan = new Date(d.getFullYear(), 0, 1).getTimezoneOffset();
        var jul = new Date(d.getFullYear(), 6, 1).getTimezoneOffset();
        return jan !== jul && d.getTimezoneOffset() === Math.min(jan, jul);
      }

      function draw() {
        if (!shown) return;
        var ctx = canvas.getContext('2d');
        var d = new Date();
        var inv = !!get('Inverted', false), slant = get('Slanted', true) ? 0.075 : 0, ghosts = !!get('Ghosts', true);
        var silver = get('CaseColor', 'black') === 'silver';
        var frame = silver ? '#aaaaaa' : '#000000', frameText = silver ? '#000000' : '#ffffff';
        var lcd = inv ? '#000000' : '#ffffff', ink = inv ? '#ffffff' : '#000000', ghost = inv ? '#1a1a1a' : '#e0e0e0';  // the watch dithers its unlit segments, so they read as a faint tint

        ctx.setTransform(2, 0, 0, 2, 0, 0);
        ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
        ctx.fillStyle = frame; ctx.fillRect(0, 0, 200, 228);
        ctx.fillStyle = '#000'; ctx.fillRect(0, 22, 200, 180);
        ctx.fillStyle = lcd; ctx.fillRect(0, 24, 200, 176);
        if (inv && !silver) { ctx.fillStyle = '#555'; ctx.fillRect(0, 23, 200, 1); ctx.fillRect(0, 200, 200, 1); }
        // button markers (optional): red bars along the LCD edges, one on the left and three on
        // the right, 5 px wide with the inside corners rounded, as on the watch
        if (get('ButtonMarkers', false)) {
          ctx.fillStyle = '#e35462';  // the display's red
          var marker = function (x, cy, innerRight) {
            for (var r = 0; r < 32; r++) {
              var cut = (r === 0 || r === 31) ? 2 : ((r === 1 || r === 30) ? 1 : 0);
              var w = 5 - cut;
              ctx.fillRect(innerRight ? x : x + 5 - w, cy - 16 + r, w, 1);
            }
          };
          marker(0, 44, true); marker(195, 44, false); marker(195, 117, false); marker(195, 190, false);
        }

        function rect(x, y, w, h, c) { ctx.fillStyle = c; ctx.fillRect(x, y, w, h); }
        // A 7-segment digit in the box (x, y, w, h); `on` lit segments, `present` the ones drawn.
        function seg(x, y, w, h, on, present) {
          for (var pass = ghosts ? 0 : 1; pass < 2; pass++) {
            for (var i = 0; i < 7; i++) {
              if (!(present & (1 << i))) continue;
              var lit = !!(on & (1 << i));
              if (lit !== (pass === 1)) continue;
              var p = POLYS[i];
              ctx.beginPath();
              for (var k = 0; k < p.length; k += 2) {
                var px = x + p[k] / 1000 * w + (1 - p[k + 1] / 1000) * h * slant, py = y + p[k + 1] / 1000 * h;
                if (k === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
              }
              ctx.closePath();
              ctx.fillStyle = lit ? ink : ghost; ctx.fill();
            }
          }
        }
        function digit(x, y, w, h, v) { seg(x, y, w, h, v >= 0 && v <= 9 ? DIGITS[v] : (v === 10 ? 0x40 : 0), 0x7F); }
        function dots(x, y, bits, cols, rows, pitchX, pitchY, dw, dh, lit) {
          for (var r = 0; r < rows; r++) for (var c = 0; c < cols; c++) {
            if (bits.charAt(r * cols + c) !== '#') continue;
            if (!lit && !ghosts) continue;
            rect(x + c * pitchX, y + r * pitchY, dw, dh, lit ? ink : ghost);
          }
        }

        // Weekday
        var day = DAYS[d.getDay()];
        for (var i = 0; i < 3; i++) {
          var g = GLYPHS[day.charAt(i)];
          for (var r = 0; r < 5; r++) for (var c = 0; c < 5; c++) {
            var lit = g.charAt(r * 5 + c) === '#';
            if (!lit && !ghosts) continue;
            rect(12 + i * 30 + c * 5, 34 + r * 6, 4, 5, lit ? ink : ghost);
          }
        }

        // Indicator box and its labels
        ctx.strokeStyle = ink; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(110, 35); ctx.lineTo(185, 35); ctx.arcTo(190, 35, 190, 40, 5);
        ctx.lineTo(190, 61); ctx.lineTo(116, 61); ctx.arcTo(110, 61, 110, 55, 5); ctx.closePath(); ctx.stroke();
        rect(111, 48, 79, 1, ink); rect(150, 36, 1, 25, ink);
        var chime = String(get('HourlyChime', '0'));
        var charging = false;
        // A label as on the watch: capitals 10 rows tall, centred in its 12-row cell (rows y+1 .. y+10),
        // squeezed sideways to fit the cell.
        function label(text, on, x, y, w) {
          if (!on && !ghosts) return;
          ctx.save();
          ctx.font = 'bold 14px "DejaVu Sans Condensed", Arial, sans-serif';
          var tw = ctx.measureText(text).width;
          var asc = ctx.measureText('H').actualBoundingBoxAscent || 10.2;  // the capitals' height
          ctx.translate(x + w / 2, y + 11); ctx.scale(Math.min(1.25, (w - 2) / tw), 10 / asc);
          ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
          ctx.fillStyle = on ? ink : ghost; ctx.fillText(text, 0, 0);
          ctx.restore();
        }
        label('BT', true, 113, 36, 37);
        label(charging ? 'CHG' : 'CHG', charging, 151, 36, 37);
        label('SIG', chime !== '0' && chime !== '4', 113, 49, 37);
        label('MUTE', false, 151, 49, 39);

        // Time
        var twelve = get('TimeFormat', 'auto');
        if (twelve === 'auto' && prefs.clock24 !== undefined) {
          twelve = prefs.clock24 ? '24' : '12';
        } else if (twelve === 'auto') {
          try { twelve = new Intl.DateTimeFormat([], { hour: 'numeric' }).resolvedOptions().hourCycle === 'h12' ? '12' : '24'; }
          catch (e) { twelve = '24'; }
        }
        var hour = d.getHours(), pm = hour >= 12;
        if (twelve === '12') { hour = hour % 12; if (hour === 0) hour = 12; }
        var tens = (hour >= 10 || (twelve === '24' && get('TimeZero', true))) ? Math.floor(hour / 10) : -1;
        if (twelve === '12') {
          seg(6, 76, 35, 70, tens === 1 ? 0x06 : 0, 0x06);
          dots(7, 74, PM_BITS, 11, 14, 1, 1, 1, 1, pm);
        } else {
          digit(6, 76, 35, 70, tens);
        }
        digit(49, 76, 35, 70, hour % 10);
        rect(93 + (70 - 25) * slant, 76 + 25 - 3, 7, 7, ink);
        rect(93 + (70 - 49) * slant, 76 + 49 - 3, 7, 7, ink);
        digit(108, 76, 35, 70, Math.floor(d.getMinutes() / 10));
        digit(153, 76, 35, 70, d.getMinutes() % 10);
        rect(0, 154, 200, 2, ink); rect(110, 154, 2, 46, ink);

        // Date
        var first = d.getDate(), second = d.getMonth() + 1;
        if (get('DateFormat', 'DM') === 'MD') { first = d.getMonth() + 1; second = d.getDate(); }
        var pad = get('DatePadding', 'zero');
        digit(19, 170, 15, 26, (first >= 10 || pad === 'zero') ? Math.floor(first / 10) : -1);
        digit(38, 170, 15, 26, first % 10);
        seg(56, 170, 8, 26, 0x40, 0x40);
        digit(67, 170, 15, 26, (second >= 10 || pad !== 'both') ? Math.floor(second / 10) : -1);
        digit(86, 170, 15, 26, second % 10);
        // DST label
        ctx.save(); ctx.font = 'bold 11px "DejaVu Sans Condensed", Arial, sans-serif';
        var dst = usesDst(d); ctx.textBaseline = 'alphabetic'; ctx.textAlign = 'left';
        if (dst || ghosts) { ctx.fillStyle = dst ? ink : ghost; ctx.fillText('DST', 5, 166); }
        ctx.restore();

        // Right box: the seconds, or (also while idle in the shake mode) the temperature
        var seconds = get('RightBox', 'temperature') === 'seconds' && get('SecondsMode', 'always') === 'always';
        if (seconds) {
          digit(131, 160, 22, 36, Math.floor(d.getSeconds() / 10));
          digit(156, 160, 22, 36, d.getSeconds() % 10);
        } else {
          var unit = get('TempUnit', 'auto');
          var f = unit === 'F' || (unit === 'auto' && imperialRegion());
          var t = f ? 63 : 17;
          seg(114, 160, 16, 36, 0, 0x40);
          seg(105, 160, 22, 36, 0, 0x06);
          digit(132, 160, 22, 36, Math.floor(t / 10));
          digit(157, 160, 22, 36, t % 10);
          var ring = '..###..' + '.#####.' + '##...##' + '##...##' + '##...##' + '.#####.' + '..###..';
          dots(183 + 32 * slant, 160, ring, 7, 7, 1, 1, 1, 1, true);
        }

        // Bezel corners
        var imperial = get('DistanceUnit', 'auto');
        var miles = imperial === 'mi' || (imperial === 'auto' && imperialRegion());
        function corner(slot, kind, text) {
          switch (kind) {
            case 'battery': return 'BATT 100%';
            case 'steps': return '8,432 STEPS';
            case 'distance': return miles ? '4.0 MI' : '6.4 KM';
            case 'hr': return 'HR 72';
            case 'text': return String(text || '').toUpperCase();
            default: return '';
          }
        }
        var top = [get('TopLeft', 'battery'), get('TopRight', 'steps')];
        var bot = [get('BottomLeft', 'wr'), get('BottomRight', 'text')];
        var topText = [get('TopLeftText', ''), get('TopRightText', '')];
        var botText = [get('BottomLeftText', ''), get('BottomRightText', '')];
        function fit(text, maxw) {
          if (ctx.measureText(text).width <= maxw) return text;
          while (text.length > 1 && ctx.measureText(text + '...').width > maxw) text = text.slice(0, -1);
          return text + '...';
        }
        // A bottom corner can be a badge: "WR", or "HR" followed by the heart rate.
        function info(kind, text, bottom) {
          if (kind === 'wr') return { badge: 'WR', text: '' };
          if (kind === 'hr' && bottom) return { badge: 'HR', text: '72' };
          return { badge: null, text: corner(0, kind, text) };
        }
        function row(kinds, texts, font, baseline, bottom) {
          ctx.font = font; ctx.fillStyle = frameText; ctx.textBaseline = 'alphabetic';
          var left = info(kinds[0], texts[0], bottom), right = info(kinds[1], texts[1], bottom);
          var textw = ctx.measureText(right.text).width + 4;
          var rw = right.badge ? 30 + (right.text ? 4 + textw : 0) : textw;
          rw = Math.min(rw, bottom ? 108 : 104);
          if (right.badge) {
            badge(190 - rw, baseline, right.badge);
            if (right.text) { ctx.textAlign = 'left'; ctx.fillStyle = frameText; ctx.fillText(right.text, 190 - rw + 34, baseline); }
          } else { ctx.textAlign = 'right'; ctx.fillText(fit(right.text, rw), 190, baseline); }
          if (left.badge) {
            badge(10, baseline, left.badge);
            if (left.text) { ctx.textAlign = 'left'; ctx.fillStyle = frameText; ctx.fillText(fit(left.text, 190 - rw - 14 - 34), 44, baseline); }
          } else { ctx.textAlign = 'left'; ctx.fillText(fit(left.text, 190 - rw - 14), 10, baseline); }
        }
        function badge(x, baseline, letters) {
          ctx.strokeStyle = frameText; ctx.lineWidth = 1;
          // a rounded outline like the watch's (30x20, radius 3), drawn with arcs
          var l = x + 0.5, t = 205.5, r = x + 29.5, b = 224.5, rad = 3;
          ctx.beginPath(); ctx.moveTo(l + rad, t); ctx.lineTo(r - rad, t); ctx.arcTo(r, t, r, t + rad, rad);
          ctx.lineTo(r, b - rad); ctx.arcTo(r, b, r - rad, b, rad); ctx.lineTo(l + rad, b);
          ctx.arcTo(l, b, l, b - rad, rad); ctx.lineTo(l, t + rad); ctx.arcTo(l, t, l + rad, t, rad);
          ctx.closePath(); ctx.stroke();
          // the watch's letters are narrower than the preview's font: squeeze them to fit the box
          ctx.save(); ctx.translate(x + 15, baseline); ctx.scale(0.74, 1);
          ctx.textAlign = 'center'; ctx.fillStyle = frameText; ctx.fillText(letters, 0, 0);
          ctx.restore();
        }
        row(top, topText, 'bold 14px "DejaVu Sans Condensed", Arial, sans-serif', 16, false);
        row(bot, botText, 'bold 19px "DejaVu Sans Condensed", Arial, sans-serif', 222, true);
      }

      draw();
      clayConfig.getAllItems().forEach(function (it) { if (it.messageKey) it.on('change', draw); });
      setInterval(draw, 1000);
    })();

    // Reset: every setting goes back to the defaultValue declared in config.js;
    // the user then taps Save. The change events above keep hidden fields in sync.
    // It takes two taps within a few seconds, so a stray tap does nothing. (A confirm()
    // dialog is not used because the Pebble app's page view may not show one.)
    var button = clayConfig.getItemById('resetDefaults');
    var armed = null;
    function disarm() {
      if (armed) { clearTimeout(armed); armed = null; }
      button.set('Reset to defaults');
    }
    button.on('click', function () {
      if (!armed) {
        button.set('Tap again to confirm');
        armed = setTimeout(disarm, 4000);
        return;
      }
      disarm();
      clayConfig.getAllItems().forEach(function (it) {
        if (it.messageKey && it.config.defaultValue !== undefined) it.set(it.config.defaultValue);
      });
    });
  });
};
