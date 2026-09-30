// Phone-side code (runs in the Pebble app): the settings page (Clay) and the
// weather fetch. Weather comes from Open-Meteo using the phone's location and is
// sent to the watch as the "Temp" app message; the watch asks for a refresh with
// "RequestWeather". Clay delivers the settings itself.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var customClay = require('./custom-clay');
var clay = new Clay(clayConfig, customClay);

function settings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(function (pos) {
    var unit = settings().TempUnit === 'F' ? 'fahrenheit' : 'celsius';
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + pos.coords.latitude.toFixed(3) +
      '&longitude=' + pos.coords.longitude.toFixed(3) +
      '&current=temperature_2m' +
      '&temperature_unit=' + unit;
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
      try {
        var cur = JSON.parse(this.responseText).current;
        Pebble.sendAppMessage({
          Temp: Math.round(cur.temperature_2m)
        });
      } catch (e) {
        console.log('Weather parse failed: ' + e);
      }
    };
    xhr.open('GET', url);
    xhr.send();
  }, function (err) {
    console.log('Location failed: ' + err.message);
  }, { timeout: 15000, maximumAge: 30 * 60 * 1000 });
}

Pebble.addEventListener('ready', fetchWeather);

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestWeather) fetchWeather();
});

// Clay sends the settings itself; refetch afterwards in case the unit changed.
Pebble.addEventListener('webviewclosed', function (e) {
  if (e && e.response) setTimeout(fetchWeather, 1000);
});
