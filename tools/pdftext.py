import pypdf, re, sys
for path in [r"D:\Users\PC\Desktop\esp32-mgxd\ref\schematic.pdf"]:
    r = pypdf.PdfReader(path)
    txt = "\n".join((p.extract_text() or "") for p in r.pages)
    print("PAGES", len(r.pages), "CHARS", len(txt))
    for kw in ["ES8311","ES7210","ES8388","I2S","MCLK","BCLK","LRCK","SPK","MIC","NS4150","音频","AMP"]:
        hits = [m.start() for m in re.finditer(kw, txt, re.I)]
        print(f"{kw:10} {len(hits)}")
    open(r"D:\Users\PC\Desktop\esp32-mgxd\ref\schematic.txt","w",encoding="utf-8").write(txt)
