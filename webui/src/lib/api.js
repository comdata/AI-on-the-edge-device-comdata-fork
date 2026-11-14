// Small helpers for the device REST API (same origin, HTTP Basic auth is
// handled by the browser once the WebUI authentication is enabled).

export async function getJson(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`${path}: HTTP ${response.status}`);
  }
  return response.json();
}

export async function getText(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`${path}: HTTP ${response.status}`);
  }
  return response.text();
}

export async function postJson(path, body) {
  const response = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!response.ok) {
    throw new Error(`${path}: HTTP ${response.status}`);
  }
  return response.text();
}

export async function get(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`${path}: HTTP ${response.status}`);
  }
  return response.text();
}

export function toast(message, isError = false) {
  let node = document.getElementById('toast');
  if (!node) {
    node = document.createElement('div');
    node.id = 'toast';
    node.className = 'toast';
    document.body.appendChild(node);
  }
  node.textContent = message;
  node.className = `toast show${isError ? ' err' : ''}`;
  clearTimeout(node._timer);
  node._timer = setTimeout(() => {
    node.className = 'toast';
  }, isError ? 6000 : 2500);
}

export function duration(seconds) {
  const value = Number(seconds);
  if (!Number.isFinite(value) || value <= 0) return '–';
  const d = Math.floor(value / 86400);
  const h = Math.floor((value % 86400) / 3600);
  const m = Math.floor((value % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}min`;
  return `${m}min`;
}

export function sequenceValue(payload, key, sequence = 'main') {
  if (!payload) return null;
  const raw = payload[key];
  if (raw && typeof raw === 'object') return raw[sequence] ?? null;
  return raw ?? null;
}

export function setText(id, value) {
  const node = document.getElementById(id);
  if (node) node.textContent = value;
}

export function statusClass(text) {
  const value = String(text || '');
  if (value.startsWith('000') || value.toLowerCase().includes('valid')) return 'ok';
  if (value.toUpperCase().startsWith('E') || value.toLowerCase().includes('error')) return 'err';
  return 'warn';
}
