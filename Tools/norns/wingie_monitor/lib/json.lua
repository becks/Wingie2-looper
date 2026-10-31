-- Minimal JSON for the Wingie2 serial protocol: objects, arrays, strings,
-- numbers, true/false/null. Arrays decode to 1-based Lua tables.

local json = {}

local function encode_value(v)
  local t = type(v)
  if t == "number" then
    if math.type(v) == "integer" then return tostring(v) end
    if v ~= v or v == math.huge or v == -math.huge then return "0" end
    local s = string.format("%.4f", v):gsub("0+$", ""):gsub("%.$", "")
    return s
  elseif t == "string" then
    return '"' .. v:gsub('[%c"\\]', function(c)
      local map = {['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t'}
      return map[c] or string.format("\\u%04x", c:byte())
    end) .. '"'
  elseif t == "boolean" then
    return v and "true" or "false"
  elseif t == "table" then
    if #v > 0 then
      local parts = {}
      for _, x in ipairs(v) do parts[#parts + 1] = encode_value(x) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    local parts = {}
    for _, k in ipairs(keys) do parts[#parts + 1] = encode_value(k) .. ":" .. encode_value(v[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end

json.encode = encode_value

local decode_value

local function skip(s, i)
  return s:find("[^ \t\r\n]", i) or #s + 1
end

local function decode_string(s, i)
  -- s:sub(i, i) == '"'
  local out, j = {}, i + 1
  while true do
    local c = s:sub(j, j)
    if c == "" then error("unterminated string") end
    if c == '"' then return table.concat(out), j + 1 end
    if c == "\\" then
      local e = s:sub(j + 1, j + 1)
      local map = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}
      if e == "u" then
        local code = tonumber(s:sub(j + 2, j + 5), 16) or 63
        out[#out + 1] = code < 128 and string.char(code) or utf8.char(code)
        j = j + 6
      else
        out[#out + 1] = map[e] or e
        j = j + 2
      end
    else
      local k = s:find('["\\]', j) or #s + 1
      out[#out + 1] = s:sub(j, k - 1)
      j = k
    end
  end
end

decode_value = function(s, i)
  i = skip(s, i)
  local c = s:sub(i, i)
  if c == "{" then
    local obj = {}
    i = skip(s, i + 1)
    if s:sub(i, i) == "}" then return obj, i + 1 end
    while true do
      if s:sub(i, i) ~= '"' then error("expected key at " .. i) end
      local key
      key, i = decode_string(s, i)
      i = skip(s, i)
      if s:sub(i, i) ~= ":" then error("expected ':' at " .. i) end
      obj[key], i = decode_value(s, i + 1)
      i = skip(s, i)
      local d = s:sub(i, i)
      if d == "}" then return obj, i + 1 end
      if d ~= "," then error("expected ',' or '}' at " .. i) end
      i = skip(s, i + 1)
    end
  elseif c == "[" then
    local arr = {}
    i = skip(s, i + 1)
    if s:sub(i, i) == "]" then return arr, i + 1 end
    while true do
      arr[#arr + 1], i = decode_value(s, i)
      i = skip(s, i)
      local d = s:sub(i, i)
      if d == "]" then return arr, i + 1 end
      if d ~= "," then error("expected ',' or ']' at " .. i) end
      i = i + 1
    end
  elseif c == '"' then
    return decode_string(s, i)
  elseif s:sub(i, i + 3) == "true" then
    return true, i + 4
  elseif s:sub(i, i + 4) == "false" then
    return false, i + 5
  elseif s:sub(i, i + 3) == "null" then
    return nil, i + 4
  end
  local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
  if not num or num == "" then error("unexpected '" .. c .. "' at " .. i) end
  return math.tointeger(tonumber(num)) or tonumber(num), i + #num
end

function json.decode(s)
  local v, i = decode_value(s, 1)
  if skip(s, i) <= #s then error("trailing data at " .. i) end
  return v
end

return json
