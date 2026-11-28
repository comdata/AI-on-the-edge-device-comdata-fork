# Agent notes for this fork

## Device under test

| | |
|---|---|
| Host | `10.0.1.20` (mDNS `watermeter.local`), hostname `watermeter` |
| Board | ESP32CAM (AI-on-the-Edge, ESP32, 8 MB PSRAM, SD 4 GB) |
| Firmware | `develop-f849ee8` (seit 2026-11-25): Branch-Build von `develop` (`Build and Release`, Artifact `AI-on-the-edge-device__esp32cam__comdataFork_develop-f849ee8`) mit Formular-Login (Standard, Basic aus), vier Session-Slots und Sprachwechsel ohne Reload. Meldet `develop-f849ee8`. Ein Release-Tag mit diesem Stand gibt es noch nicht – der Auto-Updater (`/ota/pull`) lässt Branch-Builds in Ruhe. |
| WebUI | `http://10.0.1.20/` – responsive comdataFork-WebUI (`index.html` + Sidebar, `dashboard.html`, `edit_configuration.html`), plus lokale UI-Tweaks (Inline-Manifest, `aria-current`) |
| Home Assistant | Wasserzähler hängt **nur** an der HA-API (REST, Push alle ~1 min), MQTT ist am Gerät aus und in HA vollständig entfernt (2026-11-15). |

## HTTP access (WebUI and every REST endpoint)

The device runs with **WebUI authentication enabled** (`webui.httpauth.authmode = 1`)
and since `ha-api-form-login-4797fda` (2026-11-24) uses a **form based login**:

* `GET /login` serves `sd-card/html/login.html`; `POST /login` takes
  `application/x-www-form-urlencoded` fields `username`, `password`, `next` and
  answers `303 See Other` with
  `Set-Cookie: AIOTE_SESSION=<32 hex>; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400`.
  Wrong credentials redirect to `/login?err=1&next=…`.
* `GET /logout` clears the session.
* Browser requests without a session get `303 /login?next=…`; requests that do
  **not** send `Accept: text/html` get `401`.

The HTTP basic auth scheme is now a **separate, optional switch**
(`webui.httpauth.basicauth`, boolean, **firmware default `false`**). It exists for
clients that cannot keep a cookie. **It is off on this device.** Turn it on/off in
the WebUI under *Configuration → WebUI → Basic Authentication* (checkbox), or by
posting the config (`POST /config{"webui":{"httpauth":{"basicauth":true}}}`), which
also needs a reboot or `?reload=true` like every other config change.

```
User:     aiote
Password: rtJMMja9Hwf5HqM95YHKQKrg
```

```bash
# session cookie – works everywhere, no basicauth needed
curl -c /tmp/aioe.txt -d 'username=aiote&password=rtJMMja9Hwf5HqM95YHKQKrg&next=/' \
  http://10.0.1.20/login
curl -b /tmp/aioe.txt http://10.0.1.20/info
curl -s -o raw.jpg -b /tmp/aioe.txt http://10.0.1.20/img_tmp/raw.jpg
curl -b /tmp/aioe.txt http://10.0.1.20/logout   # ends only this session

# basic auth – only while basicauth is enabled (it is off on this device)
curl -u aiote:'rtJMMja9Hwf5HqM95YHKQKrg' http://10.0.1.20/info
```

Sessions live in RAM (four slots, 24 h each), so the WebUI, a second browser and
the Home Assistant integration do not log each other out; a reboot ends all
sessions, and `GET /logout` clears only the caller's own session.

The same credentials are stored in the Home Assistant helper secrets
(`aioe_user` / `aioe_password`) and in the earlier HA integration entry.

## Useful endpoints (v18)

`/info`, `/info?type=firmware_version`, `/process_data`, `/config` (GET/POST),
`/cycle_start`, `/reboot`, `/set_fallbackvalue?sequence=main&value=-1`,
`/img_tmp/raw.jpg` (fresh capture with flashlight), `/img_tmp/alg.jpg` (aligned
image without overlays), `/img_tmp/alg_roi.jpg` (aligned with ROI overlays),
`/fileserver/config/reference.jpg?` (reference image).

In v18 a `POST /config` is **not** applied to the running process for ROI/model
changes. Either reboot (`/reboot`) or post the same full config to
`POST /config?reload=true`, which reinitializes the process in place (no reboot
needed; masked fields `******` mean "keep stored value").

