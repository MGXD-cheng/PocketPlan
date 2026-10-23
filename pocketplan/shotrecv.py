import sys, time, serial, base64, re
port = serial.Serial('COM5', 115200, timeout=0.2)
port.dtr = False
port.rts = False
time.sleep(0.3)
port.reset_input_buffer()
port.write(b'shot\n')
buf = bytearray()
deadline = time.time() + 40
while time.time() < deadline:
    chunk = port.read(4096)
    if chunk:
        buf.extend(chunk)
        if b'---PBMEND---' in buf:
            break
port.close()
text = buf.decode('utf-8', 'replace')
start = text.find('---PBMBEGIN---')
end = text.find('---PBMEND---')
if start < 0 or end < 0:
    print('NO DUMP:', text[-500:])
    sys.exit(1)
b64 = text[start + len('---PBMBEGIN---'):end]
b64 = re.sub(r'[^A-Za-z0-9+/=]', '', b64)
data = base64.b64decode(b64)
open('../shot3.pbm', 'wb').write(data)
print('got bytes', len(data), data[:12])
