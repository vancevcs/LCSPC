"""A viewer for the Android OBB's HD textures, and manual texture replacement against PPSSPP's dumps.

    python Tools/lcstxdweb.py [--port 8642] [--no-browser]

The browser-pane companion to Tools/lcstxd.py (the desktop tool ported from the VCS fork, which
also scores the PC upscale packs). Both write the same manual block through lcshd.set_manual.

Serves a page on http://127.0.0.1:8642 and opens it. Two tabs:

Android HD   every HD texture in the OBB (Textures_ETC/<txd>/<texture>.pvr, 9,178 of them), by TXD,
             searchable by name. One shows its size, format and mips, saves as a full-size PNG,
             lists the PSP textures bound to it, and ranks the dumps that look like it.
PSP dumps    every texture PPSSPP has dumped (TEXTURES/ULUS10041/new) and what textures.ini binds
             it to. One ranks the HD textures against it with lcshd's matcher, or finds them by
             name, and binds the one picked:

Bind         the HD texture, colour-matched to the dump as lcshd bind does it, goes to android/ as
             DDS (texconv; PNG without it), and its line to the ini's manual block, replacing an
             Android binding of the same hash.
No HD        an empty line in the manual block: PPSSPP reads it as "no replacement", and lcshd bind
             as taken, so a wrong automatic match never comes back.
Clear        takes the hash's line out; lcshd bind may bind it again on its next run.

The hand-made pack's block (texturereplacements) is not touched here: change that pack and re-run
lcshd merge. PPSSPP rereads textures.ini on any graphics config change: flip Texture quality LOW
and back to HIGH in the pause menu to see a bind in a running game.

Needs lcshd index (hd/android.npz). Listens on 127.0.0.1 only. Thumbnails are cached in
hd/thumbs/, read from the PVRs' own mip levels.
"""

import argparse
import io
import json
import os
import re
import sys
import threading
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcshd  # noqa: E402
import lcswad  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PACK = os.path.join(os.path.dirname(HERE), 'memstick', 'PSP', 'TEXTURES', 'ULUS10041')
THUMB = 128               # the smallest mip level at least this big is the thumbnail
PREVIEW = 256
CANDIDATES = 16           # distinct pictures shown per dump
MANUAL = lcshd.MANUAL     # the ini block the hand-picking tools own
DUMP_RE = re.compile(r'^[0-9a-f]{24}$')
HEX_RE = re.compile(r'^[0-9a-f]{8}$')
MARK_RE = lcshd.MARK_RE


