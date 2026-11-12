"""Create separate, verified local desktop copies of the Windows game and PS4 files.

Never modifies the source assets or overwrites an existing desktop package.
No operator credentials, profiles, logs or save data are copied.
"""
import argparse
import ctypes
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()

def manifest(folder):
    return {str(p.relative_to(folder)).replace('\\', '/'): sha256(p)
            for p in sorted(folder.rglob('*')) if p.is_file()}

def desktop_folder():
    buffer = ctypes.create_unicode_buffer(32768)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buffer) != 0:
        raise RuntimeError('Cannot locate the Windows Desktop folder')
    return Path(buffer.value)

def archive(folder):
    target = folder.with_suffix('.zip')
    with zipfile.ZipFile(target, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for item in sorted(folder.rglob('*')):
            if item.is_file():
                output.write(item, str(Path(folder.name) / item.relative_to(folder)))
    with zipfile.ZipFile(target) as check:
        bad = check.testzip()
        if bad: raise RuntimeError(f'Archive verification failed: {bad}')
    return target

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--assets', type=Path, default=Path(r'C:\Users\puy12\Downloads\g\CUSA00920'))
    parser.add_argument('--desktop', type=Path, default=desktop_folder())
    args = parser.parse_args()
    source = args.assets.resolve()
    if not (source / 'eboot.bin').is_file() or not (source / 'Data').is_dir():
        raise RuntimeError('Missing original eboot.bin or Data folder')
    executable = ROOT / 'port/out/Nidhogg.exe'
    if not executable.is_file(): raise RuntimeError('Build port/build.ps1 first')
    original_before = manifest(source)
    names = ['Nidhogg Windows - Ready to Play', 'Nidhogg Original - CUSA00920']
    suffix = ''
    if any((args.desktop / name).exists() or (args.desktop / (name + '.zip')).exists() for name in names):
        suffix = ' - ' + datetime.now(timezone(timedelta(hours=3))).strftime('%Y%m%d-%H%M%S')
    playable, original = [args.desktop / (name + suffix) for name in names]
    if any(p.exists() or p.with_suffix('.zip').exists() for p in [playable, original]):
        raise RuntimeError('Destination already exists; no files overwritten')
    playable.mkdir()
    shutil.copy2(executable, playable / 'Nidhogg.exe')
    shutil.copy2(ROOT / 'port/resources/nidhogg-logo.png', playable / 'Nidhogg.png')
    (playable / 'portable.txt').write_text('Portable settings and saves stay in this folder.\n')
    (playable / 'config').mkdir()
    clean_settings = {'assetPath': '', 'lobbyServerUrl': 'https://lobby.japer.me', 'playerName': 'Fencer'}
    (playable / 'config/settings.json').write_text(json.dumps(clean_settings, indent=2), encoding='utf8')
    runtime = playable / 'CUSA00920'
    runtime.mkdir()
    shutil.copy2(source / 'eboot.bin', runtime / 'eboot.bin')
    shutil.copytree(source / 'Data', runtime / 'Data')
    (playable / 'Play Online.cmd').write_text(
        '@echo off\nstart "Nidhogg Online" /D "%~dp0" "%~dp0Nidhogg.exe" --online --lobby-url https://lobby.japer.me\n',
        encoding='ascii')
    (playable / 'READ-ME-AR.txt').write_text(
        'نسخة Nidhogg لنظام Windows 10/11 x64\n\n'
        'شغّل Nidhogg.exe للعب. الملفات اللازمة للتشغيل موجودة داخل CUSA00920.\n'
        'انقل أو شارك المجلد كاملًا مع جميع محتوياته، أو شارك ملف ZIP المرفق.\n'
        'F1 يفتح الإعدادات والتحكم واللعب عبر الإنترنت.\n\n'
        'للعب عبر الإنترنت افتح Play Online.cmd أو Online Multiplayer من اللعبة.\n'
        'أنشئ غرفة خاصة وشارك كودها مع صديقك؛ اختارا Ready ثم يبدأ المضيف المباراة.\n'
        'كل لاعب يستخدم أزرار اللاعب الأول على جهازه.\n'
        'الخادم: https://lobby.japer.me\n'
        'قد يتأخر ظهور العنوان الجديد عند بعض مزودي DNS.\n'
        'الخادم يعتمد على بقاء جهاز المشغّل متصلًا بالإنترنت.\n'
        'لا يحتاج اللاعب إلى VPN أو Node.js أو cloudflared.\n\n'
        'الإعدادات والحفظ ستكون داخل هذا المجلد؛ لا تتضمن الحزمة بيانات حفظ شخصية.\n',
        encoding='utf-8-sig')
    shutil.copytree(source, original)
    copied_original = manifest(original)
    copied_runtime = manifest(runtime)
    expected_runtime = {name: value for name, value in original_before.items()
                        if name == 'eboot.bin' or name.startswith('Data/')}
    if copied_original != original_before or copied_runtime != expected_runtime:
        raise RuntimeError('Copied assets do not match their source')
    if manifest(source) != original_before:
        raise RuntimeError('Original source changed during packaging')
    archives = [archive(playable), archive(original)]
    report = {'windows_folder': str(playable), 'original_folder': str(original),
              'original_files': len(original_before), 'runtime_asset_files': len(copied_runtime),
              'copied_assets_match': True, 'original_source_unchanged': True,
              'executable_sha256': sha256(playable / 'Nidhogg.exe'),
              'archives': [{'path': str(p), 'bytes': p.stat().st_size, 'sha256': sha256(p)} for p in archives]}
    output = ROOT / 'tests/results/desktop-package-report.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2), flush=True)

if __name__ == '__main__': main()
