import re, base64, sys, io
from PIL import Image
raw = open(sys.argv[1],'rb').read().replace(b'\x00',b'').decode('utf-8','replace')
blocks = re.findall(r'---PBMBEGIN---(.*?)---PBMEND---', raw, re.S)
print('blocks', len(blocks))
for i,b in enumerate(blocks):
    s = re.sub(r'[^A-Za-z0-9+/=]','',b)
    s += '=' * ((4 - len(s)%4) % 4)
    try:
        data = base64.b64decode(s)
        im = Image.open(io.BytesIO(data))
        im.convert('L').save(f'{sys.argv[2]}{i}.png')
        print(i, len(data), im.size)
    except Exception as e:
        print(i, 'ERR', e)
