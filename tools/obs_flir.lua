-- obs_flir.lua — FLIR Boson controls for OBS (Tools → Scripts → +)
-- Palette/units/spot go to flir_loop's control file; camera controls (FFC, gain, filters, status, save) go through flir_ctl.
obs = obslua

local cfg = { ctl = script_path() .. "../flir_ctl", cci = "/dev/ttyACM0", file = "/tmp/flir_loop.ctl",
              lut = 4, unit = "C", spot = true, nr = true,
              ffc_mode = 1, tf = true, scnr = true, spnr = true, confirm_save = false }

local function q(s) return "'" .. tostring(s):gsub("'", "'\\''") .. "'" end

local function flir_ctl(args)
	local p = io.popen(q(cfg.ctl) .. " -c " .. q(cfg.cci) .. " " .. args .. " 2>&1")
	local out = p and p:read("*a") or "popen failed"
	if p then p:close() end
	obs.script_log(obs.LOG_INFO, "flir_ctl " .. args .. " → " .. out)
	return out
end

-- write tmp + rename so flir_loop never reads a half-written file
local function write_ctl()
	local tmp = cfg.file .. ".tmp"
	local f = io.open(tmp, "w")
	if not f then return obs.script_log(obs.LOG_WARNING, "cannot write " .. tmp) end
	f:write(string.format("lut=%d unit=%s spot=%d nr=%d\n", cfg.lut, cfg.unit, cfg.spot and 1 or 0, cfg.nr and 1 or 0))
	f:close()
	os.rename(tmp, cfg.file)
end

function script_description()
	return "FLIR Boson: palette/units for flir_loop; FFC, gain, camera filters, status and save via flir_ctl."
end

function script_properties()
	local p = obs.obs_properties_create()
	obs.obs_properties_add_path(p, "ctl", "flir_ctl binary", obs.OBS_PATH_FILE, "", nil)
	obs.obs_properties_add_text(p, "cci", "CCI serial port", obs.OBS_TEXT_DEFAULT)
	obs.obs_properties_add_text(p, "file", "flir_loop control file", obs.OBS_TEXT_DEFAULT)

	local lut = obs.obs_properties_add_list(p, "lut", "Palette", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_INT)
	obs.obs_property_list_add_int(lut, "Iron-bow", 4)
	obs.obs_property_list_add_int(lut, "White-hot", 0)
	obs.obs_property_list_add_int(lut, "Black-hot", 1)

	local unit = obs.obs_properties_add_list(p, "unit", "Units", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_STRING)
	for _, u in ipairs({ { "°C", "C" }, { "°F", "F" }, { "K", "K" }, { "Raw counts", "R" } }) do
		obs.obs_property_list_add_string(unit, u[1], u[2])
	end
	obs.obs_properties_add_bool(p, "spot", "Center spot meter")
	obs.obs_properties_add_bool(p, "nr", "Noise reduction")

	local gain = obs.obs_properties_add_list(p, "gain", "Gain mode (camera)", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_INT)
	for _, g in ipairs({ { "High", 0 }, { "Low", 1 }, { "Auto", 2 } }) do
		obs.obs_property_list_add_int(gain, g[1], g[2])
	end
	obs.obs_properties_add_button(p, "apply_gain", "Apply gain", function() flir_ctl("gain " .. cfg.gain) return false end)
	obs.obs_properties_add_button(p, "ffc", "Run FFC (shutter)", function() flir_ctl("ffc") return false end)
	obs.obs_properties_add_button(p, "fpa", "Log FPA temperature", function() flir_ctl("fpa") return false end)

	local ffc = obs.obs_properties_add_list(p, "ffc_mode", "FFC mode (camera)", obs.OBS_COMBO_TYPE_LIST, obs.OBS_COMBO_FORMAT_INT)
	obs.obs_property_list_add_int(ffc, "Auto", 1)
	obs.obs_property_list_add_int(ffc, "Manual", 0)
	obs.obs_properties_add_button(p, "apply_ffc", "Apply FFC mode", function() flir_ctl("ffc-mode " .. cfg.ffc_mode) return false end)
	obs.obs_properties_add_bool(p, "tf", "Camera temporal filter (TF)")
	obs.obs_properties_add_bool(p, "scnr", "Camera column-noise filter (SCNR)")
	obs.obs_properties_add_bool(p, "spnr", "Camera spatial filter (SPNR)")
	obs.obs_properties_add_button(p, "apply_filters", "Apply camera filters", function()
		for _, k in ipairs({ "tf", "scnr", "spnr" }) do flir_ctl(k .. " " .. (cfg[k] and 1 or 0)) end
		return false
	end)
	obs.obs_properties_add_button(p, "status", "Log camera status", function() flir_ctl("status") return false end)
	obs.obs_properties_add_bool(p, "confirm_save", "Confirm: allow writing settings to camera flash")
	obs.obs_properties_add_button(p, "save", "Save camera settings (flash)", function()
		if cfg.confirm_save then
			flir_ctl("save --confirm")
		else
			obs.script_log(obs.LOG_WARNING, "tick 'Confirm' first: save writes the camera's flash")
		end
		return false
	end)
	return p
end

function script_defaults(s)
	obs.obs_data_set_default_string(s, "ctl", cfg.ctl)
	obs.obs_data_set_default_string(s, "cci", cfg.cci)
	obs.obs_data_set_default_string(s, "file", cfg.file)
	obs.obs_data_set_default_int(s, "lut", cfg.lut)
	obs.obs_data_set_default_string(s, "unit", cfg.unit)
	obs.obs_data_set_default_bool(s, "spot", cfg.spot)
	obs.obs_data_set_default_bool(s, "nr", cfg.nr)
	obs.obs_data_set_default_int(s, "gain", 0)
	obs.obs_data_set_default_int(s, "ffc_mode", 1)
	obs.obs_data_set_default_bool(s, "tf", true)
	obs.obs_data_set_default_bool(s, "scnr", true)
	obs.obs_data_set_default_bool(s, "spnr", true)
	obs.obs_data_set_default_bool(s, "confirm_save", false)
end

function script_update(s)
	cfg.ctl  = obs.obs_data_get_string(s, "ctl")
	cfg.cci  = obs.obs_data_get_string(s, "cci")
	cfg.file = obs.obs_data_get_string(s, "file")
	cfg.lut  = obs.obs_data_get_int(s, "lut")
	cfg.unit = obs.obs_data_get_string(s, "unit")
	cfg.spot = obs.obs_data_get_bool(s, "spot")
	cfg.nr   = obs.obs_data_get_bool(s, "nr")
	cfg.gain = obs.obs_data_get_int(s, "gain")
	cfg.ffc_mode    = obs.obs_data_get_int(s, "ffc_mode")
	cfg.tf          = obs.obs_data_get_bool(s, "tf")
	cfg.scnr        = obs.obs_data_get_bool(s, "scnr")
	cfg.spnr        = obs.obs_data_get_bool(s, "spnr")
	cfg.confirm_save = obs.obs_data_get_bool(s, "confirm_save")
	write_ctl()
end
