# CLIPBOARD — non-text selections

The selection bridge (`src/wayland/clipboard.c`) originally carried only text:
an offer kept a single "best text MIME" and an X convert was always answered as
`STRING`.  It now keeps the **full MIME list** and matches non-text targets, so
`image/png`, `text/uri-list`, `text/html` and similar can cross in both
directions.

## Wayland → X

* An offer records every MIME it advertises (`MwWlOffer.mimes`) as well as the
  best text type (`mime`).
* `TARGETS` is answered from that list without fetching: each MIME is interned
  as an atom, plus the text aliases (`UTF8_STRING`, `STRING`, `TEXT`,
  `COMPOUND_TEXT`) when any text MIME is present.
* A convert for any other target is matched against the MIMEs by atom name
  (case-insensitively). The bytes are fetched over a pipe and written with the
  **requested target as the property type**, 8-bit, unchanged.
  `STRING`/`TEXT`/`COMPOUND_TEXT` fall back to a text MIME and are converted to
  Latin-1.

## X → Wayland

* When an X client takes `CLIPBOARD`, the source advertises the text MIMEs as
  before plus a non-text superset: `image/png`, `image/jpeg`, `image/bmp`,
  `image/tiff`, `image/x-xpixmap`, `text/html`, `text/uri-list`,
  `application/x-color`.
* `wl_data_source.send(mime)`: a text MIME still pulls X `STRING` and converts
  to UTF-8 when the peer asked for UTF-8; any other MIME converts the X target
  **named by the MIME itself** and copies the bytes through unchanged.

## Tests

`scripts/run-tests.sh`:

* `binwltox` — `clip_wl offer-mime image/png …` → `clip_x convert-target image/png`.
* `binxtowl` — `clip_x own-mime image/png …` → `clip_wl receive-mime image/png`.

## Limits

* Only `CLIPBOARD` is bridged, not `PRIMARY`/`SECONDARY`.
* A converted property is written 8-bit with the target as type. Format-32
  targets (some colour/atom forms) are not repacked.
* The X→Wayland direction offers a superset, so a type the owner cannot serve
  simply yields an empty stream; the offer list is not derived from the owner's
  `TARGETS` (that would need an async convert before `set_selection`).
* Cross-process X↔X non-text relies on the selection broker, whose relay is
  generic (it carries type/format/nitems/bytes).
