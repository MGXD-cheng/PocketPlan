#pragma once

#include "lvgl.h"

/* 1bpp SimHei fonts generated into main/fonts/ by lv_font_conv.
 * 16 px is the UI font, 20 px is the large reader font. Both cover ASCII,
 * CJK punctuation, the full CJK unified ideograph block and the FontAwesome
 * glyphs LVGL's LV_SYMBOL_* macros use. */
LV_FONT_DECLARE(pp_font_16)
LV_FONT_DECLARE(pp_font_20)
/* Digits only: countdown badges and the big focus clock. */
LV_FONT_DECLARE(pp_font_32)
LV_FONT_DECLARE(pp_font_96)