## Upstreams

`Slider0007/AI-on-the-edge-device` (`develop`) is the real upstream — merge it, it
carries the v18 line (WebUI rework, `code/components/*`, per-board sdkconfigs).

`jomjol/AI-on-the-edge-device` (`main`) is **not** an upstream: both lines split on
2023-04-19 and developed separately (jomjol at v16.1.0 with `jomjol_*` components).
A merge probe (2026-11-15) gives 152 conflicted files; a merge would trade one
codebase for another. Take individual fixes from it by hand instead, if ever.

It is set up as the git remote `upstream` (push disabled), so the regular merge is:

```bash
git fetch upstream && git merge upstream/develop
```

Merge it often (it is the only source of shared fixes) and resolve conflicts in
favour of this fork's v18 + comdata changes.

## OTA from the fork's GitHub releases (device pulls itself)

Since the `feat(ota)` change the device can update itself; the release ZIP no
longer has to travel through a browser.

* `POST /ota/pull?url=<asset-url>` (auth like every endpoint) downloads a
  release ZIP on the device: HTTPS with the certificate bundle, streamed into
  `/.ota_staged/ota_package`, then the normal reboot → unzip → flash path.
  Only URLs starting with
  `https://github.com/comdata/AI-on-the-edge-device-comdata-fork/releases/download/`
  are accepted (the handler is not a general URL fetcher).
* `GET /ota/pull` answers `idle`, `running`, `done` or `failed: …`; the WebUI
  page (`sd-card/html/sys_ota.html`) polls it while the device works.
* The page resolves the release itself via
  `https://api.github.com/repos/comdata/AI-on-the-edge-device-comdata-fork/releases/latest`
  from the browser (repo is public, no token) and picks the asset named
  `AI-on-the-edge-device__<board type lowercase>__comdataFork_*.zip`; the board
  names in the asset match `BOARD_TYPE_NAME` lowercased.
* Auto update: `startOtaAutoUpdate()` (called from `app_main` after the
  webserver starts) checks 60 s after boot and then every 6 h. It installs a
  newer release **only when the running firmware is a release tag (`v…`)** –
  an Actions artifact build (`feat/…-<hash>`) is never replaced automatically,
  because its version cannot be compared with a tag.
* Rotation: the download task needs ~12 KB stack (TLS) and runs beside the
  httpd task, so the OTA handlers stay short and only trigger tasks.

## Release and OTA deployment

Releases come from `develop`: merge the release PR from
`release-please--branches--develop`, CI (`.github/workflows/build-release.yaml`)
tags `v<version>-comdataFork` and attaches board ZIPs named
`AI-on-the-edge-device__<board>__comdataFork_v<version>.zip` (firmware + WebUI).

```bash
gh release download v<version>-comdataFork -p "*__esp32cam__comdataFork_*.zip" -D /tmp/ota
curl -b /tmp/aioe.txt --data-binary @/tmp/ota/*.zip \
  "http://10.0.1.20/ota/$(basename /tmp/ota/*.zip)"
```

(`/tmp/aioe.txt` is the cookie jar from the login in *HTTP access*; `-u aiote:<password>`
works only while `basicauth` is enabled.)

The device answers `success`, reboots and stages the package; afterwards
`firmware_version` and `html_version` must both report the new tag.

As of 2026-11-24 the device runs such a develop-style build (version string
`ha-api-form-login-4797fda`), even though earlier firmware came from the
`ha-api` line: `POST /ota/<file>.zip` works, `/ota?task=…` and
`/upload/firmware/…` answer 404. Anything that builds with
`.github/workflows/build-ha-api.yaml` only triggers on branches named
`ha-api*`; `build-release.yaml` runs for every other branch and produces the
package with the compiled WebUI (tooltips!), while the ha-api workflow copies
`sd-card/html_compiled`, which does not exist — its artifact carries the raw
`sd-card/html` with unreplaced `$TOOLTIP_*` placeholders. Deploy the
build-release artifact.

The `ha-api` builds (releases `ha-api-v*`, own OTA implementation) do **not**
understand that staged flow – use their route instead, which unzips straight
into `/sdcard` (so `/html` is replaced in the same step):

