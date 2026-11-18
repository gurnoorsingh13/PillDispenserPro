"""Fake PillDispenser for previewing web/index.html on a PC (no device needed).
Usage: python tools/mock_server.py web/index.html 8701 [idle|alert|ready|sos|fresh|low|demo|party|shabad]
Then open http://127.0.0.1:8701"""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE, PORT = sys.argv[1], int(sys.argv[2])
SCEN = sys.argv[3] if len(sys.argv) > 3 else 'idle'
now = int(time.time())
lt = time.localtime(now)
midnight = now - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec)

slots = [
    {"on": True, "h": 8, "m": 0, "days": 127, "note": "1 tablet - after breakfast"},
    {"on": True, "h": 14, "m": 0, "days": 127, "note": ""},
    {"on": True, "h": 21, "m": 30, "days": 62, "note": "after dinner"},
    {"on": False, "h": 12, "m": 0, "days": 127, "note": ""},
]
people = ["Biji", "Magenta", "Violet", "Blue", "Cyan", "Green", "Yellow", "Orange"]
cfg = {"hostname": "pilldisp", "trays": 1, "tz": "IST-5:30", "tzName": "India", "volume": 20, "mute": False, "brightness": 80,
       "invert": False, "clockSaver": True, "screenTimeout": 120, "quiet": False, "quietFrom": 22, "quietTo": 7, "quietVolume": 8,
       "remindEvery": 1, "snoozeMin": 10, "escalateAfter": 15, "missAfter": 120, "requireConfirm": True, "confirmTimeout": 30,
       "minGap": 120, "earlyWindow": 90, "maxPerDay": 0, "checkin": True, "checkinH": 11, "checkinM": 0, "capacity": 30,
       "lowStockDays": 3, "pin": "", "pinDispense": False, "pinSettings": True, "webPass": "", "tgToken": "********",
       "tgPatient": "123", "tgCare": "456", "tgDue": True, "tgDaily": True, "dailyH": 21, "dailyM": 0, "ntfyTopic": "",
       "waPhone": "+919876543210", "waKey": "********", "callUser": "", "hookUrl": "", "servoRest": 70, "servoPush": 180,
       "pushMs": 300, "restMs": 500, "cyclesPerDose": 2, "ssid": "realme 12x 5G", "pass": "********", "ssid2": "", "pass2": "",
       "people": people, "tray": [{"name": "Amlodipine 5 mg", "person": 0, "alerts": True, "voice": 0, "slots": slots}]}

hist = []
for d in range(1, 12):
    base = midnight - d * 86400
    for si, (h, m) in enumerate([(8, 0), (14, 0)]):
        t = base + h * 3600 + m * 60
        hist.append([t, 1, 1, si, 0])
        hist.append([t + 120, 2, 1, si, 0])
        ev = 5 if (d == 3 and si == 1) else 4 if d == 6 and si == 0 else 3
        hist.append([t + 300, ev, 1, si, 0])
t8 = midnight + 8 * 3600
hist += [[t8, 1, 1, 0, 0], [t8 + 90, 2, 1, 0, 0], [t8 + 240, 3, 1, 0, 0]]
hist.sort()


