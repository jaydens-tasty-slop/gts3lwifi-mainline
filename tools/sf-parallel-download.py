#!/usr/bin/env python3
"""Parallel, resumable download of one SourceForge file from several mirrors.

Usage: pdl.py <out> <size> <sha256> <sourceforge .../download URL> <mirror>...
Chunks of 4 MiB go to a pool of workers; each chunk is fetched with a Range
request from the next mirror, retried until complete, and recorded in
<out>.done so an interrupted run resumes. The SHA-256 is checked at the end.
"""
import hashlib
import os
import subprocess
import sys
import threading
import time
import urllib.request

out, size, want, base = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
mirrors = sys.argv[5:]
CHUNK = 4 << 20
WORKERS = int(os.environ.get('PDL_WORKERS', 4 * len(mirrors)))

lock = threading.Lock()
urls = {}


def resolve(m):
    """Tokenized direct URL for mirror m (SourceForge signs mirror links)."""
    r = subprocess.run(['curl', '-sIL', '-o', '/dev/null', '-m', '30', '-w', '%{url_effective}',
                        f'{base}?use_mirror={m}'], capture_output=True, text=True)
    u = r.stdout.strip()
    return u if f'{m}.dl.sourceforge.net' in u else None


for m in mirrors:
    u = resolve(m)
    if u:
        urls[m] = u
if not urls:
    sys.exit('no mirror resolved')
print('mirrors:', ', '.join(urls), flush=True)

done_path = out + '.done'
done = set()
if os.path.exists(done_path):
    done = {int(x) for x in open(done_path).read().split()}
fd = os.open(out, os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, size)
todo = [i for i in range((size + CHUNK - 1) // CHUNK) if i not in done]
total = len(todo)
got = [0]
start = time.time()


def worker(wid):
    names = list(urls)
    k = wid
    while True:
        with lock:
            if not todo:
                return
            idx = todo.pop(0)
        lo = idx * CHUNK
        hi = min(size, lo + CHUNK) - 1
        while True:
            m = names[k % len(names)]
            k += 1
            try:
                req = urllib.request.Request(urls[m], headers={'Range': f'bytes={lo}-{hi}'})
                with urllib.request.urlopen(req, timeout=30) as r:
                    if r.status != 206:
                        raise IOError(f'status {r.status}')
                    data = r.read()
                if len(data) != hi - lo + 1:
                    raise IOError(f'short read {len(data)}')
                os.pwrite(fd, data, lo)
                with lock:
                    done.add(idx)
                    with open(done_path, 'a') as f:
                        f.write(f'{idx}\n')
                    got[0] += 1
                    if got[0] % 10 == 0 or got[0] == total:
                        el = time.time() - start
                        print(f'{got[0]}/{total} chunks, {got[0] * CHUNK / el / 1048576:.1f} MB/s', flush=True)
                break
            except Exception as e:
                if '403' in str(e) or '404' in str(e):
                    u = resolve(m)
                    if u:
                        urls[m] = u
                time.sleep(1)


threads = [threading.Thread(target=worker, args=(i,)) for i in range(WORKERS)]
for t in threads:
    t.start()
for t in threads:
    t.join()
os.close(fd)

h = hashlib.sha256()
with open(out, 'rb') as f:
    for b in iter(lambda: f.read(1 << 20), b''):
        h.update(b)
ok = h.hexdigest() == want
print(out, 'sha256', 'OK' if ok else 'MISMATCH ' + h.hexdigest(), flush=True)
if ok:
    os.remove(done_path)
sys.exit(0 if ok else 1)
