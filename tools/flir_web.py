#!/usr/bin/env python3
"""flir_web.py - stdlib-only web control panel for a FLIR Boson.

Serves one HTML page; every button POSTs to /api, which runs ./flir_ctl
(one CCI command per call) and returns its JSON line.
"""
import argparse
import http.server
import json
import os
import re
import subprocess
import sys

NUM_RE = re.compile(r"-?(0[xX][0-9a-fA-F]{1,8}|[0-9]{1,10}(\.[0-9]{1,6})?)")
TEXT_RE = re.compile(r"[\x20-\x7e]{1,127}")

NO_VALUE = {"ffc", "fpa", "sn", "sensor-sn", "pn", "sensor-pn", "fw", "status", "ls"}
CONFIRM = {"save", "factory-reset", "reboot"}
# Mirror of flir_params[] in src/cci_params.c: name -> settable?
PARAMS = {
    "ffc-mode": True, "ffc-temp": True, "ffc-period": True,
    "ffc-in-progress": False, "ffc-status": False, "ffc-desired": False,
    "ffc-last-frame": False, "ffc-last-temp-kx10": False,
    "gain": True, "lut": True, "telemetry": True, "freeze": True,
    "fps": False, "frame-count": False, "myriad-temp": False, "low-power": False,
    "overtemp": False, "overtemp-count": False, "lens": False, "table": False,
    "jffs2-state": False,
    "tf": True, "tf-delta-nf": True, "tf-delta-motion": True, "tf-motion-threshold": True,
    "scnr": True, "scnr-th-col-sum": True, "scnr-th-pixel": True, "scnr-max-corr": True,
    "spnr": True, "spnr-sf-min": True, "spnr-sf-max": True, "bpr": True,
    "agc-percent-per-bin": True, "agc-linear-percent": True, "agc-outlier-cut": True,
    "agc-max-gain": True, "agc-df": True, "agc-gamma": True, "agc-detail-headroom": True,
    "agc-d2br": True, "agc-sigma-r": True, "agc-entropy": True,
    "stats-mean": False, "stats-first-bin": False, "stats-last-bin": False,
    "stats-mean-roi": False, "stats-first-bin-roi": False, "stats-last-bin-roi": False,
    "dvo-analog": True, "dvo-format": True, "dvo-display-mode": True,
    "sym": True, "tlinear": True, "radiometry": True, "spotmeter": True,
}
# Multi-value commands: allowed value counts (all numeric).
MULTI = {"agc": {1}, "zoom": {1, 3}, "agc-roi": {0, 4}, "stats-roi": {0, 4},
         "splash-duration": {1, 2}, "splash-bg": {1, 2},
         "sym-rect": {6}, "sym-box": {6}, "sym-line": {6},
         "sym-show": {2}, "sym-delete": {1}, "sym-move": {3}}


