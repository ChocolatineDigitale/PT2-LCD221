// Phone-side code (runs in the Pebble app): the settings page (Clay) and the
// weather fetch. Weather comes from Open-Meteo using the phone's location and is
// sent to the watch as the "WeatherData" app message (the air quality as "AirData"), in
// metric units: the watch converts to the units chosen. The watch asks for a refresh with
// "RequestWeather". Clay delivers the settings itself.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var customClay = require('./custom-clay');
// The watch tells us its 12/24-hour style and measurement system (bit 0: 24-hour, bit 1:
// imperial); the settings page uses them to preview "Follow watch". They are kept between runs
// and handed to the page through Clay's userData, which is copied when the page opens.
var PREFS_KEY = 'lcd221-watch-prefs';
var clayOptions = { userData: {} };
function usePrefs(bits) {
  clayOptions.userData.clock24 = !!(bits & 1);
  clayOptions.userData.imperial = !!(bits & 2);
}
try {
  var savedPrefs = localStorage.getItem(PREFS_KEY);
  if (savedPrefs !== null) usePrefs(parseInt(savedPrefs, 10) || 0);
} catch (e) { /* storage unavailable */ }
var clay = new Clay(clayConfig, customClay, clayOptions);

var lastFetch = 0;                 // last attempt in this run of the phone-side code
var MIN_FETCH_MS = 5 * 60 * 1000;  // never fetch more often than this (a fix and a web request cost battery)
var DEFAULT_REFRESH_MIN = 30;       // the watch sends its own interval with each request
var refreshMin = DEFAULT_REFRESH_MIN;
// opening the face again soon after a fetch needs no new one
function launchMs() { return (refreshMin - 5) * 60 * 1000; }
var wantAir = false;  // the watch asks for it with each request, while a corner shows it
var STORE_KEY = 'lcd221-last-weather';
var AIR_KEY = 'lcd221-want-air';
try { wantAir = localStorage.getItem(AIR_KEY) === '1'; } catch (e) { /* storage unavailable */ }
var REFRESH_KEY = 'lcd221-refresh-min';
try {
  refreshMin = parseInt(localStorage.getItem(REFRESH_KEY), 10) || DEFAULT_REFRESH_MIN;
} catch (e) { /* storage unavailable */ }

function lastWeatherTime() {
  try { return parseInt(localStorage.getItem(STORE_KEY), 10) || 0; } catch (e) { return 0; }
}

function rememberWeatherTime() {
  try { localStorage.setItem(STORE_KEY, String(Date.now())); } catch (e) { /* storage unavailable */ }
}

// The readings are sent to the watch as arrays of 16-bit numbers (low byte first), in the order the
// watch expects (WX_* and AQ_* in main.c); a reading the phone did not get is -32768.
var NONE = -32768;
function pack(values) {
  var out = [];
  values.forEach(function (v) {
    var n = typeof v === 'number' && isFinite(v) ? Math.round(v) : NONE;
    if (n < -32767 || n > 32767) n = NONE;
    out.push(n & 255, (n >> 8) & 255);
  });
  return out;
}
function scaled(v, k) { return typeof v === 'number' ? v * k : null; }
// "2026-10-15T07:10" -> minutes since midnight (the place's own time)
function minutes(iso) {
  var m = /T(\d\d):(\d\d)/.exec(iso || '');
  return m ? parseInt(m[1], 10) * 60 + parseInt(m[2], 10) : null;
}
// WMO weather code -> 0 clear, 1 partly cloudy, 2 cloudy, 3 fog, 4 drizzle, 5 rain, 6 snow,
// 7 showers, 8 thunderstorm
function condition(code) {
  if (code === 0) return 0;
  if (code === 1 || code === 2) return 1;
  if (code === 3) return 2;
  if (code === 45 || code === 48) return 3;
  if (code >= 51 && code <= 57) return 4;
  if (code >= 61 && code <= 67) return 5;
  if ((code >= 71 && code <= 77) || code === 85 || code === 86) return 6;
  if (code >= 80 && code <= 82) return 7;
  if (code >= 95 && code <= 99) return 8;
  return null;
}

