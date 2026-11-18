-- vlc_flir.lua — FLIR Boson controls for VLC
-- Install: cp tools/vlc_flir.lua ~/.local/share/vlc/lua/extensions/  then View → FLIR Boson
-- Palette/units/spot go to flir_loop's control file; FFC/gain go to the camera via flir_ctl.

-- Edit these to match your setup
local FLIR_CTL = os.getenv("HOME") .. "/Lab/flir-adk-driver/flir_ctl"
local CCI      = "/dev/ttyACM0"
local CTL_FILE = "/tmp/flir_loop.ctl"

local d, w = nil, {}

function descriptor()
	return { title = "FLIR Boson", version = "1.0", capabilities = {} }
end

local function q(s) return "'" .. tostring(s):gsub("'", "'\\''") .. "'" end

local function flir_ctl(args)
	local p = io.popen(q(FLIR_CTL) .. " -c " .. q(CCI) .. " " .. args .. " 2>&1")
	local out = p and p:read("*a") or "popen failed"
	if p then p:close() end
	w.status:set_text(args .. " → " .. out)
end

local function write_ctl()
	local lut = w.lut:get_value()
	local unit = ({ "C", "F", "K", "R" })[w.unit:get_value()]
	local f = io.open(CTL_FILE .. ".tmp", "w")
	if not f then return w.status:set_text("cannot write " .. CTL_FILE) end
	f:write(string.format("lut=%d unit=%s spot=%d nr=%d\n", lut, unit,
		w.spot:get_checked() and 1 or 0, w.nr:get_checked() and 1 or 0))
	f:close()
	os.rename(CTL_FILE .. ".tmp", CTL_FILE)
	w.status:set_text("display settings applied")
end

function activate()
	d = vlc.dialog("FLIR Boson")
	d:add_label("Palette", 1, 1)
	w.lut = d:add_dropdown(2, 1)
	w.lut:add_value("Iron-bow", 4); w.lut:add_value("White-hot", 0); w.lut:add_value("Black-hot", 1)
	d:add_label("Units", 1, 2)
	w.unit = d:add_dropdown(2, 2)
	w.unit:add_value("°C", 1); w.unit:add_value("°F", 2); w.unit:add_value("K", 3); w.unit:add_value("Raw", 4)
	w.spot = d:add_check_box("Center spot meter", true, 1, 3)
	w.nr = d:add_check_box("Noise reduction", true, 2, 3)
	d:add_button("Apply display", write_ctl, 1, 4, 2)

	d:add_label("Gain (camera)", 1, 5)
	w.gain = d:add_dropdown(2, 5)
	w.gain:add_value("High", 0); w.gain:add_value("Low", 1); w.gain:add_value("Auto", 2)
	d:add_button("Apply gain", function() flir_ctl("gain " .. w.gain:get_value()) end, 1, 6)
	d:add_button("Run FFC", function() flir_ctl("ffc") end, 2, 6)
	d:add_button("FPA temp", function() flir_ctl("fpa") end, 1, 7)
	w.status = d:add_label("", 1, 8, 2)
	d:show()
end

function deactivate() end
function close() vlc.deactivate() end
