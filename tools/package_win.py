
# -*- coding: utf-8 -*-
"""打 Windows 便携包：组装 dist/AIChat-<ver>-win-x64/ 并压成 zip，附带依赖自检与 SHA-256。"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zipfile

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
DIST = os.path.join(ROOT, 'dist')
VER = '1.0.0'
NAME = 'AIChat-%s-win-x64' % VER
OUTDIR = os.path.join(DIST, NAME)
ZIP = os.path.join(DIST, NAME + '-portable.zip')
MINGWBIN = r'D:\Code\msys2ucrt\ucrt64\bin'

# 允许的系统 DLL（其余视为需要随包分发的运行时）
SYSTEM_DLLS = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll',
    'comctl32.dll', 'comdlg32.dll', 'ws2_32.dll', 'psapi.dll', 'msvcrt.dll',
    'ucrtbase.dll', 'ole32.dll', 'oleaut32.dll', 'shlwapi.dll', 'imm32.dll',
    'dwmapi.dll', 'version.dll', 'winmm.dll', 'rpcrt4.dll', 'crypt32.dll',
    'bcrypt.dll', 'ntdll.dll', 'setupapi.dll', 'wininet.dll', 'shcore.dll',
    'uxtheme.dll', 'userenv.dll', 'normaliz.dll', 'secur32.dll', 'iphlpapi.dll',
}


def pe_imports(path):
    """读取 PE 导入表里的 DLL 名（纯标准库，不依赖 objdump）。"""
    data = open(path, 'rb').read()
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    opt = e_lfanew + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    num_dirs_off = opt + (92 if magic == 0x10B else 108)
    dir_rva, _ = struct.unpack_from('<II', data, num_dirs_off + 8 * 1)
    nsec = struct.unpack_from('<H', data, e_lfanew + 6)[0]
    sec_off = opt + struct.unpack_from('<H', data, e_lfanew + 20)[0]
    secs = []
    for i in range(nsec):
        base = sec_off + i * 40
        vsize, vaddr = struct.unpack_from('<II', data, base + 8)
        rawsize, rawptr = struct.unpack_from('<II', data, base + 16)
        secs.append((vaddr, max(vsize, rawsize), rawptr))

    def rva2off(rva):
        for va, size, ptr in secs:
            if va <= rva < va + size:
                return ptr + (rva - va)
        return None

    off = rva2off(dir_rva)
    names = []
    if off is None:
        return names
    while True:
        desc = data[off:off + 20]
        if len(desc) < 20 or desc == b'\0' * 20:
            break
        name_rva = struct.unpack_from('<I', desc, 12)[0]
        noff = rva2off(name_rva)
        if noff is None:
            break
        end = data.index(b'\0', noff)
        names.append(data[noff:end].decode('ascii', 'replace').lower())
        off += 20
    return names


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def need_dlls():
    """返回 exe 需要的非系统 DLL（从 mingw 目录取）。"""
    runtime = {}
    for exe in ['AIChat.exe', 'aic.exe', 'aic_plugin_host.exe']:
        p = os.path.join(BUILD, exe)
        for d in pe_imports(p):
            if d in SYSTEM_DLLS or d.startswith('api-ms-win'):
                continue
            src = os.path.join(MINGWBIN, d)
            if os.path.isfile(src):
                runtime[d] = src
            else:
                print('警告：%s 依赖 %s，但本机 mingw 目录里没有' % (exe, d))
    return runtime


def copy(src, dst_rel):
    dst = os.path.join(OUTDIR, dst_rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print('  +', dst_rel)


def main():
    shutil.rmtree(OUTDIR, ignore_errors=True)
    os.makedirs(OUTDIR, exist_ok=True)

    print('组装 %s' % OUTDIR)
    copy(os.path.join(BUILD, 'AIChat.exe'), 'AIChat.exe')
    copy(os.path.join(BUILD, 'aic.exe'), 'tools/aic.exe')
    copy(os.path.join(BUILD, 'aic_plugin_host.exe'), 'aic_plugin_host.exe')

    for d, src in sorted(need_dlls().items()):
        copy(src, d)

    sample = os.path.join(ROOT, 'plugins', 'sample_basic')
    copy(os.path.join(sample, 'plugin.json'), 'plugins/sample_basic/plugin.json')
    copy(os.path.join(sample, 'prompt_fragment.txt'), 'plugins/sample_basic/prompt_fragment.txt')
    copy(os.path.join(sample, 'README.md'), 'plugins/sample_basic/README.md')
    dll = os.path.join(BUILD, 'plugins', 'sample_basic.dll')
    if os.path.isfile(dll):
        copy(dll, 'plugins/sample_basic/sample_basic.dll')

    for doc in ['README.md', 'ADVANCED.md', 'PLUGIN.md', 'FR-MATRIX.md']:
        src = os.path.join(ROOT, 'docs', doc) if doc != 'README.md' else os.path.join(ROOT, 'README.md')
        if os.path.isfile(src):
            copy(src, 'docs/' + doc)

    copy(os.path.join(ROOT, 'LICENSE'), 'LICENSE')
    copy(os.path.join(ROOT, 'scripts', 'install.ps1'), 'install.ps1')

    notes = (
        'AIChat 陪伴对话 %s（Windows x64 便携包）\n'
        '\n'
        '1. 直接双击 AIChat.exe 运行；不需要安装，也不会写注册表。\n'
        '2. 数据默认放在 %%LOCALAPPDATA%%\\AIChat；想做成纯便携（如放 U 盘），\n'
        '   在同目录建一个 data 文件夹，或用 set AICHAT_DATA_DIR=.<你的目录> 指定。\n'
        '3. 首次启动进「设置 → API Key」填自己的 DeepSeek Key（保存在 Windows 凭据管理器）。\n'
        '4. tools\\aic.exe 是命令行验收器：\n'
        '     aic.exe verify            逐条跑 FR-01 ~ FR-24 自动化检查\n'
        '     aic.exe bench             万条消息分页 / 导出 / 常驻内存基准\n'
        '     aic.exe doctor            环境自检（数据库、凭据、插件宿主）\n'
        '5. 想装到开始菜单：右键 install.ps1 → 用 PowerShell 运行。\n'
        '6. 本包未做代码签名（本机无 signtool），首次运行 SmartScreen 可能提示；\n'
        '   自签证书脚本见仓库 scripts\\gen_cert.ps1。\n'
    ) % VER
    with open(os.path.join(OUTDIR, '使用说明.txt'), 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write(notes)
    print('  + 使用说明.txt')

    if os.path.isfile(ZIP):
        os.remove(ZIP)
    with zipfile.ZipFile(ZIP, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _dirs, files in os.walk(OUTDIR):
            for fn in sorted(files):
                full = os.path.join(base, fn)
                rel = os.path.relpath(full, OUTDIR)
                z.write(full, os.path.join(NAME, rel))

    total = sum(os.path.getsize(os.path.join(b, f)) for b, _d, fs in os.walk(OUTDIR) for f in fs)
    print()
    print('目录体积 %.1f MB' % (total / 1048576.0))
    print('便携包   %s（%.1f MB）' % (ZIP, os.path.getsize(ZIP) / 1048576.0))
    print('SHA-256  %s' % sha256(ZIP))
    with open(ZIP + '.sha256', 'w', encoding='ascii', newline='\n') as f:
        f.write('%s  %s\n' % (sha256(ZIP), os.path.basename(ZIP)))
    print('校验文件 %s.sha256' % ZIP)


if __name__ == '__main__':
    main()
