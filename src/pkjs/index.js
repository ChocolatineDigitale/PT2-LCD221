// Phone-side code (runs in the Pebble app): the settings page (Clay) and the
// weather fetch. Weather comes from Open-Meteo using the phone's location and is
// sent to the watch as the "Temp" app message, in tenths of a degree Celsius (the watch
// picks Celsius or Fahrenheit); the watch asks for a refresh with
// "RequestWeather". Clay delivers the settings itself.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var customClay = require('./custom-clay');
var clay = new Clay(clayConfig, customClay);

var lastFetch = 0;                 // last attempt in this run of the phone-side code
var MIN_FETCH_MS = 5 * 60 * 1000;  // never fetch more often than this (a fix and a web request cost battery)
var MIN_LAUNCH_MS = 25 * 60 * 1000; // opening the face again soon after a fetch needs no new one
var STORE_KEY = 'lcd221-last-weather';

function lastWeatherTime() {
  try { return parseInt(localStorage.getItem(STORE_KEY), 10) || 0; } catch (e) { return 0; }
}

function rememberWeatherTime() {
  try { localStorage.setItem(STORE_KEY, String(Date.now())); } catch (e) { /* storage unavailable */ }
}

// minAgeMs: skip the fetch if the last successful one is more recent than this.
function fetchWeather(minAgeMs) {
  if (Date.now() - lastFetch < MIN_FETCH_MS) return;
  if (Date.now() - lastWeatherTime() < minAgeMs) return;
  lastFetch = Date.now();
  navigator.geolocation.getCurrentPosition(function (pos) {
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + pos.coords.latitude.toFixed(3) +
      '&longitude=' + pos.coords.longitude.toFixed(3) +
      '&current=temperature_2m';
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
      try {
        var cur = JSON.parse(this.responseText).current;
        if (!cur || typeof cur.temperature_2m !== 'number') throw new Error('no temperature in reply');
        Pebble.sendAppMessage({
          Temp: Math.round(cur.temperature_2m * 10)  // tenths of a degree Celsius; the watch converts
        });
        rememberWeatherTime();
      } catch (e) {
        lastFetch = 0;
        console.log('Weather parse failed: ' + e);
      }
    };
    xhr.onerror = function () { lastFetch = 0; };
    xhr.open('GET', url);
    xhr.send();
  }, function (err) {
    lastFetch = 0;  // a failed fix may be retried on the next request
    console.log('Location failed: ' + err.message);
  }, { timeout: 15000, maximumAge: 30 * 60 * 1000 });
}

// The watch face is opened often (after the menu, a notification...): only fetch if the last
// reading is old. The watch keeps its own copy and asks again when it needs fresh data.
Pebble.addEventListener('ready', function () { fetchWeather(MIN_LAUNCH_MS); });

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestWeather) fetchWeather(0);
});
