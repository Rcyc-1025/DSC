
import os, sys, time, urllib.request
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
url = 'https://dl.google.com/android/repository/android-ndk-r27c-windows.zip'
dst = r'D:\Code\_dl\android-ndk-r27c-windows.zip'
os.makedirs(os.path.dirname(dst), exist_ok=True)
req = urllib.request.Request(url, method='HEAD', headers={'User-Agent':'Mozilla/5.0'})
with urllib.request.urlopen(req, timeout=30) as f:
    total = int(f.headers.get('Content-Length') or 0)
print('total', total, flush=True)
have = os.path.getsize(dst) if os.path.exists(dst) else 0
t0 = time.time()
for attempt in range(200):
    try:
        pos = os.path.getsize(dst) if os.path.exists(dst) else 0
        if total and pos >= total:
            print('done', pos, flush=True); break
        headers = {'User-Agent':'Mozilla/5.0'}
        if pos: headers['Range'] = 'bytes=%d-' % pos
        r = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(r, timeout=60) as f, open(dst,'ab') as out:
            while True:
                b = f.read(1<<20)
                if not b: break
                out.write(b)
                pos += len(b)
                if total and pos >= total: break
        pos = os.path.getsize(dst)
        print('progress %d / %d (%.1f%%) %.1f MB/s' % (pos, total, 100.0*pos/max(total,1), pos/(time.time()-t0)/1048576), flush=True)
        if total and pos >= total: break
    except Exception as e:
        print('retry after error:', type(e).__name__, e, flush=True)
        time.sleep(3)
print('final size', os.path.getsize(dst), flush=True)