def build_argv(ctl, cci, cmd, value=None, values=None, confirm=False):
    """Return the flir_ctl argv for a whitelisted request, else None."""
    if not isinstance(cmd, str):
        return None
    if value is not None:
        if values is not None:
            return None
        values = [value]
    if values is None:
        values = []
    if not isinstance(values, list) or not all(isinstance(v, str) for v in values):
        return None
    num = all(NUM_RE.fullmatch(v) for v in values)
    base = [ctl, "-c", cci, cmd]
    if cmd in CONFIRM:
        return base + ["--confirm"] if confirm is True and not values else None
    if cmd in NO_VALUE:
        return base if not values else None
    if cmd in PARAMS:
        if not values:
            return base
        return base + values if PARAMS[cmd] and len(values) == 1 and num else None
    if cmd in MULTI:
        return base + values if len(values) in MULTI[cmd] and num else None
    if cmd == "sym-text":
        ok = (len(values) == 9 and all(NUM_RE.fullmatch(v) for v in values[:8])
              and TEXT_RE.fullmatch(values[8]))
        return base + values if ok else None
    if cmd == "file-size":
        ok = len(values) == 1 and TEXT_RE.fullmatch(values[0]) and values[0].startswith("/")
        return base + values if ok else None
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
#status{background:#000;border:1px solid #444;padding:8px;overflow-x:auto;font-size:.85em}
.ok{color:#6c6}
.err{color:#f66}
</style></head><body>
<h1>FLIR Boson Control</h1>
<fieldset><legend>Info</legend>
<button onclick="run('fpa')">FPA temp</button>
<button onclick="run('sn')">Camera SN</button>
<button onclick="run('sensor-sn')">Sensor SN</button>
<button onclick="run('pn')">Part number</button>
<button onclick="run('fw')">Firmware</button>
<button onclick="run('sensor-pn')">Sensor PN</button>
<button onclick="showStatus()">Status</button>
<button onclick="readAll()">Read all</button>
<pre id="status"></pre>
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
<p class="note">The AGC mode select uses a Boson+ command (0x00090033) that plain Boson answers with 0x00000161; use Camera AGC below. Zoom sends zoom + centre (max zoom on this unit: 49).</p>
</fieldset>
<fieldset><legend>Modules</legend>
<div>Telemetry <button onclick="run('telemetry','1')">On</button><button onclick="run('telemetry','0')">Off</button></div>
<div>TLinear <button onclick="run('tlinear','1')">On</button><button onclick="run('tlinear','0')">Off</button></div>
<div>Radiometry <button onclick="run('radiometry','1')">On</button><button onclick="run('radiometry','0')">Off</button></div>
<div>Spot meter <button onclick="run('spotmeter','1')">On</button><button onclick="run('spotmeter','0')">Off</button></div>
<p class="note">TLinear, radiometry and spot meter need a radiometric (R) / Boson+ camera; this 6P unit answers 0x00000161.</p>
</fieldset>
<div id="groups"></div>
<fieldset><legend>Symbology</legend>
<div id="sym-row"></div>
<div>
<label>ID <input id="sym-id" size="3"></label>
<label>X <input id="sym-x" size="4"></label>
<label>Y <input id="sym-y" size="4"></label>
<label>W <input id="sym-w" size="4"></label>
<label>H <input id="sym-h" size="4"></label>
<label>Size <input id="sym-size" size="3"></label>
<label>Align <select id="sym-align">
<option value="17">17</option><option value="18">18</option><option value="19">19</option>
<option value="33">33</option><option value="34">34</option><option value="35">35</option>
<option value="49">49</option><option value="50">50</option><option value="51">51</option>
</select></label>
<label>Colour <input id="sym-colour" size="10" value="0xFFFFFFFF"></label>
<label>Text <input id="sym-text" size="16"></label>
<button onclick="drawText()">Draw text</button>
</div>
<div>
<label>ID <input id="sym-cid" size="3"></label>
<button onclick="runv('sym-show', [symId(), '1'])">Show</button>
<button onclick="runv('sym-show', [symId(), '0'])">Hide</button>
<button onclick="runv('sym-delete', [symId()])">Delete</button>
</div>
</fieldset>
<fieldset><legend>Persist (writes camera flash)</legend>
<button onclick="danger('save', 'Write current camera settings to flash as power-up defaults?')">Save settings</button>
<button onclick="danger('factory-reset', 'Load factory defaults into the camera? (Save afterwards to keep them)')">Factory defaults</button>
<button onclick="danger('reboot', 'Reboot the camera? Video stops for a few seconds.')">Reboot</button>
</fieldset>
<h2>Log</h2>
<pre id="log"></pre>
<script>
var HINTS = {"0x00000161": "not supported on this camera variant",
             "0xffffffff": "no response from camera"};
// [legend, [[name, settable, hint?]...], roi command or null]
var GROUPS = [
  ["FFC", [["ffc-mode", true, "0 manual, 1 auto, 2 external"], ["ffc-temp", true, "degC x10"],
           ["ffc-period", true, "s"], ["ffc-status", false], ["ffc-in-progress", false],
           ["ffc-last-frame", false], ["ffc-last-temp-kx10", false]], null],
  ["Camera AGC (8-bit AGC8/NV12 stream only)", [
    ["agc-linear-percent", true], ["agc-percent-per-bin", true], ["agc-outlier-cut", true],
    ["agc-max-gain", true], ["agc-gamma", true], ["agc-df", true],
    ["agc-detail-headroom", true], ["agc-d2br", true], ["agc-sigma-r", true],
    ["agc-entropy", true]], "agc-roi"],
  ["Noise filters (on camera)", [
    ["tf", true], ["tf-delta-nf", true], ["tf-delta-motion", true], ["tf-motion-threshold", true],
    ["scnr", true], ["scnr-th-col-sum", true], ["scnr-th-pixel", true], ["scnr-max-corr", true],
    ["spnr", true], ["spnr-sf-min", true], ["spnr-sf-max", true], ["bpr", true]], null],
  ["System", [["freeze", true], ["fps", false], ["frame-count", false], ["myriad-temp", false],
              ["low-power", false], ["overtemp", false], ["overtemp-count", false],
              ["lens", false], ["table", false], ["jffs2-state", false]], null],
  ["Image statistics", [["stats-mean", false], ["stats-first-bin", false],
    ["stats-last-bin", false], ["stats-mean-roi", false], ["stats-first-bin-roi", false],
    ["stats-last-bin-roi", false]], "stats-roi"],
  ["DVO (CMOS/analog output only)", [
    ["dvo-analog", true], ["dvo-format", true, "0 RGB, 1 YCbCr, 2 default"],
    ["dvo-display-mode", true, "0 continuous, 1 one-shot"]], null]
];

function log(text, cls) {
  var line = document.createElement("div");
  line.className = cls;
  line.textContent = new Date().toLocaleTimeString() + "  " + text;
  var el = document.getElementById("log");
  el.appendChild(line);
  el.scrollTop = el.scrollHeight;
}
async function post(body, label) {
  var d;
  try {
    var r = await fetch("/api", {method: "POST",
      headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)});
    d = await r.json();
  } catch (e) {
    log(label + ": ERROR " + e, "err");
    return undefined;
  }
  if (d.ok) {
    var msg = label + ": OK";
    if (body.value === undefined && d.value !== undefined)
      msg += " -> " + d.value + (body.cmd === "fpa" ? " C" : "");
    log(msg, "ok");
  } else {
    var msg = label + ": ERROR " + d.error;
    if (d.status) msg += " status " + d.status + (HINTS[d.status] ? " (" + HINTS[d.status] + ")" : "");
    if (d.error === "open_failed") msg += " (cannot open CCI device: check --cci path and dialout group)";
    if (d.detail) msg += " - " + d.detail;
    log(msg, "err");
  }
  return d;
}
function run(cmd, value) {
  var label = value === undefined ? cmd : cmd + "=" + value;
  var body = value === undefined ? {cmd: cmd} : {cmd: cmd, value: String(value)};
  return post(body, label);
}
function runv(cmd, values) {
  return post({cmd: cmd, values: values}, cmd + "[" + values.join(" ") + "]");
}
function danger(cmd, msg) {
  if (!confirm(msg)) return;
  return post({cmd: cmd, confirm: true}, cmd);
}
function runSel(id) { run(id, document.getElementById(id).value); }
async function readAll() { for (const c of ["fpa", "sn", "sensor-sn", "pn"]) await run(c); }
async function showStatus() {
  var d = await run("status");
  if (d) document.getElementById("status").textContent = JSON.stringify(d, null, 1);
}
function symId() { return document.getElementById("sym-cid").value; }
function drawText() {
  var ids = ["sym-id", "sym-x", "sym-y", "sym-w", "sym-h", "sym-size",
             "sym-align", "sym-colour", "sym-text"];
  runv("sym-text", ids.map(function (i) { return document.getElementById(i).value; }));
}

function button(text, fn) {
  var b = document.createElement("button");
  b.textContent = text;
  b.onclick = fn;
  return b;
}
function paramRow(name, settable, hint) {
  var row = document.createElement("div");
  var lab = document.createElement("label");
  lab.htmlFor = "p-" + name;
  lab.textContent = name + (hint ? " (" + hint + ")" : "") + " ";
  var input = document.createElement("input");
  input.id = "p-" + name;
  input.size = 10;
  row.appendChild(lab);
  row.appendChild(input);
  row.appendChild(button("Get", async function () {
    var d = await run(name);
    if (d && d.ok && d.value !== undefined) input.value = d.value;
  }));
  if (settable) row.appendChild(button("Set", function () { run(name, input.value); }));
  return row;
}
function roiRow(cmd) {
  var row = document.createElement("div");
  var lab = document.createElement("span");
  lab.textContent = cmd + " (r0 r1 c0 c1) ";
  row.appendChild(lab);
  var inputs = [];
  for (var i = 0; i < 4; i++) {
    var inp = document.createElement("input");
    inp.type = "number";
    inp.size = 5;
    inputs.push(inp);
    row.appendChild(inp);
  }
  row.appendChild(button("Get", async function () {
    var d = await runv(cmd, []);
    if (d && d.ok && Array.isArray(d.value))
      inputs.forEach(function (inp, i) { if (d.value[i] !== undefined) inp.value = d.value[i]; });
  }));
  row.appendChild(button("Set", function () {
    runv(cmd, inputs.map(function (inp) { return inp.value; }));
  }));
  return row;
}
function buildGroups() {
  var host = document.getElementById("groups");
  GROUPS.forEach(function (g) {
    var fs = document.createElement("fieldset");
    var lg = document.createElement("legend");
    lg.textContent = g[0];
    fs.appendChild(lg);
    g[1].forEach(function (p) { fs.appendChild(paramRow(p[0], p[1], p[2])); });
    if (g[2]) fs.appendChild(roiRow(g[2]));
    host.appendChild(fs);
  });
  document.getElementById("sym-row").appendChild(paramRow("sym", true));
}
buildGroups();
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
                          req.get("cmd"), req.get("value"),
                          req.get("values"), req.get("confirm", False))
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
