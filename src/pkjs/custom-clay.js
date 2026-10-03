// Runs inside the Clay settings page (injected via .toString(), so it must be
// self-contained). It hides settings that don't apply to the current choices and
// wires up the "Reset to defaults" button.
module.exports = function () {
  var clayConfig = this;

  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function () {
    function item(key) { return clayConfig.getItemByMessageKey(key); }

    // A custom text only matters while its switch is off.
    [['ShowBattery', 'TopLeftText'], ['ShowSteps', 'TopRightText']].forEach(function (pair) {
      var toggle = item(pair[0]), text = item(pair[1]);
      function sync() { if (toggle.get()) text.hide(); else text.show(); }
      toggle.on('change', sync);
      sync();
    });

    // The hour's leading zero only matters in the 24-hour format ("Follow watch" may be either).
    var timeFormat = item('TimeFormat'), hourZero = item('TimeZero');
    function syncHourZero() { if (timeFormat.get() === '12') hourZero.hide(); else hourZero.show(); }
    timeFormat.on('change', syncHourZero);
    syncHourZero();

    // The seconds mode only matters while the right box shows the seconds; the temperature
    // unit while the temperature is shown, which includes the idle time of "after a shake".
    var rightBox = item('RightBox'), secondsMode = item('SecondsMode'), unit = item('TempUnit');
    var duration = item('SecondsDuration');
    function syncRightBox() {
      var seconds = rightBox.get() === 'seconds';
      if (seconds) secondsMode.show(); else secondsMode.hide();
      if (seconds && secondsMode.get() === 'shake') duration.show(); else duration.hide();
      if (!seconds || secondsMode.get() === 'shake') unit.show(); else unit.hide();
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
