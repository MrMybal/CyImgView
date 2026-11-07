"""Create faithful WebP copies and declared crops of existing app screenshots.

Requires Pillow only when updating assets. Does not capture windows or recreate UI.
"""
from pathlib import Path
import json
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / '.CyDocBuild/assets/screenshots'
OUTPUT = ROOT / 'website/assets'
OUTPUT.mkdir(parents=True, exist_ok=True)
records = []

def save(name, source, crop=None):
    image = Image.open(source).convert('RGB')
    if crop:
        image = image.crop(crop)
    image.save(OUTPUT / name, 'WEBP', quality=92 if crop else 90, method=6)
    record = {'asset': name, 'source': source.relative_to(ROOT).as_posix(),
              'kind': 'screenshot detail' if crop else 'full screenshot'}
    if crop:
        record['crop'] = list(crop)
    record['width'], record['height'] = image.size
    records.append(record)

for locale in ('en', 'fr'):
    for mode in ('viewer', 'editor', 'develop', 'black-white', 'effects', 'effect-preview', 'new-document'):
        save(f'{mode}-{locale}.webp', SOURCE / f'{mode}-{locale}.png')
    crops = [
        ('layer-detail', 'editor', (1193, 100, 1430, 890)),
        ('tools-detail', 'editor', (6, 98, 310, 890)),
        ('develop-detail', 'develop', (1126, 108, 1430, 890)),
        ('effect-detail', 'effect-preview', (1193, 100, 1430, 344)),
        ('gallery-detail', 'effects', (330, 110, 1110, 790)),
        ('filmstrip-detail', 'viewer', (465, 810, 975, 895)),
        ('color-scene', 'develop', (12, 126, 1116, 862)),
        ('mono-scene', 'black-white', (12, 126, 1116, 862)),
    ]
    for name, mode, crop in crops:
        save(f'{name}-{locale}.webp', SOURCE / f'{mode}-{locale}.png', crop)

logo = Image.open(ROOT / '.CyDocBuild/assets/app-logo.png').convert('RGBA')
logo.thumbnail((128, 128), Image.Resampling.LANCZOS)
logo.save(OUTPUT / 'logo.png', optimize=True)
records.append({'asset': 'logo.png', 'source': '.CyDocBuild/assets/app-logo.png',
                'kind': 'resized application logo', 'width': logo.width, 'height': logo.height})
(ROOT / 'website/asset-provenance.json').write_bytes(
    (json.dumps(records, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
print(f'{len(records)} faithful local assets prepared.')
