
base = r"D:\Users\PC\Desktop\esp32-mgxd\pocketplan\main" + "\\"
def rw(p, pairs):
    p = base + p
    s = open(p, encoding="utf-8").read()
    for a, b in pairs:
        if a not in s:
            raise SystemExit("MISS in " + p + ": " + a[:70])
        s = s.replace(a, b)
    open(p, "w", encoding="utf-8").write(s)
    print("patched", p)

rw("page_ai.c", [
 ('lv_obj_set_style_border_color(bubble, lv_color_hex(user ? 0xC6DAFC : 0xE3E6EA), 0);',
  'lv_obj_set_style_border_color(bubble, lv_color_hex(0xC8C8C8), 0);'),
 ('lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? 0xE8F0FE : 0xFFFFFF), 0);',
  'lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? 0xE4E4E4 : 0xFFFFFF), 0);'),
 ('lv_obj_set_style_bg_color(clear, lv_color_hex(0xF1F3F4), 0);',
  'lv_obj_set_style_bg_color(clear, lv_color_hex(0xE4E4E4), 0);'),
])
rw("page_files.c", [
 ('lv_obj_set_style_text_color(icon, lv_color_hex(entry->dir ? C_ACCENT : 0x9AA0A6), 0);',
  'lv_obj_set_style_text_color(icon, lv_color_hex(0x000000), 0);'),
 ('lv_obj_set_style_bg_color(up, lv_color_hex(0xE8F0FE), 0);',
  'lv_obj_set_style_bg_color(up, lv_color_hex(0xE4E4E4), 0);'),
 ('lv_obj_set_style_bg_color(prev, lv_color_hex(0xE8F0FE), 0);',
  'lv_obj_set_style_bg_color(prev, lv_color_hex(0xE4E4E4), 0);'),
 ('lv_obj_set_style_bg_color(next, lv_color_hex(0xE8F0FE), 0);',
  'lv_obj_set_style_bg_color(next, lv_color_hex(0xE4E4E4), 0);'),
])
rw("page_home.c", [
 ('lv_obj_set_style_bg_color(card, lv_color_hex(plan->done_today ? 0xE9F6EE : C_CARD), 0);',
  'lv_obj_set_style_bg_color(card, lv_color_hex(plan->done_today ? 0xE4E4E4 : C_CARD), 0);'),
 ('lv_obj_set_style_border_color(card, lv_color_hex(plan->done_today ? 0xB7E3C6 : C_LINE), 0);',
  'lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);'),
 ('lv_obj_set_style_border_color(circle, lv_color_hex(plan->done_today ? C_DONE : 0xB0B6BE),',
  'lv_obj_set_style_border_color(circle, lv_color_hex(C_DONE),'),
])
rw("page_music.c", [
 ('lv_obj_set_style_bg_color(s_banner, lv_color_hex(0xFFF4E5), 0);',
  'lv_obj_set_style_bg_color(s_banner, lv_color_hex(0xE4E4E4), 0);'),
 ('lv_obj_set_style_text_color(s_banner, lv_color_hex(0x8A5A00), 0);',
  'lv_obj_set_style_text_color(s_banner, lv_color_hex(0x000000), 0);'),
])
rw("ui.c", [("lv_obj_set_style_border_color(s_nav, lv_color_hex(0xE0E0E0), 0);",
             "lv_obj_set_style_border_color(s_nav, lv_color_hex(0xC8C8C8), 0);")])
