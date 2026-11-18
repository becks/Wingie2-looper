-- wingie monitor: the Wingie2 Loop Monitor on norns, shared by the script
-- (wingie_monitor.lua) and the mod (lib/mod.lua). Both load it with require, so
-- with the mod enabled there is one copy, one serial port and one set of pages.
--
-- Same serial protocol as Tools/loop_monitor.html (framing "@{json}\n" out,
-- "<{json}\n" in, 115200 8N1). Unlike the browser monitor, opening the port
-- from norns does not reset the Wingie2 (checked on hardware: uptime kept).
--
-- Nothing here uses clock.run: loading any script cancels every clock and frees
-- every metro (Script.clear), and this module outlives scripts. Retries and timed
-- messages run from the poll metro, which exists only while a page is showing, so
-- the Wingie2 is not polled while another script has the screen.

local json = require "wingie_monitor/lib/json"

local core = {}

local STATUS_INTERVAL = 0.15   -- s, as the browser monitor
local WAVE_INTERVAL = 0.5      -- s, while recording or overdubbing
local REQUEST_TIMEOUT = 1.0    -- s
local HELLO_DEADLINE = 10      -- s after the port opens

local MODES = {"Poly", "String", "Bar", "Cave", "Ratio", "Piano", "Tube", "Membrane", "Bell",
  "Glass", "Gong", "Plate", "Fifths", "Major 9", "Minor 11", "Air", "Chorus", "Tanpura", "Custom"}
local SPEED_STEPS = {1/4, 1/2, 2/3, 3/4, 1, 4/3, 3/2, 2}
local SPEED_LABELS = {"1/4", "1/2", "2/3", "3/4", "1", "4/3", "3/2", "2"}
local SPEED_MODES = {"vari", "pitch", "strch"}
local FILTER_TYPES = {"off", "LP", "HP", "BP"}
-- What the note keys play (firmware with key_target): resonator, loop (transposed) or both.
local KEY_TARGETS = {"reson", "loop", "both"}
-- Gate modes (firmware with gate_modes; older firmware has off/on).
local GATE_MODES = {"off", "hold", "1shot", "trig"}
local SIDE_NAMES = {"left", "right"}
-- One LOOP page per loop (A, then B): its keys and encoders act on that loop.
local PAGES = {"LOOP A", "LOOP B", "CTRL", "NOTES", "PANEL", "METERS", "SYS"}
local KEY_NAMES = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
local BLACK = {[1] = true, [3] = true, [6] = true, [8] = true, [10] = true}
local NOTES_BADGES = {[0] = "--", "REC", "PLAY", "STOP"}
local LEVEL_RATE = 44100 / 64 / 8   -- level entries per second
local LEVEL_HISTORY = 84            -- strip width in pixels, about 1 s
local CLIP_HOLD = 1.5               -- s
local LATCH_LABELS = {[0] = "off", "on", "locked"}

-- serial device and transport
local dev = nil
local device_attrs = "-"
local rx = ""
local next_id = 1
local queue = {}        -- requests waiting to be sent
local inflight = nil    -- {id, op, sent, cb}
local connected = false
local opened_at = 0
local hello_at = nil    -- time of the next hello while connecting
local firmware = ""
local has_state = false   -- firmware accepts set_param looper_state
local has_window = false  -- firmware has the playback window (looper_start/end)
local has_filter = false  -- firmware has the side output filter (filter_*)
local has_keys = false    -- firmware has key_target (the keys can play the loop)
local has_gate_modes = false  -- looper_gate 0-3 (off, hold, one-shot, trigger)

-- device model
local status = nil
local status_at = 0
local sides = {
  {peaks = nil, wave_length = -1, wave_state = -1, wave_at = 0, wave_busy = false,
   notes = {}, notes_revision = -1, notes_busy = false},
  {peaks = nil, wave_length = -1, wave_state = -1, wave_at = 0, wave_busy = false,
   notes = {}, notes_revision = -1, notes_busy = false},
}

-- ui
local page = 1
local message = "waiting for Wingie2…"
local message_until = nil   -- time a temporary message goes away

local function say(text, seconds)
  message = text
  message_until = seconds and util.time() + seconds or nil
end

-- diagnostics
local stats = {rtt = 0, rtt_max = 0, polls = 0, timeouts = 0, errors = 0, resets = 0, last_ms = nil}

local poll_metro, redraw_metro

----------------------------------------------------------------------------
-- transport

local function send_next()
  if inflight or not dev or #queue == 0 then return end
  local req = table.remove(queue, 1)
  req.sent = util.time()
  inflight = req
  serial.send(dev, "@" .. req.line .. "\n")
end

local function request(op, fields, cb)
  local id = next_id
  next_id = next_id + 1
  local obj = {v = 1, id = id, op = op}
  for k, v in pairs(fields or {}) do obj[k] = v end
  local req = {id = id, op = op, obj = obj, line = json.encode(obj), cb = cb}
  table.insert(queue, req)
  send_next()
  return req
end

local function queued(op)
  if inflight and inflight.op == op then return true end
  for _, r in ipairs(queue) do if r.op == op then return true end end
  return false
end

local function receive(text)
  local ok, payload = pcall(json.decode, text)
  if not ok or type(payload) ~= "table" then
    stats.errors = stats.errors + 1
    return
  end
  if not inflight or payload.id ~= inflight.id then return end
  local req = inflight
  inflight = nil
  local rtt = util.time() - req.sent
  stats.rtt = rtt
  if rtt > stats.rtt_max then stats.rtt_max = rtt end
  if req.cb then req.cb(payload.ok == true, payload) end
  send_next()
end

local function on_data(data)
  rx = rx .. data
  if #rx > 16384 then rx = rx:sub(-4096) end
  while true do
    local nl = rx:find("\n", 1, true)
    if not nl then break end
    local line = rx:sub(1, nl - 1):gsub("\r$", "")
    rx = rx:sub(nl + 1)
    if line:sub(1, 1) == "<" then receive(line:sub(2)) end
  end
end

local function check_timeout()
  if inflight and util.time() - inflight.sent > REQUEST_TIMEOUT then
    local req = inflight
    inflight = nil
    stats.timeouts = stats.timeouts + 1
    if req.cb then req.cb(false, {error = {code = "timeout"}}) end
    send_next()
  end
end

----------------------------------------------------------------------------
-- protocol

