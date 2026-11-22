-- MW5 Motion settings panel. Copy this file to the "scripts" folder of the UEVR
-- profile. The panel appears in the UEVR menu under LuaLoader > MW5 Motion.
-- The plugin checks every value, applies it and saves it to mw5_motion.cfg.
local protocol = 1
local command_event = "mw5.motion.command.v1"
local snapshot_event = "mw5.motion.snapshot.v1"

local status = nil
local frame, last_snapshot, last_drawn, last_telemetry = 0, 0, -1000, -1000
local request = 0
local error_text = nil
-- One set of these for the general settings and one for the current chassis.
-- draft: the values that the panel shows. held: sliders that are being dragged.
-- ready: changes that wait to be sent. sent: the request that carried a change,
-- so that an older answer from the plugin cannot undo it on screen.
local function new_store() return {draft = {}, held = {}, ready = {}, sent = {}} end
local general, chassis = new_store(), new_store()

local function send(action, values)
    request = request + 1
    local lines = {tostring(protocol), tostring(request), action}
    if values then
        for key, value in pairs(values) do
            local text = type(value) == "boolean" and (value and "true" or "false") or tostring(value)
            lines[#lines + 1] = key .. "=" .. text
        end
    end
    uevr.api:dispatch_custom_event(command_event, table.concat(lines, "\n"))
    return request
end

local function flush(store, action)
    if not status or not next(store.ready) then return end
    local id = send(action, store.ready)
    for key, _ in pairs(store.ready) do store.sent[key] = id end
    store.ready = {}
end

local function accept(store, values, answered)
    for key, value in pairs(values) do
        local waiting = store.sent[key]
        if waiting and answered >= waiting then store.sent[key], waiting = nil, nil end
        if not waiting and not store.held[key] and store.ready[key] == nil then store.draft[key] = value end
    end
end

uevr.sdk.callbacks.on_lua_event(function(event, text)
    if event ~= snapshot_event then return end
    local ok, value = pcall(json.load_string, text)
    if not ok or type(value) ~= "table" or value.protocol ~= protocol or type(value.settings) ~= "table" then
        error_text = "The panel script and the plugin are different versions. Install matching files."
        return
    end
    local answered = value.request or 0
    if status and status.chassis ~= value.chassis then chassis = new_store() end
    status = value
    last_snapshot = frame
    error_text = value.error ~= "" and value.error or nil
    accept(general, value.settings, answered)
    local own = value.chassis_settings or {}
    accept(chassis, own, answered)
    for key, _ in pairs(chassis.draft) do
        if own[key] == nil and not chassis.held[key] and chassis.ready[key] == nil and not chassis.sent[key] then
            chassis.draft[key] = nil
        end
    end
end)

uevr.sdk.callbacks.on_frame(function()
    frame = frame + 1
    if status and frame - last_snapshot > 900 then status = nil end
    flush(general, "set")
    flush(chassis, "set_chassis")
    local open = frame - last_drawn < 30
    -- The telemetry page asks for fresh values several times a second.
    local watching = frame - last_telemetry < 30
    if (not status and frame % 120 == 1) or (open and frame % 90 == 0) or (watching and frame % 8 == 0) then send("get") end
end)

local function text(s) imgui.text(tostring(s)) end

local function slider(store, label, key, format)
    local meta = status.keys[key]
    if not meta then return end
    local current = store.draft[key]
    if type(current) ~= "number" then current = meta.def end
    local changed, value = imgui.slider_float(label, current, meta.min, meta.max, format or "%.2f")
    -- A slider sends its value when it is released, not while it is dragged.
    local active = imgui.is_item_active()
    if changed then
        store.draft[key], store.held[key] = value, true
    end
    if store.held[key] and not active then
        store.ready[key], store.held[key] = store.draft[key], nil
    end
end

local function check(label, key)
    if not status.keys[key] then return end
    local changed, value = imgui.checkbox(label, general.draft[key] == true)
    if changed then general.draft[key], general.ready[key] = value, value end
end

local function defaults(id, keys)
    if imgui.button("Reset this group to defaults##" .. id) then
        for _, key in ipairs(keys) do
            local meta = status.keys[key]
            if meta then general.draft[key], general.ready[key], general.held[key] = meta.def, meta.def, nil end
        end
    end
end

local function bounce()
    slider(general, "Comfort level (G)", "HeaveComfortG")
    text("Higher: running bounces harder. Lower: running is softer.")
    slider(general, "Heave gain", "HeaveGain")
    text("Higher: all up and down motion is stronger. Lower: weaker.")
    slider(general, "Heave limit (G)", "HeaveLimitG")
    text("Higher: jumps and landings hit harder. Lower: softer.")
    check("Safety net for mechs without bounce data", "HeaveSafetyNet")
    defaults("bounce", {"HeaveComfortG", "HeaveGain", "HeaveLimitG", "HeaveSafetyNet"})
end

local function this_mech()
    if status.chassis == "" then
        text("Enter a mech to set values for its chassis.")
        return
    end
    local own = chassis.draft.HeaveComfortG ~= nil or chassis.draft.HeaveGain ~= nil
    if not own then
        text(status.chassis .. " uses the general bounce values.")
        if imgui.button("Give " .. status.chassis .. " its own values") then
            chassis.draft.HeaveComfortG = general.draft.HeaveComfortG
            chassis.draft.HeaveGain = general.draft.HeaveGain
            chassis.ready.HeaveComfortG = general.draft.HeaveComfortG
            chassis.ready.HeaveGain = general.draft.HeaveGain
        end
        return
    end
    text("These values apply to every " .. status.chassis .. " and replace the general ones.")
    if chassis.draft.HeaveComfortG ~= nil then slider(chassis, "Comfort level (G)##chassis", "HeaveComfortG") end
    if chassis.draft.HeaveGain ~= nil then slider(chassis, "Heave gain##chassis", "HeaveGain") end
    if imgui.button("Remove the values of " .. status.chassis) then
        chassis = new_store()
        send("clear_chassis")
    end
end

local smoothing_keys = {"PositionSmoothingHz", "VelocitySmoothingHz", "AccelerationSmoothingHz", "AngularSmoothingHz"}
local function smoothing()
    text("Higher: motion reacts faster with more jitter. Lower: smoother but slower.")
    slider(general, "Position (Hz)", "PositionSmoothingHz", "%.1f")
    slider(general, "Velocity (Hz)", "VelocitySmoothingHz", "%.1f")
    slider(general, "Acceleration (Hz)", "AccelerationSmoothingHz", "%.1f")
    slider(general, "Rotation (Hz)", "AngularSmoothingHz", "%.1f")
    defaults("smoothing", smoothing_keys)
end

local function jets_and_masc()
    check("Engine Vibrations effect (jump jets, MASC, power)", "HapticEngineEnable")
    text("Vibration levels: higher is faster and smoother, lower is slower and rougher.")
    slider(general, "Jump jets firing", "JumpJetLevel")
    slider(general, "Jump jets warming up", "JumpJetWarmupLevel")
    check("Vibration while MASC runs", "MascVibrationEnable")
    slider(general, "MASC with an empty gauge", "MascVibrationStartLevel")
    slider(general, "MASC at the damage point", "MascVibrationDamageLevel")
    check("Gear Change effect when MASC switches", "HapticGearEnable")
    check("ABS warning taps before MASC damage", "MascWarningAbsEnable")
    slider(general, "First warning taps per second", "MascWarningAbsMinHz", "%.1f")
    slider(general, "Taps per second at the damage point", "MascWarningAbsMaxHz", "%.1f")
    defaults("jets", {"HapticEngineEnable", "JumpJetLevel", "JumpJetWarmupLevel", "MascVibrationEnable",
        "MascVibrationStartLevel", "MascVibrationDamageLevel", "HapticGearEnable", "MascWarningAbsEnable",
        "MascWarningAbsMinHz", "MascWarningAbsMaxHz"})
end

local function power()
    if not status.shaker then text("No MechShaker events found. These effects need MechWarriorVR's bridge.") end
    check("Vibration on power-up and shutdown", "PowerVibrationEnable")
    text("Vibration levels: higher is faster and smoother, lower is slower and rougher.")
    slider(general, "Power-up: first level", "PowerUpStartLevel")
    slider(general, "Power-up: last level", "PowerUpEndLevel")
    slider(general, "Shutdown: first level", "PowerDownStartLevel")
    slider(general, "Shutdown: last level", "PowerDownEndLevel")
    slider(general, "Mission start (seconds)", "PowerInitSeconds", "%.1f")
    slider(general, "Power-up (seconds)", "PowerUpSeconds", "%.1f")
    slider(general, "Shutdown (seconds)", "PowerDownSeconds", "%.1f")
    defaults("power", {"PowerVibrationEnable", "PowerUpStartLevel", "PowerUpEndLevel", "PowerDownStartLevel",
        "PowerDownEndLevel", "PowerInitSeconds", "PowerUpSeconds", "PowerDownSeconds"})
end

local jet_states = {[0] = "off", [1] = "warming up", [2] = "firing", [3] = "cooling down"}
local reader_states = {
    ["ok"] = "reading the mech",
    ["no pawn"] = "no player character yet",
    ["pawn is not a mech"] = "the player is not in a mech",
}
local function number(value, format) return type(value) == "number" and string.format(format, value) or "-" end
local function yes(value) return (value == true or value == 1) and "yes" or "no" end
local function line(label, value) imgui.text(label .. ": " .. tostring(value)) end
local function heading(label)
    imgui.spacing()
    imgui.text_colored(label, 0xFF8FC9EC)
end

local function telemetry()
    last_telemetry = frame
    local f, s = status.frame or {}, status.simhub or {}

    heading("Mech reported to SimHub")
    if status.chassis == "" then
        line("Mech", "none")
    else
        line("Vehicle name sent to SimHub", status.vehicle ~= "" and status.vehicle or "not chosen yet")
        line("Chassis", status.chassis)
        line("Variant", status.variant)
        line("Weight class", status.weight ~= "" and status.weight or "-")
        line("Top speed without MASC", number(status.baseMaxSpeedKmh, "%.1f km/h"))
    end
    line("Game reader", reader_states[status.reader] or status.reader)

    heading("Connection")
    line("SimHub output", s.enabled and ("on, to " .. tostring(s.address)) or "off")
    line("Packets sent", number(s.packets, "%.0f"))
    line("Motion output active", yes(f.OutputActive))
    line("Game paused", yes(f.Paused))
    line("Power effects (MechShaker events)", status.shaker and "available" or "not found")

    heading("Speed")
    line("Speed from cockpit motion", number(f.SpeedKmh, "%.1f km/h"))
    line("Ground speed", number(f.GroundSpeedKmh, "%.1f km/h"))
    line("Speed reported by the game", number(f.GameSpeedKmh, "%.1f km/h"))
    line("Top speed now", number(f.MaxSpeedKmh, "%.1f km/h"))
    line("Throttle", number(f.Throttle, "%.2f"))

    heading("Angles and rotation")
    line("Pitch", number(f.PitchDegrees, "%.1f deg"))
    line("Roll", number(f.RollDegrees, "%.1f deg"))
    line("Yaw", number(f.YawDegrees, "%.1f deg"))
    line("Pitch rate", number(type(f.PitchSpeed) == "number" and f.PitchSpeed * 57.2958 or nil, "%.1f deg/s"))
    line("Roll rate", number(type(f.RollSpeed) == "number" and f.RollSpeed * 57.2958 or nil, "%.1f deg/s"))
    line("Yaw rate", number(type(f.YawSpeed) == "number" and f.YawSpeed * 57.2958 or nil, "%.1f deg/s"))

    heading("Forces (m/s2)")
    line("Heave, up and down", number(f.LocalAccelerationY, "%.2f"))
    line("Surge, forward and back", number(f.LocalAccelerationZ, "%.2f"))
    line("Sway, left and right", number(f.LocalAccelerationX, "%.2f"))

    heading("Comfort cap")
    line("Bounce data for this chassis", yes(f.GaitKnown))
    line("Bounce expected at this speed", number(f.BouncePeakG, "%.2f G"))
    line("Share of the bounce that is sent", number(type(f.HeaveFactor) == "number" and f.HeaveFactor * 100 or nil, "%.0f %%"))
    line("Safety net", number(type(f.SafetyGain) == "number" and f.SafetyGain * 100 or nil, "%.0f %%"))

    heading("Effects sent to SimHub")
    line("Gear (2 while MASC runs)", number(s.gear, "%.0f"))
    line("Engine RPM", number(s.rpm, "%.0f") .. " of " .. number(s.maxRpm, "%.0f"))
    line("ABS warning tap", yes(s.abs))

    heading("MASC, jump jets and heat")
    line("MASC fitted", yes(f.MascInstalled))
    line("MASC running", yes(f.MascEngaged))
    line("MASC gauge", number(f.MascGauge, "%.2f") .. "  (warning " .. number(f.MascWarningPoint, "%.2f")
        .. ", damage " .. number(f.MascDamagePoint, "%.2f") .. ")")
    line("Jump jets fitted", yes(f.JumpJetsInstalled))
    line("Jump jets", jet_states[f.JumpJetState] or "-")
    line("Heat", number(type(f.HeatPercent) == "number" and f.HeatPercent * 100 or nil, "%.0f %%"))
end

local function group(label, draw)
    if imgui.tree_node(label) then
        draw()
        imgui.tree_pop()
    end
end

uevr.sdk.callbacks.on_draw_ui(function()
    if not imgui.tree_node("MW5 Motion") then return end
    last_drawn = frame
    if not status then
        text(error_text or "Waiting for the MW5 Motion plugin.")
        text("The panel needs MW5Motion.dll 0.9.0 or later and a recent UEVR version.")
        if imgui.button("Connect again") then send("get") end
        imgui.tree_pop()
        return
    end
    text("MW5 Motion " .. tostring(status.version))
    if status.chassis == "" then
        text("Mech: none")
    else
        text("Mech: " .. status.chassis .. " " .. status.variant)
        if status.vehicle ~= "" then text("SimHub vehicle name: " .. status.vehicle) end
        if status.cap ~= "" then text("Running bounce: " .. status.cap) end
    end
    if error_text then imgui.text_colored(error_text, 0xFF70BCFF) end
    text("Motion platforms can injure people. Change values in small steps.")
    text("A slider applies when you release it. Changes are saved to mw5_motion.cfg.")
    group("Bounce", bounce)
    local mech_label = status.chassis == "" and "This mech" or ("This mech: " .. status.chassis)
    group(mech_label .. "###thismech", this_mech)
    group("Smoothing", smoothing)
    group("Jump jets and MASC", jets_and_masc)
    group("Power-up and shutdown", power)
    group("Telemetry", telemetry)
    if imgui.button("Read mw5_motion.cfg again") then
        general, chassis = new_store(), new_store()
        send("reload")
    end
    imgui.tree_pop()
end)
