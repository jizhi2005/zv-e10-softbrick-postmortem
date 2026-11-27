import serial, time, sys

PORT = 'COM11'
BAUD = 1500000

cmd = sys.argv[1] if len(sys.argv) > 1 else 'echo hello'
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
tail = int(sys.argv[3]) if len(sys.argv) > 3 else 20000

s = serial.Serial(PORT, BAUD, timeout=5)
time.sleep(0.3)
s.reset_input_buffer()
s.write((cmd + '\r\n').encode())
time.sleep(wait)
out = s.read(tail).decode('utf-8', 'replace')
s.close()
print(out[-4000:])