```bash
curl -b /tmp/aioe.txt --data-binary @pkg.zip \
  "http://10.0.1.20/upload/firmware/pkg.zip?overwrite=true"   # 303
curl -b /tmp/aioe.txt \
  "http://10.0.1.20/ota?task=update&file=pkg.zip"             # answers "reboot"
curl -b /tmp/aioe.txt "http://10.0.1.20/reboot"
```

Gotchas:

* Beim Wechsel **von** einem `ha-api`-Build (dessen OTA unzip direkt nach `/sdcard`
  macht) auf `develop`: vorher prüfen, dass `/.ota_staged` leer ist. Sonst legt
  `updateOtaAssets()` beim nächsten Boot die dort noch liegenden alten
  WebUI-Dateien über das frisch entpackte `/html` – Firmware neu, UI alt,
  Versions-Warnung im Log.
* Die OTA-Route gehört zum **laufenden** Stand: `develop` = `POST /ota/<name>.zip`,
  `ha-api`-Builds = `/upload/firmware/…` + `/ota?task=update&file=…` + `/reboot`.
* Der HA-API-Stand für dieses Gerät ist der Branch `feat/ha-api-on-develop`
  (develop + Cherry-pick `8cd8668c`); Firmware baut CI
  (`.github/workflows/build-release.yaml`, Artifact `AI-on-the-edge-device__esp32cam__comdataFork_*`).

* The version suffix lives in the release tag. If `.release-please-manifest.json`
  names a version with no matching git tag, release-please falls back to an old
  baseline and proposes a wrong version (it offered 16.0.0 after the suffix
  change). `v18.0.0-comdataFork` on the 18.0.0 release commit bridges the gap to
  the `v18.0.0-SLFork` release.
* The repo setting "Allow GitHub Actions to create and approve pull requests"
  must stay enabled, otherwise `prepare-release` fails right after creating its
  branch.
* `release-please` opens the release PR as a **draft**; `gh pr ready <nr>` before
  `gh pr merge`, otherwise the merge is refused. Merging it creates the tag and
  runs the build in the same workflow.
* `gh-pages-webinstaller` holds the web installer; a malformed
  `manifest_<board>.json` there fails only that board's job.

## Home Assistant (custom integration, polling, no MQTT)

One HA device "Wasserzähler" owns every entity; the source of truth is the small custom
integration in this repo: `ha/custom_components/ai_on_the_edge/`. It polls
`/process_data` and `/info` over the device's REST API (Basic auth, default 60 s) and
creates `sensor.wasserzahler_zahlerstand` (m³, `total_increasing`), `_rohwert`,
`_wertestatus`, `_durchfluss`, `_letzte_messung`, `_firmware`, `_laufzeit`,
`_wlan_signal`, `_cpu_temperatur`, `_prozessstatus`, `_prozesszustand`,
`_prozessfehler`, `_messzyklen`, `binary_sensor.wasserzahler_problem`,
`camera.wasserzahler_kamera` and the buttons `_messung_starten` / `_gerat_neu_starten`
(the reboot button is disabled by default).

* The per-sequence entities exist **per number sequence** the device reports: the first
  sequence keeps the plain names and unique_ids, further ones get a ` · <sequence>`
  suffix. `sensor.wasserzahler_zahlerstand` therefore keeps its entity_id.
* `binary_sensor.wasserzahler_problem` is on when a `value_status` is anything but
  `000 Valid` (see `code/include/defines.h`, e.g. `E91 Rate negative`) or
  `process_error` is set.
* Settings (poll interval, port/user/password, camera image `alg`/`raw`/`alg_roi`,
  HTTPS and TLS verification) live in the entry **options**; changing them reloads the
  entry. Reauth and reconfigure are supported.
* Services: `ai_on_the_edge.set_config` (partial config, deep-merged into the full
  `/config` and posted with `?reload=true`) and
  `ai_on_the_edge.set_fallback_value` (`-1` clears it). Both take an optional `entry_id`
  when several devices are set up. They replace the `rest_command` entries in
  `ha/ai_on_the_edge_package.yaml` for this device.

Deploy (the sandbox reaches HA over SSH, port 22, as root):

```bash
scp -r ha/custom_components/ai_on_the_edge \
  root@192.168.1.156:/config/custom_components/
```

A new/changed platform only loads after a Home Assistant restart (`ha core check`, then
`ha core restart` or the MCP restart) – reloading the config entry is not enough.
`ha core check` fails on this installation for an unrelated reason (`/media/llmvision*`
missing from `allowlist_external_dirs`); the restart itself works.