class Store:
    """Everything the page reads: the Android index, the OBB, the dumps and the pack's ini."""

    def __init__(self, work, pack, dump):
        self.work, self.pack, self.dump = work, pack, dump
        self.android = lcshd.load_android(work)
        self.keys = [str(k) for k in self.android['keys']]
        self.by_key = {k.lower(): i for i, k in enumerate(self.keys)}
        self.by_hash = {int(h): i for i, h in enumerate(self.android['hashes'])}
        self.arch = lcshd._open_arch(work)
        self.arch_lock = threading.Lock()   # one file handle per WAD, seek + read
        self.ini_lock = threading.Lock()
        self.dump_lock = threading.Lock()
        self.thumbs = os.path.join(work, 'thumbs')
        os.makedirs(self.thumbs, exist_ok=True)
        self.texconv = lcshd.find_texconv()
        self.matches = {}
        path = os.path.join(work, 'matches.csv')
        if os.path.exists(path):
            self.matches = {r['dump']: r for r in lcshd.read_matches(work)}
        self.dumps = {}
        self.dump_arrays = None
        self.refresh_dumps()

    # --- dumps ---

    def refresh_dumps(self):
        """Tops up the dump signatures (lcshd's cache) when PPSSPP has dumped more."""
        with self.dump_lock:
            names = [f for f in os.listdir(self.dump) if f.lower().endswith('.png')] if os.path.isdir(self.dump) else []
            if len(names) == len(self.dumps) and self.dump_arrays is not None:
                return
            self.dumps = lcshd.load_dump(self.dump, self.work) if names else {}
            hs = list(self.dumps)
            if hs:
                f = [self.dumps[h] for h in hs]
                self.dump_arrays = dict(hashes=hs, vectors=np.stack([x[1] for x in f]),
                                        alphas=np.array([x[2] for x in f]), means=np.stack([x[3] for x in f]),
                                        fine=np.stack([x[5] for x in f]))
            else:
                self.dump_arrays = dict(hashes=[])

    # --- the OBB ---

    def pvr_header(self, i):
        w, e = self.arch.by_hash[int(self.android['hashes'][i])]
        with self.arch_lock:
            return lcswad.pvr_header(w.read(e, 52)), (w, e)

    def decode(self, i, level=0):
        hd, (w, e) = self.pvr_header(i)
        offset, size, lw, lh = lcswad.pvr_level(hd, level)
        with self.arch_lock:
            body = w.read(e, size, offset)
        return Image.fromarray(lcswad.decode_level(hd, body, lw, lh), 'RGBA')

    def thumb(self, i):
        h = int(self.android['hashes'][i])
        path = os.path.join(self.thumbs, '%08x.png' % h)
        if not os.path.exists(path):
            hd, _ = self.pvr_header(i)
            level = 0
            while level < hd['mips'] and max(hd['width'], hd['height']) >> (level + 1) >= THUMB:
                level += 1
            self.decode(i, level).save(path)
        with open(path, 'rb') as f:
            return f.read()

    # --- the ini ---

    def ini_path(self):
        return os.path.join(self.pack, 'textures.ini')

    def read_ini(self):
        p = self.ini_path()
        if not os.path.exists(p):
            return lcshd.DEFAULT_INI_HEAD.splitlines()
        return open(p, encoding='utf-8', errors='replace').read().splitlines()

    def bindings(self):
        """{dump hash: (status, value)} from [hashes]. status: handmade (a merged pack's block),
        manual, auto (an Android binding lcshd made), nohd (an empty line), other."""
        out = {}
        section, block = '', None
        for line in self.read_ini():
            t = line.strip()
            m = MARK_RE.match(t)
            if m:
                block = m.group(2) if m.group(1) == 'begin' else None
                continue
            if t.startswith('['):
                section = t.lower()
                continue
            if section != '[hashes]' or t.startswith('#') or '=' not in t:
                continue
            k, v = (x.strip() for x in t.split('=', 1))
            k = k.lower()
            if block and block != MANUAL:
                status = 'handmade'
            elif not v:
                status = 'nohd'
            elif block == MANUAL:
                status = 'manual'
            elif v.lower().startswith(lcshd.PACK_SUBDIR + '/'):
                status = 'auto'
            else:
                status = 'other'
            out[k] = (status, v)
        return out

    def android_of(self, value):
        """The HD texture a pack file was made from: android/<key>[~n].dds -> index, or None."""
        v = value.replace('\\', '/').lower()
        if not v.startswith(lcshd.PACK_SUBDIR + '/'):
            return None
        key = os.path.splitext(v[len(lcshd.PACK_SUBDIR) + 1:])[0].split('~')[0]
        return self.by_key.get(key)

    # --- actions ---

    def bind(self, dump, i):
        with self.ini_lock:
            current = self.bindings().get(dump)
            if current and current[0] == 'handmade':
                raise ValueError('the hand-made pack has this hash; change it there and re-run lcshd merge')
            with Image.open(os.path.join(self.dump, dump + '.png')) as d:
                image, gains = lcshd.colour_matched(self.decode(i), d)
            key = self.keys[i].lower()
            rel = '%s/%s~%s.png' % (lcshd.PACK_SUBDIR, key, dump[-8:])
            path = os.path.join(self.pack, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            image.save(path)
            if self.texconv and lcshd.png_to_dds(self.texconv, path):
                rel = rel[:-4] + '.dds'
            old, refused = lcshd.set_manual(self.pack, {dump: rel}, {dump: '<- %s (android)' % self.keys[i]})
            if refused:
                raise ValueError('the hand-made pack has this hash; change it there and re-run lcshd merge')
            if old[dump] != rel:
                lcshd.drop_unused(self.pack, [old[dump]])
            return dict(file=rel, gains=[round(float(g), 3) for g in gains])

    def set_line(self, dump, value):
        with self.ini_lock:
            current = self.bindings().get(dump)
            if current and current[0] == 'handmade':
                raise ValueError('the hand-made pack has this hash; change it there and re-run lcshd merge')
            old, refused = lcshd.set_manual(self.pack, {dump: value})
            if refused:
                raise ValueError('the hand-made pack has this hash; change it there and re-run lcshd merge')
            lcshd.drop_unused(self.pack, [old[dump]])
            return dict(file=value)

    # --- ranking ---

    def candidates(self, dump, n=CANDIDATES):
        size, v, alpha, mean, _std, fine = self.dumps[dump]
        ranked = lcshd.rank(self.android, v, alpha, mean, fine, top=n * 4)
        picked = []
        for j, d in ranked:
            if any(lcshd.same_picture(self.android, j, k) for k, _ in picked):
                continue
            picked.append((j, d))
            if len(picked) == n:
                break
        return picked

    def dumps_like(self, i, n=12):
        a = self.android
        if not self.dump_arrays['hashes']:
            return []
        ranked = lcshd.rank(self.dump_arrays, a['vectors'][i], a['alphas'][i], a['means'][i], a['fine'][i], top=n)
        return [(self.dump_arrays['hashes'][j], d) for j, d in ranked]


def android_row(store, i, bound):
    a = store.android
    w, h = (int(x) for x in a['sizes'][i])
    return [i, store.keys[i], '%08x' % int(a['hashes'][i]), w, h, bool(a['alphas'][i]), bound.get(i, 0)]


def png_bytes(img):
    b = io.BytesIO()
    img.save(b, 'PNG')
    return b.getvalue()


def as_drawn(hd, dump_size, box):
    """The HD texture in the dump's shape, at most `box` on a side: PPSSPP stretches a replacement
    over the original's UV space whatever its size, so a square 256x256 bound to a 32x64 draws as
    a 1:2 rectangle. The Android art for a 2:1 PSP texture is that layout on a square canvas."""
    w, h = dump_size
    s = box / max(w, h)
    return hd.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)


