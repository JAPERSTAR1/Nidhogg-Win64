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
