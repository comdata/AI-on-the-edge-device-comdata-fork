# Agent notes for this fork

## Device under test

| | |
|---|---|
| Host | `10.0.1.20` (mDNS `watermeter.local`), hostname `watermeter` |
| Board | ESP32CAM (AI-on-the-Edge, ESP32, 8 MB PSRAM, SD 4 GB) |
| Firmware | `ha-api` branch / `v18.0.0-SLFork` base, HA API publishing |
| WebUI | `http://10.0.1.20/` |

## HTTP access (WebUI and every REST endpoint)

The device runs with **WebUI HTTP authentication enabled** (`webui.httpauth.authmode = 1`).
Every request – API, images, files, OTA – needs HTTP Basic auth:

```
User:     aiote
Password: rtJMMja9Hwf5HqM95YHKQKrg
```

Example:

```bash
curl -u aiote:'rtJMMja9Hwf5HqM95YHKQKrg' http://10.0.1.20/info
curl -s -o raw.jpg -u aiote:'rtJMMja9Hwf5HqM95YHKQKrg' http://10.0.1.20/img_tmp/raw.jpg
```

The same credentials are stored in the Home Assistant helper secrets
(`aioe_user` / `aioe_password`) and in the earlier HA integration entry.

## Useful endpoints (v18)

`/info`, `/info?type=firmware_version`, `/process_data`, `/config` (GET/POST),
`/cycle_start`, `/reboot`, `/set_fallbackvalue?sequence=main&value=-1`,
`/img_tmp/raw.jpg` (fresh capture with flashlight), `/img_tmp/alg.jpg` (aligned
image without overlays), `/img_tmp/alg_roi.jpg` (aligned with ROI overlays),
`/fileserver/config/reference.jpg?` (reference image).

In v18 a `POST /config` is **not** applied to the running process for ROI/model
changes – reboot the device (`/reboot`) after changing the geometry.

## Calibration notes (2026-11-14)

Camera sits farther from the meter than the original setup (digits ~20 px wide
at `zoomfactor` 1378), so the whole face including the four decimal dials only
fits at 1378; higher zoom cuts the dials off.

* `imagealignment.alignmentalgo = 3` (rotation only), `imagerotation = 10.7`
  levels the digit row; markers are then unused.
* Digit model `dig-class11_2000_s2_q.tflite` (class100 misreads more here).
* Digit ROIs are tight boxes around the last three digits (see `/config`).
* Analog ROIs are 86×86 boxes on the four small dials.
* Marker files, if used, must be written to `/config/marker1.jpg` and
  `/config/marker2.jpg` (not `/img_tmp`).