// The air quality (US and European AQI, and the pollutants), in a message of its own.
function fetchAir(lat, lon) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    try {
      var c = JSON.parse(this.responseText).current;
      if (!c) throw new Error('no air quality in reply');
      Pebble.sendAppMessage({
        AirData: pack([c.us_aqi, c.european_aqi, scaled(c.pm2_5, 10), scaled(c.pm10, 10),
                       scaled(c.ozone, 10), scaled(c.nitrogen_dioxide, 10),
                       scaled(c.sulphur_dioxide, 10), scaled(c.carbon_monoxide, 10),
                       scaled(c.dust, 10)])
      });
    } catch (e) {
      console.log('Air quality parse failed: ' + e);
    }
  };
  xhr.timeout = 20000;
  xhr.open('GET', 'https://air-quality-api.open-meteo.com/v1/air-quality?latitude=' + lat.toFixed(3) +
    '&longitude=' + lon.toFixed(3) +
    '&current=us_aqi,european_aqi,pm2_5,pm10,ozone,nitrogen_dioxide,sulphur_dioxide,carbon_monoxide,dust');
  xhr.send();
}

// minAgeMs: skip the fetch if the last successful one is more recent than this.
// wantAir: also fetch the air quality (a second, separate Open-Meteo request).
function fetchWeather(minAgeMs, wantAir) {
  if (Date.now() - lastFetch < MIN_FETCH_MS) return;
  if (Date.now() - lastWeatherTime() < minAgeMs) return;
  lastFetch = Date.now();
  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude, lon = pos.coords.longitude;
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + lat.toFixed(3) + '&longitude=' + lon.toFixed(3) +
      '&current=temperature_2m,apparent_temperature,relative_humidity_2m,dew_point_2m,precipitation,' +
      'snowfall,snow_depth,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,' +
      'wind_gusts_10m,visibility,uv_index' +
      '&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max' +
      '&timezone=auto&forecast_days=1';
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
      try {
        var reply = JSON.parse(this.responseText);
        var c = reply.current, d = reply.daily || {};
        if (!c || typeof c.temperature_2m !== 'number') throw new Error('no temperature in reply');
        Pebble.sendAppMessage({
          // Temperatures in tenths of a degree Celsius, speeds in tenths of a km/h, rain and snowfall
          // in tenths of a mm / cm, pressure in tenths of a hPa, visibility in tenths of a km, UV in
          // tenths, snow depth in cm; the watch converts to the units chosen.
          WeatherData: pack([
            scaled(c.temperature_2m, 10), scaled(c.apparent_temperature, 10),
            scaled(d.temperature_2m_max && d.temperature_2m_max[0], 10),
            scaled(d.temperature_2m_min && d.temperature_2m_min[0], 10),
            scaled(c.dew_point_2m, 10), c.relative_humidity_2m, scaled(c.precipitation, 10),
            d.precipitation_probability_max && d.precipitation_probability_max[0],
            scaled(c.snowfall, 10), scaled(c.snow_depth, 100), scaled(c.wind_speed_10m, 10),
            scaled(c.wind_gusts_10m, 10), c.wind_direction_10m, scaled(c.pressure_msl, 10),
            c.cloud_cover, scaled(c.visibility, 0.01), scaled(c.uv_index, 10),
            scaled(d.uv_index_max && d.uv_index_max[0], 10),
            minutes(d.sunrise && d.sunrise[0]), minutes(d.sunset && d.sunset[0]),
            condition(c.weather_code)
          ])
        });
        rememberWeatherTime();
        if (wantAir) fetchAir(lat, lon);
      } catch (e) {
        lastFetch = 0;
        console.log('Weather parse failed: ' + e);
      }
    };
    xhr.onerror = function () { lastFetch = 0; };
    xhr.timeout = 20000;  // a hung request must not keep the phone's radio busy
    xhr.ontimeout = function () { lastFetch = 0; };
    xhr.open('GET', url);
    xhr.send();
  }, function (err) {
    lastFetch = 0;  // a failed fix may be retried on the next request
    console.log('Location failed: ' + err.message);
  }, { timeout: 15000, maximumAge: 2 * 60 * 60 * 1000 });
}

// The watch face is opened often (after the menu, a notification...): only fetch if the last
// reading is old. The watch keeps its own copy and asks again when it needs fresh data.
Pebble.addEventListener('ready', function () {
  Pebble.sendAppMessage({ RequestPrefs: 1 });
  fetchWeather(launchMs(), wantAir);
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.RequestWeather) {
    wantAir = !!e.payload.RequestAQI;
    refreshMin = e.payload.RequestWeather * 10;  // sent in tens of minutes
    try {
      localStorage.setItem(REFRESH_KEY, String(refreshMin));
      localStorage.setItem(AIR_KEY, wantAir ? '1' : '0');
    } catch (err) { /* ignore */ }
    fetchWeather(0, wantAir);
  }
  if (e.payload.WatchPrefs !== undefined) {
    usePrefs(e.payload.WatchPrefs);
    try { localStorage.setItem(PREFS_KEY, String(e.payload.WatchPrefs)); } catch (err) { /* ignore */ }
  }
});
