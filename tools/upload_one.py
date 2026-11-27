import serial, time, base64, hashlib, sys

PORT = 'COM11'
BAUD = 1500000

local = sys.argv[1] if len(sys.argv) > 1 else 'parity_probe'
remote = sys.argv[2] if len(sys.argv) > 2 else '/tmp/_dyn/parity_probe'

data = open(local, 'rb').read()
md5 = hashlib.md5(data).hexdigest()
print('local size', len(data), 'md5', md5)

s = serial.Serial(PORT, BAUD, timeout=5)
time.sleep(0.3)

s.reset_input_buffer()
s.write(b'mkdir -p /tmp/_dyn\r\n')
time.sleep(0.4)

b64 = base64.b64encode(data).decode()
lines = [b64[i:i+76] for i in range(0, len(b64), 76)]
print('lines', len(lines))

s.write(("cat > %s.b64 << 'ZZEOF9'\r\n" % remote).encode())
time.sleep(0.2)
for ln in lines:
    s.write((ln + '\r\n').encode())
    time.sleep(0.0015)
s.write(b'ZZEOF9\r\n')
time.sleep(0.6)

cmd = ('base64 -d %s.b64 > %s && chmod +x %s && md5sum %s && rm -f %s.b64\r\n'
       % (remote, remote, remote, remote, remote))
s.write(cmd.encode())
time.sleep(3.0)

out = s.read(50000).decode('utf-8', 'replace')
print(out[-1500:])
s.close()
