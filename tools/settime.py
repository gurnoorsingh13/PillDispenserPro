"""Set the dispenser's clock and time zone from this PC over USB (handy when there is no Wi-Fi).

    python tools/settime.py COM11
    python tools/settime.py COM11 --tz "IST-5:30" --name India

Needs pyserial. The time zone defaults to this PC's; the names match the web app's time zone list.
"""
import argparse, json, subprocess, time
import serial

# Windows time zone id -> (POSIX TZ, name in the web app's list)
WIN = {
    'India Standard Time': ('IST-5:30', 'India'),
    'Pakistan Standard Time': ('PKT-5', 'Karachi, Lahore'),
    'Bangladesh Standard Time': ('<+06>-6', 'Dhaka'),
    'Nepal Standard Time': ('<+0545>-5:45', 'Kathmandu'),
    'Arabian Standard Time': ('<+04>-4', 'Dubai, Muscat'),
    'Arab Standard Time': ('<+03>-3', 'Riyadh, Kuwait, Doha'),
    'GMT Standard Time': ('GMT0IST,M3.5.0/1,M10.5.0', 'London, Dublin'),
    'W. Europe Standard Time': ('CET-1CEST,M3.5.0,M10.5.0/3', 'Paris, Berlin, Rome, Madrid'),
    'Eastern Standard Time': ('EST5EDT,M3.2.0,M11.1.0', 'New York, Toronto'),
    'Central Standard Time': ('CST6CDT,M3.2.0,M11.1.0', 'Chicago, Winnipeg'),
    'Pacific Standard Time': ('PST8PDT,M3.2.0,M11.1.0', 'Los Angeles, Vancouver'),
    'UTC': ('UTC0', 'UTC'),
}


def pc_zone():
    try:
        wid = subprocess.run(['tzutil', '/g'], capture_output=True, text=True).stdout.strip()
        if wid in WIN: return WIN[wid]
    except OSError:
        pass
    off = -time.altzone if time.daylight and time.localtime().tm_isdst else -time.timezone  # seconds east of UTC
    h, m = divmod(abs(off) // 60, 60)
    sign = '+' if off >= 0 else '-'
    posix = '<%s%02d%02d>%s%d%s' % (sign, h, m, '-' if off >= 0 else '', h, ':%02d' % m if m else '')
    return posix, 'UTC%s%d%s' % (sign, h, ':%02d' % m if m else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--tz')
    ap.add_argument('--name')
    a = ap.parse_args()
    tz, name = (a.tz, a.name or a.tz) if a.tz else pc_zone()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 2
    s.dtr = s.rts = False  # keep the board running
    s.open()
    time.sleep(0.3)
    s.write(b'J' + json.dumps({'tz': tz, 'tzName': name}).encode() + b'\n')
    time.sleep(0.8)
    s.write(b'T%d\n' % int(time.time()))
    time.sleep(0.8)
    reply = s.read(s.in_waiting or 1).decode('utf-8', 'replace')
    s.close()
    print('Time zone %s (%s), clock %s' % (name, tz, time.strftime('%Y-%m-%d %H:%M:%S')))
    if 'OK T' not in reply: print('No confirmation from the dispenser - is firmware v2.3+ running?')


if __name__ == '__main__':
    main()
