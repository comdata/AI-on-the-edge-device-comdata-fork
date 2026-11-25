# Initial setup wizard: automatic camera, alignment and ROI search

Date: 2026-11-25
Status: design, awaiting approval

## Intent

The initial setup (`sd-card/html/setup.html`) already walks through flashlight,
camera/reference, alignment markers, ROIs and WebUI security. But every value is
set by hand: exposure is tweaked by eye, the image rotation is guessed, each digit
and each analog dial is drawn as a box. Setting up a new meter is therefore the
hardest part of using the device.

The wizard shall do as much of that as possible itself: take test shots, pick a
well-exposed setting, level the image, create the reference and propose the digit
and analog ROIs. Everything runs locally on the device and in the browser; no
external service, no new firmware endpoint, no new model.

## Goals

1. **Auto exposure/flash** – sweep flashlight intensity and exposure level with
   single-shot captures, score them, and set the best values.
2. **Auto rotation** – estimate the skew of the digit row from the same capture,
   set `imagealignment.alignmentalgo = 3` (rotation only) and
   `imagealignment.imagerotation`.
3. **Auto reference** – take the final shot, save it rotated as
   `/config/reference.jpg`, and persist camera + alignment config.
4. **Auto ROIs** – detect digit boxes and analog dial boxes on the saved reference
   and prefill the existing ROI editor; the user reviews and saves.
5. **Wizard integration** – the new actions are the obvious path in steps 2 and 4;
   the manual editors remain unchanged and reachable.

## Non-goals

- No cloud or ML service, no new JS library, no bundled OpenCV.
- No new firmware endpoint and no firmware release dependency.
- No marker creation for SAD alignment; markers stay optional/manual (step 3).
- No analog `ccw` inference: direction still depends on the meter and is set by
  the user if the default is wrong.
- No post-processing tuning beyond enabling digit/analog and the decimal shift
  that follows from the detected ROI count.
- No promise that detection works on every meter; it proposes, the user confirms.
- No automatic framing (zoom/offset) search: the user frames the meter in step 2
  with the existing zoom controls; the automatic exposure uses that frame.
- No automatic decimal-shift write and no second digit band: the wizard proposes
  one digit row and one dial group for the selected sequence; further sequences
  remain manual.
- No guided-split fallback in v1: when detection finds nothing, the existing
  manual ROI editor is the fallback and the page says so.

## Anti-goals

- Silently writing low-confidence ROIs into the config.
- Overwriting a working configuration outside setup mode or an explicit button.
- A slow wizard that blocks on many captures without progress feedback.

## Constraints

- Pages are served from SD by the ESP32 HTTP server with session-cookie auth;
  camera captures use `GET /camera?task=capture` (temporary parameters, returns
  JPEG, no config change), see `code/components/camera_ctrl/server_camera.cpp`.
- Reference upload: `POST /upload/config/reference.jpg?overwrite=true`; config:
  `POST /config?reload=true` (existing `saveConfig` helper).
- `edit_reference.html` saves the reference **already rotated** by
  `imagealignment.imagerotation`; ROIs are drawn in that rotated frame.
- Output frame is 640×480 on the target board; other boards may be larger.
- Cache-busting `?v=$COMMIT_HASH` must be added to the new script reference.
- German UI comes from `i18n.de.js`; new English text nodes need entries.
- Deployment without OTA must work (`/upload/html/...?overwrite=true`).

## Approaches

| Way | Effort | Robustness |
| --- | --- | --- |
| A – wizard asks for one box around the digit row and splits it evenly | small, WebUI only | very robust, but no detection |
| B – browser CV proposes boxes, user confirms (chosen) | medium, WebUI only | good on clear images, correctable |
| C – detection in firmware | large, build + OTA | strongest, highest risk |

**Chosen: B, with A as the fallback inside it.** The browser already receives the
reference JPEG, has `canvas.getImageData`, and `/camera?task=capture` allows test
shots without touching the config. C was rejected because it needs a release cycle
and memory/CPU that cannot be validated from this workspace. A alone does not meet
the "search" requirement, but it is cheap to keep as the fallback when detection
finds nothing.

## Design

### Flow

`setup.html` keeps its five steps; step 2 and step 4 gain an automatic action.

1. Flashlight & focus (unchanged).
2. **Camera & reference**: new button *Auto-Adjust & Create Reference*. It takes
   test shots, picks exposure/flash, estimates rotation, then captures, rotates and
   saves the reference plus camera/alignment config. The existing manual controls
   stay below.
3. Alignment markers (unchanged, now explicitly optional because step 2 set
   `alignmentalgo = 3`).
4. **Number sequences**: opened by the wizard with `?autosearch=1`; when the
   selected sequence has no ROIs, the page runs the auto search once and shows the
   proposals. An *Auto-Detect ROIs* button repeats it in the standalone page too.
   Only the ROI type that was actually found is replaced, the other one is kept.
   The user adjusts boxes as today and presses *Save And Apply*.
5. Security & completion (unchanged).

### New shared script `sd-card/html/autoRoi.js`

Plain ES5-compatible script, no dependencies. Works on a plain image buffer so it
can also run in Node for the test:

