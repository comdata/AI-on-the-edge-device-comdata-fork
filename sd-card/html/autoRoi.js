// Local, dependency-free proposal engine for digit and analog ROIs.
// Runs in the browser on the reference image; also loaded by tools/test_auto_roi.mjs.
// All results are proposals: the user reviews them in the ROI editor before saving.

(function (root) {
  'use strict';

  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

  // RGBA image -> grayscale, downscaled so the longest side is at most maxSide.
  function toGray(image, maxSide) {
    const scale = Math.min(1, maxSide / Math.max(image.width, image.height));
    const width = Math.max(1, Math.round(image.width * scale));
    const height = Math.max(1, Math.round(image.height * scale));
    const data = new Uint8Array(width * height);
    const src = image.data;

    for (let y = 0; y < height; y++) {
      const sy = Math.min(image.height - 1, Math.floor(y / scale));
      for (let x = 0; x < width; x++) {
        const sx = Math.min(image.width - 1, Math.floor(x / scale));
        const i = (sy * image.width + sx) * 4;
        data[y * width + x] = (src[i] * 77 + src[i + 1] * 150 + src[i + 2] * 29) >> 8;
      }
    }

    return { width, height, data, scale: image.width / width };
  }

  // Absolute horizontal gradient, row and column sums, as used for digit bands.
  function horizontalEdges(gray) {
    const { width: w, height: h, data } = gray;
    const ex = new Float32Array(w * h);
    const row = new Float32Array(h);

    for (let y = 0; y < h; y++) {
      const off = y * w;
      for (let x = 1; x < w - 1; x++) {
        const v = Math.abs(data[off + x + 1] - data[off + x - 1]);
        ex[off + x] = v;
        row[y] += v;
      }
    }

    return { ex, row };
  }

  function median(values) {
    const sorted = Array.from(values).sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)] || 0;
  }

  function smooth1d(values, radius) {
    const out = new Float32Array(values.length);
    for (let i = 0; i < values.length; i++) {
      let sum = 0;
      let n = 0;
      for (let k = -radius; k <= radius; k++) {
        const j = i + k;
        if (j >= 0 && j < values.length) {
          sum += values[j];
          n++;
        }
      }
      out[i] = sum / n;
    }
    return out;
  }

  function percentile(values, p) {
    const sorted = Array.from(values).sort((a, b) => a - b);
    if (!sorted.length) return 0;
    return sorted[Math.min(sorted.length - 1, Math.floor((sorted.length - 1) * p))];
  }

  // Contiguous row segments with edge activity, the candidates for the digit band.
  function rowSegments(row, height) {
    const p60 = percentile(row, 0.6);
    const p95 = percentile(row, 0.95);
    const threshold = Math.max(1, p60 + (p95 - p60) * 0.5);
    const segments = [];
    let start = -1;

    for (let y = 0; y < height; y++) {
      if (row[y] >= threshold) {
        if (start < 0) start = y;
      } else if (start >= 0) {
        segments.push({ top: start, bottom: y - 1 });
        start = -1;
      }
    }
    if (start >= 0) segments.push({ top: start, bottom: height - 1 });

    return segments.filter((s) => {
      const h = s.bottom - s.top + 1;
      return h >= Math.max(10, Math.round(height * 0.02)) && h <= height * 0.55;
    });
  }

  // Connected dark/bright blobs inside a row band.
  function blobs(gray, top, bottom, threshold, bright) {
    const { width: w, data } = gray;
    const h = bottom - top + 1;
    const mask = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const v = data[(top + y) * w + x];
        if (bright ? v >= threshold : v <= threshold) mask[y * w + x] = 1;
      }
    }

    const comps = [];
    const stack = [];
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const start = y * w + x;
        if (!mask[start]) continue;
        let x0 = x;
        let x1 = x;
        let y0 = y;
        let y1 = y;
        let area = 0;
        stack.push(start);
        mask[start] = 0;
        while (stack.length) {
          const p = stack.pop();
          const px = p % w;
          const py = (p / w) | 0;
          area++;
          if (px < x0) x0 = px;
          if (px > x1) x1 = px;
          if (py < y0) y0 = py;
          if (py > y1) y1 = py;
          for (let dy = -1; dy <= 1; dy++) {
            const ny = py + dy;
            if (ny < 0 || ny >= h) continue;
            for (let dx = -1; dx <= 1; dx++) {
              const nx = px + dx;
              if (nx < 0 || nx >= w) continue;
              const q = ny * w + nx;
              if (mask[q]) {
                mask[q] = 0;
                stack.push(q);
              }
            }
          }
        }
        comps.push({ x0, y0: y0 + top, x1, y1: y1 + top, area });
      }
    }
    return comps;
  }

  // Digit boxes in one row band, from the blobs that form a regular row.
  function digitBoxesInBand(gray, top, bottom) {
    const h = bottom - top + 1;
    if (h < 8) return null;

    const samples = [];
    for (let y = top; y <= bottom; y++) {
      for (let x = 0; x < gray.width; x += 2) samples.push(gray.data[y * gray.width + x]);
    }
    const p05 = percentile(samples, 0.05);
    const p50 = percentile(samples, 0.5);
    const p95 = percentile(samples, 0.95);

    let best = null;
    for (const bright of [false, true]) {
      const threshold = bright ? p50 + (p95 - p50) * 0.35 : p50 - (p50 - p05) * 0.35;
      const filtered = blobs(gray, top, bottom, threshold, bright).filter((c) => {
        const cw = c.x1 - c.x0 + 1;
        const ch = c.y1 - c.y0 + 1;
        return ch >= h * 0.3 && ch <= h * 1.2 && cw >= 5 && cw <= ch * 1.2 && c.area >= 0.12 * cw * ch;
      });
      if (filtered.length < 3) continue;

      const medH = median(filtered.map((c) => c.y1 - c.y0 + 1));
      const medW = median(filtered.map((c) => c.x1 - c.x0 + 1));
      const medCy = median(filtered.map((c) => (c.y0 + c.y1) / 2));
      const row = filtered
        .filter((c) => {
          const cw = c.x1 - c.x0 + 1;
          const ch = c.y1 - c.y0 + 1;
          return (
            ch >= medH * 0.7 &&
            ch <= medH * 1.4 &&
            cw >= medW * 0.65 &&
            cw <= medW * 1.5 &&
            Math.abs((c.y0 + c.y1) / 2 - medCy) <= medH * 0.35
          );
        })
        .sort((a, b) => a.x0 - b.x0);
      if (row.length < 3) continue;

      const pitches = [];
      for (let i = 1; i < row.length; i++) pitches.push((row[i].x0 + row[i].x1 - row[i - 1].x0 - row[i - 1].x1) / 2);
      const medPitch = median(pitches.filter((p) => p > 0));

      let run = [];
      let bestRun = [];
      let prev = null;
      for (const c of row) {
        const center = (c.x0 + c.x1) / 2;
        const pitch = prev ? center - (prev.x0 + prev.x1) / 2 : 0;
        // Split off superscripts/units and unrelated blobs that do not share the digit pitch.
        if (prev && (pitch < medPitch * 0.65 || pitch > medPitch * 1.3)) {
          if (run.length > bestRun.length) bestRun = run;
          run = [];
        }
        run.push(c);
        prev = c;
      }
      if (run.length > bestRun.length) bestRun = run;
      if (bestRun.length < 3 || bestRun.length > 9) continue;

      const heights = bestRun.map((c) => c.y1 - c.y0 + 1);
      const meanH = heights.reduce((a, b) => a + b, 0) / heights.length;
      const deviation = Math.sqrt(heights.reduce((a, b) => a + (b - meanH) * (b - meanH), 0) / heights.length) || 1;
      const topY = Math.min(...bestRun.map((c) => c.y0));
      const bottomY = Math.max(...bestRun.map((c) => c.y1));
      const runH = bottomY - topY + 1;

      const rois = bestRun.map((c) => {
        const cw = c.x1 - c.x0 + 1;
        const x = Math.max(1, Math.round(c.x0 - cw * 0.12));
        return {
          x,
          y: Math.max(1, Math.round(topY - runH * 0.08)),
          dx: Math.round(cw * 1.4),
          dy: Math.round(runH * 1.2),
        };
      });

      const confidence = clamp((bestRun.length / 5) * (1 - deviation / meanH), 0, 1);
      if (!best || confidence > best.confidence) best = { rois, confidence };
    }
    return best;
  }

  function digitBands(gray, row) {
    const maxExpansion = Math.max(8, Math.round(gray.height * 0.02));
    const bands = [];
    for (const segment of rowSegments(row, gray.height)) {
      const expansion = Math.min(maxExpansion, Math.round((segment.bottom - segment.top + 1) * 0.8));
      bands.push({
        top: Math.max(0, segment.top - expansion),
        bottom: Math.min(gray.height - 1, segment.bottom + expansion),
      });
    }
    return bands;
  }

  function detectDigitRois(image) {
    const gray = toGray(image, 600);
    const edges = horizontalEdges(gray);
    const row = smooth1d(edges.row, 2);

    let best = null;
    for (const band of digitBands(gray, row)) {
      const result = digitBoxesInBand(gray, band.top, band.bottom);
      if (result && (!best || result.confidence > best.confidence)) best = result;
    }

    if (!best) return { rois: [], confidence: 0 };
    return {
      rois: best.rois.map((r) => ({
        x: Math.round(r.x * gray.scale),
        y: Math.round(r.y * gray.scale),
        dx: Math.round(r.dx * gray.scale),
        dy: Math.round(r.dy * gray.scale),
      })),
      confidence: best.confidence,
    };
  }

  // Skew of the digit band, in the same convention as the preview's canvas rotation.
  function estimateSkew(image) {
    const gray = toGray(image, 320);
    const edges = horizontalEdges(gray);
    const row = smooth1d(edges.row, 2);
    const bandResult = detectDigitBand(gray, row);
    if (!bandResult) return { angle: 0, score: 0 };

    const { width: w, height: h, data } = gray;
    const margin = Math.ceil(Math.tan((15 * Math.PI) / 180) * (w / 2)) + 3;
    const top = Math.max(0, bandResult.top - margin);
    const bottom = Math.min(h - 1, bandResult.bottom + margin);
    const ch = bottom - top + 1;
    const crop = new Uint8Array(w * ch);
    for (let y = 0; y < ch; y++) crop.set(data.subarray((top + y) * w, (top + y) * w + w), y * w);

    // Vertical-edge map of the crop: sharpest horizontal concentration when upright.
    const edgesCrop = new Float32Array(w * ch);
    for (let y = 0; y < ch; y++) {
      for (let x = 1; x < w - 1; x++) {
        const i = y * w + x;
        edgesCrop[i] = Math.abs(crop[i + 1] - crop[i - 1]);
      }
    }

    const cx = w / 2;
    const cy = ch / 2;
    let bestAngle = 0;
    let bestMetric = -1;
    let metricSum = 0;
    let metricCount = 0;
    const metrics = [];

    for (let i = 0; i <= 30; i++) {
      const metric = rotatedRowMetric(edgesCrop, w, ch, -15 + i, cx, cy);
      metrics.push(metric);
      metricSum += metric;
      metricCount++;
      if (metric > bestMetric) {
        bestMetric = metric;
        bestAngle = -15 + i;
      }
    }

    const mean = metricSum / Math.max(1, metricCount);
    let variance = 0;
    for (const m of metrics) variance += (m - mean) * (m - mean);
    variance /= Math.max(1, metricCount);
    const deviation = Math.sqrt(variance) || 1;

    // Coarse 0.1 degree refinement around the best coarse angle.
    let refined = bestAngle;
    let refinedMetric = bestMetric;
    for (let step = -5; step <= 5; step++) {
      if (step === 0) continue;
      const angle = bestAngle + step * 0.1;
      const metric = rotatedRowMetric(edgesCrop, w, ch, angle, cx, cy);
      if (metric > refinedMetric) {
        refinedMetric = metric;
        refined = angle;
      }
    }

    if (Math.abs(refined) < 0.7) refined = 0;
    return { angle: Math.round(refined * 10) / 10, score: (bestMetric - mean) / deviation };
  }

  // Rotate the edge map by angleDeg (same convention as the preview canvas) and
  // measure how sharply its vertical edges concentrate in single rows.
  function rotatedRowMetric(edgesCrop, w, ch, angleDeg, cx, cy) {
    const a = (angleDeg * Math.PI) / 180;
    const ca = Math.cos(a);
    const sa = Math.sin(a);
    const profile = new Float32Array(ch);
    for (let y = 0; y < ch; y++) {
      const dy = y - cy;
      let sum = 0;
      for (let x = 0; x < w; x++) {
        const dx = x - cx;
        const sx = Math.round(ca * dx + sa * dy + cx);
        const sy = Math.round(-sa * dx + ca * dy + cy);
        if (sx < 0 || sx >= w || sy < 0 || sy >= ch) continue;
        sum += edgesCrop[sy * w + sx];
      }
      profile[y] = sum;
    }
    let metric = 0;
    for (let y = 0; y < ch; y++) metric += profile[y] * profile[y];
    return metric;
  }

  // Band used by the skew estimator: the one where the digit row was found.
  function detectDigitBand(gray, row) {
    let best = null;
    for (const band of digitBands(gray, row)) {
      const result = digitBoxesInBand(gray, band.top, band.bottom);
      if (result && (!best || result.confidence > best.confidence)) best = { ...band, confidence: result.confidence };
    }
    return best;
  }

  function edgeMap(gray) {
    const { width: w, height: h, data } = gray;
    const gx = new Float32Array(w * h);
    const gy = new Float32Array(w * h);
    const mag = new Float32Array(w * h);
    for (let y = 1; y < h - 1; y++) {
      for (let x = 1; x < w - 1; x++) {
        const i = y * w + x;
        const dx = data[i + 1] - data[i - 1];
        const dy = data[i + w] - data[i - w];
        gx[i] = dx;
        gy[i] = dy;
        mag[i] = Math.sqrt(dx * dx + dy * dy);
      }
    }
    return { gx, gy, mag };
  }

  // Some meters mark the analog dials with a coloured pointer. Those blobs are
  // far easier to find than the faint printed dial rims, so use them when present.
  function detectPointerBlobs(image) {
    const scale = Math.min(1, 500 / Math.max(image.width, image.height));
    const width = Math.max(1, Math.round(image.width * scale));
    const height = Math.max(1, Math.round(image.height * scale));
    const data = image.data;
    const mask = new Uint8Array(width * height);

    for (let y = 0; y < height; y++) {
      const sy = Math.min(image.height - 1, Math.floor(y / scale));
      for (let x = 0; x < width; x++) {
        const sx = Math.min(image.width - 1, Math.floor(x / scale));
        const i = (sy * image.width + sx) * 4;
        const r = data[i];
        const g = data[i + 1];
        const b = data[i + 2];
        if (r > 110 && r - g > 35 && r - b > 35) mask[y * width + x] = 1;
      }
    }

    const minArea = Math.max(30, width * height * 0.00025);
    const maxSide = Math.min(width, height) * 0.25;
    const blobsFound = [];
    const stack = [];
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const start = y * width + x;
        if (!mask[start]) continue;
        let x0 = x;
        let x1 = x;
        let y0 = y;
        let y1 = y;
        let area = 0;
        let sumX = 0;
        let sumY = 0;
        stack.push(start);
        mask[start] = 0;
        while (stack.length) {
          const p = stack.pop();
          const px = p % width;
          const py = (p / width) | 0;
          area++;
          sumX += px;
          sumY += py;
          if (px < x0) x0 = px;
          if (px > x1) x1 = px;
          if (py < y0) y0 = py;
          if (py > y1) y1 = py;
          for (let dy = -1; dy <= 1; dy++) {
            const ny = py + dy;
            if (ny < 0 || ny >= height) continue;
            for (let dx = -1; dx <= 1; dx++) {
              const nx = px + dx;
              if (nx < 0 || nx >= width) continue;
              const q = ny * width + nx;
              if (mask[q]) {
                mask[q] = 0;
                stack.push(q);
              }
            }
          }
        }
        const bw = x1 - x0 + 1;
        const bh = y1 - y0 + 1;
        if (area < minArea || bw > maxSide || bh > maxSide) continue;
        if (bw / bh < 0.25 || bw / bh > 4) continue;
        blobsFound.push({ cx: sumX / area, cy: sumY / area, bw, bh, area });
      }
    }

    if (blobsFound.length < 2) return { rois: [], confidence: 0 };
    const pointerSizes = blobsFound.map((b) => Math.max(b.bw, b.bh) * 1.9);
    const side = Math.round(clamp(median(pointerSizes), 30, Math.min(width, height) * 0.5));
    const rois = blobsFound
      .sort((a, b) => a.cx - b.cx)
      .slice(0, 6)
      .map((b) => {
        const x = Math.max(1, Math.round((b.cx - side / 2) * (image.width / width)));
        const y = Math.max(1, Math.round((b.cy - side / 2) * (image.height / height)));
        const s = Math.round(side * (image.width / width));
        return { x, y, dx: s, dy: s };
      });

    return { rois, confidence: clamp(0.45 + 0.15 * rois.length, 0, 0.95) };
  }

  function detectAnalogRois(image) {
    const pointer = detectPointerBlobs(image);
    const circles = detectAnalogCircles(image);
    if (pointer.rois.length >= 2 && pointer.confidence >= circles.confidence) return pointer;
    if (circles.rois.length) return circles;
    return pointer;
  }

  function detectAnalogCircles(image) {

    const gray = toGray(image, 500);
    const { width: w, height: h } = gray;
    const { gx, gy, mag } = edgeMap(gray);

    let sum = 0;
    let sumSq = 0;
    for (let i = 0; i < mag.length; i++) {
      sum += mag[i];
      sumSq += mag[i] * mag[i];
    }
    const mean = sum / mag.length;
    const std = Math.sqrt(Math.max(0, sumSq / mag.length - mean * mean));
    const edgeThreshold = Math.max(30, mean + 1.2 * std);

    const minR = Math.max(5, Math.round(Math.min(w, h) * 0.012));
    const maxR = Math.max(minR + 2, Math.round(Math.min(w, h) * 0.09));
    const radii = [];
    for (let r = minR; r <= maxR; r += 2) radii.push(r);

    const accumulators = radii.map(() => new Float32Array(w * h));
    const edgePoints = [];
    for (let y = 1; y < h - 1; y++) {
      for (let x = 1; x < w - 1; x++) {
        const i = y * w + x;
        if (mag[i] < edgeThreshold) continue;
        const n = Math.hypot(gx[i], gy[i]) || 1;
        edgePoints.push({ x, y, ux: gx[i] / n, uy: gy[i] / n });
      }
    }
    if (!edgePoints.length) return { rois: [], confidence: 0 };

    for (let ri = 0; ri < radii.length; ri++) {
      const r = radii[ri];
      const acc = accumulators[ri];
      for (const p of edgePoints) {
        for (const sign of [1, -1]) {
          const cx = Math.round(p.x + sign * r * p.ux);
          const cy = Math.round(p.y + sign * r * p.uy);
          if (cx >= 0 && cx < w && cy >= 0 && cy < h) acc[cy * w + cx]++;
        }
      }
    }

    // Quantized gradients spread the centre votes over a few pixels, so score
    // centres with a small box sum instead of a single accumulator pixel.
    const candidates = [];
    for (let ri = 0; ri < radii.length; ri++) {
      const r = radii[ri];
      const acc = accumulators[ri];
      const box = 3;
      const integral = new Float64Array((w + 1) * (h + 1));
      for (let y = 0; y < h; y++) {
        let rowSum = 0;
        for (let x = 0; x < w; x++) {
          rowSum += acc[y * w + x];
          integral[(y + 1) * (w + 1) + x + 1] = integral[y * (w + 1) + x + 1] + rowSum;
        }
      }
      const boxSum = (x, y) => {
        const x0 = Math.max(0, x - box);
        const y0 = Math.max(0, y - box);
        const x1 = Math.min(w, x + box + 1);
        const y1 = Math.min(h, y + box + 1);
        return (
          integral[y1 * (w + 1) + x1] - integral[y0 * (w + 1) + x1] - integral[y1 * (w + 1) + x0] + integral[y0 * (w + 1) + x0]
        );
      };

      const minVotes = 0.8 * 2 * Math.PI * r;
      const local = [];
      for (let y = box + 1; y < h - box - 1; y++) {
        for (let x = box + 1; x < w - box - 1; x++) {
          const v = boxSum(x, y);
          if (v < minVotes) continue;
          if (v < boxSum(x - 2, y) || v < boxSum(x + 2, y) || v < boxSum(x, y - 2) || v < boxSum(x, y + 2)) continue;
          local.push({ x, y, r, votes: v });
        }
      }
      local.sort((a, b) => b.votes - a.votes);
      candidates.push(...local.slice(0, 12));
    }

    candidates.sort((a, b) => b.votes - a.votes);
    const picked = [];
    for (const c of candidates) {
      if (picked.length >= 24) break;
      if (picked.some((p) => Math.hypot(p.x - c.x, p.y - c.y) < Math.max(p.r, c.r) * 1.1)) continue;
      picked.push(c);
    }

    const verified = [];
    for (const c of picked) {
      const support = circleSupport(gx, gy, mag, w, h, c.x, c.y, c.r, edgeThreshold);
      if (support >= 0.45) verified.push({ ...c, support });
    }
    if (!verified.length) return { rois: [], confidence: 0 };

    // The dials of one meter are a row of circles with a similar radius.
    let bestGroup = [];
    for (const seed of verified) {
      const group = verified.filter(
        (c) => Math.abs(c.r - seed.r) <= seed.r * 0.4 && Math.abs(c.y - seed.y) <= seed.r * 1.2 && Math.abs(c.x - seed.x) <= 40 * seed.r,
      );
      if (group.length > bestGroup.length) bestGroup = group;
    }
    const group = bestGroup.length ? bestGroup : [verified[0]];

    group.sort((a, b) => a.x - b.x);
    const meanSupport = group.reduce((acc, c) => acc + c.support, 0) / group.length;

    return {
      rois: group.map((c) => {
        const side = Math.round(c.r * 2.6 * gray.scale);
        const cx = Math.round(c.x * gray.scale);
        const cy = Math.round(c.y * gray.scale);
        return {
          x: Math.max(1, cx - Math.round(side / 2)),
          y: Math.max(1, cy - Math.round(side / 2)),
          dx: side,
          dy: side,
        };
      }),
      confidence: clamp(meanSupport * (group.length >= 2 ? 1 : 0.25), 0, 1),
    };
  }

  // Fraction of the circle where an edge exists AND its gradient points radially.
  // Noise has edges but no consistent radial direction, so it scores low.
  function circleSupport(gx, gy, mag, w, h, cx, cy, r, threshold) {
    const samples = Math.max(24, Math.round(2 * Math.PI * r));
    let hits = 0;
    for (let i = 0; i < samples; i++) {
      const a = (i / samples) * 2 * Math.PI;
      const dx = Math.cos(a);
      const dy = Math.sin(a);
      let aligned = false;
      for (const rr of [r, r - 1, r + 1]) {
        const x = Math.round(cx + rr * dx);
        const y = Math.round(cy + rr * dy);
        if (x < 1 || x >= w - 1 || y < 1 || y >= h - 1) continue;
        const i0 = y * w + x;
        if (mag[i0] < threshold) continue;
        const dot = Math.abs((gx[i0] * dx + gy[i0] * dy) / mag[i0]);
        if (dot > 0.7) {
          aligned = true;
          break;
        }
      }
      if (aligned) hits++;
    }
    return hits / samples;
  }

  const AutoRoi = {
    toGray,
    estimateSkew,
    detectDigitRois,
    detectAnalogRois,
    // Confidence needed before a proposal is offered without an explicit warning.
    threshold: 0.3,
  };

  root.AutoRoi = AutoRoi;
  if (typeof module !== 'undefined' && module.exports) module.exports = AutoRoi;
})(typeof window !== 'undefined' ? window : globalThis);
