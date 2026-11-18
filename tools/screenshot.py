"""Take screenshots of the dispenser's touch screen over USB.

    python tools/screenshot.py COM11                 # whatever is on screen now -> screens/now.png
    python tools/screenshot.py COM11 h a r o m t     # preview screens with sample data, one PNG each

Keys: h home, a alarm, b dispensing, r pills ready, o SOS, m message, t tray, f refill,
      s settings, w Wi-Fi, i phone setup, p PIN, v idle clock, y party.
Needs pyserial (pip install pyserial). Opening the port does not restart the dispenser.
"""
import os, struct, sys, time, zlib
import serial

NAMES = {'h': 'home', 'a': 'alarm', 'b': 'dispensing', 'r': 'ready', 'o': 'sos', 'm': 'message', 't': 'tray',
         'f': 'refill', 's': 'settings', 'w': 'wifi', 'i': 'phone-setup', 'p': 'pin', 'v': 'clock', 'y': 'party'}
W, H = 320, 240


def png(path, rgb, w, h, scale=2):
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            line += rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * scale
        rows.extend([b'\x00' + bytes(line)] * scale)
    raw = b''.join(rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


class Reader:
    def __init__(self, s, data=b''):
        self.s, self.data, self.deadline = s, data, time.time() + 30

    def take(self, n):
        while len(self.data) < n:
            more = self.s.read(4096)
            if not more and time.time() > self.deadline: raise TimeoutError('screenshot cut short')
            self.data += more
        out, self.data = self.data[:n], self.data[n:]
        return out


def grab(s):
    s.reset_input_buffer()
    s.write(b'S')
    buf, deadline = b'', time.time() + 30
    while b'SCR2 320 240\n' not in buf:
        buf += s.read(256)
        if time.time() > deadline: raise TimeoutError('no screenshot header (is v2.3+ firmware running?)')
    r = Reader(s, buf.split(b'SCR2 320 240\n', 1)[1])
    px = [0] * (W * H)
    while True:
        tag = r.take(1)
        if tag == b'Z': break
        if tag != b'A': raise ValueError('unexpected data in screenshot stream: %r' % tag)
        x1, y1, x2, y2 = struct.unpack('<4H', r.take(8))
        w, i, total = x2 - x1 + 1, 0, (x2 - x1 + 1) * (y2 - y1 + 1)
        while i < total:
            n, lo, hi = r.take(3)
            for k in range(i, i + n):
                px[(y1 + k // w) * W + x1 + k % w] = lo | hi << 8
            i += n
    return px


def to_rgb(px):
    out = bytearray()
    for p in px:
        r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
        out += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    return out


def main():
    port, keys = sys.argv[1], sys.argv[2:]
    os.makedirs('screens', exist_ok=True)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False  # keep the board running
    s.open()
    time.sleep(0.3)
    for k in keys or [None]:
        if k:
            s.write(b'P' + k.encode())
            time.sleep(1.6)
        out = os.path.join('screens', NAMES.get(k, 'now') + '.png')
        png(out, to_rgb(grab(s)), W, H)
        print('saved', out)
    s.write(b'Px')  # end the preview
    s.close()


if __name__ == '__main__':
    main()
