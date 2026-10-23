
import sys
from PIL import Image, ImageDraw, ImageFont
f16 = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 16)
f20 = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 20)
text = "设置 阅读 计划打卡 音乐播放器 文件管理 ABC abc 123"
W,H = 640, 220
img = Image.new("L", (W,H), 255)
d = ImageDraw.Draw(img)
# antialiased 16
d.text((8,8), "AA16: " + text, font=f16, fill=0)
d.text((8,34), "AA20: " + text, font=f20, fill=0)
img.save(r"D:\Users\PC\Desktop\esp32-mgxd\.fontgen\preview_aa.png")
# 1bpp 16
img2 = Image.new("1", (W,H), 1)
d2 = ImageDraw.Draw(img2)
d2.text((8,8), "B16: " + text, font=f16, fill=0)
d2.text((8,40), "B20: " + text, font=f20, fill=0)
img2.convert("L").save(r"D:\Users\PC\Desktop\esp32-mgxd\.fontgen\preview_1bpp.png")
print("ok")

