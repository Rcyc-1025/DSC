# -*- coding: utf-8 -*-
"""离屏界面截图驱动：跑 build\\AIChat.exe --shot 生成 BMP，再交给 tools\\bmp2png.mjs 转 PNG。

用法（在本仓库根目录）：
    D:\\Code\\python312\\python.exe tools\\shots.py            # 全部
    D:\\Code\\python312\\python.exe tools\\shots.py 01 03      # 只跑编号前缀
每个场景用独立数据目录 build\\shotsdata\\<key>（首次自动新建并删旧），互不污染。
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'AIChat.exe')
OUT = os.path.join(ROOT, 'build', 'shots')
DATA = os.path.join(ROOT, 'build', 'shotsdata')
NODE = r'C:\Program Files\nodejs\node.exe'
BMP2PNG = os.path.join(ROOT, 'tools', 'bmp2png.mjs')

SCENES = [
    ('01_chat', ['--ready', '--demo']),
    ('02_me', ['--ready', '--demo', '--tab', '0']),
    ('03_settings', ['--ready', '--demo', '--tab', '1']),
    ('04_qa_thread', ['--ready', '--demo', '--qa']),
    ('05_guide', ['--ready', '--demo', '--guide']),
    ('06_ai_info', ['--ready', '--demo', '--overlay', '1']),
    ('07_wizard', ['--shot-first']),
    ('08_narrow', ['--ready', '--demo', '--size', '900x620']),
]


def run(name, args):
    data_dir = os.path.join(DATA, name)
    if os.path.isdir(data_dir):
        shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir, exist_ok=True)
    bmp = os.path.join(OUT, name + '.bmp')
    a = [EXE, '--shot', bmp]
    if args == ['--shot-first']:
        a = [EXE, '--shot', bmp]
    else:
        a += args
    env = dict(os.environ)
    env['AICHAT_DATA_DIR'] = data_dir
    p = subprocess.run(a, env=env, capture_output=True, timeout=90)
    if not os.path.exists(bmp):
        print('FAIL %s rc=%d %s %s' % (name, p.returncode,
              p.stdout.decode('utf-8', 'replace')[-400:],
              p.stderr.decode('utf-8', 'replace')[-400:]))
        return False
    png = os.path.join(OUT, name + '.png')
    subprocess.run([NODE, BMP2PNG, bmp, png], check=True, capture_output=True)
    print('ok %s  %d bytes' % (name, os.path.getsize(png)))
    return True


def main():
    want = sys.argv[1:]
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    for name, args in SCENES:
        if want and not any(name.startswith(w) for w in want):
            continue
        if not run(name, args):
            bad += 1
    print('%d scene(s), %d failed' % (len(SCENES) if not want else len(want), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
