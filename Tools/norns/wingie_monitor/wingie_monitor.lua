-- wingie monitor
-- Wingie2 Loop Monitor over USB serial
--
-- E1: page
-- page LOOP:   both loops, B on top and A below as on the panel;
--               E2 selects loop (up B, down A), E3 speed of the selected loop,
--               K2 record -> play -> overdub <-> play, K3 stop / play
-- page CTRL:   K2/K3 row up/down, E2 value A (left), E3 value B (right)
-- page NOTES:  note loops (read only)
-- page PANEL:  keys, knobs, octave, threshold and gains (read only)
-- page METERS: input and output levels (read only)
-- page SYS:    K3 reload waveforms and clear max RTT
--
-- Same serial protocol as Tools/loop_monitor.html (framing "@{json}\n" out,
-- "<{json}\n" in, 115200 8N1). Unlike the browser monitor, opening the port
-- from norns does not reset the Wingie2 (checked on hardware: uptime kept).

local json = include("lib/json")

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
local firmware = ""
local has_state = false   -- firmware accepts set_param looper_state
local has_window = false  -- firmware has the playback window (looper_start/end)
local has_filter = false  -- firmware has the side output filter (filter_*)

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
  table.insert(queue, {id = id, op = op, line = json.encode(obj), cb = cb})
  send_next()
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
      has_state, has_window, has_filter = false, false, false
      for _, c in ipairs(r.capabilities or {}) do
        if c == "looper_state" then has_state = true end
        if c == "looper_window" then has_window = true end
        if c == "side_filter" then has_filter = true end
      end
      message = ""
      for _, s in ipairs(sides) do s.wave_length = -1 end
    elseif util.time() - opened_at < HELLO_DEADLINE then
      message = "Wingie2 is starting…"
      clock.run(function() clock.sleep(0.3); if dev and not connected then hello() end end)
    else
      message = ok and "not a Wingie2" or ("no answer: " .. ((r.error and r.error.code) or "?"))
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

local function poll()
  check_timeout()
  if not connected or queued("loop_status") then return end
  request("loop_status", {}, function(ok, r)
    if not ok then
      local code = r.error and r.error.code
      if code ~= "timeout" then message = "loop_status: " .. tostring(code) end
      return
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
  end)
end

local function set_param(target, name, value)
  if not connected then return end
  request("set_param", {target = target, name = name, value = value}, function(ok, r)
    if not ok then message = name .. ": " .. tostring(r.error and r.error.code) end
    poll()
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
-- handlers and open ports across script reloads. So: the handler table outlives
-- the script and remembers its open port in `_dev`; a reload takes the port over
-- (no reopen, so no Wingie2 reset), and a device already plugged in at load is
-- re-announced to udev once.
local HANDLER_ID = "wingie2"

local function attach(d, fresh)
  dev = d
  rx = ""
  queue, inflight = {}, nil
  connected = false
  stats.last_ms = nil
  opened_at = util.time()
  message = fresh and "Wingie2 is starting…" or "connecting…"
  clock.run(function() clock.sleep(fresh and 0.5 or 0); if dev == d then hello() end end)
end

local function rescan()
  -- Only USB-serial bridges of the Wingie2's kind, so grids and crow are left alone.
  -- The USB vendor is read from sysfs (tty -> interface -> device).
  os.execute("for t in /sys/class/tty/ttyUSB*; do [ -e \"$t\" ] || continue; "
    .. "v=$(cat \"$(readlink -f \"$t/device\")/../../idVendor\" 2>/dev/null); "
    .. "case \"$v\" in 1a86|10c4) sudo -n udevadm trigger --action=add \"$t\";; esac; done")
end

local function register()
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
      message = "Wingie2 unplugged"
    end,
    event = function(id, data) on_data(data) end,
  }
  serial.add_handler(handler)
  if kept then attach(kept, false) else rescan() end
end

-- After the script stops, keep tracking the port for the next load and drop data.
local function unregister()
  local handler = serial._handlers[HANDLER_ID]
  if not handler then return end
  handler.event = function() end
  handler.add = function(id, name, d) handler._dev = d end
  handler.remove = function() handler._dev = nil end
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
  {label = "pan", name = "pan",
   get = function(s) return s.x[4] or 0 end, show = pct,
   step = function(v, d) return util.clamp(math.floor(v * 100 + 0.5 + d) / 100, 0, 1) end},
  {label = "gate", name = "looper_gate",
   get = function(s) return s.a[11] or 0 end, show = onoff,
   step = function(v, d) return d > 0 and 1 or 0 end},
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
    message = "flash firmware with looper_state"
    clock.run(function() clock.sleep(2); message = "" end)
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
    message = "flash firmware with the filter"
    clock.run(function() clock.sleep(2); message = "" end)
    return
  end
  if row.window and not has_window then
    message = "flash firmware with the loop window"
    clock.run(function() clock.sleep(2); message = "" end)
    return
  end
  local v = row.step(row_value(row, side), d, side)
  if row.name == "looper_latch" and v == 1 and row_value(row, side) == 2 then return end
  if v == row_value(row, side) then return end
  local key_side = row.shared and 0 or side
  pending[row.name .. key_side] = {value = v, at = util.time()}
  set_param(row.shared and "shared" or SIDE_NAMES[side], row.name, v)
end

function enc(n, d)
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

function key(n, z)
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

function redraw()
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

function init()
  register()
  poll_metro = metro.init(poll, STATUS_INTERVAL)
  poll_metro:start()
  redraw_metro = metro.init(function() redraw() end, 1 / 15)
  redraw_metro:start()
end

function cleanup()
  unregister()
  if poll_metro then poll_metro:stop() end
  if redraw_metro then redraw_metro:stop() end
end
