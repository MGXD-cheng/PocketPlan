
base = r"D:\Users\PC\Desktop\esp32-mgxd\pocketplan\main" + "\\"

def rw(p, pairs):
    p = base + p
    s = open(p, encoding="utf-8").read()
    orig = s
    for a, b in pairs:
        if a not in s:
            raise SystemExit("MISS in " + p + ": " + a[:70])
        s = s.replace(a, b)
    open(p, "w", encoding="utf-8").write(s)
    print("patched", p, len(orig), "->", len(s))

rw("CMakeLists.txt", [('        "config.c"\n', '        "config.c"\n        "fonts/pp_font_16.c"\n        "fonts/pp_font_20.c"\n')])

rw("ui.c", [
 ('#include "pages.h"\n#include "plans.h"', '#include "pages.h"\n#include "plans.h"\n#include "pp_fonts.h"'),
 ('''    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);''',
  '''    /* One monochrome font for the whole UI. The stock Montserrat default has
     * no CJK glyphs at all and the bundled Source Han subsets only carry a few
     * thousand hand-picked characters, which is why most Chinese text came out
     * as empty boxes. pp_font_16 is a 1bpp SimHei: pure black on white, with no
     * anti-aliased grey fringe. */
    lv_display_t *disp = pp_board_display();
    if (disp != NULL) {
        lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(0x000000),
                                                  lv_color_hex(0x000000), false,
                                                  &pp_font_16);
        lv_display_set_theme(disp, theme);
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), 0);'''),
 ('''    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1A73E8), 0);''',
  '''    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);'''),
 ('lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);', 'lv_obj_set_style_text_color(s_title, lv_color_hex(0x000000), 0);'),
 ('lv_obj_set_style_text_color(s_clock, lv_color_hex(0xFFFFFF), 0);', 'lv_obj_set_style_text_color(s_clock, lv_color_hex(0x000000), 0);'),
 ('lv_obj_set_style_text_color(s_status, lv_color_hex(0xD2E3FC), 0);', 'lv_obj_set_style_text_color(s_status, lv_color_hex(0x000000), 0);'),
 ('''        lv_obj_set_style_bg_color(s_nav_buttons[i],
                                  lv_color_hex(i == index ? 0x1A73E8 : 0xFFFFFF), 0);''',
  '''        lv_obj_set_style_bg_color(s_nav_buttons[i],
                                  lv_color_hex(i == index ? 0xE4E4E4 : 0xFFFFFF), 0);'''),
 ('''            lv_obj_set_style_text_color(label,
                                        lv_color_hex(i == index ? 0xFFFFFF : 0x5F6368), 0);''',
  '''            lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);'''),
 ('''            lv_obj_set_style_text_color(icon,
                                        lv_color_hex(i == index ? 0xFFFFFF : 0x5F6368), 0);''',
  '''            lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);'''),
 ('lv_obj_set_style_text_color(icon, lv_color_hex(0x5F6368), 0);', 'lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);'),
 ('lv_obj_set_style_text_color(label, lv_color_hex(0x5F6368), 0);', 'lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);'),
 ('''    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x323232), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0xFFFFFF), 0);''',
  '''    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0x000000), 0);'''),
])

rw("page_reader.c", [
 ('#include "reader.h"', '#include "pp_fonts.h"\n#include "reader.h"'),
 ('''#define C_TEXT 0x202124
#define C_DIM 0x5F6368
#define C_ACCENT 0x1A73E8''',
  '''#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000'''),
 ('''static const lv_font_t *reader_font(void)
{
    pp_config_t *cfg = pp_config();
#if LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    if (cfg->font_size <= 14) {
        return &lv_font_source_han_sans_sc_14_cjk;
    }
#endif
    return &lv_font_source_han_sans_sc_16_cjk;
}''',
  '''static const lv_font_t *reader_font(void)
{
    pp_config_t *cfg = pp_config();
    /* 1bpp SimHei in two sizes; both are pure black with no grey edges. */
    return (cfg->font_size >= 20) ? &pp_font_20 : &pp_font_16;
}'''),
 ('lv_obj_set_style_bg_color(bar, lv_color_hex(0xF7F8FA), 0);', 'lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);'),
 ('lv_obj_set_style_bg_color(btn, lv_color_hex(0xE8F0FE), 0);', 'lv_obj_set_style_bg_color(btn, lv_color_hex(0xE4E4E4), 0);'),
])

pal = [
 ('#define C_BG 0xF7F8FA', '#define C_BG 0xFFFFFF'),
 ('#define C_TEXT 0x202124', '#define C_TEXT 0x000000'),
 ('#define C_DIM 0x5F6368', '#define C_DIM 0x000000'),
 ('#define C_ACCENT 0x1A73E8', '#define C_ACCENT 0x000000'),
 ('#define C_LINE 0xE3E6EA', '#define C_LINE 0xD0D0D0'),
 ('#define C_DONE 0x34A853', '#define C_DONE 0x000000'),
]
for f in ["page_ai.c", "page_files.c", "page_home.c", "page_music.c"]:
    p = base + f
    s = open(p, encoding="utf-8").read()
    for a, b in pal:
        s = s.replace(a, b)
    open(p, "w", encoding="utf-8").write(s)
    print("palette", f)
