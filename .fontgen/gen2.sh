#!/usr/bin/env bash
set -e
cd /d/Users/PC/Desktop/esp32-mgxd/pocketplan
FA="managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
gen() {
  size=$1; name=$2; out=$3
  npx lv_font_conv --font "C:/Windows/Fonts/simhei.ttf" \
    -r 0x20-0x7E -r 0x00A0-0x00FF -r 0x2010-0x2027 -r 0x3000-0x303F -r 0x4E00-0x9FA5 \
    -r 0xFF00-0xFF5E \
    --font "$FA" -r 0xF000-0xF060 -r 0xF067-0xF079 -r 0xF07B -r 0xF093-0xF0F3 \
    -r 0xF11C -r 0xF124 -r 0xF158 -r 0xF15B-0xF15C -r 0xF1EB -r 0xF240-0xF244 \
    -r 0xF287 -r 0xF293 -r 0xF2ED -r 0xF304 -r 0xF55A -r 0xF8A0-0xF8FF \
    --size $size --bpp 1 --format lvgl --no-compress --lv-font-name $name -o $out
  python - "$out" <<'PY'
import sys
p=sys.argv[1]
s=open(p,encoding='utf-8').read()
old='''#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif'''
if old in s:
    s=s.replace(old,'#include "lvgl.h"')
    open(p,'w',encoding='utf-8').write(s)
    print('patched include in',p)
else:
    print('include pattern not found in',p)
PY
}
gen 16 pp_font_16 main/fonts/pp_font_16.c
gen 20 pp_font_20 main/fonts/pp_font_20.c
ls -l main/fonts/
