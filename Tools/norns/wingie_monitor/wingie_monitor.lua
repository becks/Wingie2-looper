-- wingie monitor
-- Wingie2 Loop Monitor over USB serial
--
-- E1: page
-- page LOOP A, LOOP B: both loops, B on top and A below as on the panel;
--               K2 record -> play -> overdub <-> play, K3 stop / play,
--               E2 start and E3 end of the playback window
-- page CTRL:   K2/K3 row up/down, E2 value A (left), E3 value B (right)
-- page NOTES:  note loops (read only)
-- page PANEL:  keys, knobs, octave, threshold and gains (read only)
-- page METERS: input and output levels (read only)
-- page SYS:    K3 reload waveforms and clear max RTT
--
-- The same pages are in the system menu with the mod enabled (SYSTEM > MODS),
-- next to any other script. All the code is in lib/core.lua.

local mods = require "core/mods"
local CORE, JSON = "wingie_monitor/lib/core", "wingie_monitor/lib/json"

-- With the mod enabled the core is the mod's. Without it, read the files again, so
-- that reloading the script picks up an update as usual.
if not mods.is_loaded("wingie_monitor") then
  package.loaded[CORE], package.loaded[JSON] = nil, nil
end
local core = require(CORE)

function init() core.start("script", function() redraw() end) end
function cleanup() core.stop("script") end
function enc(n, d) core.enc(n, d) end
function key(n, z) core.key(n, z) end
function redraw() core.redraw() end
