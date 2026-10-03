# Store listing

What to enter in the Rebble Developer Portal (dev-portal.rebble.io) for LCD 221:

| Field | Value |
|---|---|
| Publish under the name | Kicou |
| Type | Watchface |
| Title | LCD 221 |
| Description | `description.txt` (under 1600 characters) |
| Screenshots (Pebble Time 2) | `screenshots/1-default.png`, `2-inverted.png`, `3-seconds.png`, in that order |
| Release | `LCD221-1.2.2.pbw` (made with `python3 tools/strip_pbw.py`) |
| Release notes | `release-notes.txt` |
| Support email | a throwaway address, not the account email |
| Source code URL, website | leave blank if the portal allows it |
| Category | the closest digital / retro one |
| Marketing banner | `banner/lcd221-banner-720x320.png` (720x320) |
| Icons | `icons/lcd221-icon-80.png` (80x80) and `icons/lcd221-icon-144.png` (144x144) |

`LCD221-1.2.2.pbw` is a copy of `build/release.pbw`; rebuild it with `pebble build` then
`python3 tools/strip_pbw.py --deny <your real names>` and copy it here after every change.
