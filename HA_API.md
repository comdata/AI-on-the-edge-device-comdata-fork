# Home Assistant API publishing (firmware feature)

This fork adds a publish step to the firmware: after every successfully
processed digitization cycle the device pushes its values **into Home Assistant
through the Home Assistant REST API** – authenticated with a long-lived access
token of your Home Assistant user. No MQTT broker and no custom integration on
the Home Assistant side are involved.

Base version: `v17.4.1-SLFork` (commit `3cf0ac7`), branch `ha-api`.

## Configuration on the device

Create `/config/homeassistant.json` on the device's SD card (web UI → *File
server* → `config/`, or `POST /upload/config/homeassistant.json`):

```json
{
  "enabled": true,
  "url": "http://192.168.1.156:8123",
  "token": "<long-lived access token>",
  "entity_prefix": "ai_on_the_edge",
  "publish_rate": true
}
```

A template ships as `config/homeassistant.json.dist` in the firmware package.
Missing file = feature disabled. Changes are applied on the next config reload
or reboot.

The token is sent as `Authorization: Bearer …`; create it in Home Assistant
under *Profile → Security → Long-lived access tokens*.

## Entities written per number sequence

For prefix `ai_on_the_edge` and sequence `main` the device posts
`POST /api/states/<entity_id>` every cycle:

| Entity | Content |
|---|---|
| `sensor.ai_on_the_edge_main_value` | meter reading (m³, attributes: raw value, last valid value, status, timestamps) |
| `sensor.ai_on_the_edge_main_raw_value` | value before post-processing corrections |
| `sensor.ai_on_the_edge_main_value_status` | post-processing status, e.g. `000 Valid` |
| `sensor.ai_on_the_edge_main_rate` | flow in m³/h (`publish_rate: false` disables it) |

Publishing never breaks a measurement: HTTP errors are logged and reported as
a warning, the cycle continues.

## Last camera image in Home Assistant

The device keeps serving its images; Home Assistant pulls them with the
built-in *Generic Camera* integration (no custom code):

```yaml
# Settings → Devices & services → Add integration → Generic camera
still_image_url: http://10.0.1.20/img_tmp/raw.jpg      # or /img_tmp/alg_roi.jpg (with ROIs)
username: aiote                                        # WebUI HTTP auth of the device
password: <device password>
```

## Controlling the configuration from Home Assistant

The device stays the server for its configuration, so Home Assistant uses
`rest_command` entries (see `ha/ai_on_the_edge_package.yaml`):

```yaml
service: rest_command.aioe_trigger_cycle
service: rest_command.aioe_set_config
data:
  config: '{"operationmode": {"automaticprocessinterval": "2.00"}}'
```

## Behaviour notes

- The states are regular Home Assistant state objects: dashboards, history and
  automations work. They are **not** entries of the entity registry, so they
  carry no device page and disappear from the UI after a Home Assistant
  restart until the next cycle republishes them (one process interval).
- Attribute names follow the device's value semantics; the reading uses
  `state_class: total_increasing`, so `utility_meter` and the energy dashboard
  work with it.

## Building

The firmware is built by GitHub Actions
(`.github/workflows/build-ha-api.yaml`, PlatformIO target `esp32cam`). The
artifact `aioe-ha-api-esp32cam` contains `firmware.bin`, `bootloader.bin`,
`partitions.bin`, `html/` and `config/`.

Update a running device (OTA):

```bash
zip -r aioe-ha-api.zip firmware.bin bootloader.bin partitions.bin html config
curl -u aiote:<password> "http://<device>/ota?task=emptyfirmwaredir"
curl -u aiote:<password> --data-binary @aioe-ha-api.zip "http://<device>/upload/firmware/aioe-ha-api.zip"
curl -u aiote:<password> "http://<device>/ota?task=update&file=aioe-ha-api.zip"
curl -u aiote:<password> "http://<device>/reboot"
```

The device unpacks the zip on boot, flashes `firmware.bin` and reboots again.
Keep the previous release zip at hand: a self-built firmware that does not boot
can only be recovered over USB.

## Image API

The device serves its images through a stable endpoint, so Home Assistant and
scripts do not have to know the internal file names:

| Request | Answer |
|---|---|
| `GET /api/v1/image?type=aligned` | image of the last processed cycle with the regions of interest (default, no flash) |
| `GET /api/v1/image?type=raw` | new capture **with flashlight** (same as `/img_tmp/raw.jpg`) |
| `GET /api/v1/image?type=reference` | the configured reference image |

The endpoint uses the same HTTP Basic auth as the rest of the API. Point the
Home Assistant *Generic camera* at it:

```yaml
still_image_url: http://10.0.1.20/api/v1/image?type=aligned
username: aiote
password: <device password>
```

## Mobile web UI (Astro)

`webui/` contains the new mobile-first interface, built with Astro into
`sd-card/html` (`index.html`, `m.html`, `m/*.html`, `_astro/*`):

```bash
cd webui
npm install
npm run build          # writes webui/dist
# then copy dist/index.html, dist/m.html, dist/m/ and dist/_astro/ into sd-card/html/
```

Pages: **Übersicht** (values, live image, actions), **Bild** (last/aligned,
reference, raw capture), **Konfig** (complete `/config` editor generated from
the JSON, booleans as switches, arrays as JSON), **Protokoll** (log files and
current run), **System** (device facts, OTA update, HA discovery, fallback
value, reboot). The classic pages stay reachable (e.g. the ROI editors, which
are linked from the new pages) and now carry a viewport meta tag so they are
usable on a phone as well. The old overview is kept as `index_legacy.html`.
