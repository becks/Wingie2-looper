-- Host test for the WINGIE2 norns params of Tools/norns/wingie_monitor/lib/core.lua:
-- minimal stand-ins for the norns globals the core uses, and a fake Wingie2 that
-- answers set_param / loop_status / hello over the stand-in serial port.
-- Run from the repository root: lua tests/norns/core_params_test.lua

package.path = "Tools/norns/?.lua;" .. package.path

local json = require "wingie_monitor/lib/json"

local now = 100
local tmp = os.tmpname()
os.remove(tmp)
os.execute("mkdir -p " .. tmp)

util = {
  time = function() return now end,
  clamp = function(v, lo, hi) return math.max(lo, math.min(hi, v)) end,
  make_dir = function(path) os.execute("mkdir -p " .. path) end,
}
_path = {data = tmp .. "/"}

-- tab.save/load as a plain Lua chunk (enough for flat tables of scalars)
local function serialize(v)
  if type(v) == "table" then
    local parts = {}
    for k, x in pairs(v) do parts[#parts + 1] = "[" .. serialize(k) .. "]=" .. serialize(x) end
    return "{" .. table.concat(parts, ",") .. "}"
  elseif type(v) == "string" then
    return string.format("%q", v)
  end
  return tostring(v)
end
tab = {
  save = function(t, file)
    local f = assert(io.open(file, "w"))
    f:write("return " .. serialize(t))
    f:close()
  end,
  load = function(file)
    local chunk = loadfile(file)
    return chunk and chunk() or nil
  end,
}

controlspec = {
  new = function(min, max, warp, step, default, units, quantum)
    local s = {min = min, max = max, warp = warp, step = step, default = default}
    function s:map(raw)
      local v = warp == "exp" and min * (max / min) ^ raw or min + (max - min) * raw
      if step and step > 0 then v = math.floor(v / step + 0.5) * step end
      return v
    end
    function s:unmap(v)
      v = math.max(min, math.min(max, v))
      if warp == "exp" then return math.log(v / min) / math.log(max / min) end
      return (v - min) / (max - min)
    end
    return s
  end,
}

-- ParamSet subset: the add_* calls, actions, silent sets, save flags
params = {lookup = {}, list = {}, group = nil, saved = {}}
local function add(p)
  params.list[#params.list + 1] = p
  assert(params.lookup[p.id] == nil, "id collision " .. p.id)
  params.lookup[p.id] = #params.list
  p.action = function() end
  params.saved[p.id] = true
end
function params:add_group(id, name, n) self.group = {id = id, name = name, n = n, first = #self.list + 1} end
function params:add_option(id, name, options, default)
  add({id = id, name = name, kind = "option", options = options, value = default or 1})
end
function params:add_control(id, name, spec)
  add({id = id, name = name, kind = "control", controlspec = spec, raw = spec:unmap(spec.default)})
end
function params:add_binary(id, name, behavior, default)
  add({id = id, name = name, kind = "binary", behavior = behavior, value = default or 0})
end
function params:lookup_param(id) return self.list[self.lookup[id]] end
function params:set_action(id, fn) self:lookup_param(id).action = fn end
function params:set_save(id, state) self.saved[id] = state end
function params:get(id)
  local p = self:lookup_param(id)
  if p.kind == "control" then return p.controlspec:map(p.raw) end
  return p.value
end
function params:set(id, v, silent)
  local p = self:lookup_param(id)
  local before = self:get(id)
  if p.kind == "control" then
    p.raw = p.controlspec:unmap(v)
  elseif p.kind == "binary" and p.behavior == "trigger" then
    if not silent then p.action(1) end
    return
  else
    p.value = v
  end
  if not silent and self:get(id) ~= before then p.action(self:get(id)) end
end
-- params:bang() as norns: every param except triggers fires its action
function params:bang()
  for _, p in ipairs(self.list) do
    if not (p.kind == "binary" and p.behavior == "trigger") then p.action(self:get(p.id)) end
  end
end

-- norns.pmap with the fields core touches
local function new_rev()
  local rev = {}
  for d = 1, 16 do rev[d] = {}; for c = 1, 16 do rev[d][c] = {} end end
  return rev
end
local pmap_writes = 0
norns = {pmap = {data = {}, rev = new_rev()}}
norns.pmap.write = function() pmap_writes = pmap_writes + 1 end

metro = {init = function() return {start = function() end} end, free = function() end}
screen = {}

-- fake Wingie2 on the serial port: answers are held until flush()
local sent, outbox = {}, {}
local device = {
  mode = {0, 0}, volume = {0.25, 0.25}, sos = {0.8, 0.8}, post = {0, 0}, state = {0, 0},
  length = {0, 0}, pre_clip_gain = 0.495, live = 0, filter_target = {0, 0},
}
local SIDE = {left = 1, right = 2}
local handler
serial = {
  _handlers = {},
  add_handler = function(h) handler = h end,
  send = function(_, line)
    local req = json.decode(line:match("^@(.*)\n$"))
    sent[#sent + 1] = req
    local answer = {v = 1, id = req.id, ok = true, op = req.op}
    if req.op == "hello" then
      answer.device = "Wingie2"
      answer.firmware = "test"
      answer.capabilities = {"looper_state", "looper_window", "side_filter"}
    elseif req.op == "set_param" then
      local i = SIDE[req.target]
      if req.name == "looper_state" then
        device.state[i] = req.value
        if req.value == 1 then device.length[i] = 0 end
        if req.value == 2 and device.length[i] == 0 then device.length[i] = 4900 end
        if req.value == 4 then device.state[i], device.length[i] = 0, 0 end
      elseif req.name == "looper_post" then
        device.post[i] = req.value
      elseif req.target == "shared" then
        device[req.name == "looper_live_input" and "live" or req.name] = req.value
      else
        device[req.name][i] = req.value
      end
      answer.value = req.value
    elseif req.op == "loop_status" then
      answer.ms = 1000
      answer.fps = 4900
      answer.mono, answer.latch, answer.live = 0, 0, device.live
      answer.t = {0.0825, 0.165}
      answer.g = {device.pre_clip_gain, 0.495}
      answer.sides = {}
      for i = 1, 2 do
        answer.sides[i] = {
          a = {device.state[i], device.length[i], 0, 90112, 1, 1, 0, device.sos[i], 0, device.post[i], 0, 0, 1},
          n = {0, 0, 0, 0, 0}, m = device.mode[i], o = 0, p = 60, k = 0,
          x = {1, 5, device.volume[i], i == 1 and 0 or 1}, f = {0, 1000, 0, device.filter_target[i]},
        }
      end
      answer.lv = {"", "", "", ""}
    elseif req.op == "loop_notes" then
      answer.events, answer.count, answer.revision = {}, 0, 0
    elseif req.op == "loop_wave" then
      local i = SIDE[req.side]
      answer.peaks, answer.length, answer.state = "", device.length[i], device.state[i]
    end
    outbox[#outbox + 1] = "<" .. json.encode(answer) .. "\n"
  end,
}
local function flush()
  while #outbox > 0 do handler.event(1, table.remove(outbox, 1)) end
end
local function count(op, name)
  local n = 0
  for _, r in ipairs(sent) do if r.op == op and (not name or r.name == name) then n = n + 1 end end
  return n
end

local core = require "wingie_monitor/lib/core"
local function check(cond, what)
  if not cond then error("FAIL: " .. what, 2) end
end

-- Plugged in, no page showing (as with the mod and another script on screen).
core.register()
handler.add(1, "ttyUSB0", "dev")

core.add_params()
local g = params.group
check(g and g.name == "WINGIE2", "group added")
check(g.n == #params.list - g.first + 1, "group size matches the params after it")
check(params.lookup["wingie_volume_a"] and params.lookup["wingie_clear_b"] and params.lookup["wingie_pre_clip_gain"],
  "side, trigger and shared params")
for id in pairs(params.lookup) do check(params.saved[id] == false, id .. " kept out of PSETs") end
local total = #params.list
core.add_params()
check(#params.list == total, "a second add_params (script + mod) adds nothing")

-- A value equal to the default is not sent (params:bang() from a script).
params:bang()
check(#sent == 0, "bang sends nothing")

-- A MIDI knob: the first value says hello first, the rest coalesce while waiting.
params:set("wingie_volume_a", 0.4)
params:set("wingie_volume_a", 0.5)
params:set("wingie_volume_a", 0.6)
check(sent[1].op == "hello", "hello before the first set_param")
flush()
check(count("set_param", "volume") == 1, "queued knob values coalesced into one set_param")
check(sent[2].target == "left" and math.abs(sent[2].value - 0.6) < 1e-6, "latest volume sent to the left side")
check(math.abs(device.volume[1] - 0.6) < 1e-6, "device volume set")

-- Option and binary params send wire values; shared params target "shared".
params:set("wingie_mode_b", 13)
flush()
check(device.mode[2] == 12, "mode option index 13 -> mode 12 (Fifths)")
params:set("wingie_looper_post_a", 1)
flush()
check(device.post[1] == 1, "loop post toggle")
params:set("wingie_filter_target_b", 2)
flush()
check(device.filter_target[2] == 1, "filter on: option 2 -> loop only")
params:set("wingie_pre_clip_gain", 0.66)
flush()
check(math.abs(device.pre_clip_gain - 0.66) < 1e-6, "shared pre clip gain")

-- The panel moves a value: the next status moves the param, silently.
now = now + 1
device.volume[2] = 0.75
local before = count("set_param")
core.add_params()  -- no-op
handler.event(1, "")  -- nothing pending
-- a poll happens after each set_param answer; trigger one through a param change
params:set("wingie_sos_b", 0.5)
flush()
flush()
check(math.abs(params:get("wingie_volume_b") - 0.75) < 1e-6, "volume B follows the Wingie2")
check(count("set_param", "volume") == 1, "following the Wingie2 sends nothing back")
check(count("set_param") == before + 1, "only the sos change was sent")

-- Transport triggers: rec on an empty loop records, again closes it, clear empties it.
now = now + 1
params:set("wingie_rec_a", 1)
flush(); flush()
check(device.state[1] == 1, "rec trigger starts recording")
now = now + 1
params:set("wingie_rec_a", 1)
flush(); flush()
check(device.state[1] == 2, "rec trigger again plays the loop")
now = now + 1
params:set("wingie_stop_a", 1)
flush(); flush()
check(device.state[1] == 0, "stop trigger stops it")
now = now + 1
params:set("wingie_clear_a", 1)
flush(); flush()
check(device.length[1] == 0, "clear trigger clears it")

-- MIDI maps follow from one script to the next.
norns.pmap.data["wingie_volume_a"] = {cc = 20, ch = 1, dev = 1, in_lo = 0, in_hi = 127, out_lo = 0, out_hi = 1,
  accum = false, echo = false, value = 0}
norns.pmap.data["script_param"] = {cc = 21, ch = 1, dev = 1}
norns.pmap.write()
check(pmap_writes == 1, "the script's own map is still written")
local saved = tab.load(tmp .. "/wingie_monitor/params.pmap")
check(saved and saved["wingie_volume_a"] and saved["wingie_volume_a"].cc == 20, "wingie map saved")
check(saved["script_param"] == nil, "the script's maps are not ours")

-- Next script: params and maps cleared, then the mod adds the params again.
params.lookup, params.list, params.group = {}, {}, nil
norns.pmap.data, norns.pmap.rev = {}, new_rev()
core.add_params()
check(norns.pmap.data["wingie_volume_a"] and norns.pmap.data["wingie_volume_a"].cc == 20, "map restored")
check(norns.pmap.rev[1][1][20] == "wingie_volume_a", "reverse lookup restored")

-- A script that maps the same CC keeps it; ours stays in the file for other scripts.
params.lookup, params.list, params.group = {}, {}, nil
norns.pmap.data, norns.pmap.rev = {other = {cc = 20, ch = 1, dev = 1}}, new_rev()
norns.pmap.rev[1][1][20] = "other"
core.add_params()
check(norns.pmap.data["wingie_volume_a"] == nil, "a CC the script maps is left to it")
local saves = 0
local save = tab.save
tab.save = function(...) saves = saves + 1; return save(...) end
norns.pmap.write()
saved = tab.load(tmp .. "/wingie_monitor/params.pmap")
check(saved["wingie_volume_a"] and saved["wingie_volume_a"].cc == 20, "the unrestored map stays saved")
check(pmap_writes == 2 and saves == 1, "the write wrapper is installed once")

os.execute("rm -rf " .. tmp)
print("core_params_test passed")
