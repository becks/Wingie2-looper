-- wingie monitor as a norns mod: the pages of the script in the system menu, so
-- the Wingie2 can be played from norns while any other script runs.
--
-- SYSTEM > MODS > WINGIE_MONITOR: E3 enables it (then SYSTEM > RESTART), K3 opens
-- the pages. K1 goes to the running script and back here; E1 to the left of the
-- first page goes back to the list of mods. K2, K3, E2 and E3 work as in the script.

local mod = require "core/mods"
local core = require "wingie_monitor/lib/core"

-- At load, so that the device scan at startup already finds the handler.
core.register()

-- Loading a script frees all metros (see core).
mod.hook.register("script_post_cleanup", "wingie_monitor", core.script_cleared)
mod.hook.register("script_pre_init", "wingie_monitor", core.script_starting)

local m = {}

m.key = core.key

m.enc = function(n, d)
  if n == 1 and d < 0 and core.first_page() then
    mod.menu.exit()
    return
  end
  core.enc(n, d)
end

m.redraw = function()
  screen.aa(0)  -- the menu's page indicator leaves it on
  core.redraw()
end

-- The menu keeps E1 for its own panels (MIX, TAPE, HOME, PARAMS) and has E2 without
-- acceleration: while the pages are showing, the encoders come straight here.
m.init = function()
  norns.encoders.callback = m.enc
  norns.encoders.set_accel(2, true)
  core.start("mod", function() mod.menu.redraw() end)
end

m.deinit = function()
  core.stop("mod")
  norns.encoders.callback = _menu.enc
  norns.encoders.set_accel(2, false)
end

mod.menu.register(mod.this_name, m)