def status():
    tray = {"name": "Amlodipine 5 mg", "person": 0, "alerts": True, "stock": 18, "days": 7, "last": t8 + 90, "ev": 3, "evT": t8 + 240,
            "sched": "08:00 daily  14:00 daily  21:30 Mon-Fri", "st": 0, "slot": 1, "due": 0, "snooze": 0}
    s = {"v": "2.4.0", "team": "Maverick Innovators", "members": "Gurnoor Singh, Ansh Sharma, Samarpit Gujral, Harman Singh Gill", "host": "pilldisp", "ip": "192.168.1.42", "ssid": "realme 12x 5G", "rssi": -58, "ap": False, "timeOk": True,
         "now": int(time.time()), "tzName": "India", "up": 5400, "heap": 92000, "mode": "idle", "alertPerson": -1, "readyPerson": -1,
         "busy": False, "servoOk": True, "trays": 1, "cap": 30, "tg": True, "safe": False, "ai": SCEN != 'fresh', "demo": int(time.time()) + 42 if SCEN == 'demo' else 0, "ntest": "WhatsApp: sent", "chan": 5,
         "people": people, "tray": [tray], "next": {"tray": 1, "slot": 1, "t": midnight + 14 * 3600 if lt.tm_hour < 14 else midnight + 86400 + 8 * 3600},
         "msg": {"text": "I will visit you today", "from": "Gurnoor", "at": int(time.time()) - 900, "read": int(time.time()) - 600}}
    if SCEN == 'sos':
        s["sos"] = {"at": int(time.time()) - 120, "ack": "Gurnoor"}
    if SCEN == 'alert':
        s["mode"] = "alert"; s["alertPerson"] = 0; tray.update(st=1, slot=1, due=int(time.time()) - 420)
    if SCEN == 'ready':
        s["mode"] = "ready"; s["readyPerson"] = 0; tray.update(st=3, slot=1)
    if SCEN == 'fresh':
        s.update(timeOk=False, chan=0, ssid="", ap=True, apSsid="PillDispenser-FC08", ip="192.168.4.1"); s.pop("msg", None); s.pop("next", None)
        tray.update(stock=4, days=1)
    if SCEN == 'party':
        s["party"] = 1
    if SCEN == 'shabad':
        s["party"] = 2
    if SCEN == 'low':
        tray.update(stock=3, days=1)
    return s


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        p = self.path.split('?')[0]
        if p == '/':
            html = open(PAGE, encoding='utf-8').read()
            trap = "<script>addEventListener('load',()=>{const m=location.search.match(/do=([^&]+)/);if(m)setTimeout(()=>eval(decodeURIComponent(m[1])),1200)})</script><script>window.__errs=[];window.onerror=(m,s,l,c)=>{__errs.push(m+' @'+l+':'+c);document.documentElement.setAttribute('data-err',__errs.join(' | '))};window.addEventListener('unhandledrejection',e=>{__errs.push('promise: '+e.reason);document.documentElement.setAttribute('data-err',__errs.join(' | '))});</script>"
            return self.send(200, html.replace('<head>', '<head>' + trap, 1), "text/html; charset=utf-8")
        if p == '/frame':
            h=self.path.split('h=')[-1]
            return self.send(200, '<html><body style="margin:0;background:#888;display:flex;gap:16px;padding:8px">'+''.join(f'<iframe src="/#{t}" style="width:390px;height:{h}px;border:0;background:#fff"></iframe>' for t in self.path.split('t=')[1].split('&')[0].split(','))+'</body></html>', "text/html")
        if p == '/hx':
            from urllib.parse import unquote
            us=unquote(self.path.split('u=')[1]).split('|')
            hh = self.path.split('h=')[1].split('&')[0] if 'h=' in self.path.split('u=')[0] else '900'
            return self.send(200, '<html><body style="margin:0;background:#888;display:flex;gap:16px;padding:8px">'+''.join(f'<iframe src="{u}" style="width:390px;height:{hh}px;border:0"></iframe>' for u in us)+'</body></html>', "text/html")
        if p == '/api/status': return self.send(200, json.dumps(status()))
        if p == '/api/config':
            if SCEN == 'fresh':
                c = json.loads(json.dumps(cfg)); c["people"][0] = "Red"; c["tray"][0]["name"] = "Tray 1"
                for sl in c["tray"][0]["slots"]: sl["on"] = False
                return self.send(200, json.dumps(c))
            return self.send(200, json.dumps(cfg))
        if p == '/api/ai':
            return self.send(200, json.dumps({"key": "" if SCEN == 'fresh' else "gsk_mock", "model": "openai/gpt-oss-120b", "vision": "qwen/qwen3.8-27b"}))
        if p == '/api/wifi/scan':
            return self.send(200, json.dumps([{"ssid": "Home", "rssi": -52, "open": False}, {"ssid": "Neighbour 5G", "rssi": -74, "open": False}, {"ssid": "Cafe", "rssi": -80, "open": True}]))
        if p == '/api/history': return self.send(200, json.dumps(hist))
        return self.send(404, '{"ok":false,"msg":"not found"}')

    def do_POST(self):
        return self.send(200, '{"ok":true,"msg":"ok"}')


ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