Without `jq`/`python3` on the HAOS host shell, inspect the registry with
`ssh root@192.168.1.156 'jq … /config/.storage/core.entity_registry'` and call services
through HA's REST API (`POST /api/services/…`) with a long-lived token.

A broken `services.yaml` does not break the integration: Home Assistant only logs
`Unable to parse services.yaml …` and the service form in the UI stays empty, so check
`ha core logs -n 3000 | grep services.yaml` after deploying one. A number selector's
`step` must be at least `0.001`.

There is no HA API token in the sandbox, so entity states are not reachable. To prove the
integration really polls, append a `logger: logs: custom_components.ai_on_the_edge: debug`
block to `/config/configuration.yaml` (back it up first), `ha core restart`, wait ~140 s and
grep `ha core logs` for `Finished fetching … (success: True)`; then restore the backup and
restart again, otherwise HA keeps the debug level. The SSH call needs a timeout above 200 s
or it dies mid-restart and leaves the block in place.

Consumers that must follow the reading entity: the `utility_meter` helper "Wasser pro Tag"
(`sensor.wasserzaehler_wasser_pro_tag`), the Energy dashboard's water source
(`ha_manage_energy_prefs`), and the NSPanel blueprints (`automation.panelbad`,
`automation.panelwohnzimmer`, inputs `entities_entity08` /
`utilities_page01_group04_value2`).

The device's own push (`/config/homeassistant.json`, `sensor.ai_on_the_edge_*`) is
**disabled** (`enabled: false`) – it would duplicate every state. MQTT for this device is
off: `mqtt.enabled=false`, `homeassistant.discoveryenabled=false`,
`retainprocessdata=false`, and in HA the old `sensor.wasserzaehler_*` discovery entities,
their retained `homeassistant/<comp>/Wasserzaehler/*/config` topics and the re-discovery
automation are gone (2026-11-15). Do not re-enable MQTT on this device.

## WebUI direkt auf das Gerät schieben (ohne OTA)

Die WebUI liegt auf der SD-Karte unter `/html` und wird vom Dateiserver
(`/fileserver/…`) bedient. Ein einzelnes geändertes HTML/CSS/JS lässt sich
deshalb direkt ersetzen – kein Flash, kein Reboot, sofort sichtbar:

```bash
curl -b /tmp/aioe.txt -X POST --data-binary @sd-card/html/style.css \
  "http://10.0.1.20/upload/html/style.css?overwrite=true"
```

Gotchas:

* Ohne `?overwrite=true` antwortet der Endpunkt `400 File already exists`; die
  Antwort ist `303` auf den Ordner.
* Alle Antworten tragen `Cache-Control: max-age=31536000`. Im Repo steht
  `?v=$COMMIT_HASH`, das erst der Build (`code/scripts/post_build_tasks.py`)
  durch den Commit-Hash ersetzt – auf der SD bleibt der Platzhalter stehen.
  Für einen direkten Upload deshalb vorher `sed 's/?v=\$COMMIT_HASH/?v=<neu>/g'`
  anwenden, sonst sehen Browser weiter die alte Datei. `global.js` reicht die
  Version inzwischen an `i18n.<lang>.js` weiter.
* Der Endpunkt akzeptiert bis 16 MB pro Datei (`MAX_FILE_SIZE`), also
  `edit_configuration.html` mit ~175 kB problemlos.
* `GET /` funktioniert, `GET /?v=…` liefert 404 – die Root-Route matcht nur
  exakt `/`. Für Tests mit Cache-Buster `/index.html?v=…` nehmen.
* Vorher die Gerätefassung sichern: `curl -u … -o backup/<datei> http://10.0.1.20/<datei>`
  pro Datei (es gibt kein ZIP-Download).
* Beim nächsten OTA wird `/html` komplett ersetzt – direkte Uploads sind also
  nur für Zwischenstände gedacht.
* Der Manifest-Link in `index.html` ist eine **inline `data:`-URL**: Chrome holt
  `/site.webmanifest` ohne Credentials, die HTTP-Auth antwortet darauf mit 401
  und die Konsole zeigt bei jedem Laden einen Fehler. Chrome akzeptiert das
  data:-Manifest (`Page.getAppManifest` → `errors: []`), die Datei bleibt nur
  als Quelle liegen.
