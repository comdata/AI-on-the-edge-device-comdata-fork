// Self-check for sd-card/html/autoRoi.js with synthetic images.
// Run: node --test tools/test_auto_roi.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const AutoRoi = require('../sd-card/html/autoRoi.js');

function makeImage(width, height, paint) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) {
    data[i] = 200;
    data[i + 1] = 200;
    data[i + 2] = 200;
    data[i + 3] = 255;
  }
  const setGray = (x, y, v) => {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const i = (y * width + x) * 4;
    data[i] = data[i + 1] = data[i + 2] = v;
  };
  paint(setGray, width, height);
  return { width, height, data };
}

function rect(setGray, x0, y0, w, h, value) {
  for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) setGray(x, y, value);
}

function digitStrip({ slope = 0 } = {}) {
  return makeImage(640, 480, (setGray) => {
    rect(setGray, 145, 170, 360, 100, 40);
    for (let i = 0; i < 6; i++) {
      const x0 = 158 + i * 56;
      const y0 = 188 + Math.round(Math.tan((slope * Math.PI) / 180) * (x0 - 320));
      rect(setGray, x0, y0, 36, 64, 245);
    }
  });
}

function dials() {
  return makeImage(640, 480, (setGray) => {
    for (const cx of [100, 260, 420, 580]) {
      for (let y = -34; y <= 34; y++) {
        for (let x = -34; x <= 34; x++) {
          const r = Math.hypot(x, y);
          if (r >= 29 && r <= 33) setGray(cx + x, 240 + y, 20);
        }
      }
    }
  });
}

function noise() {
  return makeImage(640, 480, (setGray, width, height) => {
    let seed = 12345;
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        setGray(x, y, seed % 256);
      }
    }
  });
}

test('finds a row of six digits', () => {
  const result = AutoRoi.detectDigitRois(digitStrip());
  assert.ok(result.rois.length >= 5 && result.rois.length <= 7, `got ${result.rois.length} ROIs`);
  const centers = result.rois.map((r) => r.x + r.dx / 2).sort((a, b) => a - b);
  const expected = Array.from({ length: 6 }, (_, i) => 158 + i * 56 + 18);
  for (const e of expected) {
    assert.ok(centers.some((c) => Math.abs(c - e) <= 10), `no ROI near ${e}: ${centers.join(',')}`);
  }
  const width = result.rois.reduce((a, r) => a + r.dx, 0) / result.rois.length;
  assert.ok(width > 36 && width < 56, `unexpected ROI width ${width}`);
  assert.ok(result.confidence >= AutoRoi.threshold, `confidence ${result.confidence}`);
});

test('finds four dials in a row', () => {
  const result = AutoRoi.detectAnalogRois(dials());
  assert.equal(result.rois.length, 4);
  const centers = result.rois.map((r) => r.x + r.dx / 2).sort((a, b) => a - b);
  for (const e of [100, 260, 420, 580]) {
    assert.ok(centers.some((c) => Math.abs(c - e) <= 14), `no dial near ${e}: ${centers.join(',')}`);
  }
  assert.ok(result.confidence >= AutoRoi.threshold, `confidence ${result.confidence}`);
});

test('rejects noise', () => {
  const digits = AutoRoi.detectDigitRois(noise());
  assert.ok(digits.confidence < AutoRoi.threshold, `digit confidence ${digits.confidence}`);
  const analog = AutoRoi.detectAnalogRois(noise());
  assert.ok(analog.confidence < AutoRoi.threshold, `analog confidence ${analog.confidence}`);
});

test('estimates a sloped digit row', () => {
  const result = AutoRoi.estimateSkew(digitStrip({ slope: 5 }));
  assert.ok(Math.abs(Math.abs(result.angle) - 5) <= 1.5, `angle ${result.angle}`);
  assert.ok(result.angle < 0, `expected clockwise content to need a negative rotation, got ${result.angle}`);
});

test('scores a well filled frame higher than a small or a cut off one', () => {
  // Same meter face, drawn at three sizes: small, filling most of the frame, cut at the border.
  const face = (size) =>
    makeImage(640, 480, (setGray) => {
      rect(setGray, 320 - size / 2, 240 - size / 2, size, size, 20);
      rect(setGray, 320 - size / 2 + 20, 240 - size / 2 + 20, size - 40, size - 40, 200);
      // Some dial-like detail inside the face so the score is not only its outline.
      for (let i = 1; i < 4; i++) {
        rect(setGray, 320 - size / 2 + i * (size / 4), 240 - size / 4, 8, size / 2, 40);
      }
    });

  const small = AutoRoi.scoreFraming(face(120)).score;
  const filling = AutoRoi.scoreFraming(face(420)).score;
  const cut = AutoRoi.scoreFraming(face(700)).score;

  assert.ok(filling > small, `filling ${filling} should beat small ${small}`);
  assert.ok(filling > cut, `filling ${filling} should beat cut ${cut}`);
});