local function hello()
  request("hello", {}, function(ok, r)
    if ok and r.device == "Wingie2" then
      connected = true
      firmware = r.firmware or "?"
      has_state, has_window, has_filter, has_keys, has_gate_modes = false, false, false, false, false
      for _, c in ipairs(r.capabilities or {}) do
        if c == "looper_state" then has_state = true end
        if c == "looper_window" then has_window = true end
        if c == "side_filter" then has_filter = true end
        if c == "key_target" then has_keys = true end
        if c == "gate_modes" then has_gate_modes = true end
      end
      say("")
      for _, s in ipairs(sides) do s.wave_length = -1 end
    elseif util.time() - opened_at < HELLO_DEADLINE then
      say("Wingie2 is starting…")
      hello_at = util.time() + 0.3
    else
      say(ok and "not a Wingie2" or ("no answer: " .. ((r.error and r.error.code) or "?")))
    end
  end)
end

local function refresh_wave(i, now)
  local s = sides[i]
  local a = status.sides[i].a
  local state, length = a[1], a[2]
  local writing = state == 1 or state == 3
  local changed = length ~= s.wave_length or state ~= s.wave_state
  if s.wave_busy or not (changed or (writing and now - s.wave_at > WAVE_INTERVAL)) then return end
  if length == 0 then
    s.peaks, s.wave_length, s.wave_state = nil, 0, state
    return
  end
  s.wave_busy = true
  s.wave_at = now
  request("loop_wave", {side = SIDE_NAMES[i]}, function(ok, w)
    s.wave_busy = false
    if not ok then return end
    local peaks = {}
    for k = 1, #w.peaks - 1, 2 do peaks[#peaks + 1] = tonumber(w.peaks:sub(k, k + 1), 16) end
    s.peaks, s.wave_length, s.wave_state = peaks, w.length, w.state
  end)
end

