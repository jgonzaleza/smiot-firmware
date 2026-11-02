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

The HTTPS CA check for context 1 (`sslversion` 3, SNI, `cacert`, `authmode` 1, then `https_set_ssl_index(1)`) and the MQTT `sslversion` 3 change are applied on this snapshot.

LilyGO commit `d6165e3` reports TinyGSM 1.0.0 in `lib/TinyGSM/library.json`. A sparse fetch of that commit failed (`couldn't find remote ref d6165e3` on a depth-1 clone), so the 1.0.0 tree was not swapped in. Replacing `TinyGsmHttpsA76xx.h` with `TinyGsmHttpsComm.h` without a compiled tree would drop the GPS sign fix, the download deadline and the `CCERTLIST` guard. Those stay on this snapshot until that tree can be built.
