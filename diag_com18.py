"""Diagnostic COM18: send HELLO/GET like Motion Center does and dump raw RX bytes."""
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM18"
ser = serial.Serial(port=PORT, baudrate=115200, timeout=0.02,
                    write_timeout=2.0, dsrdtr=False, rtscts=False)
t0 = time.perf_counter()

def stamp():
    return f"{(time.perf_counter()-t0)*1000:8.1f} ms"

def tx(s):
    ser.write((s + "\n").encode())
    print(f"[{stamp()}] TX> {s}")

def dump_rx(duration):
    end = time.perf_counter() + duration
    while time.perf_counter() < end:
        chunk = ser.read(256)
        if chunk:
            print(f"[{stamp()}] RX  {len(chunk):3d}B: {chunk!r}")

# mimic app sequence: HELLO+GET, then HELLO, then HELLO+GET, then bare GETs
tx("HELLO"); tx("GET")
dump_rx(1.4)
tx("HELLO")
dump_rx(1.2)
tx("HELLO"); tx("GET")
dump_rx(3.0)
for i in range(3):
    tx("GET")
    dump_rx(2.0)
ser.close()
print("done")
