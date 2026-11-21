
import os, sys, zipfile, time
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
src = r'D:\Code\_dl\android-ndk-r27c-windows.zip'
dst = r'D:\Code\android-ndk'
prefix = 'android-ndk-r27c/toolchains/llvm/'
os.makedirs(dst, exist_ok=True)
t0 = time.time(); n = 0
with zipfile.ZipFile(src) as z:
    names = [i for i in z.infolist() if i.filename.startswith(prefix) and not i.is_dir()]
    print('entries', len(names), flush=True)
    for i, info in enumerate(names):
        target = os.path.join(dst, info.filename)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(info) as f, open(target, 'wb') as out:
            while True:
                b = f.read(1 << 20)
                if not b: break
                out.write(b)
        n += 1
        if n % 200 == 0:
            print('extracted', n, '/', len(names), flush=True)
print('done', n, 'in %.1fs' % (time.time()-t0), flush=True)
