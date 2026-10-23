import re, base64, sys, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

src = sys.argv[1] if len(sys.argv) > 1 else '../tour.log'
prefix = sys.argv[2] if len(sys.argv) > 2 else '../tour'
raw = open(src, 'rb').read().replace(b'\x00', b'').decode('utf-8', 'replace')
blocks = re.findall(r'---PBMBEGIN---(.*?)---PBMEND---', raw, re.S)
print('blocks', len(blocks), 'from', src)
for i, b in enumerate(blocks):
    s = re.sub(r'[^A-Za-z0-9+/=]', '', b)
    s += '=' * (-len(s) % 4)
    try:
        data = base64.b64decode(s)
        im = Image.open(io.BytesIO(data))
        im.load()
        im.convert('L').save(f'{prefix}{i}.png')
        print(i, len(data), im.size)
    except Exception as exc:
        print(i, 'SKIP', exc)
