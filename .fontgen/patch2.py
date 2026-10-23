
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

rw("page_settings.c", [
 ('#include "ui.h"', '#include "pp_fonts.h"\n#include "ui.h"'),
 ('''#define C_BG 0xF7F8FA
#define C_CARD 0xFFFFFF
#define C_TEXT 0x202124
#define C_DIM 0x5F6368
#define C_ACCENT 0x1A73E8''',
  '''#define C_BG 0xFFFFFF
#define C_CARD 0xFFFFFF
#define C_TEXT 0x000000
#define C_DIM 0x000000
#define C_ACCENT 0x000000
#define C_CHIP 0xE4E4E4
#define C_EDGE 0xC8C8C8'''),
 ('''static lv_obj_t *s_font16;
static lv_obj_t *s_font14;''',
  '''static lv_obj_t *s_font_large;
static lv_obj_t *s_font_small;'''),
 ('''static void font_clicked(lv_event_t *e)
{
    int size = (int)(intptr_t)lv_event_get_user_data(e);
    pp_config_t *cfg = pp_config();
    cfg->font_size = (uint8_t)size;
    pp_config_save();
    pp_ui_toast(size == 14 ? "字号已设为 14" : "字号已设为 16");
    lv_obj_set_style_bg_color(s_font14, lv_color_hex(size == 14 ? C_ACCENT : 0xE8F0FE), 0);
    lv_obj_set_style_bg_color(s_font16, lv_color_hex(size == 16 ? C_ACCENT : 0xE8F0FE), 0);
    lv_obj_t *l14 = lv_obj_get_child(s_font14, 0);
    lv_obj_t *l16 = lv_obj_get_child(s_font16, 0);
    if (l14) {
        lv_obj_set_style_text_color(l14, lv_color_hex(size == 14 ? 0xFFFFFF : C_ACCENT), 0);
    }
    if (l16) {
        lv_obj_set_style_text_color(l16, lv_color_hex(size == 16 ? 0xFFFFFF : C_ACCENT), 0);
    }
}''',
  '''/* The selected size is shown by filling the chip, never by changing the text
 * colour: every label on this screen stays pure black. */
static void font_mark(int size)
{
    bool large = (size >= 20);
    lv_obj_set_style_bg_color(s_font_small, lv_color_hex(large ? 0xFFFFFF : C_CHIP), 0);
    lv_obj_set_style_bg_color(s_font_large, lv_color_hex(large ? C_CHIP : 0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_font_small, large ? 1 : 0, 0);
    lv_obj_set_style_border_width(s_font_large, large ? 0 : 1, 0);
    lv_obj_set_style_border_color(s_font_small, lv_color_hex(C_EDGE), 0);
    lv_obj_set_style_border_color(s_font_large, lv_color_hex(C_EDGE), 0);
}

static void font_clicked(lv_event_t *e)
{
    int size = (int)(intptr_t)lv_event_get_user_data(e);
    pp_config_t *cfg = pp_config();
    cfg->font_size = (uint8_t)size;
    pp_config_save();
    pp_ui_toast(size >= 20 ? "字号已设为 20" : "字号已设为 16");
    font_mark(size);
}'''),
 ('''    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE8F0FE), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);''',
  '''    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CHIP), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);'''),
 ('''    s_font14 = make_chip(row, "14", font_clicked, (void *)(intptr_t)14);
    s_font16 = make_chip(row, "16", font_clicked, (void *)(intptr_t)16);''',
  '''    s_font_small = make_chip(row, "16", font_clicked, (void *)(intptr_t)16);
    s_font_large = make_chip(row, "20", font_clicked, (void *)(intptr_t)20);'''),
 ('''    lv_obj_set_style_bg_color(s_font14, lv_color_hex(cfg->font_size == 14 ? C_ACCENT : 0xE8F0FE), 0);
    lv_obj_set_style_bg_color(s_font16, lv_color_hex(cfg->font_size == 16 ? C_ACCENT : 0xE8F0FE), 0);''',
  '''    font_mark(cfg->font_size);'''),
])

rw("config.c", [
 ('    c->font_size = 16;', '    c->font_size = 16;'),
 ('''    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "font_size");''',
  '''    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "font_size");'''),
 ('        s_cfg.font_size = (f->valueint <= 14) ? 14 : 16;',
  '        /* 16 = normal UI font, 20 = large reader font; anything older (14/16)\n         * loads as 16. */\n        s_cfg.font_size = (f->valueint >= 20) ? 20 : 16;'),
])
