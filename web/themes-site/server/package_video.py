#!/usr/bin/env python3
"""Builds optimized MP4 video theme packages for OmniLaunch 1.0.0.

While legacy SwitchU 2.6.5 themes used ~300 uncompressed DDS image frames (70+ MB),
OmniLaunch 1.0.0 plays H.264 MP4 videos natively with hardware acceleration.

This script creates `theme-video.zip` (and content-addressed `theme-video-<hash>.zip`)
containing:
  - theme.json (with background.video pointing to media/video.mp4)
  - media/video.mp4 (from preview_1080.mp4)
  - media/screenshots/* (if present)
  - media/music/* (if present)
"""

import os
import sys
import json
import zipfile
import hashlib
import glob

THEMES = '/srv/themes/themes'

def build_video_theme(theme_dir):
    theme_id = os.path.basename(theme_dir)
    video_src = os.path.join(theme_dir, 'preview_1080.mp4')
    if not os.path.isfile(video_src):
        mp4s = [f for f in os.listdir(theme_dir) if f.endswith('.mp4')]
        if not mp4s:
            return None
        video_src = os.path.join(theme_dir, mp4s[0])

    manifest_src = os.path.join(theme_dir, 'theme.json')
    if not os.path.isfile(manifest_src):
        return None

    try:
        with open(manifest_src, 'r', encoding='utf-8') as f:
            manifest = json.load(f)
    except Exception as e:
        print(f"[{theme_id}] Error reading theme.json: {e}", file=sys.stderr)
        return None

    video_manifest = dict(manifest)
    theme_obj = dict(video_manifest.get('theme', {}))
    bg_obj = dict(theme_obj.get('background', {}))

    if 'image' in bg_obj:
        del bg_obj['image']
    bg_obj['video'] = 'media/video.mp4'
    bg_obj['count'] = 1
    bg_obj['opacity'] = 0.0
    theme_obj['background'] = bg_obj
    video_manifest['theme'] = theme_obj

    out_zip = os.path.join(theme_dir, 'theme-video.zip')
    tmp_zip = out_zip + '.building'

    if os.path.isfile(out_zip):
        zip_mtime = os.path.getmtime(out_zip)
        src_mtime = max(os.path.getmtime(video_src), os.path.getmtime(manifest_src))
        if zip_mtime >= src_mtime:
            return out_zip

    print(f"[{theme_id}] Packaging video theme -> theme-video.zip")
    with zipfile.ZipFile(tmp_zip, 'w', compression=zipfile.ZIP_STORED) as zf:
        manifest_data = json.dumps(video_manifest, indent=2, ensure_ascii=False).encode('utf-8')
        zf.writestr('theme.json', manifest_data)
        zf.write(video_src, 'media/video.mp4')

        shots_dir = os.path.join(theme_dir, 'media', 'screenshots')
        if os.path.isdir(shots_dir):
            for name in sorted(os.listdir(shots_dir)):
                if name.lower().endswith(('.jpg', '.jpeg', '.png')):
                    zf.write(os.path.join(shots_dir, name), f'media/screenshots/{name}')

        music_dir = os.path.join(theme_dir, 'media', 'music')
        if os.path.isdir(music_dir):
            for name in sorted(os.listdir(music_dir)):
                if name.lower().endswith(('.mp3', '.ogg', '.wav', '.flac')):
                    zf.write(os.path.join(music_dir, name), f'media/music/{name}')

    os.replace(tmp_zip, out_zip)
    return out_zip

def immutable_video_package(theme_dir):
    canonical = os.path.join(theme_dir, 'theme-video.zip')
    if not os.path.isfile(canonical):
        return None
    digest = hashlib.sha256()
    with open(canonical, 'rb') as fh:
        while True:
            chunk = fh.read(1048576)
            if not chunk: break
            digest.update(chunk)
    target_name = 'theme-video-%s.zip' % digest.hexdigest()[:16]
    target_path = os.path.join(theme_dir, target_name)
    if not os.path.isfile(target_path):
        for stale in glob.glob(os.path.join(theme_dir, 'theme-video-[0-9a-f]*.zip')):
            try: os.unlink(stale)
            except OSError: pass
        try:
            os.link(canonical, target_path)
        except OSError:
            import shutil
            shutil.copy2(canonical, target_path)
    return target_path

def main():
    themes = sorted(os.listdir(THEMES))
    count = 0
    for name in themes:
        td = os.path.join(THEMES, name)
        if not os.path.isdir(td): continue
        if build_video_theme(td):
            immutable_video_package(td)
            count += 1
    print(f"Processed {count} video themes successfully.")

if __name__ == '__main__':
    main()
