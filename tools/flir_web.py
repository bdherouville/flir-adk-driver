#!/usr/bin/env python3
"""flir_web.py - stdlib-only web control panel for a FLIR Boson.

Serves one HTML page; every button POSTs to /api, which runs ./flir_ctl
(one CCI command per call) and returns its JSON line.
"""
import argparse
import http.server
import json
import os
import subprocess
import sys

NO_VALUE = {"ffc", "fpa", "sn", "sensor-sn", "pn"}
VALUES = {
    "gain": {str(i) for i in range(5)},
    "lut": {str(i) for i in range(10)},
    "agc": {str(i) for i in range(6)},
    "zoom": {str(i) for i in range(101)},
    "telemetry": {"0", "1"},
    "tlinear": {"0", "1"},
    "radiometry": {"0", "1"},
    "spotmeter": {"0", "1"},
}


def build_argv(ctl, cci, cmd, value):
    """Return the flir_ctl argv for a whitelisted request, else None."""
    if not isinstance(cmd, str):
        return None
    if cmd in NO_VALUE:
        return [ctl, "-c", cci, cmd] if value is None else None
    allowed = VALUES.get(cmd)
    if allowed is not None and isinstance(value, str) and value in allowed:
        return [ctl, "-c", cci, cmd, value]
    return None


def run_ctl(argv):
    """Run flir_ctl; return (http_status, dict)."""
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 500, {"ok": False, "error": "ctl_failed", "detail": str(e)}
    lines = p.stdout.strip().splitlines()
    try:
        out = json.loads(lines[-1])
    except (IndexError, ValueError):
        out = None
    if not isinstance(out, dict):
        return 500, {"ok": False, "error": "ctl_failed",
                     "detail": p.stderr.strip()[-200:]}
    return 200, out


PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FLIR Boson Control</title>
<style>
body{font-family:system-ui,sans-serif;margin:16px;max-width:720px;background:#111;color:#eee}
fieldset{border:1px solid #444;margin:0 0 12px;padding:8px 12px}
legend{color:#fa0}
button,select,input{font:inherit;margin:4px 4px 4px 0;padding:4px 8px}
.note{color:#999;font-size:.85em}
#log{background:#000;border:1px solid #444;padding:8px;height:240px;overflow-y:auto;font-family:monospace;white-space:pre-wrap;margin:0}
.ok{color:#6c6}
.err{color:#f66}
</style></head><body>
<h1>FLIR Boson Control</h1>
<fieldset><legend>Info</legend>
<button onclick="run('fpa')">FPA temp</button>
<button onclick="run('sn')">Camera SN</button>
<button onclick="run('sensor-sn')">Sensor SN</button>
<button onclick="run('pn')">Part number</button>
<button onclick="readAll()">Read all</button>
</fieldset>
<fieldset><legend>Calibration</legend>
<button onclick="run('ffc')">Run FFC</button>
<label>Gain <select id="gain">
<option value="0">High</option><option value="1">Low</option><option value="2">Auto</option>
<option value="3">Dual</option><option value="4">Manual</option>
</select></label><button onclick="runSel('gain')">Apply</button>
</fieldset>
<fieldset><legend>Image</legend>
<label>Color LUT <select id="lut">
<option value="0">White hot</option><option value="1">Black hot</option><option value="2">Rainbow</option>
<option value="3">Rainbow HC</option><option value="4">Ironbow</option><option value="5">Lava</option>
<option value="6">Arctic</option><option value="7">Globow</option><option value="8">Graded fire</option>
<option value="9">Hottest</option>
</select></label><button onclick="runSel('lut')">Apply</button><br>
<label>AGC <select id="agc">
<option value="0">Normal</option><option value="1">Hold</option><option value="2">Threshold</option>
<option value="3">Auto bright</option><option value="4">Auto linear</option><option value="5">Manual</option>
</select></label><button onclick="runSel('agc')">Apply</button><br>
<label>Zoom <input id="zoom" type="number" min="0" max="100" value="1"></label><button onclick="runSel('zoom')">Apply</button>
<p class="note">AGC and zoom need a radiometric (R) variant; a 6P unit answers status 0x00000161.</p>
</fieldset>
<fieldset><legend>Modules</legend>
<div>Telemetry <button onclick="run('telemetry','1')">On</button><button onclick="run('telemetry','0')">Off</button></div>
<div>TLinear <button onclick="run('tlinear','1')">On</button><button onclick="run('tlinear','0')">Off</button></div>
<div>Radiometry <button onclick="run('radiometry','1')">On</button><button onclick="run('radiometry','0')">Off</button></div>
<div>Spot meter <button onclick="run('spotmeter','1')">On</button><button onclick="run('spotmeter','0')">Off</button></div>
<p class="note">TLinear, radiometry and spot meter need a radiometric (R) variant.</p>
</fieldset>
<h2>Log</h2>
<pre id="log"></pre>
<script>
var HINTS = {"0x00000161": "not supported on this camera variant",
             "0xffffffff": "no response from camera"};
function log(text, cls) {
  var line = document.createElement("div");
  line.className = cls;
  line.textContent = new Date().toLocaleTimeString() + "  " + text;
  var el = document.getElementById("log");
  el.appendChild(line);
  el.scrollTop = el.scrollHeight;
}
async function run(cmd, value) {
  var label = value === undefined ? cmd : cmd + "=" + value;
  var body = value === undefined ? {cmd: cmd} : {cmd: cmd, value: String(value)};
  try {
    var r = await fetch("/api", {method: "POST",
      headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)});
    var d = await r.json();
    if (d.ok) {
      var msg = label + ": OK";
      if (value === undefined && d.value !== undefined)
        msg += " -> " + d.value + (cmd === "fpa" ? " C" : "");
      log(msg, "ok");
    } else {
      var msg = label + ": ERROR " + d.error;
      if (d.status) msg += " status " + d.status + (HINTS[d.status] ? " (" + HINTS[d.status] + ")" : "");
      if (d.error === "open_failed") msg += " (cannot open CCI device: check --cci path and dialout group)";
      if (d.detail) msg += " - " + d.detail;
      log(msg, "err");
    }
  } catch (e) {
    log(label + ": ERROR " + e, "err");
  }
}
function runSel(id) { run(id, document.getElementById(id).value); }
async function readAll() { for (const c of ["fpa", "sn", "sensor-sn", "pn"]) await run(c); }
</script></body></html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/":
            self.send_json(404, {"ok": False, "error": "not_found"})
            return
        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/api":
            self.send_json(404, {"ok": False, "error": "not_found"})
            return
        # JSON-only: a cross-site <form> cannot send this content type.
        ctype = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
        if ctype != "application/json":
            self.send_json(415, {"ok": False, "error": "unsupported_media_type"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length <= 0 or length > 1024:
            self.send_json(400, {"ok": False, "error": "bad_request"})
            return
        try:
            req = json.loads(self.rfile.read(length))
        except ValueError:
            req = None
        if not isinstance(req, dict):
            self.send_json(400, {"ok": False, "error": "bad_request"})
            return
        argv = build_argv(self.server.ctl, self.server.cci,
                          req.get("cmd"), req.get("value"))
        if argv is None:
            self.send_json(400, {"ok": False, "error": "bad_request"})
            return
        code, out = run_ctl(argv)
        self.send_json(code, out)


def main():
    default_ctl = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "flir_ctl"))
    ap = argparse.ArgumentParser(description="FLIR Boson web control panel")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--cci", default="/dev/ttyACM0")
    ap.add_argument("--ctl", default=default_ctl)
    args = ap.parse_args()
    if args.host != "127.0.0.1":
        print("warning: no authentication; anyone who can reach this port "
              "can control the camera", file=sys.stderr)
    # Single-threaded on purpose: serializes access to the serial port.
    srv = http.server.HTTPServer((args.host, args.port), Handler)
    srv.ctl = args.ctl
    srv.cci = args.cci
    print(f"Serving on http://{args.host}:{args.port}/", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
