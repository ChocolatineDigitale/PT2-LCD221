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

    // The temperature unit only matters while the right box shows the temperature.
    var rightBox = item('RightBox'), unit = item('TempUnit');
    function syncUnit() { if (rightBox.get() === 'temperature') unit.show(); else unit.hide(); }
    rightBox.on('change', syncUnit);
    syncUnit();

    // The colour picker only matters while "Custom color..." is the backlight choice.
    var backlight = item('BacklightColor'), custom = item('BacklightCustom');
    function syncCustom() { if (backlight.get() === 'custom') custom.show(); else custom.hide(); }
    backlight.on('change', syncCustom);
    syncCustom();

    // Reset: every setting goes back to the defaultValue declared in config.js;
    // the user then taps Save. The change events above keep hidden fields in sync.
    var button = clayConfig.getItemById('resetDefaults');
    button.on('click', function () {
      clayConfig.getAllItems().forEach(function (it) {
        if (it.messageKey && it.config.defaultValue !== undefined) it.set(it.config.defaultValue);
      });
    });
  });
};
