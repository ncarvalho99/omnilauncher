#!/usr/bin/env python3
"""Rebuild index.json and omnilaunch.json from whatever theme folders are actually on disk."""

import glob
import hashlib
import json
import os
import sys
import zipfile

ROOT = '/srv/themes'
THEMES = os.path.join(ROOT, 'themes')
VIDEOS = os.path.join(ROOT, 'videos')

COVER_CANDIDATES = [
    ('preview', 'screenshots'), ('preview', 'cover'), ('preview', 'screenshot'),
    ('screenshots',), ('cover',), ('screenshot',), ('thumbnail',),
]

VECTEEZY_MAP = {
    'blue-neon-glow': 'vecteezy_glowing_blue_neon_lights_on_dark_background_82011296.mp4',
    'digital-flow': 'vecteezy_minimalist_background_with_an_elegant_flowing_blue_digital.mp4',
    'festive-lights': 'vecteezy_abstract-festive-looping-background_2018590.mp4',
    'golden-curtain': 'vecteezy_golden_light_curtain_sparkle_animation_on_black_77357555.mp4',
    'purple-particles': 'vecteezy_purple_themed_particle_form_futuristic_neon_graphic_15287575.mp4',
    'white-strings': 'vecteezy_white_particles_strings_fading_over_blue_background_2276219.mp4',
    'blue-motion': 'vecteezy_abstract-blue-motion-background-loop_81298801.mp4',
    'blue-bubbles-live-wallpaper': 'Blue Bubbles Live Wallpaper.webm',
    'digital-code-live-wallpaper': 'Digital Code Live Wallpaper.webm',
}

def find_direct_video(theme_name):
    if theme_name in VECTEEZY_MAP:
        cand = os.path.join(VIDEOS, VECTEEZY_MAP[theme_name])
        if os.path.isfile(cand):
            return VECTEEZY_MAP[theme_name], cand

    if not os.path.isdir(VIDEOS):
        return None, None

    video_files = os.listdir(VIDEOS)
    # 1. Exact match with moewalls suffix
    target = f"{theme_name}-moewalls-com.mp4"
    if target in video_files:
        return target, os.path.join(VIDEOS, target)

    # 2. Match without suffix
    target = f"{theme_name}.mp4"
    if target in video_files:
        return target, os.path.join(VIDEOS, target)

    # 3. Normalized search
    norm_theme = theme_name.lower().replace("-", "").replace("_", "")
    for vf in video_files:
        norm_v = vf.replace("-moewalls-com.mp4", "").replace(".mp4", "").replace(".webm", "").lower().replace("-", "").replace("_", "").replace(" ", "")
        if norm_v == norm_theme:
            return vf, os.path.join(VIDEOS, vf)

    return None, None

def pick_preview_video(theme_dir, rel_dir):
    try:
        names = sorted(os.listdir(theme_dir))
    except OSError:
        return None
    webm = [n for n in names if n.startswith('preview') and n.endswith('.webm')]
    if webm:
        return rel_dir + '/' + webm[0]
    if os.path.isfile(os.path.join(theme_dir, 'preview_1080.mp4')):
        return rel_dir + '/preview_1080.mp4'
    return None

def pick_cover(manifest, theme_dir, rel_dir):
    def first_existing(value):
        if isinstance(value, str): value = [value]
        if not isinstance(value, list): return None
        for item in value:
            if isinstance(item, str) and os.path.isfile(os.path.join(theme_dir, item)):
                return rel_dir + '/' + item.lstrip('/')
        return None
    for keys in COVER_CANDIDATES:
        node = manifest
        for key in keys:
            node = node.get(key) if isinstance(node, dict) else None
            if node is None: break
        found = first_existing(node)
        if found: return found
    shots = os.path.join(theme_dir, 'media', 'screenshots')
    if os.path.isdir(shots):
        for name in sorted(os.listdir(shots)):
            if name.lower().endswith(('.jpg', '.jpeg', '.png')):
                return '%s/media/screenshots/%s' % (rel_dir, name)
    return None

def describe(manifest, theme_dir):
    image = (manifest.get('theme', {}).get('background', {}) or {}).get('image', {}) or {}
    frames = image.get('frames') or []
    info = {'animated': bool(frames), 'frameCount': len(frames), 'fps': image.get('fps')}
    total = 0
    for base, _dirs, files in os.walk(theme_dir):
        for name in files:
            try: total += os.path.getsize(os.path.join(base, name))
            except OSError: pass
    info['bytes'] = total
    if os.path.isfile(os.path.join(theme_dir, 'preview.webp')):
        info['preview'] = 'themes/%s/preview.webp' % os.path.basename(theme_dir)
    music = (manifest.get('theme', {}).get('audio', {}) or {}).get('music') or []
    info['music'] = len(music)
    return info

def immutable_package(theme_dir):
    canonical = os.path.join(theme_dir, 'theme.zip')
    if not os.path.isfile(canonical):
        legacy = sorted(glob.glob(os.path.join(theme_dir, 'theme-[0-9a-f]*.zip')),
                        key=os.path.getmtime, reverse=True)
        return legacy[0] if legacy else None
    digest = hashlib.sha256()
    with open(canonical, 'rb') as fh:
        for block in iter(lambda: fh.read(1024 * 1024), b''):
            digest.update(block)
    published = os.path.join(theme_dir, 'theme-%s.zip' % digest.hexdigest()[:16])
    if not os.path.exists(published):
        os.link(canonical, published)
    return published

