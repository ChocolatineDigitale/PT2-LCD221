// Vibration patterns; the values are the watch's VibeChoice enum (see main.c).
var VIBE_PATTERNS = [
  { "label": "None", "value": "0" },
  { "label": "Short pulse", "value": "1" },
  { "label": "Long pulse", "value": "2" },
  { "label": "Double pulse", "value": "3" },
  { "label": "Triple pulse", "value": "4" },
  { "label": "Heartbeat", "value": "5" },
  { "label": "SOS", "value": "6" }
];

module.exports = [
  { "type": "heading", "defaultValue": "LCD 221" },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Time & date" },
      {
        "type": "select", "messageKey": "TimeFormat", "label": "Time format", "defaultValue": "24",
        "options": [
          { "label": "24-hour", "value": "24" },
          { "label": "12-hour", "value": "12" }
        ]
      },
      {
        "type": "select", "messageKey": "DateFormat", "label": "Date format", "defaultValue": "DM",
        "options": [
          { "label": "DD-MM", "value": "DM" },
          { "label": "MM-DD", "value": "MD" }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Right box" },
      {
        "type": "select", "messageKey": "RightBox", "label": "Right box shows",
        "defaultValue": "temperature",
        "description": "Seconds redraw the watch face every second, which uses more battery.",
        "options": [
          { "label": "Temperature", "value": "temperature" },
          { "label": "Seconds", "value": "seconds" }
        ]
      },
      {
        "type": "select", "messageKey": "TempUnit", "label": "Temperature unit",
        "defaultValue": "C",
        "options": [
          { "label": "Celsius", "value": "C" },
          { "label": "Fahrenheit", "value": "F" }
        ]
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Top bezel" },
      {
        "type": "toggle", "messageKey": "ShowBattery", "label": "Show battery level", "defaultValue": true
      },
      {
        "type": "input", "messageKey": "TopLeftText", "label": "Left text",
        "defaultValue": "30 DAY BATT",
        "description": "Shown instead of the battery level, in capitals. Leave empty for none.",
        "attributes": { "maxlength": 19, "autocapitalize": "characters" }
      },
      {
        "type": "toggle", "messageKey": "ShowSteps", "label": "Show step count", "defaultValue": true
      },
      {
        "type": "input", "messageKey": "TopRightText", "label": "Right text",
        "defaultValue": "WR 3ATM",
        "description": "Shown instead of the step count, in capitals. Leave empty for none.",
        "attributes": { "maxlength": 19, "autocapitalize": "characters" }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Bottom bezel" },
      {
        "type": "toggle", "messageKey": "HeartRate", "label": "Show heart rate",
        "defaultValue": false,
        "description": "Replaces the WR badge with HR and your latest heart rate."
      },
      {
        "type": "input", "messageKey": "BezelLabel", "label": "Label", "defaultValue": "PEBBLE",
        "description": "Text on the right of the bottom bezel, shown in capitals. Leave empty for none.",
        "attributes": { "maxlength": 12, "autocapitalize": "characters" }
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Appearance" },
      {
        "type": "select", "messageKey": "CaseColor", "label": "Case color", "defaultValue": "black",
        "options": [
          { "label": "Black", "value": "black" },
          { "label": "Silver", "value": "silver" }
        ]
      },
      {
        "type": "toggle", "messageKey": "Inverted", "label": "Inverted colors",
        "defaultValue": false,
        "description": "Light digits on a dark LCD, like a negative-display watch. The case keeps its color."
      },
      {
        "type": "toggle", "messageKey": "Slanted", "label": "Slanted digits", "defaultValue": true,
        "description": "Lean the digits like the original watch. Straight digits are sharper."
      },
      {
        "type": "toggle", "messageKey": "Ghosts", "label": "Show unlit segments",
        "defaultValue": true,
        "description": "Faintly show the segments that are off, like a real LCD."
      },
      {
        "type": "select", "messageKey": "BacklightColor", "label": "Backlight color",
        "defaultValue": "system",
        "options": [
          { "label": "System default", "value": "system" },
          { "label": "Amber", "value": "FFA020" },
          { "label": "Warm white", "value": "FFD8A0" },
          { "label": "Red", "value": "FF2000" },
          { "label": "Orange", "value": "FF6000" },
          { "label": "Yellow", "value": "FFE000" },
          { "label": "Green", "value": "30FF40" },
          { "label": "Cyan", "value": "00E0FF" },
          { "label": "Blue", "value": "2060FF" },
          { "label": "Purple", "value": "A040FF" },
          { "label": "Pink", "value": "FF40A0" },
          { "label": "Custom color...", "value": "custom" }
        ]
      },
      {
        "type": "color", "messageKey": "BacklightCustom", "label": "Custom color", "defaultValue": "ffaa00",
        "description": "The backlight LED may look a bit different from the swatch."
      }
    ]
  },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Alerts" },
      {
        "type": "select", "messageKey": "VibeDisconnect", "label": "Vibrate on phone disconnect",
        "defaultValue": "3",
        "options": VIBE_PATTERNS
      },
      {
        "type": "select", "messageKey": "VibeConnect", "label": "Vibrate on phone reconnect",
        "defaultValue": "1",
        "description": "No vibration during Quiet Time.",
        "options": VIBE_PATTERNS
      }
    ]
  },
  {
    "type": "section",
    "items": [
      {
        "type": "button", "id": "resetDefaults", "defaultValue": "Reset to defaults",
        "description": "Restores every setting above. Tap Save to apply."
      }
    ]
  },
  { "type": "submit", "defaultValue": "Save" }
];
