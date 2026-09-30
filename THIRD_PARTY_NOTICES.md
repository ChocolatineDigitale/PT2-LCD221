# Third-party notices

## 7-Segment font (segment outlines)

The digit shapes on the watch face are the segment outlines of the **7-Segment**
font by **Jan Bobrowski** ("~jb"), <https://torinak.com/font/7-segment>, version 3.0.

- Licence: **SIL Open Font License 1.1** (full text in [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt)).
- How it is used: `tools/gen_segments.py` reads the font, straightens its slight italic
  and writes the seven segment outlines as polygons into `src/c/segments.h`. The font
  file itself is not included in this repository or in the watch app; only these
  derived outlines are. Because that makes `segments.h` a modified derivative of the
  font, it stays under the same licence. The outlines are not sold on their own.
- The font file carries no copyright line or Reserved Font Name.

## Clay (settings page library)

The phone-side settings page is built with **pebble-clay**, Copyright (c) 2016 Pebble
Technology, MIT licence (full text in [`LICENSES/pebble-clay-MIT.txt`](LICENSES/pebble-clay-MIT.txt)).

## Weather data

Temperatures come from the **Open-Meteo** API, <https://open-meteo.com>, which is
free for non-commercial use and licensed CC BY 4.0. Weather data by Open-Meteo.com.

## Trademarks

Casio and W-221H are trademarks of Casio Computer Co., Ltd. Pebble is a trademark of
its owners. This watch face is an independent, unofficial tribute to the look of the
Casio W-221H and is not affiliated with or endorsed by Casio or Pebble.
