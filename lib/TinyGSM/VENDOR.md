# TinyGSM vendor record

This tree vendors a TinyGSM snapshot imported with firmware 3.0.0.

| Field | Value |
| --- | --- |
| Declared version | 0.11.7 (`library.json`) |
| Fork | LilyGO [LilyGo-Modem-Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series) |
| Upstream SHA | Unknown. The import did not record a commit. |
| `library.json` sha256 | `f6b7d04011b6d0fdc489488089e6b077aeff65af2db43be09d3f9226e37defdd` |
| `src/TinyGsmClientA7608.h` sha256 | `27b4b7a0bc28d733feb60452f3e5c40618ed47642facfccf8cea5a254e369e18` |

No separate local delta could be extracted from the 3.0.0 import: the snapshot is the baseline, and its LilyGO commit was not identified.

## Local edits after import

`patches/v3.1.0-local-edits.patch` (applied in this tree):

- `https_get_size()` waits at most 20 seconds instead of 120. The A7608 `waitResponse` loop calls `TINY_GSM_YIELD()`, which the firmware defines to feed the task watchdog.

The N/S and E/W GPS sign is applied in `TinyGsmClientA7608.h` (v3.2.0). South and west readings are stored as negative degrees. The diff is `patches/v3.2.0-gps-sign.patch`.