class Handler(BaseHTTPRequestHandler):
    store = None

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype, extra=None):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj, code=200):
        self.send(code, json.dumps(obj).encode(), 'application/json')

    def fail(self, code, msg):
        self.json({'error': msg}, code)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        p = u.path
        s = self.store
        try:
            if p == '/':
                return self.send(200, PAGE.encode(), 'text/html; charset=utf-8')
            if p == '/api/android':
                bound = {}
                for status, v in s.bindings().values():
                    i = s.android_of(v) if status in ('auto', 'manual') else None
                    if i is not None:
                        bound[i] = bound.get(i, 0) + 1
                return self.json([android_row(s, i, bound) for i in range(len(s.keys))])
            if p == '/api/dumps':
                s.refresh_dumps()
                b = s.bindings()
                rows = []
                for h, (size, *_r) in s.dumps.items():
                    status, value = b.get(h, ('none', ''))
                    m = s.matches.get(h, {})
                    rows.append([h, int(size[0]), int(size[1]), status, value, m.get('tier', ''),
                                 float(m['distance']) if m else None, s.android_of(value) if value else None])
                return self.json(dict(rows=rows, dir=s.dump, texconv=bool(s.texconv)))
            m = re.match(r'^/api/dump/([0-9a-f]{24})$', p)
            if m:
                h = m.group(1)
                s.refresh_dumps()
                if h not in s.dumps:
                    return self.fail(404, 'no such dump')
                status, value = s.bindings().get(h, ('none', ''))
                return self.json(dict(hash=h, status=status, value=value, android=s.android_of(value) if value else None,
                                      match=s.matches.get(h),
                                      candidates=[[int(j), round(d, 4)] for j, d in s.candidates(h)]))
            m = re.match(r'^/api/android/(\d+)$', p)
            if m:
                i = int(m.group(1))
                if i >= len(s.keys):
                    return self.fail(404, 'no such texture')
                hd, _ = s.pvr_header(i)
                bound = [h for h, (st, v) in s.bindings().items() if st in ('auto', 'manual') and s.android_of(v) == i]
                s.refresh_dumps()
                return self.json(dict(i=i, key=s.keys[i], hash='%08x' % int(s.android['hashes'][i]),
                                      width=hd['width'], height=hd['height'], mips=hd['mips'],
                                      format={lcswad.PVR_ETC1: 'ETC1', lcswad.PVR_RGBA4444: 'RGBA4444'}.get(hd['format'], hex(hd['format'])),
                                      bound=bound, like=[[h, round(d, 4)] for h, d in s.dumps_like(i)]))
            m = re.match(r'^/img/a/(\d+)$', p)
            if m:
                i = int(m.group(1))
                if i >= len(s.keys):
                    return self.fail(404, 'no such texture')
                if q.get('full'):
                    extra = {}
                    if q.get('dl'):
                        name = s.keys[i].replace('/', '_') + '.png'
                        extra['Content-Disposition'] = 'attachment; filename="%s"' % name
                    return self.send(200, png_bytes(s.decode(i)), 'image/png', extra)
                return self.send(200, s.thumb(i), 'image/png', {'Cache-Control': 'max-age=86400'})
            m = re.match(r'^/img/d/([0-9a-f]{24})\.png$', p)
            if m:
                path = os.path.join(s.dump, m.group(1) + '.png')
                if not os.path.exists(path):
                    return self.fail(404, 'no such dump')
                with open(path, 'rb') as f:
                    return self.send(200, f.read(), 'image/png', {'Cache-Control': 'max-age=86400'})
            if p == '/img/preview':
                h, i = q.get('dump', ''), int(q.get('a', '-1'))
                if not DUMP_RE.match(h) or not 0 <= i < len(s.keys):
                    return self.fail(400, 'dump and a')
                with Image.open(os.path.join(s.dump, h + '.png')) as d:
                    img, _g = lcshd.colour_matched(s.decode(i), d)
                    size = d.size
                return self.send(200, png_bytes(as_drawn(img, size, PREVIEW)), 'image/png', {'Cache-Control': 'max-age=3600'})
            if p == '/img/pack':
                rel = q.get('f', '')
                path = os.path.normpath(os.path.join(s.pack, rel))
                if (not path.startswith(os.path.normpath(s.pack) + os.sep) or not rel.lower().endswith('.png')
                        or not os.path.exists(path)):
                    return self.fail(404, 'not a PNG in the pack')
                with open(path, 'rb') as f:
                    return self.send(200, f.read(), 'image/png')
            return self.fail(404, 'not found')
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # noqa: BLE001 - the page shows it
            self.fail(500, '%s: %s' % (type(e).__name__, e))

    def do_POST(self):
        # The custom header can't be sent cross-origin without a preflight this server never
        # answers, so another site in the browser can't bind or clear anything.
        if self.headers.get('X-LCS') != '1':
            return self.fail(403, 'missing X-LCS header')
        s = self.store
        try:
            n = int(self.headers.get('Content-Length', '0'))
            body = json.loads(self.rfile.read(n) or b'{}')
            h = str(body.get('dump', ''))
            if not DUMP_RE.match(h) or h not in s.dumps:
                return self.fail(400, 'unknown dump')
            if self.path == '/api/bind':
                i = int(body.get('a', -1))
                if not 0 <= i < len(s.keys):
                    return self.fail(400, 'unknown HD texture')
                return self.json(s.bind(h, i))
            if self.path == '/api/nohd':
                return self.json(s.set_line(h, ''))
            if self.path == '/api/clear':
                return self.json(s.set_line(h, None))
            return self.fail(404, 'not found')
        except ValueError as e:
            self.fail(409, str(e))
        except Exception as e:  # noqa: BLE001
            self.fail(500, '%s: %s' % (type(e).__name__, e))