* Übersetzungen hängen an exakten Textknoten. Dynamische Texte deshalb in zwei
  Knoten schreiben (`output.append(label, ' (…dBm)')`), sonst findet das
  Wörterbuch den Schlüssel nicht – und Schlüssel ohne Emoji halten, wenn das
  Markup keine Emojis mehr enthält.

## CPU frequency / thermal

`system.powermanagement.cpu.maxfrequency` accepts `80`, `160` and `240` (`configClass.cpp`,
clamped 80…240). `setCPUFrequency()` (`code/components/misc_helper/system.cpp`) configures
`esp_pm_configure` with that maximum and **min 80 MHz**, i.e. dynamic frequency scaling. 80 MHz
is the floor because WiFi and the PSRAM need it; the old code only handled `240` and left PM
unconfigured at 160.

Measured on the device (2026-11-28): DFS never steps down while WiFi is connected (APB lock),
`chip_frequency` stays at the maximum. The clock has almost no effect on the temperature —
61–62 °C at 160 MHz *and* at fixed 80 MHz, because WiFi/camera/PSRAM dominate. Fixed 80 MHz only
doubles `process_time` (17 s → 29 s). So CPU clock is **not** a thermal lever here; 160 MHz is
the sane default.

## Calibration notes (2026-11-14)

Camera sits farther from the meter than the original setup (digits ~20 px wide
at `zoomfactor` 1378), so the whole face including the four decimal dials only
fits at 1378; higher zoom cuts the dials off.

* `imagealignment.alignmentalgo = 3` (rotation only), `imagerotation = 10.7`
  levels the digit row; markers are then unused.
* Digit model `dig-class100_0182_s2_q.tflite` (since 2026-11-15). With
  `dig-class11_2000_s2_q.tflite` the last digit `6` was read as `8` and the value
  jumped 446.5 → 448.6; rate check froze it there. `dig-class11_2000` is the
  alternative if class100 ever degrades.
* Digit ROIs are tight boxes around the last three digits (see `/config`).
* Analog ROIs are 86×86 boxes on the four small dials.
* Marker files, if used, must be written to `/config/marker1.jpg` and
  `/config/marker2.jpg` (not `/img_tmp`).

## WebUI language selection (i18n)

English is the source language: the HTML keeps English texts, and
`sd-card/html/i18n.de.js` holds a dictionary of English → German strings.
`global.js` (loaded by every page) swaps the text at runtime, so no page needs
its own markup for it:

* `getLanguage()` / `setLanguage()` persist the choice in `localStorage` and a
  cookie (`lang`, 400 days), same pattern as the theme switch. Switching
  reloads the page.
* The dropdown sits in the header of `index.html` and `setup.html`
  (`#langSelect`); iframe pages pick the language up from storage.
* Matching happens on text nodes and the `title`, `placeholder`, `alt` and
  `aria-label` attributes, with collapsed whitespace, so multi-line HTML works.
  Anything missing from the dictionary stays English. `script`, `style`, `code`,
  `pre` and `textarea` are skipped; add class `no-i18n` to opt out per element.
* A further language = new `i18n.<lang>.js` (fills `window.I18N_DICT`), one entry
  in `I18N_SOURCES` in `global.js`, and one `<option>` in the two headers.
* Not translated, intentionally: the API docs pages (markdown under `docs/API`)
  and the parameter tooltips (`docs/Configuration/Parameter/*.md`, compiled into
  `/tooltips.json` by `tools/webui/generate_param_tooltips.py`). Those are a
  separate, much larger batch.

## Dependency updates (Dependabot)

`.github/dependabot.yml` covers the two ecosystems this repo has manifests for:

* `github-actions` – the `uses:` pins in `.github/workflows/` (monthly, grouped into one PR).
* `pip` – `requirements.txt` at the repo root (`markdown`, `esp_idf_size`), which both build
  workflows install with `pip install -r requirements.txt`. PlatformIO itself stays
  unpinned on purpose (the build tracks the latest core).

PlatformIO (`code/platformio.ini`) and the ESP-IDF managed components
(`code/components/*/idf_component.yml`) have no Dependabot ecosystem and stay manual, as
does the HA integration (empty `requirements` list). Whether Dependabot version updates and
alerts are on is a repo setting; the sandbox token has no admin right there.