-- Note loop events, read in pages; a revision change mid-read retries on the next poll.
local function refresh_notes(i)
  local s = sides[i]
  local revision = status.sides[i].n[5]
  if s.notes_busy or revision == s.notes_revision then return end
  s.notes_busy = true
  local events = {}
  local function page_from(offset)
    request("loop_notes", {side = SIDE_NAMES[i], offset = offset}, function(ok, r)
      if not ok then s.notes_busy = false; return end
      if r.revision ~= revision and #events > 0 then s.notes_busy = false; return end
      for _, e in ipairs(r.events) do events[#events + 1] = {time = e[1], key = e[2]} end
      if #r.events > 0 and #events < r.count then return page_from(#events) end
      s.notes, s.notes_revision, s.notes_busy = events, r.revision, false
    end)
  end
  page_from(0)
end

-- Input L/R and output L/R level entries: queued as they arrive with each poll,
-- played out at their own rate by redraw for a steady scroll.
local levels = {queue = {{}, {}, {}, {}}, history = {{}, {}, {}, {}}, clip_until = {0, 0, 0, 0},
  budget = 0, last = 0}

local function queue_levels(lv)
  if type(lv) ~= "table" then return end
  for ch = 1, 4 do
    local hex = lv[ch] or ""
    local q = levels.queue[ch]
    for k = 1, #hex - 1, 2 do q[#q + 1] = tonumber(hex:sub(k, k + 1), 16) end
  end
end

local function advance_levels(now)
  local elapsed = levels.last > 0 and math.min(0.25, now - levels.last) or 0
  levels.last = now
  levels.budget = levels.budget + elapsed * LEVEL_RATE
  local backlog = #levels.queue[1]
  if backlog > LEVEL_RATE * 0.5 then levels.budget = levels.budget + backlog - LEVEL_RATE * 0.25 end
  local take = math.min(math.floor(levels.budget), backlog)
  levels.budget = levels.budget - take
  if backlog == 0 then levels.budget = math.min(levels.budget, 1) end
  for _ = 1, take do
    for ch = 1, 4 do
      local code = table.remove(levels.queue[ch], 1)
      if code then
        if code >= 255 then levels.clip_until[ch] = now + CLIP_HOLD end
        local h = levels.history[ch]
        h[#h + 1] = code
        if #h > LEVEL_HISTORY then table.remove(h, 1) end
      end
    end
  end
end

local sync_params  -- defined with the norns params

-- A loop_status answer: the model, waveforms and notes, levels, params.
local function on_status(ok, r)
  if not ok then
    local code = r.error and r.error.code
    if code ~= "timeout" then say("loop_status: " .. tostring(code)) end
    return false
  end
  -- The firmware uptime going backwards means the Wingie2 rebooted.
  if stats.last_ms and r.ms < stats.last_ms then stats.resets = stats.resets + 1 end
  stats.last_ms = r.ms
  stats.polls = stats.polls + 1
  status = r
  status_at = util.time()
  for i = 1, 2 do
    refresh_wave(i, status_at)
    refresh_notes(i)
  end
  queue_levels(r.lv)
  sync_params()
  return true
end

local function poll()
  check_timeout()
  if dev and not connected and hello_at and util.time() >= hello_at then
    hello_at = nil
    hello()
  end
  if not connected or queued("loop_status") then return end
  request("loop_status", {}, on_status)
end

-- MIDI-mapped params can arrive while no page is showing, so without the poll
-- metro: time out a lost answer here, and say hello first if not yet connected.
local function wake()
  check_timeout()
  if dev and not connected and not queued("hello") then
    hello_at = nil
    hello()
  end
end

-- A knob sends many values: one still waiting in the queue for the same target and
-- name takes the new value instead of queueing another request.
local function set_param(target, name, value)
  if not dev then return end
  wake()
  for _, r in ipairs(queue) do
    if r.op == "set_param" and r.obj.target == target and r.obj.name == name then
      r.obj.value = value
      r.line = json.encode(r.obj)
      return
    end
  end
  request("set_param", {target = target, name = name, value = value}, function(ok, r)
    if not ok then say(name .. ": " .. tostring(r.error and r.error.code), 2) end
    poll()
  end)
end

-- Runs fn with a fresh status: the polled one while a page shows, else asked for.
local STATUS_FRESH = 0.5
local function with_status(fn)
  if status and util.time() - status_at < STATUS_FRESH then return fn() end
  if not dev then return end
  wake()
  request("loop_status", {}, function(ok, r)
    if on_status(ok, r) then fn() end
  end)
end

----------------------------------------------------------------------------
-- serial device

local function attrs_text(attrs)
  local parts = {}
  for k, v in pairs(attrs) do parts[#parts + 1] = k .. "=" .. tostring(v) end
  table.sort(parts)
  return table.concat(parts, " ")
end

-- CH340 (1a86, current boards) or CP210x (10c4, earlier boards).
local function is_bridge(attrs)
  local t = attrs_text(attrs):lower()
  for _, p in ipairs({"1a86", "qinheng", "ch34", "usb_serial", "usb2.0%-serial", "10c4", "cp210", "silicon_labs"}) do
    if t:find(p) then return true end
  end
  return false
end

-- Linux keeps the baud rate in the CBAUD bits of c_cflag, and matron (250926)
-- applies cflag after cfsetispeed/cfsetospeed: the speed has to be in cflag too,
-- or the port stays at its previous rate (9600 after plug-in).
local CBAUD = 0x100F

local function configure(term)
  term.ispeed = serial.speed.B115200
  term.ospeed = serial.speed.B115200
  -- raw 8N1, no flow control, no echo, no line translation
  term.cflag = (term.cflag & ~(CBAUD | serial.cflag.CSIZE | serial.cflag.PARENB | serial.cflag.CSTOPB))
    | serial.speed.B115200 | serial.cflag.CS8 | serial.cflag.CREAD | serial.cflag.CLOCAL
  term.lflag = term.lflag & ~(serial.lflag.ICANON | serial.lflag.ECHO | serial.lflag.ECHOE
    | serial.lflag.ISIG | serial.lflag.IEXTEN)
  term.iflag = term.iflag & ~(serial.iflag.IXON | serial.iflag.IXOFF | serial.iflag.ICRNL
    | serial.iflag.INCLR | serial.iflag.ISTRIP)
  term.oflag = term.oflag & ~serial.oflag.OPOST
  return term
end

-- norns matches serial devices only when udev reports them (hotplug), and keeps
-- handlers and open ports across script reloads. So: the handler table remembers
-- its open port in `_dev`; a new copy of this module takes the port over (no reopen,
-- so no Wingie2 reset), and a device already plugged in is re-announced to udev.
local HANDLER_ID = "wingie2"

local function attach(d, fresh)
  dev = d
  rx = ""
  queue, inflight = {}, nil
  connected = false
  stats.last_ms = nil
  opened_at = util.time()
  hello_at = opened_at + (fresh and 0.5 or 0)
  -- requests dropped above never call back: nothing may stay marked as in progress
  for _, s in ipairs(sides) do
    s.wave_busy, s.notes_busy, s.notes_revision = false, false, -1
  end
  say(fresh and "Wingie2 is starting…" or "connecting…")
end

local function rescan()
  -- Only USB-serial bridges of the Wingie2's kind, so grids and crow are left alone.
  -- The USB vendor is read from sysfs (tty -> interface -> device).
  os.execute("for t in /sys/class/tty/ttyUSB*; do [ -e \"$t\" ] || continue; "
    .. "v=$(cat \"$(readlink -f \"$t/device\")/../../idVendor\" 2>/dev/null); "
    .. "case \"$v\" in 1a86|10c4) sudo -n udevadm trigger --action=add \"$t\";; esac; done")
end

local registered = false

function core.register()
  if registered then return end
  registered = true
  local old = serial._handlers[HANDLER_ID]
  local kept = old and old._dev
  serial._handlers[HANDLER_ID] = nil
  local handler
  handler = {
    id = HANDLER_ID,
    _dev = kept,
    match = function(attrs)
      print("wingie monitor: serial device " .. attrs_text(attrs))
      local ok = is_bridge(attrs)
      if ok then device_attrs = attrs_text(attrs) end
      return ok
    end,
    configure = configure,
    add = function(id, name, d)
      print("wingie monitor: connected " .. tostring(name))
      handler._dev = d
      attach(d, true)
    end,
    remove = function(id)
      print("wingie monitor: disconnected")
      handler._dev = nil
      dev, connected, status = nil, false, nil
      queue, inflight = {}, nil
      say("Wingie2 unplugged")
    end,
    event = function(id, data) on_data(data) end,
  }
  serial.add_handler(handler)
  if kept then attach(kept, false) end
end

----------------------------------------------------------------------------
-- model helpers

local function side_status(i) return status and status.sides and status.sides[i] end

local function nearest_step(speed)
  local best, dist = 5, math.huge
  for k, v in ipairs(SPEED_STEPS) do
    local d = math.abs(math.log(speed / v))
    if d < dist then best, dist = k, d end
  end
  return best
end

local function speed_label(speed)
  local k = nearest_step(speed)
  if math.abs(speed - SPEED_STEPS[k]) < 0.005 then return SPEED_LABELS[k] end
  return string.format("%.2f", speed)
end

-- Playback window frames as the firmware computes them (at least WINDOW_MIN frames).
local WINDOW_MIN = 64
local function window_frames(length, start, finish)
  local b = math.min(length, math.floor(start * length))
  local e = math.min(length, math.floor(finish * length + 0.5))
  local min = math.min(length, WINDOW_MIN)
  if e < b + min then
    e = b + min
    if e > length then e, b = length, length - min end
  end
  return b, e - b
end

-- Playback wraps inside the window; a position outside it (the window just moved)
-- stays as polled until the Wingie2 has jumped.
local function audio_position(a, elapsed)
  local state, length, position, speed, reverse = a[1], a[2], a[3], a[5], a[7] == 1
  if (state ~= 2 and state ~= 3) or length == 0 then return position end
  local begin, span = window_frames(length, a[12] or 0, a[13] or 1)
  local relative = position - begin
  if span == 0 or relative < 0 or relative >= span then return position end
  local fps = (status and status.fps) or 4900
  local p = relative + elapsed * fps * speed * (reverse and -1 or 1)
  return begin + p % span
end

local function audio_badge(state, length)
  if state == 1 then return "REC" end
  if state == 2 then return "PLAY" end
  if state == 3 then return "DUB" end
  return length > 0 and "BYP" or "--"
end

----------------------------------------------------------------------------
-- controls

-- A value just sent is shown (and used as the base for the next encoder step)
-- until the polled status has had time to catch up.
local PENDING_HOLD = 0.6
local pending = {}

local function current(row, side, value)
  local p = pending[row.name .. side]
  if p and util.time() - p.at < PENDING_HOLD then return p.value end
  return value
end

local function onoff(v) return v == 1 and "on" or "off" end
local function pct(v) return tostring(math.floor(v * 100 + 0.5)) end

local current_window  -- defined after ROWS

-- Window edge as seconds into the recording ("-" without a recording).
local function window_label(v, side)
  local s = side_status(side)
  if not s or s.a[2] == 0 then return "-" end
  local fps = (status and status.fps) or 4900
  return string.format("%.2fs", v * s.a[2] / fps)
end

-- One encoder step of a window edge: 1/1000 of the recording, more when turned
-- fast; the edges keep the firmware's minimum length between them.
local function window_step(v, d, side, edge)
  local s = side_status(side)
  if not s or s.a[2] == 0 or s.a[1] == 1 then return v end
  local min = math.min(1, WINDOW_MIN / s.a[2])
  local nv = v + d * math.abs(d) * 0.001
  local start, finish = current_window(side)
  if edge == "start" then return util.clamp(nv, 0, math.max(0, finish - min)) end
  return util.clamp(nv, math.min(1, start + min), 1)
end

-- Rows of the CTRL page. get(s) reads the side's loop_status entry (or the shared
-- frame), set(v, d) turns the current value one encoder step.
local ROWS = {
  {label = "speed", name = "speed_step",
   get = function(s) return nearest_step(s.a[5]) - 1 end,
   show = function(v) return SPEED_LABELS[v + 1] end,
   step = function(v, d) return util.clamp(v + d, 0, #SPEED_STEPS - 1) end},
  {label = "spd mode", name = "speed_mode",
   get = function(s) return s.a[9] or 0 end,
   show = function(v) return SPEED_MODES[v + 1] end,
   step = function(v, d) return util.clamp(v + d, 0, #SPEED_MODES - 1) end},
  {label = "sos", name = "sos",
   get = function(s) return s.a[8] end, show = pct,
   step = function(v, d) return util.clamp(math.floor(v * 100 + 0.5 + d) / 100, 0, 1) end},
  -- loop level, in the firmware's 1/127 steps (MIDI CC 82)
  {label = "level", name = "looper_level",
   get = function(s) return s.a[6] end, show = pct,
   step = function(v, d) return util.clamp(math.floor(v * 127 + 0.5 + d) / 127, 0, 1) end},
  {label = "mode", name = "mode",
   get = function(s) return s.m end,
   show = function(v) return MODES[v + 1] or "?" end,
   step = function(v, d) return util.clamp(v + d, 0, #MODES - 1) end},
  {label = "keys", name = "key_target", keys = true,
   get = function(s) return s.kt or 0 end,
   show = function(v) return KEY_TARGETS[v + 1] or "?" end,
   step = function(v, d) return util.clamp(v + d, 0, #KEY_TARGETS - 1) end},
  {label = "pan", name = "pan",
   get = function(s) return s.x[4] or 0 end, show = pct,
   step = function(v, d) return util.clamp(math.floor(v * 100 + 0.5 + d) / 100, 0, 1) end},
  {label = "gate", name = "looper_gate",
   get = function(s) return s.a[11] or 0 end,
   show = function(v) return GATE_MODES[v + 1] or "?" end,
   step = function(v, d) return util.clamp(v + d, 0, has_gate_modes and #GATE_MODES - 1 or 1) end},
  -- playback window edges, shares of the recording; fast turns move further
  {label = "start", name = "looper_start", window = true,
   get = function(s) return s.a[12] or 0 end,
   show = function(v, side) return window_label(v, side) end,
   step = function(v, d, side) return window_step(v, d, side, "start") end},
  {label = "end", name = "looper_end", window = true,
   get = function(s) return s.a[13] or 1 end,
   show = function(v, side) return window_label(v, side) end,
   step = function(v, d, side) return window_step(v, d, side, "end") end},
  -- side output filter: type, cutoff (a semitone per step, more when turned fast), resonance
  {label = "filter", name = "filter_type", filter = true,
   get = function(s) return s.f and s.f[1] or 0 end,
   show = function(v) return FILTER_TYPES[v + 1] or "?" end,
   step = function(v, d) return util.clamp(v + d, 0, #FILTER_TYPES - 1) end},
  {label = "cutoff", name = "filter_cutoff", filter = true,
   get = function(s) return s.f and s.f[2] or 1000 end,
   show = function(v) return v >= 1000 and string.format("%.1fk", v / 1000) or string.format("%d", math.floor(v + 0.5)) end,
   step = function(v, d) return util.clamp(math.floor(v * 2 ^ (d * math.abs(d) / 12) + 0.5), 20, 16000) end},
  {label = "reso", name = "filter_resonance", filter = true,
   get = function(s) return s.f and s.f[3] or 0 end, show = pct,
   step = function(v, d) return util.clamp(math.floor(v * 100 + 0.5 + d) / 100, 0, 1) end},
  {label = "position", name = "looper_post",
   get = function(s) return s.a[10] or 0 end,
   show = function(v) return v == 1 and "post" or "pre" end,
   step = function(v, d) return d > 0 and 1 or 0 end},
  {label = "live input", name = "looper_live_input", shared = true,
   get = function(st) return st.live or 0 end, show = onoff,
   step = function(v, d) return d > 0 and 1 or 0 end},
  {label = "latch", name = "looper_latch", shared = true,
   get = function(st) return st.latch or 0 end,
   show = function(v) return LATCH_LABELS[v] or "?" end,
   -- 2 (locked) is a panel state: the setting itself is on/off
   step = function(v, d) return d > 0 and 1 or 0 end},
  -- Applied at once, kept over power-off only after a Save (config page or
  -- holding both Mode buttons for 3 s), like the other configuration settings.
  {label = "line in", name = "line_input_mono", shared = true,
   get = function(st) return st.mono or 0 end,
   show = function(v) return v == 1 and "mono" or "stereo" end,
   step = function(v, d) return d > 0 and 1 or 0 end},
}
local row_sel = 1
-- The loop a LOOP page acts on (1 A, 2 B), nil on the other pages.
local function loop_page()
  if PAGES[page] == "LOOP A" then return 1 end
  if PAGES[page] == "LOOP B" then return 2 end
end

local function row_named(name)
  for _, row in ipairs(ROWS) do if row.name == name then return row end end
end

-- The window as shown: a value just sent, else the polled one.
current_window = function(side)
  local s = side_status(side)
  if not s then return 0, 1 end
  return current(row_named("looper_start"), side, s.a[12] or 0),
         current(row_named("looper_end"), side, s.a[13] or 1)
end

-- Looper state as on the Wingie2 (MIDI CC 80): 0 stop/bypass, 1 recording,
-- 2 playback, 3 overdub. A command just sent is used for the next press.
local function loop_state(side)
  local a = side_status(side).a
  local p = pending["looper_state" .. side]
  if p and util.time() - p.at < PENDING_HOLD then return p.value, a[2] end
  return a[1], a[2]
end

local function send_state(side, state)
  if not has_state then
    say("flash firmware with looper_state", 2)
    return
  end
  pending["looper_state" .. side] = {value = state, at = util.time()}
  set_param(SIDE_NAMES[side], "looper_state", state)
end

-- K2: record; while recording, close the loop and play it; then play <-> overdub.
local function record_or_overdub(side)
  local state = loop_state(side)
  if state == 0 then send_state(side, 1)
  elseif state == 2 then send_state(side, 3)
  else send_state(side, 2) end
end

-- K3: stop a playing or overdubbing loop (it is kept), play a stopped one;
-- while recording, close the loop and play it.
local function stop_or_play(side)
  local state, length = loop_state(side)
  if state == 2 or state == 3 then send_state(side, 0)
  elseif state == 1 or length > 0 then send_state(side, 2) end
end

local function row_value(row, side)
  if row.shared then return current(row, 0, row.get(status)) end
  return current(row, side, row.get(side_status(side)))
end

local function change_row(row, side, d)
  if row.filter and not has_filter then
    say("flash firmware with the filter", 2)
    return
  end
  if row.keys and not has_keys then
    say("flash firmware with key_target", 2)
    return
  end
  if row.window and not has_window then
    say("flash firmware with the loop window", 2)
    return
  end
  local v = row.step(row_value(row, side), d, side)
  if row.name == "looper_latch" and v == 1 and row_value(row, side) == 2 then return end
  if v == row_value(row, side) then return end
  local key_side = row.shared and 0 or side
  pending[row.name .. key_side] = {value = v, at = util.time()}
  set_param(row.shared and "shared" or SIDE_NAMES[side], row.name, v)
end

function core.enc(n, d)
  if n == 1 then
    page = util.clamp(page + d, 1, #PAGES)
    return
  end
  if not status then return end
  local side = loop_page()
  if side then
    -- E2 the start of the playback window, E3 its end.
    change_row(row_named(n == 2 and "looper_start" or "looper_end"), side, d)
    return
  end
  if PAGES[page] ~= "CTRL" then return end
  -- E2 the A (left) column, E3 the B (right) one; shared rows take either.
  change_row(ROWS[row_sel], n == 2 and 1 or 2, d)
end

function core.key(n, z)
  if z == 0 then return end
  local side = loop_page()
  if side then
    if not status then return end
    if n == 2 then record_or_overdub(side)
    elseif n == 3 then stop_or_play(side) end
  elseif PAGES[page] == "CTRL" and n > 1 then
    row_sel = util.clamp(row_sel + (n == 2 and -1 or 1), 1, #ROWS)
  elseif n == 3 and PAGES[page] == "SYS" then
    for _, w in ipairs(sides) do w.wave_length = -1; w.notes_revision = -1 end
    stats.rtt_max = 0
  end
end

----------------------------------------------------------------------------
-- norns params: the controls in the PARAMS menu (group WINGIE2), so that they can
-- be mapped to a MIDI controller with norns' MIDI learn (PARAMS > MAP). A param
-- sends its value as the CTRL page does; the polled status moves the params
-- (silently) to what the Wingie2 has, so the panel and the pages stay in step.

local function unit_spec(default)
  return controlspec.new(0, 1, "lin", 0.01, default, "")
end

local function step_spec(min, max, step, default)
  return controlspec.new(min, max, "lin", step, default, "", step / (max - min))
end

local OFF_ON = {"off", "on"}

-- kind: "control" (spec, wire = value), "option" (wire = index - 1), "binary"
-- (toggle, wire 0/1). get(s, st, i) reads the value in wire units from the side's
-- loop_status entry s, the whole status st and the side index i.
local SIDE_PARAMS = {
  {id = "mode", name = "mode", kind = "option", options = MODES, get = function(s) return s.m end},
  {id = "key_target", name = "keys play", kind = "option", options = KEY_TARGETS,
   get = function(s) return s.kt end},
  {id = "volume", name = "volume", kind = "control", spec = function() return unit_spec(0.25) end,
   get = function(s) return s.x[3] end},
  {id = "mix", name = "mix", kind = "control", spec = function() return unit_spec(1) end,
   get = function(s) return s.x[1] end},
  {id = "decay", name = "decay", kind = "control",
   spec = function() return controlspec.new(0.1, 10, "exp", 0.01, 5, "s") end,
   get = function(s) return s.x[2] end},
  {id = "threshold", name = "threshold", kind = "control",
   spec = function() return step_spec(0.0825, 0.99, 0.0825, 0.0825) end,
   get = function(s, st, i) return st.t and st.t[i] end},
  {id = "pan", name = "pan", kind = "control", spec = function(i) return unit_spec(i == 1 and 0 or 1) end,
   get = function(s) return s.x[4] end},
  {id = "filter_type", name = "filter", kind = "option", options = FILTER_TYPES,
   get = function(s) return s.f and s.f[1] end},
  {id = "filter_cutoff", name = "cutoff", kind = "control",
   spec = function() return controlspec.new(20, 16000, "exp", 1, 1000, "Hz") end,
   get = function(s) return s.f and s.f[2] end},
  {id = "filter_resonance", name = "resonance", kind = "control", spec = function() return unit_spec(0) end,
   get = function(s) return s.f and s.f[3] end},
  {id = "speed_step", name = "speed", kind = "option", options = SPEED_LABELS, default = 5,
   get = function(s) return nearest_step(s.a[5]) - 1 end},
  {id = "speed_mode", name = "speed mode", kind = "option", options = SPEED_MODES,
   get = function(s) return s.a[9] end},
  {id = "sos", name = "sos", kind = "control", spec = function() return unit_spec(0.8) end,
   get = function(s) return s.a[8] end},
  {id = "looper_level", name = "loop level", kind = "control", spec = function() return unit_spec(1) end,
   get = function(s) return s.a[6] end},
  {id = "looper_start", name = "loop start", kind = "control", spec = function() return unit_spec(0) end,
   get = function(s) return s.a[12] end},
  {id = "looper_end", name = "loop end", kind = "control", spec = function() return unit_spec(1) end,
   get = function(s) return s.a[13] end},
  {id = "looper_post", name = "loop post", kind = "binary", get = function(s) return s.a[10] end},
  {id = "looper_gate", name = "loop gate", kind = "option", options = GATE_MODES,
   get = function(s) return s.a[11] end},
}

local SHARED_PARAMS = {
  {id = "pre_clip_gain", name = "pre clip gain", kind = "control",
   spec = function() return step_spec(0.0825, 0.99, 0.0825, 0.495) end,
   get = function(st) return st.g and st.g[1] end},
  {id = "post_clip_gain", name = "post clip gain", kind = "control",
   spec = function() return step_spec(0.385, 0.99, 0.055, 0.495) end,
   get = function(st) return st.g and st.g[2] end},
  {id = "looper_live_input", name = "live input", kind = "binary", get = function(st) return st.live end},
  -- 2 (locked) is a panel state: the setting itself is on/off
  {id = "looper_latch", name = "latch", kind = "binary",
   get = function(st) return st.latch and math.min(st.latch, 1) end},
  {id = "line_input_mono", name = "line in mono", kind = "binary", get = function(st) return st.mono end},
}

-- Loop transport, as K2/K3 on the LOOP pages, and clear (MIDI CC 80 = 4).
local SIDE_TRIGGERS = {
  {id = "rec", name = "rec / dub", run = function(side) record_or_overdub(side) end},
  {id = "stop", name = "stop / play", run = function(side) stop_or_play(side) end},
  {id = "clear", name = "clear", run = function(side) send_state(side, 4) end},
}

local PARAM_PREFIX = "wingie_"
local LETTERS = {"a", "b"}
local defined = {}     -- param id -> {p = spec, side = 1, 2 or 0 (shared)}
local touched = {}     -- param id -> time it was last changed here
local known = {}       -- param id -> value last sent or read from the Wingie2

local function param_id(p, side)
  return PARAM_PREFIX .. p.id .. (side > 0 and ("_" .. LETTERS[side]) or "")
end

local function to_wire(p, v)
  if p.kind == "option" then return v - 1 end
  return v
end

local function from_wire(p, w)
  if p.kind == "option" then return math.floor(w + 0.5) + 1 end
  if p.kind == "binary" then return w > 0 and 1 or 0 end
  return w
end

-- A value the Wingie2 already has is not sent: params:bang() from a script then
-- does not push stale values over what was played on the panel.
local function send_param(id, v)
  local d = defined[id]
  if known[id] == v then return end
  known[id] = v
  touched[id] = util.time()
  local w = to_wire(d.p, v)
  pending[d.p.id .. d.side] = {value = w, at = util.time()}
  set_param(d.side > 0 and SIDE_NAMES[d.side] or "shared", d.p.id, w)
end

local function add_param(p, side)
  local id = param_id(p, side)
  local name = (side > 0 and (side == 1 and "A " or "B ") or "") .. p.name
  if p.kind == "option" then
    params:add_option(id, name, p.options, p.default or 1)
  elseif p.kind == "binary" then
    params:add_binary(id, name, "toggle", 0)
  else
    params:add_control(id, name, p.spec(side))
  end
  params:set_action(id, function(v) send_param(id, v) end)
  -- the Wingie2 keeps these: not in the script's PSETs
  params:set_save(id, false)
  defined[id] = {p = p, side = side}
  known[id] = params:get(id)
end

-- Only a real change is set: a mapping with echo on sends its CC on every set.
local function differs(id, p, v)
  if p.kind ~= "control" then return params:get(id) ~= v end
  local param = params:lookup_param(id)
  return math.abs(param.raw - param.controlspec:unmap(v)) > 0.0005
end

-- Moves the params to the polled status, without their actions, except one just
-- changed here (the Wingie2 has not caught up yet).
sync_params = function()
  if not params or not status or not status.sides then return end
  local now = util.time()
  for id, d in pairs(defined) do
    if params.lookup[id] and not (touched[id] and now - touched[id] < PENDING_HOLD) then
      local w
      if d.side > 0 then
        local s = side_status(d.side)
        w = s and d.p.get(s, status, d.side)
      else
        w = d.p.get(status)
      end
      if w ~= nil and differs(id, d.p, from_wire(d.p, w)) then
        params:set(id, from_wire(d.p, w), true)
        known[id] = params:get(id)
      end
    end
  end
end

-- Mappings of these params in a file of their own, so that with the mod they follow
-- from one script to the next (norns keeps one map per script).
local PMAP_FILE = "wingie_monitor/params.pmap"
local saved_maps = {}   -- id -> mapping, as last saved
local kept_maps = {}    -- ids not restored (their CC is mapped by the script): kept in the file

local function pmap_path() return _path.data .. PMAP_FILE end

local function copy_map(m)
  local c = {}
  for k, v in pairs(m) do c[k] = v end
  return c
end

local function save_maps()
  local out = {}
  for id in pairs(kept_maps) do out[id] = saved_maps[id] end
  for id, m in pairs(norns.pmap.data) do
    if type(id) == "string" and id:sub(1, #PARAM_PREFIX) == PARAM_PREFIX then out[id] = copy_map(m) end
  end
  util.make_dir(_path.data .. "wingie_monitor")
  tab.save(out, pmap_path())
  saved_maps = out
end

local function restore_maps()
  saved_maps = tab.load(pmap_path()) or {}
  kept_maps = {}
  local rev = norns.pmap.rev
  for id, m in pairs(saved_maps) do
    if params.lookup[id] and not norns.pmap.data[id] then
      if rev[m.dev] and rev[m.dev][m.ch] and rev[m.dev][m.ch][m.cc] == nil then
        norns.pmap.data[id] = copy_map(m)
        rev[m.dev][m.ch][m.cc] = id
      else
        kept_maps[id] = true
      end
    end
  end
end

-- norns writes the script's map whenever a mapping changes: save ours with it.
local function hook_pmap_write()
  local pmap = norns.pmap
  if pmap._wingie_write then return end
  local write = pmap.write
  pmap._wingie_write = true
  pmap.write = function(...)
    write(...)
    local ok, err = pcall(save_maps)
    if not ok then print("wingie monitor: saving MIDI maps failed: " .. tostring(err)) end
  end
end

-- Adds the WINGIE2 group to the PARAMS menu; once per script (the mod calls it
-- after every script's init, the script from its own).
function core.add_params()
  if not params or params.lookup[param_id(SIDE_PARAMS[1], 1)] then return end
  defined, touched, known = {}, {}, {}
  params:add_group("wingie_monitor", "WINGIE2", 2 * (#SIDE_PARAMS + #SIDE_TRIGGERS) + #SHARED_PARAMS)
  for side = 1, 2 do
    for _, p in ipairs(SIDE_PARAMS) do add_param(p, side) end
    for _, t in ipairs(SIDE_TRIGGERS) do
      -- a binary trigger, not add_trigger: norns can map only the former to MIDI
      local id = param_id(t, side)
      params:add_binary(id, (side == 1 and "A " or "B ") .. t.name, "trigger")
      params:set_action(id, function() with_status(function() t.run(side) end) end)
      params:set_save(id, false)
    end
  end
  for _, p in ipairs(SHARED_PARAMS) do add_param(p, 0) end
  restore_maps()
  hook_pmap_write()
  sync_params()
end

----------------------------------------------------------------------------
-- drawing

local function draw_wave(i, top, height, elapsed)
  local s = side_status(i)
  local w = sides[i]
  local middle = top + math.floor(height / 2)
  screen.level(1)
  screen.move(0, middle + 0.5)
  screen.line(128, middle + 0.5)
  screen.stroke()
  if not s then return end
  local a = s.a
  local state, length, capacity = a[1], a[2], a[4]
  if capacity == 0 then return end
  local span = state == 1 and capacity or math.max(length, 1)
  local drawn = 128 * math.min(1, w.wave_length > 0 and w.wave_length / span or 0)
  -- the playback window, in screen columns (whole loop while recording)
  local wl, wr = 0, 128
  local windowed = has_window and state ~= 1 and length > 0
  if windowed then
    local start, finish = current_window(i)
    local begin, wspan = window_frames(length, start, finish)
    wl, wr = math.floor(begin / length * 128), math.floor((begin + wspan) / length * 128)
    windowed = begin > 0 or begin + wspan < length
  end
  if w.peaks and #w.peaks > 0 and w.wave_length > 0 then
    local count = #w.peaks
    local half = height / 2 - 1
    for x = 0, math.floor(drawn) - 1 do
      local inside = x >= wl and x < wr
      screen.level(inside and (state == 0 and 5 or 10) or 2)
      local lo = math.floor(x * count / drawn) + 1
      local hi = math.max(lo, math.floor((x + 1) * count / drawn))
      local peak = 0
      for k = lo, math.min(hi, count) do if w.peaks[k] > peak then peak = w.peaks[k] end end
      local h = math.max(0.5, peak / 127 * half)
      screen.move(x + 0.5, middle - h)
      screen.line(x + 0.5, middle + h + 1)
      screen.stroke()
    end
  end
  if windowed then
    -- edge marks: brackets at the top and bottom of the wave
    screen.level(12)
    screen.rect(wl, top, 1, height); screen.fill()
    screen.rect(math.max(0, wr - 1), top, 1, height); screen.fill()
    screen.rect(wl, top, 3, 1); screen.fill()
    screen.rect(wl, top + height - 1, 3, 1); screen.fill()
    screen.rect(math.max(0, wr - 3), top, 3, 1); screen.fill()
    screen.rect(math.max(0, wr - 3), top + height - 1, 3, 1); screen.fill()
  end
  if state == 1 then
    screen.level(3)
    local x = 128 * math.min(1, length / capacity)
    screen.rect(math.floor(x), top, 1, height)
    screen.fill()
  elseif (state == 2 or state == 3) and length > 0 then
    screen.level(15)
    local x = math.floor(audio_position(a, elapsed) / length * 128)
    screen.rect(x, top, 1, height)
    screen.fill()
  end
end

local function draw_side_header(i, y)
  local s = side_status(i)
  -- on a LOOP page its loop is bright, the other dim
  local selected = loop_page()
  local active = not selected or i == selected
  if selected and active then
    screen.level(4)
    screen.rect(0, y - 6, 6, 7)
    screen.fill()
  end
  screen.level(active and 15 or 4)
  screen.move(1, y)
  screen.text(i == 1 and "A" or "B")
  if not s then return end
  local a = s.a
  local fps = (status and status.fps) or 4900
  screen.level(active and 10 or 4)
  screen.move(8, y)
  screen.text(audio_badge(a[1], a[2]))
  screen.move(30, y)
  screen.text(string.format("%.1fs", a[2] / fps))
  screen.move(54, y)
  -- a step just sent shows at once; otherwise the polled (possibly MIDI-continuous) speed
  local p = pending["speed_step" .. i]
  local speed = (p and util.time() - p.at < PENDING_HOLD) and SPEED_STEPS[p.value + 1] or a[5]
  screen.text("x" .. speed_label(speed) .. (a[7] == 1 and "<" or ""))
  screen.move(128, y)
  local ps, pe = pending["looper_start" .. i], pending["looper_end" .. i]
  local now = util.time()
  if selected == i and a[2] > 0 and ((ps and now - ps.at < 1.5) or (pe and now - pe.at < 1.5)) then
    -- the window edges in seconds while they are being moved
    local start, finish = current_window(i)
    local begin, span = window_frames(a[2], start, finish)
    screen.text_right(string.format("%.2f-%.2f", begin / fps, (begin + span) / fps))
  elseif selected then
    -- both loops show their level on the LOOP pages
    screen.text_right("vol " .. pct(current(row_named("looper_level"), i, a[6])))
  else
    screen.text_right(MODES[s.m + 1] or "?")
  end
end

-- CTRL page: label, then the A and B columns (shared rows span both).
local CTRL_VISIBLE = 6

local function draw_ctrl()
  local first = util.clamp(row_sel - 2, 1, math.max(1, #ROWS - CTRL_VISIBLE + 1))
  screen.level(15)
  screen.move(88, 7)
  screen.text_right("A")
  screen.move(126, 7)
  screen.text_right("B")
  for k = 0, CTRL_VISIBLE - 1 do
    local index = first + k
    local row = ROWS[index]
    if not row then break end
    local y = 18 + k * 9
    local selected = index == row_sel
    if selected then
      screen.level(2)
      screen.rect(0, y - 7, 128, 9)
      screen.fill()
    end
    screen.level(selected and 15 or 6)
    screen.move(2, y)
    screen.text(row.label)
    if row.shared then
      screen.move(126, y)
      screen.text_right(row.show(row_value(row, 0), 0))
    else
      for side = 1, 2 do
        screen.level(selected and 15 or 6)
        screen.move(side == 1 and 88 or 126, y)
        screen.text_right(row.show(row_value(row, side), side))
      end
    end
  end
  -- scroll hint
  screen.level(3)
  if first > 1 then screen.move(64, 9); screen.text_center("^") end
end

local function notes_position(n, elapsed)
  local state, length_ms, position_ms = n[1], n[3], n[4]
  if state == 1 then return position_ms + elapsed * 1000 end
  if state ~= 2 or length_ms == 0 then return 0 end
  return (position_ms + elapsed * 1000) % length_ms
end

-- One side's note loop: header, then 12 rows of 2 px (C at the bottom).
local function draw_notes_side(i, y, elapsed)
  local s = side_status(i)
  local n = s.n
  screen.level(15)
  screen.move(0, y)
  screen.text(i == 1 and "A" or "B")
  screen.level(10)
  screen.move(8, y)
  screen.text(NOTES_BADGES[n[1]] or "?")
  screen.move(34, y)
  screen.text(n[2] .. " ev")
  screen.move(128, y)
  screen.text_right(n[3] > 0 and string.format("%.2fs", n[3] / 1000) or "-")
  local top, bottom = y + 2, y + 2 + 24
  for key = 0, 11 do
    if BLACK[key] then
      screen.level(1)
      screen.rect(0, bottom - (key + 1) * 2, 128, 2)
      screen.fill()
    end
  end
  if n[1] == 0 then return end
  local position = notes_position(n, elapsed)
  local span = n[1] == 1 and math.max(4000, position * 1.25) or math.max(n[3], 1)
  screen.level(n[1] == 1 and 15 or 10)
  for _, e in ipairs(sides[i].notes) do
    local x = math.floor(e.time / span * 128)
    screen.rect(x, bottom - (e.key + 1) * 2, 2, 2)
    screen.fill()
  end
  if n[1] == 1 or n[1] == 2 then
    screen.level(15)
    screen.rect(math.floor(position / span * 128), top, 1, 24)
    screen.fill()
  end
end

local function draw_notes(elapsed)
  draw_notes_side(2, 6, elapsed)
  draw_notes_side(1, 38, elapsed)
end

local function note_name(midi)
  return KEY_NAMES[midi % 12 + 1] .. (midi // 12 - 1)
end

-- Threshold and clip gains move in 12 key steps (Mode button + key).
local function step_key(value, base, step)
  local k = math.floor((value - base) / step + 0.5)
  return KEY_NAMES[util.clamp(k, 0, 11) + 1]
end

local function draw_panel_side(i, y)
  local s = side_status(i)
  screen.level(15)
  screen.move(0, y)
  screen.text(i == 1 and "A" or "B")
  screen.level(10)
  screen.move(8, y)
  screen.text(MODES[s.m + 1] or "?")
  screen.move(54, y)
  screen.text((s.o > 0 and "+" or "") .. s.o .. " " .. note_name(s.p))
  -- held keys, one cell per key
  for key = 0, 11 do
    local held = (s.k >> key) & 1 == 1
    screen.level(held and 15 or (BLACK[key] and 2 or 4))
    screen.rect(92 + key * 3, y - 5, 2, 5)
    screen.fill()
  end
end

-- The panel knobs drive both sides; MIDI or the config page can split them: "A/B".
local function knob_text(index, format)
  local a, b = side_status(1).x[index], side_status(2).x[index]
  if math.abs(a - b) < 0.03 * math.max(1, math.abs(a)) then return format(a) end
  return format(a) .. "/" .. format(b)
end

local function percent(v) return math.floor(v * 100 + 0.5) .. "%" end
local function decimal(v) return string.format("%.2f", v) end

local function draw_panel()
  draw_panel_side(2, 7)
  draw_panel_side(1, 16)
  screen.level(10)
  local t, g = status.t or {0, 0}, status.g or {0, 0}
  local lines = {
    "mix " .. knob_text(1, percent) .. "  vol " .. knob_text(3, percent),
    "decay " .. knob_text(2, decimal),
    string.format("thr A %s %.2f  B %s %.2f", step_key(t[1], 0.0825, 0.0825), t[1],
      step_key(t[2], 0.0825, 0.0825), t[2]),
    string.format("pre %s %.2f  post %s %.2f", step_key(g[1], 0.0825, 0.0825), g[1],
      step_key(g[2], 0.385, 0.055), g[2]),
    "in " .. tostring(status.src) .. "  line " .. (status.mono == 1 and "mono" or "stereo")
      .. (status.mono_active == 1 and " (active)" or ""),
  }
  for k, l in ipairs(lines) do
    screen.move(0, 18 + k * 9)
    screen.text(l)
  end
end

local function level_db(code) return code >= 255 and 0 or code / 4 - 60 end

local function draw_meters(now)
  local labels = {"in L", "in R", "out L", "out R"}
  for ch = 1, 4 do
    local top = (ch - 1) * 16
    local middle = top + 8
    local h = levels.history[ch]
    local clipping = levels.clip_until[ch] > now
    screen.level(clipping and 15 or 6)
    screen.move(0, middle + 3)
    screen.text(labels[ch])
    screen.level(1)
    screen.move(24, middle + 0.5)
    screen.line(24 + LEVEL_HISTORY, middle + 0.5)
    screen.stroke()
    local start = LEVEL_HISTORY - #h
    for k, code in ipairs(h) do
      if code > 0 then
        local half = math.max(0.5, (level_db(code) + 60) / 60 * 6)
        screen.level(code >= 255 and 15 or 8)
        screen.move(24 + start + k - 0.5, middle - half)
        screen.line(24 + start + k - 0.5, middle + half + 1)
        screen.stroke()
      end
    end
  end
  -- latest level of each channel in dB, right of its strip
  for ch = 1, 4 do
    local h = levels.history[ch]
    local last = 0
    for k = math.max(1, #h - 7), #h do if h[k] and h[k] > last then last = h[k] end end
    screen.level(levels.clip_until[ch] > now and 15 or 10)
    screen.move(128, (ch - 1) * 16 + 11)
    screen.text_right(last == 0 and "-" or (last >= 255 and "CLIP" or string.format("%.0f", level_db(last))))
  end
end

local function draw_sys()
  screen.level(10)
  local lines = {
    "K3 sync",
    "fw " .. firmware,
    string.format("rtt %3.0f ms  max %3.0f", stats.rtt * 1000, stats.rtt_max * 1000),
    string.format("polls %d  t/o %d  err %d", stats.polls, stats.timeouts, stats.errors),
    string.format("reboots %d", stats.resets),
  }
  for k, l in ipairs(lines) do
    screen.move(0, 18 + k * 9)
    screen.text(l)
  end
end

function core.redraw()
  if message_until and util.time() >= message_until then say("") end
  screen.clear()
  screen.font_face(1)
  screen.font_size(8)
  if message ~= "" or not status then
    screen.level(10)
    screen.move(64, 34)
    screen.text_center(message ~= "" and message or "…")
    screen.update()
    return
  end
  local elapsed = util.time() - status_at
  advance_levels(util.time())
  local name = PAGES[page]
  if name == "CTRL" then
    draw_ctrl()
  elseif name == "NOTES" then
    draw_notes(elapsed)
  elseif name == "PANEL" then
    draw_panel()
  elseif name == "METERS" then
    draw_meters(util.time())
  elseif name == "SYS" then
    draw_sys()
  else
    -- B on top, A below: as the Wingie2 panel.
    draw_side_header(2, 7)
    draw_wave(2, 10, 20, elapsed)
    draw_side_header(1, 40)
    draw_wave(1, 43, 20, elapsed)
  end
  screen.update()
end

----------------------------------------------------------------------------

-- Running: `owner` ("script" or "mod") is showing, and `draw` redraws it. Polling
-- and redrawing go on while at least one owner is showing.
local owners = {}

local function start_metros()
  if not poll_metro then
    poll_metro = metro.init(poll, STATUS_INTERVAL)
    if poll_metro then poll_metro:start() end
  end
  if not redraw_metro then
    redraw_metro = metro.init(function()
      for _, draw in pairs(owners) do draw() end
    end, 1 / 15)
    if redraw_metro then redraw_metro:start() end
  end
end

function core.start(owner, draw)
  core.register()
  owners[owner] = draw
  start_metros()
  if not dev then
    rescan()
  elseif not connected then
    opened_at = util.time()
    hello_at = opened_at
  end
end

function core.stop(owner)
  owners[owner] = nil
  if next(owners) then return end
  if poll_metro then metro.free(poll_metro.id) end
  if redraw_metro then metro.free(redraw_metro.id) end
  poll_metro, redraw_metro = nil, nil
end

-- A script is being cleared: its metro.free_all() takes these metros too, and their
-- ids may go to the next script. Called from the mod's hooks.
function core.script_cleared()
  poll_metro, redraw_metro = nil, nil
end

-- A new script starts: go on if a page is still showing.
function core.script_starting()
  if next(owners) then start_metros() end
end

function core.first_page() return page == 1 end

return core
