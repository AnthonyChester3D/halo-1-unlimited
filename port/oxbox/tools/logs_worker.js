// logs_worker.js: Halo 1 Unlimited's log uploads (docs/log_uploads.md), a Cloudflare Worker. Bindings: a KV namespace
// as LOGS; a secret READ_KEY (for reading them back).
//
//   POST   /v1/logs          an OXLG bundle, from a console's "Send logs": kept 30 days; answers "ok <code>"
//   GET    /v1/logs          the list (JSON), newest last           } each with the header
//   GET    /v1/logs/<name>   one bundle                             } Authorization: Bearer <READ_KEY>
//   DELETE /v1/logs/<name>   one gone                               }
//
// A bundle is checked before it's kept (its header, its size, its CRC32), so the address takes nothing else. Nothing
// about the sender is kept but its country (Cloudflare's guess): not its address.

const MOST = 8 * 1024 * 1024;            // bytes a bundle
const KEEP_SECONDS = 30 * 24 * 60 * 60;
const HEADER = 0x40;
const MOST_FILES = 64;
const CODE = /^[23456789ABCDEFGHJKLMNPQRSTUVWXYZ]{8}$/;

let table = null;
function crc32(bytes) {
  if (!table) {
    table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = table[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function answer(status, text, type = 'text/plain; charset=utf-8') {
  return new Response(text, { status, headers: { 'content-type': type, 'cache-control': 'no-store' } });
}

function authorized(request, env) {
  const want = 'Bearer ' + (env.READ_KEY || '');
  const got = request.headers.get('authorization') || '';
  if (!env.READ_KEY || env.READ_KEY.length < 16 || got.length !== want.length) return false;
  let differ = 0;
  for (let i = 0; i < want.length; i++) differ |= want.charCodeAt(i) ^ got.charCodeAt(i);
  return differ === 0;
}

// the bundle's header checked: its fields, or a reason it's refused
function check(body) {
  if (body.length < HEADER) return { refused: 'too short' };
  const view = new DataView(body.buffer, body.byteOffset, body.byteLength);
  const text = (at, n) => String.fromCharCode(...body.subarray(at, at + n));
  if (text(0, 4) !== 'OXLG' || view.getUint32(4, true) !== 1) return { refused: 'not an H1U log bundle' };
  const code = text(8, 8);
  if (!CODE.test(code)) return { refused: 'bad code' };
  const fields = {
    code,
    source: view.getUint32(0x10, true),
    build: view.getUint32(0x14, true),
    installer: view.getUint32(0x18, true),
    files: view.getUint32(0x1c, true),
  };
  if (view.getUint32(0x20, true) !== body.length) return { refused: 'wrong size' };
  if (fields.files > MOST_FILES || fields.source < 1 || fields.source > 2) return { refused: 'bad header' };
  if (crc32(body.subarray(HEADER)) !== view.getUint32(0x24, true)) return { refused: 'damaged (CRC32)' };
  return fields;
}

async function upload(request, env) {
  const length = Number(request.headers.get('content-length') || 0);
  if (!length || length > MOST) return answer(413, 'refused: too big');
  const body = new Uint8Array(await request.arrayBuffer());
  if (body.length !== length) return answer(400, 'refused: wrong size');
  const fields = check(body);
  if (fields.refused) return answer(400, 'refused: ' + fields.refused);
  const stamp = new Date().toISOString().slice(0, 19).replace(/[-:]/g, '');  // 20261123T073102
  const name = stamp + '-' + fields.code;
  await env.LOGS.put(name, body, {
    expirationTtl: KEEP_SECONDS,
    metadata: { ...fields, size: body.length, country: (request.cf && request.cf.country) || '' },
  });
  return answer(200, 'ok ' + fields.code);
}

async function list(env) {
  const all = [];
  let cursor;
  do {
    const page = await env.LOGS.list({ cursor });
    for (const k of page.keys) all.push({ name: k.name, ...(k.metadata || {}) });
    cursor = page.list_complete ? undefined : page.cursor;
  } while (cursor);
  return answer(200, JSON.stringify(all), 'application/json');
}

export default {
  async fetch(request, env) {
    const path = new URL(request.url).pathname;
    if (path === '/v1/logs' && request.method === 'POST') return upload(request, env);
    if (path !== '/v1/logs' && !path.startsWith('/v1/logs/')) return answer(404, 'nothing here');
    if (!authorized(request, env)) return answer(401, 'no');
    if (path === '/v1/logs' && request.method === 'GET') return list(env);
    const name = decodeURIComponent(path.slice('/v1/logs/'.length));
    if (!/^[0-9T]{15}-[0-9A-Z]{8}$/.test(name)) return answer(404, 'no such bundle');
    if (request.method === 'GET') {
      const data = await env.LOGS.get(name, 'arrayBuffer');
      return data ? new Response(data, { headers: { 'content-type': 'application/octet-stream' } })
        : answer(404, 'no such bundle');
    }
    if (request.method === 'DELETE') {
      await env.LOGS.delete(name);
      return answer(200, 'deleted');
    }
    return answer(405, 'no');
  },
};