def installed_size(package_path):
    try:
        with zipfile.ZipFile(package_path) as archive:
            return sum(info.file_size for info in archive.infolist() if not info.is_dir())
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        return 0

def main():
    entries, omni_entries, review = [], [], []
    if os.path.isdir(THEMES):
        for name in sorted(os.listdir(THEMES)):
            theme_dir = os.path.join(THEMES, name)
            manifest_path = os.path.join(theme_dir, 'theme.json')
            if not os.path.isfile(manifest_path): continue
            try:
                with open(manifest_path, encoding='utf-8') as fh: manifest = json.load(fh)
            except (OSError, ValueError) as exc:
                continue
            rel_dir = 'themes/' + name
            entry = {'id': manifest.get('id') or name, 'name': manifest.get('name') or name,
                     'author': manifest.get('author') or 'unknown',
                     'version': str(manifest.get('version') or '1.0.0'),
                     'path': rel_dir, 'manifest': rel_dir + '/theme.json'}
            package = immutable_package(theme_dir)
            if package:
                entry['package'] = rel_dir + '/' + os.path.basename(package)
                entry['packageBytes'] = os.path.getsize(package)
                unpacked = installed_size(package)
                if unpacked:
                    entry['installedBytes'] = unpacked

            # Find direct MP4 video from /srv/themes/videos/
            vname, vpath = find_direct_video(name)
            if vname and vpath:
                vsize = os.path.getsize(vpath)
                entry['packageVideo'] = 'videos/' + vname
                entry['packageVideoBytes'] = vsize
                entry['installedVideoBytes'] = vsize
            else:
                local_v = os.path.join(theme_dir, 'preview_1080.mp4')
                if os.path.isfile(local_v):
                    vsize = os.path.getsize(local_v)
                    entry['packageVideo'] = rel_dir + '/preview_1080.mp4'
                    entry['packageVideoBytes'] = vsize
                    entry['installedVideoBytes'] = vsize

            package_hd = os.path.join(theme_dir, 'theme-hd.zip')
            if os.path.isfile(package_hd):
                entry['packageHd'] = rel_dir + '/theme-hd.zip'
                entry['packageHdBytes'] = os.path.getsize(package_hd)
            shots = []
            for rel in (manifest.get('preview', {}) or {}).get('screenshots', []) or []:
                if isinstance(rel, str) and os.path.isfile(os.path.join(theme_dir, rel)):
                    shots.append(rel_dir + '/' + rel.lstrip('/'))
            if shots: entry['screenshots'] = shots
            image = (manifest.get('theme', {}).get('background', {}) or {}).get('image', {}) or {}
            frames, fps = image.get('frames') or [], image.get('fps') or 0
            sheet = os.path.join(theme_dir, 'preview_sheet.jpg')
            if os.path.isfile(sheet):
                entry['thumbSheet'] = rel_dir + '/preview_sheet.jpg'
                hd = os.path.join(theme_dir, 'preview_sheet_hd.jpg')
                if os.path.isfile(hd): entry['thumbSheetHd'] = rel_dir + '/preview_sheet_hd.jpg'
                video = pick_preview_video(theme_dir, rel_dir)
                if video:
                    entry['previewVideo'] = video
                    if os.path.isfile(os.path.join(theme_dir, 'preview_1080.jpg')):
                        entry['previewPoster'] = rel_dir + '/preview_1080.jpg'
                entry['thumbCols'], entry['thumbRows'] = 5, 4
                loop = (len(frames) / float(fps)) if (frames and fps) else 0.0
                window = min(loop, 2.5) if loop > 0 else 2.5
                entry['thumbFps'] = round(20.0 / window, 2) if window > 0.2 else 8.0
            cover = pick_cover(manifest, theme_dir, rel_dir)
            if cover: entry['cover'] = cover

            # Standard entry (used for index.json for SwitchU 2.6.5)
            entries.append(entry)

            # OmniLaunch dedicated entry (omnilaunch.json)
            omni_entry = dict(entry)
            if 'packageVideo' in entry:
                omni_entry['package'] = entry['packageVideo']
                omni_entry['packageBytes'] = entry['packageVideoBytes']
                omni_entry['installedBytes'] = entry['installedVideoBytes']
                omni_entry['isVideoTheme'] = True
            omni_entries.append(omni_entry)

            extra = dict(entry); extra.update(describe(manifest, theme_dir))
            extra['license'], extra['source'] = manifest.get('license'), manifest.get('source')
            if 'packageVideo' in entry:
                extra['packageVideo'] = entry['packageVideo']
                extra['packageVideoBytes'] = entry['packageVideoBytes']
                extra['installedVideoBytes'] = entry['installedVideoBytes']
            review.append(extra)

    for filename, payload in [
        ('index.json', {'schemaVersion': 1, 'themes': entries}),
        ('omnilaunch.json', {'schemaVersion': 1, 'themes': omni_entries}),
        ('review.json', {'themes': review})
    ]:
        with open(os.path.join(ROOT, filename), 'w', encoding='utf-8') as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=2); fh.write('\n')
    print('indexed %d themes (standard: index.json, omnilaunch: omnilaunch.json)' % len(entries))

if __name__ == '__main__':
    main()
