#!/usr/bin/env python3
"""Screenshots of the real web page (main/web/index.html) and the setup page (HTML taken from main/provision.c),
served by a mock backend that returns INVENTED data. Uses headless Chrome with a throwaway profile, so a running
browser is not touched. Output: PNGs in the given directory. Needs Google Chrome and Pillow."""
import calendar, http.server, json, os, re, subprocess, sys, tempfile, threading
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
os.makedirs(OUT, exist_ok=True)

def ts(day, h, m): return calendar.timegm((2026, 3, day, h, m, 0))

HISTORY = [  # newest first; flags: 1 optimise, 2 scheduled, 4 startup
    (ts(14, 3, 0), "optimal", 12, 2), (ts(13, 3, 0), "optimal", 11, 2), (ts(12, 18, 42), "optimized", 34, 1),
    (ts(12, 18, 40), "optimizable", 13, 0), (ts(12, 3, 0), "error", 0, 2), (ts(11, 3, 0), "optimal", 12, 2),
    (ts(10, 22, 15), "optimal", 12, 4), (ts(10, 3, 0), "optimal", 12, 2), (ts(9, 3, 0), "optimal", 11, 2),
]

def status(scn):
    st = {"state": "optimal", "busy": False, "optimize": False}
    hist = list(HISTORY)
    if scn == "running":
        st.update(state="optimizing", busy=True, optimize=True)
    elif scn == "error":
        st.update(state="error")
        hist.insert(0, (ts(14, 3, 0), "error", 0, 2))
    return {**st, "next": ts(15, 3, 0), "sched_en": 1,
            "wifi": {"ssid": "HomeMesh", "rssi": -52, "channel": 6, "bssid": "02:00:5e:00:53:01", "ip": "192.168.1.50"},
            "mqtt": {"enabled": True, "connected": True},
            "sys": {"heap": 121000, "min_heap": 29000, "largest": 78000, "uptime": 3 * 86400 + 5 * 3600},
            "history": [{"ts": a, "v": b, "dur": c, "flags": d} for a, b, c, d in hist]}

CONFIG = {"wifi_ssid": "HomeMesh", "deco_host": "192.168.1.1", "deco_email": "user@example.com",
          "mqtt_uri": "mqtt://homeassistant.local:1883", "mqtt_user": "deco", "sched_en": True, "sched_h": 3, "sched_m": 0,
          "wifi_pass_set": True, "deco_pass_set": True, "mqtt_pass_set": True, "hostkey_set": True,
          "hostkey": "0a:1b:2c:3d:4e:5f:60:71:82:93:a4:b5:c6:d7:e8:f9:0a:1b:2c:3d"}

class H(http.server.BaseHTTPRequestHandler):
    scenario, dark = "idle", True
    def log_message(self, *a): pass
    def _send(self, body, ctype):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == "/api/status": return self._send(json.dumps(status(H.scenario)), "application/json")
        if self.path == "/api/config": return self._send(json.dumps(CONFIG), "application/json")
        if self.path == "/setup": return self._send(setup_page(), "text/html")
        html = open(os.path.join(ROOT, "main/web/index.html")).read()
        # The page follows the browser's colour scheme, and headless Chrome follows the OS, so pin it:
        # dark = drop the light override, light = apply it unconditionally.
        light = r"@media\(prefers-color-scheme:light\)\{(:root\{[^}]*\})\}"
        html = re.sub(light, "" if H.dark else r"\1", html)
        self._send(html, "text/html")
    def do_POST(self):
        self._send('{"ok":true}', "application/json")

def c_literals(segment):
    return "".join(bytes(x, "utf-8").decode("unicode_escape") for x in re.findall(r'"((?:[^"\\]|\\.)*)"', segment))

def setup_page():
    """Assembles the captive-portal page exactly as provision.c sends it, with an invented network list."""
    src = open(os.path.join(ROOT, "main/provision.c")).read()
    head = c_literals(re.search(r'PAGE_HEAD =(.*?")\s*;\s*\n', src, re.S).group(1))   # ends at ";" after the closing quote, not a ";" in the CSS
    body = src[src.index("static esp_err_t root_get"):]
    body = body[:body.index("return ESP_OK;")]
    calls = re.findall(r"httpd_resp_sendstr_chunk\(r,(.*?)\);", body, re.S)
    out, i = head, 0
    nets = ["HomeMesh", "HomeMesh-Guest", "CafeWifi"]
    for c in calls:
        c = c.strip()
        if c == "PAGE_HEAD" or c == "NULL": continue
        if c.startswith('"<option value='):                 # the loop over scanned networks
            out += "".join('<option value="%s">' % n for n in nets); continue
        if c == '"\\">"': continue
        if c.startswith("s_nets"): continue
        out += c_literals(c)
    return out

def shoot(url, name, width, height, tz="UTC"):
    path = os.path.join(OUT, name + ".png")
    with tempfile.TemporaryDirectory() as profile:
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                        "--no-default-browser-check", "--user-data-dir=" + profile, "--force-device-scale-factor=2",
                        "--window-size=%d,%d" % (width, height), "--virtual-time-budget=8000",
                        "--screenshot=" + path, url], env={**os.environ, "TZ": tz},
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
    im = Image.open(path).convert("RGB")
    bg = im.getpixel((2, 2))
    px = im.load(); last = 0
    for y in range(im.height - 1, -1, -1):          # crop the empty space below the page
        if any(px[x, y] != bg for x in range(0, im.width, 7)): last = y; break
    im.crop((0, 0, im.width, min(im.height, last + 40))).save(path)
    print("wrote", name + ".png")

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = "http://127.0.0.1:%d" % srv.server_port
for name, scn, dark in [("web_idle_dark", "idle", True), ("web_idle_light", "idle", False),
                        ("web_running_dark", "running", True), ("web_error_dark", "error", True)]:
    H.scenario, H.dark = scn, dark
    shoot(base + "/", name, 760, 3600)
H.dark = True
shoot(base + "/setup", "web_setup_dark", 480, 1400)
srv.shutdown()
