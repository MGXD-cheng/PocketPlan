
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

rw("page_home.c", [
 ('lv_obj_set_style_bg_color(circle, lv_color_hex(plan->done_today ? C_DONE : 0xFFFFFF), 0);',
  'lv_obj_set_style_bg_color(circle, lv_color_hex(0xFFFFFF), 0);'),
 ('''    lv_obj_set_style_border_color(circle, lv_color_hex(C_DONE),
                                  0);''',
  '''    lv_obj_set_style_border_color(circle, lv_color_hex(plan->done_today ? C_DONE : 0xC8C8C8),
                                  0);'''),
 ('lv_obj_set_style_text_color(ok, lv_color_hex(0xFFFFFF), 0);',
  'lv_obj_set_style_text_color(ok, lv_color_hex(0x000000), 0);'),
])
rw("page_ai.c", [
 ('''    lv_obj_set_style_bg_color(s_send, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_send, 0, 0);''',
  '''    lv_obj_set_style_bg_color(s_send, lv_color_hex(0xE4E4E4), 0);
    lv_obj_set_style_border_width(s_send, 1, 0);
    lv_obj_set_style_border_color(s_send, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(s_send, 0, 0);'''),
 ('lv_obj_set_style_text_color(sl, lv_color_hex(0xFFFFFF), 0);',
  'lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);'),
])
