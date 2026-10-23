
import glob
cps=set()
for f in glob.glob(r'main/**/*.c',recursive=True)+glob.glob(r'main/**/*.h',recursive=True)+[r'main/www/index.html']:
    if 'fonts' in f: continue
    s=open(f,encoding='utf-8',errors='replace').read()
    for ch in s:
        o=ord(ch)
        if o>0x7E: cps.add(o)
def covered(o):
    ranges=[(0x20,0x7E),(0x2010,0x2027),(0x3000,0x303F),(0x4E00,0x9FA5),(0xFF00,0xFF5E),(0x00A0,0x00FF)]
    fa=[(0xF000,0xF060),(0xF067,0xF079),(0xF07B,0xF07B),(0xF093,0xF0F3),(0xF11C,0xF11C),(0xF124,0xF124),(0xF158,0xF158),(0xF1EB,0xF1EB),(0xF240,0xF244),(0xF287,0xF287),(0xF293,0xF293),(0xF2ED,0xF2ED),(0xF304,0xF304),(0xF55A,0xF55A),(0xF15B,0xF15C)]
    return any(a<=o<=b for a,b in ranges+fa)
miss=sorted(o for o in cps if not covered(o))
open('miss.txt','w',encoding='utf-8').write('\n'.join('U+%04X %s'%(o,chr(o)) for o in miss))
print('total',len(cps),'missing',len(miss))