```js
AutoRoi.toGray({width, height, data}, maxSide)            -> {width, height, data}
AutoRoi.estimateSkew(gray, band)                          -> {angle, score}
AutoRoi.detectDigitRois(gray)                             -> {rois, confidence, band}
AutoRoi.detectAnalogRois(gray)                            -> {rois, confidence}
AutoRoi.scaleRois(rois, factor)                           -> rois in original pixels
```

Exposed as `window.AutoRoi`; `module.exports = AutoRoi` when `module` exists.

**Digit search**
1. Grayscale, longest side ≤ 600 px; vertical gradient `|Gx|` per row.
2. Row segments with elevated edge activity are the candidate bands; each is
   expanded until the digit strokes fit.
3. Inside a band, dark and bright connected components are extracted with a
   percentile threshold (both polarities are tried).
4. Components are filtered by height/width/area, then aligned by their y centre.
   Components are split into runs where the centre pitch deviates more than
   ±30 % from the median pitch, which drops units such as `m³`.
5. The longest run of 3…9 components becomes the proposal; common y and height
   are taken from the run, each box is expanded by 40 % width and 20 % height.
6. `confidence = count/5 * (1 - height variation)`; below 0.3 no proposal.

**Analog search**
1. First try coloured pointer blobs (RGBA, red-dominant, connected components,
   area/aspect filters). The blob centroid is near the dial centre; the box is
   a square of `1.9 * pointer length`, up to 6 dials. This is what works on the
   reference meter in this repo.
2. Fallback: grayscale Hough for dial rims (radii 6…30 px downscaled), centre
   votes summed over a 7×7 box, radial-gradient support ≥ 0.45, grouped by radius
   and row.
3. `confidence` from the number of found dials and their support.

**Skew**
Search −15°…+15° in 0.5° steps over the detected band; metric is the variance of
the horizontal projection of binarized pixels (maximal when digit edges are
horizontal). Angles below 0.7° or a flat score landscape yield 0. The same canvas
transform as the preview is used, so the preview always shows the result that will
be saved.

### Auto exposure in `edit_reference.html`

- Candidates: flashlight intensity {30, 50, 70} × auto-exposure level {−1, 0, 1}
  when the camera uses automatic exposure; otherwise manual exposure values
  {200, 400, 800, 1200} with automatic gain.
- Each candidate uses `/camera?task=capture` twice (first frame discarded so AEC
  can settle), loads the second into a 320 px canvas and scores
  `P98 − P2 − 6·(whiteClip + blackClip)` on the centre 70 % of the frame.
- Best candidate is applied to `jsonConfigModifiedDelta` only; the existing
  *Save And Apply* path persists it. The button shows progress text and can be
  cancelled; timeout per shot follows the flash time (existing `doTake` pattern).
- If no candidate scores above the current setting, keep the current setting and
  say so.

### Persisting the automatic setup

- `alignmentalgo = 3`, `imagerotation = angle` in `jsonConfigModifiedDelta`.
- Chosen `flashintensity` and exposure values in `jsonConfigModifiedDelta`.
- Reference: rotated canvas → `/upload/config/reference.jpg?overwrite=true` →
  `saveConfig(..., true)`; same order as the existing `saveReference()`.
- ROI proposals go into `jsonConfigModifiedDelta.digit/analog.sequence[i].roi`
  as the editor already does; `digit.enabled` / `analog.enabled` are set when
  boxes were found. Nothing is persisted until *Save And Apply*.

### Failure handling

- No camera / auth error → firework error, manual path stays available.
- Low confidence → keep the current ROI set, show "No reliable proposal found –
  please set the ROIs manually" and offer the guided split (A) as the fallback.
- The auto button never deletes existing ROIs without the user's confirmation.
- Captures are rate-limited by awaiting each response; no parallel requests.

### Tests

- `tools/test_auto_roi.mjs` (Node, no dependencies): synthetic 640×480 buffers with
  (a) six digit-like bars in a row, (b) four dial circles, (c) random noise, (d) a
  5° sloped digit row. Asserts the expected boxes, low confidence on noise and the
  skew estimate. Run: `node --test tools/test_auto_roi.mjs`.
- Browser check was not possible in this workspace (no Chrome installed); the
  page scripts are syntax-checked with `node --check`.
- On-device check: run the wizard on a real meter, confirm proposals, first reading.

### Deployment

Upload the changed `autoRoi.js`, `edit_reference.html`, `edit_sequence.html`,
`setup.html`, `i18n.de.js` with cache-busted `?v=` values via
`/upload/html/...?overwrite=true`; include them in the next release normally.

## Decisions taken after the builder check

- Framing stays manual (zoom controls in step 2); detection and exposure use the
  current frame. No digits found means a warning, not a guess.
- Step 4 auto-runs only with `?autosearch=1` from the wizard, and only for a
  sequence without ROIs. The standalone button is always available.
- The saved `/config/reference.jpg` is the detection base; a missing image shows a
  warning. Staleness is not verified.
- The decimal shift is not written; the existing post-processing setting stays as
  the user configured it.
- Detection failure falls back to the existing manual editor (no guided split).

## Left to the builder

- Exact thresholds and score weights, tuned against the real reference image.
- Minor layout of the new buttons and progress text in the existing cards.