PAGE = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LCS Textures</title>
<style>
:root{--bg:#141619;--panel:#1c1f24;--line:#2b3038;--text:#e3e6ea;--dim:#8b939e;--accent:#e8a33d;
--auto:#4f8fdb;--manual:#3fb07a;--handmade:#a77be0;--nohd:#d0605a;--other:#8b939e;
--chk1:#3a3d42;--chk2:#2c2f33}
*{box-sizing:border-box}html,body{height:100%;margin:0}
body{background:var(--bg);color:var(--text);font:13px/1.4 system-ui,Segoe UI,sans-serif;display:flex;flex-direction:column}
header{display:flex;gap:12px;align-items:center;padding:8px 14px;border-bottom:1px solid var(--line);background:var(--panel);flex-wrap:wrap}
header h1{font-size:15px;margin:0 8px 0 0;font-weight:600;letter-spacing:.02em}
.tabs button,.chip,button.act{background:transparent;border:1px solid var(--line);color:var(--text);padding:5px 11px;border-radius:6px;cursor:pointer;font:inherit}
.tabs button.on{background:var(--accent);border-color:var(--accent);color:#1a1205;font-weight:600}
input[type=search]{background:var(--bg);border:1px solid var(--line);color:var(--text);padding:6px 10px;border-radius:6px;font:inherit;min-width:240px}
select{background:var(--bg);border:1px solid var(--line);color:var(--text);padding:5px;border-radius:6px;font:inherit}
.hint{color:var(--dim);font-size:12px}
main{flex:1;display:flex;min-height:0}
aside{width:230px;border-right:1px solid var(--line);overflow:auto;padding:8px 0;flex-shrink:0}
aside .row{display:flex;justify-content:space-between;padding:3px 14px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
aside .row:hover{background:var(--panel)}aside .row.on{background:#2a2f37;color:var(--accent)}
aside .row span:last-child{color:var(--dim);margin-left:8px}
aside h4{margin:12px 14px 4px;color:var(--dim);font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.06em}
#grid{flex:1;overflow:auto;padding:12px;align-content:start}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:10px}
.tile{background:var(--panel);border:1px solid var(--line);border-radius:6px;cursor:pointer;overflow:hidden;position:relative}
.tile:hover{border-color:#4a515c}.tile.on{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
.chk{background-color:var(--chk2);background-image:conic-gradient(var(--chk1) 25%,transparent 0 50%,var(--chk1) 0 75%,transparent 0);background-size:16px 16px}
.tile .im{aspect-ratio:1;display:flex;align-items:center;justify-content:center}
.tile img{width:100%;height:100%;object-fit:contain}
.tile .cap{padding:4px 6px;font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;color:var(--dim)}
.tile .cap b{color:var(--text);font-weight:500}
.badge{position:absolute;top:5px;left:5px;font-size:10px;padding:1px 6px;border-radius:9px;font-weight:600;color:#fff}
.b-auto{background:var(--auto)}.b-manual{background:var(--manual)}.b-handmade{background:var(--handmade)}.b-nohd{background:var(--nohd)}.b-other{background:var(--other)}
.dot{position:absolute;top:6px;right:6px;width:8px;height:8px;border-radius:50%;background:var(--manual)}
#detail{width:460px;border-left:1px solid var(--line);overflow:auto;padding:14px;flex-shrink:0;background:var(--panel)}
#detail h2{font-size:14px;margin:0 0 2px;word-break:break-all}
#detail h3{font-size:12px;margin:18px 0 8px;color:var(--dim);font-weight:500;text-transform:uppercase;letter-spacing:.06em}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}
.pair figure,.big{margin:0}
.pair .frame,.big .frame{aspect-ratio:1;display:flex;align-items:center;justify-content:center;border:1px solid var(--line);border-radius:6px;overflow:hidden}
.pair img,.big img{width:100%;height:100%;object-fit:contain}
.pix{image-rendering:pixelated}
figcaption{font-size:11px;color:var(--dim);margin-top:4px}
.actions{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
button.act.primary{background:var(--manual);border-color:var(--manual);color:#04150d;font-weight:600}
button.act:disabled{opacity:.4;cursor:default}
.pill{display:inline-block;padding:1px 8px;border-radius:9px;font-size:11px;font-weight:600;color:#fff}
.kv{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;margin-top:8px;font-size:12px}.kv span:nth-child(odd){color:var(--dim)}
.tiles.small{grid-template-columns:repeat(4,1fr);gap:8px}
.tiles.small .cap{font-size:10px}
.msg{margin-top:10px;padding:8px 10px;border-radius:6px;background:#22262c;font-size:12px}
.msg.err{background:#3a1f1f;color:#f0b4b0}
a{color:var(--accent)}
.more{margin:14px auto;display:block}
.empty{color:var(--dim);padding:40px;text-align:center}
@media (max-width:900px){main{flex-direction:column}aside{width:auto;max-height:140px;border-right:0;border-bottom:1px solid var(--line)}#detail{width:auto;border-left:0;border-top:1px solid var(--line)}}
</style></head><body>
<header>
  <h1>LCS textures</h1>
  <div class="tabs"><button data-tab="dumps">PSP dumps</button> <button data-tab="android">Android HD</button></div>
  <input type="search" id="q" placeholder="Search">
  <select id="sort"></select>
  <span class="hint" id="info"></span>
</header>
<main><aside id="side"></aside><section id="grid"></section><section id="detail"><div class="empty">Pick a texture.</div></section></main>
<script>
const S={tab:'dumps',android:[],dumps:[],group:null,q:'',limit:400,sel:null,cand:null,dumpInfo:null};
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const LABEL={auto:'Android',manual:'Manual',handmade:'Hand-made',nohd:'No HD',other:'Other'};
async function get(u){const r=await fetch(u);const j=await r.json();if(!r.ok)throw new Error(j.error||r.status);return j}
async function post(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json','X-LCS':'1'},body:JSON.stringify(b)});const j=await r.json();if(!r.ok)throw new Error(j.error||r.status);return j}
function tile(img,cap,o={}){
  return `<div class="tile${o.on?' on':''}" data-k="${esc(o.k)}" title="${esc(o.title||'')}">${o.badge?`<span class="badge b-${o.badge}">${LABEL[o.badge]}</span>`:''}${o.dot?'<span class="dot"></span>':''}<div class="im chk"><img loading="lazy" src="${img}" class="${o.pix?'pix':''}"></div><div class="cap">${cap}</div></div>`}
// ---- data
async function loadDumps(){const d=await get('/api/dumps');S.dumps=d.rows;S.texconv=d.texconv;S.dumpDir=d.dir}
async function loadAndroid(){S.android=await get('/api/android')}
const A=i=>S.android[i];
// ---- side + grid
function setSort(){
  const o=S.tab==='dumps'?[['dist','Best match first'],['size','Largest first'],['hash','By hash']]:[['name','By name'],['size','Largest first']];
  $('sort').innerHTML=o.map(([v,l])=>`<option value="${v}">${l}</option>`).join('');
}
function dumpFilter(r){
  const g=S.group||'all';
  if(g==='all')return true; if(g==='none')return r[3]==='none';
  if(g.startsWith('tier:'))return r[3]==='none'&&r[5]===g.slice(5);
  return r[3]===g}
function renderSide(){
  let h='';
  if(S.tab==='dumps'){
    const c={all:S.dumps.length,none:0,auto:0,manual:0,handmade:0,nohd:0,other:0,'tier:likely':0,'tier:review':0,'tier:sure':0};
    for(const r of S.dumps){c[r[3]]++; if(r[3]==='none'&&r[5])c['tier:'+r[5]]++}
    const row=(k,l)=>`<div class="row${(S.group||'all')===k?' on':''}" data-g="${k}"><span>${l}</span><span>${c[k]||0}</span></div>`;
    h='<h4>Status</h4>'+row('all','All dumps')+row('none','Not replaced')+row('auto','Android (auto)')+row('manual','Manual')+row('handmade','Hand-made pack')+row('nohd','No HD')+(c.other?row('other','Other'):'')+
      '<h4>Not replaced, by matcher tier</h4>'+row('tier:likely','Likely')+row('tier:review','Review')+row('tier:sure','Sure (unbound)');
  }else{
    const g={};for(const r of S.android){const t=r[1].split('/')[0];g[t]=(g[t]||0)+1}
    const names=Object.keys(g).sort((a,b)=>(a.startsWith('folder')-b.startsWith('folder'))||a.localeCompare(b));
    const row=(k,l,n)=>`<div class="row${S.group===k?' on':''}" data-g="${esc(k)}"><span>${esc(l)}</span><span>${n}</span></div>`;
    h=row('','All textures',S.android.length)+'<h4>Named TXDs</h4>'+names.filter(n=>!n.startsWith('folder')).map(n=>row(n,n,g[n])).join('')+
      '<h4>Unnamed (world, by folder)</h4>'+names.filter(n=>n.startsWith('folder')).map(n=>row(n,n,g[n])).join('');
  }
  $('side').innerHTML=h;
  for(const e of $('side').querySelectorAll('.row'))e.onclick=()=>{S.group=e.dataset.g||null;S.limit=400;renderSide();renderGrid()};
}
function renderGrid(){
  const q=S.q.toLowerCase();let items,html;
  if(S.tab==='dumps'){
    items=S.dumps.filter(r=>dumpFilter(r)&&(!q||r[0].includes(q)||(r[4]||'').toLowerCase().includes(q)||(r[7]!=null&&A(r[7])&&A(r[7])[1].toLowerCase().includes(q))));
    const so=$('sort').value;
    items.sort(so==='size'?(a,b)=>b[1]*b[2]-a[1]*a[2]:so==='hash'?(a,b)=>a[0]<b[0]?-1:1:(a,b)=>(a[6]??9)-(b[6]??9));
    html=items.slice(0,S.limit).map(r=>tile(`/img/d/${r[0]}.png`,`<b>${r[1]}×${r[2]}</b> ${r[5]&&r[3]==='none'?r[5]:''}`,{k:r[0],badge:r[3]!=='none'?r[3]:null,on:S.sel===r[0],pix:true,title:r[0]+(r[4]?' → '+r[4]:'')})).join('');
  }else{
    items=S.android.filter(r=>(!S.group||r[1].split('/')[0]===S.group)&&(!q||r[1].toLowerCase().includes(q)||r[2].includes(q)));
    if($('sort').value==='size')items.sort((a,b)=>b[3]*b[4]-a[3]*a[4]);
    html=items.slice(0,S.limit).map(r=>tile(`/img/a/${r[0]}`,`<b>${esc(r[1].split('/').slice(1).join('/'))}</b><br>${esc(r[1].split('/')[0])} · ${r[3]}×${r[4]}`,{k:r[0],dot:r[6]>0,on:S.sel===r[0],title:r[1]+(r[6]?` (bound to ${r[6]} PSP texture${r[6]>1?'s':''})`:'')})).join('');
  }
  $('grid').innerHTML=items.length?`<div class="tiles">${html}</div>`+(items.length>S.limit?`<button class="act more" id="more">Show more (${items.length-S.limit} left)</button>`:''):'<div class="empty">Nothing matches.</div>';
  $('info').textContent=`${items.length} shown`+(S.tab==='dumps'?` · dump folder ${S.dumpDir}`:'');
  for(const e of $('grid').querySelectorAll('.tile'))e.onclick=()=>open(S.tab,e.dataset.k);
  if($('more'))$('more').onclick=()=>{S.limit+=400;renderGrid()};
}
function markSel(){for(const e of $('grid').querySelectorAll('.tile'))e.classList.toggle('on',e.dataset.k==String(S.sel))}
// ---- detail
function open(kind,k){
  if(kind!==S.tab){S.tab=kind;S.group=null;S.q='';$('q').value='';setTabs();setSort();renderSide();renderGrid()}
  S.sel=kind==='dumps'?k:+k;markSel();
  return kind==='dumps'?openDump(k):openAndroid(+k)}
async function openDump(h,preselect){
  $('detail').innerHTML='<div class="empty">Ranking…</div>';
  const d=await get('/api/dump/'+h);S.dumpInfo=d;S.cand=preselect??null;renderDump()}
function renderDump(msg){
  const d=S.dumpInfo,h=d.hash,r=S.dumps.find(x=>x[0]===h)||[h,0,0];
  let cur='';
  if(d.status==='handmade')cur=`<img src="/img/pack?f=${encodeURIComponent(d.value)}">`;
  else if(d.android!=null)cur=`<img src="/img/preview?dump=${h}&a=${d.android}">`;
  const showing=S.cand!=null?`<img src="/img/preview?dump=${h}&a=${S.cand}">`:cur;
  const capR=S.cand!=null?`Picked: ${esc(A(S.cand)[1])} (${A(S.cand)[3]}×${A(S.cand)[4]}), colour-matched and in the PSP texture's shape, as it will draw`:
    d.status==='none'?'Not replaced':d.status==='nohd'?'Marked "no HD"':d.status==='handmade'?'Hand-made pack: '+esc(d.value):`Bound: ${esc(d.value)}`;
  const m=d.match;
  const cands=d.candidates.map(([j,dist])=>tile(`/img/a/${j}`,`<b>${dist.toFixed(3)}</b> ${esc(A(j)[1])}`,{k:j,on:S.cand===j,title:A(j)[1]})).join('');
  $('detail').innerHTML=`<h2>${h}</h2>
   <div>${d.status!=='none'?`<span class="pill b-${d.status}">${LABEL[d.status]}</span> `:''}<span class="hint">${r[1]}×${r[2]}${m?` · matcher: ${m.tier}, distance ${m.distance}, margin ${m.margin}`:''}</span></div>
   <div class="pair"><figure><div class="frame chk"><img class="pix" src="/img/d/${h}.png"></div><figcaption>PSP dump ${r[1]}×${r[2]}</figcaption></figure>
   <figure><div class="frame chk">${showing||'<span class="hint">no HD</span>'}</div><figcaption>${capR}</figcaption></figure></div>
   <div class="actions">${d.status==='handmade'?'<span class="hint">The hand-made pack owns this hash. Change it in texturereplacements and re-run lcshd merge.</span>':
     `<button class="act primary" id="bind" ${S.cand==null?'disabled':''}>Bind picked HD</button><button class="act" id="nohd">No HD</button><button class="act" id="clear" ${d.status==='none'?'disabled':''}>Clear</button>`}</div>
   ${msg?`<div class="msg${msg.err?' err':''}">${msg.text}</div>`:''}
   <h3>Closest HD textures</h3><div class="tiles small" id="cands">${cands}</div>
   <h3>Find by name</h3><input type="search" id="aq" placeholder="txd or texture name, e.g. fatmale01" style="width:100%">
   <div class="tiles small" id="ares" style="margin-top:8px"></div>
   <p class="hint" style="margin-top:16px">The right-hand picture is squeezed to the PSP texture's shape: PPSSPP stretches a replacement over the original whatever its size, so square Android art for a 2:1 PSP texture draws right. The file itself stays at the Android size. A bind writes android/&lt;txd&gt;/&lt;texture&gt;~${h.slice(-8)}.${S.texconv?'dds':'png'} and a line in textures.ini's manual block. In a running game, flip Texture quality to LOW and back to HIGH in the pause menu to reload.</p>`;
  const pick=e=>{S.cand=+e.dataset.k;renderDump()};
  for(const e of $('cands').querySelectorAll('.tile'))e.onclick=()=>pick(e);
  const aq=$('aq');aq.oninput=()=>{const q=aq.value.trim().toLowerCase();if(q.length<2){$('ares').innerHTML='';return}
    const res=S.android.filter(x=>x[1].toLowerCase().includes(q)).slice(0,40);
    $('ares').innerHTML=res.map(x=>tile(`/img/a/${x[0]}`,esc(x[1]),{k:x[0],on:S.cand===x[0],title:x[1]})).join('')||'<span class="hint">none</span>';
    for(const e of $('ares').querySelectorAll('.tile'))e.onclick=()=>pick(e)};
  const act=async(fn)=>{try{const out=await fn();await loadDumps();await loadAndroid();const keep=S.cand;S.dumpInfo=await get('/api/dump/'+h);S.cand=null;renderSide();renderGridKeep();renderDump({text:out})}catch(e){renderDump({text:esc(e.message),err:1})}};
  if($('bind'))$('bind').onclick=()=>act(async()=>{const o=await post('/api/bind',{dump:h,a:S.cand});return `Bound to ${esc(o.file)} (gains ${o.gains.join(', ')}).`});
  if($('nohd'))$('nohd').onclick=()=>act(async()=>{await post('/api/nohd',{dump:h});return 'Marked "no HD": PPSSPP keeps the PSP texture and lcshd bind leaves it alone.'});
  if($('clear'))$('clear').onclick=()=>act(async()=>{await post('/api/clear',{dump:h});return 'Cleared. lcshd bind may bind it again on its next run.'});
}
function renderGridKeep(){const y=$('grid').scrollTop;renderGrid();$('grid').scrollTop=y}
async function openAndroid(i){
  $('detail').innerHTML='<div class="empty">Loading…</div>';
  const d=await get('/api/android/'+i);
  const bound=d.bound.map(h=>{const r=S.dumps.find(x=>x[0]===h);return tile(`/img/d/${h}.png`,r?`${r[1]}×${r[2]}`:h.slice(-8),{k:h,pix:true,title:h})}).join('');
  const like=d.like.map(([h,dist])=>{const r=S.dumps.find(x=>x[0]===h);return tile(`/img/d/${h}.png`,`<b>${dist.toFixed(3)}</b> ${r?r[1]+'×'+r[2]:''}`,{k:h,pix:true,badge:r&&r[3]!=='none'?r[3]:null,title:h})}).join('');
  $('detail').innerHTML=`<h2>${esc(d.key)}</h2>
   <div class="big" style="margin-top:10px"><div class="frame chk"><img src="/img/a/${i}?full=1"></div></div>
   <div class="kv"><span>Size</span><span>${d.width}×${d.height}, ${d.mips} mips below</span><span>Format</span><span>${d.format}</span><span>OBB hash</span><span>${d.hash}</span><span>Path</span><span>Textures_ETC/${esc(d.key)}.pvr</span></div>
   <div class="actions"><a class="act" href="/img/a/${i}?full=1&dl=1" style="text-decoration:none"><button class="act">Save PNG</button></a></div>
   <h3>Bound to (${d.bound.length})</h3><div class="tiles small" id="bnd">${bound||'<span class="hint">no PSP texture yet</span>'}</div>
   <h3>PSP dumps that look like it</h3><div class="tiles small" id="like">${like||'<span class="hint">no dumps</span>'}</div>
   <p class="hint">Click a dump to bind this texture to it.</p>`;
  for(const e of $('detail').querySelectorAll('#bnd .tile,#like .tile'))e.onclick=()=>{const h=e.dataset.k;S.tab='dumps';S.group=null;S.q='';$('q').value='';setTabs();setSort();renderSide();renderGrid();S.sel=h;markSel();openDump(h,i)};
}
// ---- chrome
function setTabs(){for(const b of document.querySelectorAll('.tabs button'))b.classList.toggle('on',b.dataset.tab===S.tab)}
for(const b of document.querySelectorAll('.tabs button'))b.onclick=()=>{S.tab=b.dataset.tab;S.group=null;S.limit=400;setTabs();setSort();renderSide();renderGrid()};
$('q').oninput=()=>{S.q=$('q').value.trim();S.limit=400;renderGrid()};
$('sort').onchange=()=>renderGrid();
(async()=>{setTabs();setSort();$('grid').innerHTML='<div class="empty">Loading the index…</div>';
  try{await Promise.all([loadDumps(),loadAndroid()]);renderSide();renderGrid()}catch(e){$('grid').innerHTML=`<div class="empty">${esc(e.message)}</div>`}})();
</script></body></html>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', default=lcshd.DEFAULT_WORK)
    ap.add_argument('--pack', default=DEFAULT_PACK, help="PPSSPP's TEXTURES/ULUS10041")
    ap.add_argument('--dump', help="PPSSPP's dump folder (default: <pack>/new)")
    ap.add_argument('--port', type=int, default=8642)
    ap.add_argument('--no-browser', action='store_true')
    args = ap.parse_args()
    if not os.path.exists(os.path.join(args.work, 'android.npz')):
        sys.exit('no %s: run "python Tools/lcshd.py index" first' % os.path.join(args.work, 'android.npz'))
    Handler.store = Store(args.work, args.pack, args.dump or os.path.join(args.pack, 'new'))
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    url = 'http://127.0.0.1:%d/' % args.port
    print('%d HD textures, %d dumps; serving %s (Ctrl+C stops)'
          % (len(Handler.store.keys), len(Handler.store.dumps), url), flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
