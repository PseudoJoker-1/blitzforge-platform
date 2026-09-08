#include "lua_preludes.h"

namespace wotbmod {
namespace lua {
namespace {

// ---------------------------------------------------------------------------
// wotb.log - leveled logging over wotb.core.log
// ---------------------------------------------------------------------------
//
// Today's examples reach for bare print(), which routes every line through
// WotbModV3CoreApiV1::log at INFO under the category "lua". That is one level
// and one category for the whole sandbox: a mod cannot say "this is a warning",
// and a player reading a log cannot tell which mod wrote a line.
//
// The category defaults to "lua" - the same channel print() uses - rather than
// to the script id, because the script id is genuinely not reachable from
// inside the sandbox in this build. LuaScript::Create binds it as an upvalue of
// print() (lua_script.cpp InstallSandboxedPrint) and uses it only on the
// OutputDebugStringA fallback path; there is no global, no wotb.* field and no
// ABI slot that hands it back. Rather than invent one or guess, set_category is
// the one line an author writes to get their own id into every line - and the
// docstring says exactly that instead of pretending the default is their name.
const char kLogLibrary[] = R"lua(
do
  -- Levels mirror WotbModV3LogLevel in include/wotbmod/core_v1.h exactly.
  -- They are values, not names, because that is what the ABI slot takes.
  local log = {
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    ERROR = 4,
    FATAL = 5,
  }

  -- Resolved at call time, never captured at load time: a module that cached
  -- wotb.core here would be permanently broken for a client that publishes
  -- the interface later, and permanently useless to a script whose state was
  -- built before the query succeeded.
  local function sink()
    local core = wotb and wotb.core
    if type(core) ~= "table" or type(core.log) ~= "function" then
      return nil, "wotb.core.log is unavailable"
    end
    return core.log
  end

  local category = "lua"

  function log.category()
    return category
  end

  function log.set_category(name)
    if type(name) ~= "string" or name == "" then
      return nil, "log.set_category: argument 1: expected a non-empty string"
    end
    if #name > 96 then
      return nil,
          "log.set_category: argument 1: a category longer than 96 bytes is " ..
          "not a category, it is a message"
    end
    category = name
    return true
  end

  -- The one place that touches the ABI. Two deliberate properties:
  --
  --  * with no extra arguments the message is used verbatim, so
  --    log.info("50% done") is safe. string.format is only reached when the
  --    caller actually passed something to format.
  --  * string.format can raise (a %d against a table, a bad directive), and a
  --    logging call must never be the thing that kills a frame. It runs under
  --    pcall and the failure comes back as a value.
  local function emit(where, level, format, ...)
    local slot, slot_error = sink()
    if not slot then return nil, where .. ": " .. slot_error end
    if type(format) ~= "string" then
      return nil, where .. ": argument 1: expected a format string"
    end
    local text = format
    if select("#", ...) > 0 then
      local formatted_ok, formatted = pcall(string.format, format, ...)
      if not formatted_ok then
        return nil, where .. ": " .. tostring(formatted)
      end
      text = formatted
    end
    -- The permission fence lives on wotb.core.log itself and is never probed
    -- from here: a table and its functions exist whether or not this script
    -- may call them, so type(slot) == "function" says nothing about whether
    -- the call is allowed. The denial surfaces from the call and is prefixed
    -- rather than swallowed.
    local ok, log_error = slot(level, category, text)
    if not ok then return nil, where .. ": " .. tostring(log_error) end
    return true
  end

  function log.write(level, format, ...)
    if type(level) ~= "number" or level % 1 ~= 0 then
      return nil, "log.write: argument 1: expected an integer level"
    end
    local value = math.tointeger(level)
    if value == nil or value < 0 or value > 5 then
      return nil, "log.write: argument 1: expected a level in [0, 5]"
    end
    return emit("log.write", value, format, ...)
  end

  function log.trace(format, ...) return emit("log.trace", 0, format, ...) end
  function log.debug(format, ...) return emit("log.debug", 1, format, ...) end
  function log.info(format, ...) return emit("log.info", 2, format, ...) end
  function log.warn(format, ...) return emit("log.warn", 3, format, ...) end
  function log.error(format, ...) return emit("log.error", 4, format, ...) end
  function log.fatal(format, ...) return emit("log.fatal", 5, format, ...) end

  -- The ABI spells this level WARNING; authors type warn(). Both exist and
  -- they are the same function, so neither spelling is the wrong guess.
  log.warning = log.warn

  wotb.log = log
end
)lua";

// ---------------------------------------------------------------------------
// wotb.json - encode/decode in pure Lua
// ---------------------------------------------------------------------------
//
// There is no JSON interface anywhere in the frozen ABI - yaml_v1.h is the
// only structured-text interface - and wotb.http hands a mod a response body
// as a string. Every HTTP-using mod therefore needs this on its first day, and
// without it every one of them writes its own, badly.
//
// The two properties that matter for a sandbox that must not be able to crash
// the game:
//
//   * decode never recurses. It walks an explicit stack, so a 100,000-deep
//     nested input costs heap, not C stack - a Lua error here is a longjmp,
//     and a C stack overflow is not an error at all, it is the process.
//     The depth limit is a second, cheaper fence in front of the same thing.
//   * neither direction uses string.rep, and every pattern used against
//     untrusted input is a single character class with a single quantifier.
//     Those are linear in the matched span with no backtracking, so there is
//     no input that turns a pattern match into a hang.
const char kJsonLibrary[] = R"lua(
do
  local json = {}

  -- The depth limit, stated here because a caller has to be able to know it:
  -- decode refuses at 64 levels of nesting and encode refuses at the same
  -- depth. Both refuse with nil, message. Override per call with
  -- json.decode(text, { max_depth = n }) for any n in [1, 64] - the ceiling
  -- cannot be raised from Lua, only lowered.
  local MAX_DEPTH = 64
  json.MAX_DEPTH = MAX_DEPTH

  -- JSON null is a value, and Lua nil is the absence of one. Storing nil in a
  -- table deletes the key, so a decoded {"a":null} would silently become {}
  -- and round-trip to {} - a lie about what arrived. This sentinel is what
  -- decode stores instead, and what encode writes back as null.
  --
  -- It also means a successful decode never returns nil, so `nil` from decode
  -- unambiguously means "this input was rejected, read the second return".
  local null = setmetatable({}, { __tostring = function() return "null" end })
  json.null = null

  -- Empty object versus empty array, the documented rule:
  --
  --   * decode tags every array it builds with this metatable, so a decoded
  --     [] and a decoded {} are distinguishable and round-trip exactly.
  --   * encode writes a tagged table as an array, always - even when empty.
  --   * an untagged table encodes as an array when its keys are exactly
  --     1..n for n >= 1, and as an object otherwise. An untagged *empty*
  --     table therefore encodes as {} - an empty object. Call
  --     json.as_array(t) on a table you build yourself and mean as a list.
  local array_mt = { __name = "wotb.json.array" }

  function json.as_array(value)
    if type(value) ~= "table" then
      return nil, "json.as_array: argument 1: expected a table"
    end
    local ok, set_error = pcall(setmetatable, value, array_mt)
    if not ok then return nil, "json.as_array: " .. tostring(set_error) end
    return value
  end

  function json.is_array(value)
    if type(value) ~= "table" then return false end
    local ok, meta = pcall(getmetatable, value)
    return ok and meta == array_mt
  end

  -- ---- encode ------------------------------------------------------------

  local escapes = {
    ['"'] = '\\"',
    ['\\'] = '\\\\',
    ['\b'] = '\\b',
    ['\f'] = '\\f',
    ['\n'] = '\\n',
    ['\r'] = '\\r',
    ['\t'] = '\\t',
  }

  local function escape_char(character)
    local mapped = escapes[character]
    if mapped then return mapped end
    return string.format("\\u%04x", string.byte(character))
  end

  local function encode_string(value)
    -- One character class, one quantifier per position: linear, no
    -- backtracking, no input that turns this into a hang. Bytes above 0x7e
    -- pass through untouched, which is what makes valid UTF-8 in equal valid
    -- UTF-8 out.
    return '"' .. string.gsub(value, '[%c"\\]', escape_char) .. '"'
  end

  local function encode_number(value)
    if value ~= value then return nil, "a nan cannot be written as JSON" end
    if value == math.huge or value == -math.huge then
      return nil, "an infinity cannot be written as JSON"
    end
    if math.type(value) == "integer" then return string.format("%d", value) end
    -- 17 significant digits is what makes a double survive the round trip.
    return string.format("%.17g", value)
  end

  -- Decides what a table is without running a single metamethod: next, rawlen
  -- and rawget rather than pairs, # and indexing, because a table handed in
  -- from a mod may carry an __index or __pairs that raises, allocates or
  -- never terminates, and encode is not the place to find that out.
  local function classify(value, sorted)
    local ok, meta = pcall(getmetatable, value)
    if ok and meta == array_mt then
      return { kind = "array", value = value, index = 1,
               count = rawlen(value) }
    end
    local keys = {}
    local count = 0
    local all_strings = true
    for key in next, value do
      count = count + 1
      if type(key) == "string" then
        all_strings = all_strings and true
      elseif math.type(key) == "integer" and key >= 1 then
        all_strings = false
      else
        return nil, "a table with a key that is neither a string nor a " ..
            "positive integer cannot be written as JSON"
      end
      keys[count] = key
    end
    if count == 0 then
      return { kind = "object", value = value, index = 1, count = 0,
               keys = keys }
    end
    for index = 1, count do
      if type(keys[index]) ~= "string" then
        -- At least one integer key: this can only be an array, and only if
        -- the integer keys are exactly 1..n with nothing else in the table.
        if rawlen(value) ~= count then
          return nil, "a table mixing string keys with integer keys, or " ..
              "with holes in its integer keys, cannot be written as JSON"
        end
        return { kind = "array", value = value, index = 1, count = count }
      end
    end
    if sorted then table.sort(keys) end
    return { kind = "object", value = value, index = 1, count = count,
             keys = keys }
  end

  -- Iterative, for the same reason decode is: nesting must cost heap rather
  -- than C stack. `seen` is the cycle guard - a table that is already open
  -- further down the stack would otherwise be walked forever.
  function json.encode(value, options)
    local sorted = true
    if options ~= nil then
      if type(options) ~= "table" then
        return nil, "json.encode: argument 2: expected a table of options"
      end
      if options.sorted ~= nil then
        if type(options.sorted) ~= "boolean" then
          return nil, "json.encode: options.sorted: expected a boolean"
        end
        sorted = options.sorted
      end
    end

    local out = {}
    local length = 0
    local stack = {}
    local depth = 0
    local seen = {}

    local function put(text)
      length = length + 1
      out[length] = text
    end

    -- Writes a scalar, or opens a container and pushes its frame. Returns
    -- nil plus a reason on anything it refuses.
    local function emit(item)
      if item == null or item == nil then put("null") return true end
      local kind = type(item)
      if kind == "boolean" then
        put(item and "true" or "false")
        return true
      end
      if kind == "number" then
        local text, number_error = encode_number(item)
        if text == nil then return nil, number_error end
        put(text)
        return true
      end
      if kind == "string" then put(encode_string(item)) return true end
      if kind ~= "table" then
        return nil, "a value of type " .. kind .. " cannot be written as JSON"
      end
      if seen[item] then
        return nil, "this value refers back to itself"
      end
      if depth >= MAX_DEPTH then
        return nil, "nesting deeper than " .. MAX_DEPTH .. " levels"
      end
      local frame, classify_error = classify(item, sorted)
      if frame == nil then return nil, classify_error end
      seen[item] = true
      depth = depth + 1
      stack[depth] = frame
      put(frame.kind == "array" and "[" or "{")
      return true
    end

    local ok, emit_error = emit(value)
    if not ok then return nil, "json.encode: " .. emit_error end

    while depth > 0 do
      local frame = stack[depth]
      if frame.index > frame.count then
        put(frame.kind == "array" and "]" or "}")
        seen[frame.value] = nil
        stack[depth] = nil
        depth = depth - 1
      elseif frame.kind == "array" then
        if frame.index > 1 then put(",") end
        local item = rawget(frame.value, frame.index)
        frame.index = frame.index + 1
        local item_ok, item_error = emit(item)
        if not item_ok then return nil, "json.encode: " .. item_error end
      else
        if frame.index > 1 then put(",") end
        local key = frame.keys[frame.index]
        frame.index = frame.index + 1
        put(encode_string(key))
        put(":")
        local item_ok, item_error = emit(rawget(frame.value, key))
        if not item_ok then return nil, "json.encode: " .. item_error end
      end
    end

    return table.concat(out)
  end
)lua"
// Two adjacent raw literals, and this seam is a compiler limit rather than a
// structural one: MSVC refuses a single string literal over 16,380 bytes with
// C2026, "string too big, trailing characters truncated" - which truncates
// rather than failing outright, so the first symptom would have been a Lua
// syntax error in a chunk that looked fine in the editor. Adjacent literals
// are concatenated in translation phase 6 and the limit does not apply to the
// result, so this is one chunk to Lua and two to the compiler. Any module here
// that grows past about 16 KB needs the same seam; kBattleLibrary and
// kConfigLibrary are the next closest.
R"lua(
  -- ---- decode ------------------------------------------------------------

  local unescape = {
    [34] = '"',
    [92] = '\\',
    [47] = '/',
    [98] = '\b',
    [102] = '\f',
    [110] = '\n',
    [114] = '\r',
    [116] = '\t',
  }

  -- No utf8 library in this sandbox (base, table, string and math are the
  -- only ones lua_script.cpp opens), so the four UTF-8 forms are assembled by
  -- hand rather than through utf8.char.
  local function utf8_bytes(code)
    if code < 0x80 then return string.char(code) end
    if code < 0x800 then
      return string.char(0xC0 + (code // 0x40), 0x80 + (code % 0x40))
    end
    if code < 0x10000 then
      return string.char(0xE0 + (code // 0x1000),
                         0x80 + ((code // 0x40) % 0x40),
                         0x80 + (code % 0x40))
    end
    return string.char(0xF0 + (code // 0x40000),
                       0x80 + ((code // 0x1000) % 0x40),
                       0x80 + ((code // 0x40) % 0x40),
                       0x80 + (code % 0x40))
  end

  function json.decode(text, options)
    if type(text) ~= "string" then
      return nil, "json.decode: argument 1: expected a string"
    end
    local max_depth = MAX_DEPTH
    if options ~= nil then
      if type(options) ~= "table" then
        return nil, "json.decode: argument 2: expected a table of options"
      end
      if options.max_depth ~= nil then
        if math.type(options.max_depth) ~= "integer" or
           options.max_depth < 1 or options.max_depth > MAX_DEPTH then
          return nil, "json.decode: options.max_depth: expected an integer " ..
              "in [1, " .. MAX_DEPTH .. "]"
        end
        max_depth = options.max_depth
      end
    end

    local length = #text
    local byte = string.byte
    local sub = string.sub
    local find = string.find
    local at = 1

    local function fail(why)
      return nil, "json.decode: " .. why .. " at byte " .. at
    end

    local function skip_space()
      local _, stop = find(text, "^[ \t\n\r]*", at)
      if stop then at = stop + 1 end
    end

    local function read_hex4()
      if at + 3 > length then return nil end
      local digits = sub(text, at, at + 3)
      if not find(digits, "^%x%x%x%x$") then return nil end
      at = at + 4
      return tonumber(digits, 16)
    end

    -- Enters with text[at] == '"'. Leaves at just past the closing quote.
    local function read_string()
      at = at + 1
      local parts = nil
      local count = 0
      local start = at
      while true do
        -- Jumps straight to the next byte that could end or change the
        -- string. One character class, no backtracking, and the scan itself
        -- runs in C rather than one Lua instruction per byte.
        local stop = find(text, '["\\%c]', at)
        if not stop then
          at = length + 1
          return nil, "unterminated string"
        end
        local here = byte(text, stop)
        if here == 34 then
          local piece = sub(text, start, stop - 1)
          at = stop + 1
          if parts == nil then return piece end
          count = count + 1
          parts[count] = piece
          return table.concat(parts)
        end
        if here ~= 92 then
          at = stop
          return nil, "an unescaped control character inside a string"
        end
        if parts == nil then parts = {} end
        count = count + 1
        parts[count] = sub(text, start, stop - 1)
        local escape = byte(text, stop + 1)
        if escape == nil then
          at = stop
          return nil, "an unterminated escape sequence"
        end
        local mapped = unescape[escape]
        if mapped ~= nil then
          count = count + 1
          parts[count] = mapped
          at = stop + 2
        elseif escape == 117 then
          at = stop + 2
          local code = read_hex4()
          if code == nil then return nil, "a malformed \\u escape" end
          if code >= 0xD800 and code <= 0xDBFF then
            if byte(text, at) ~= 92 or byte(text, at + 1) ~= 117 then
              return nil, "a high surrogate with no low surrogate after it"
            end
            at = at + 2
            local low = read_hex4()
            if low == nil then return nil, "a malformed \\u escape" end
            if low < 0xDC00 or low > 0xDFFF then
              return nil, "a high surrogate followed by a non-surrogate"
            end
            code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
          elseif code >= 0xDC00 and code <= 0xDFFF then
            return nil, "a low surrogate with no high surrogate before it"
          end
          count = count + 1
          parts[count] = utf8_bytes(code)
        else
          at = stop
          return nil, "an unknown escape sequence"
        end
        start = at
      end
    end

    local function read_number()
      local start = at
      local _, stop = find(text, "^-?%d+", at)
      if not stop then return nil, "a malformed number" end
      -- Strict JSON forbids a leading zero on a multi-digit integer part.
      -- Refusing it here is what keeps decode(encode(x)) == x an equality
      -- about JSON rather than about this implementation's tolerance.
      local digits_from = (byte(text, start) == 45) and start + 1 or start
      if stop > digits_from and byte(text, digits_from) == 48 then
        return nil, "a number with a leading zero"
      end
      at = stop + 1
      local _, fraction = find(text, "^%.%d+", at)
      if fraction then at = fraction + 1 end
      local _, exponent = find(text, "^[eE][-+]?%d+", at)
      if exponent then at = exponent + 1 end
      local value = tonumber(sub(text, start, at - 1))
      if value == nil then return nil, "a malformed number" end
      if value ~= value or value == math.huge or value == -math.huge then
        return nil, "a number outside the range of a double"
      end
      return value
    end

    local function read_scalar()
      local here = byte(text, at)
      if here == 34 then return read_string() end
      if here == 116 then
        if sub(text, at, at + 3) == "true" then at = at + 4 return true end
        return nil, "a malformed literal"
      end
      if here == 102 then
        if sub(text, at, at + 4) == "false" then at = at + 5 return false end
        return nil, "a malformed literal"
      end
      if here == 110 then
        if sub(text, at, at + 3) == "null" then at = at + 4 return null end
        return nil, "a malformed literal"
      end
      if here == 45 or (here ~= nil and here >= 48 and here <= 57) then
        return read_number()
      end
      return nil, "an unexpected character"
    end

    -- The explicit stack that replaces recursion. `mode` is the parser state:
    -- "value" wants a value, "key" wants an object key, "close" finishes the
    -- container on top, "after" wants a separator or a closing bracket.
    local stack = {}
    local depth = 0
    local result = nil
    local mode = "value"

    local function store(value)
      if depth == 0 then
        result = value
        return
      end
      local frame = stack[depth]
      if frame.kind == "array" then
        frame.count = frame.count + 1
        frame.container[frame.count] = value
      else
        frame.container[frame.key] = value
        frame.key = nil
      end
    end

    while true do
      if mode == "value" then
        skip_space()
        if at > length then return fail("unexpected end of input") end
        local here = byte(text, at)
        if here == 123 then
          if depth >= max_depth then
            return fail("nesting deeper than " .. max_depth .. " levels")
          end
          at = at + 1
          depth = depth + 1
          stack[depth] = { kind = "object", container = {}, key = nil }
          skip_space()
          if byte(text, at) == 125 then
            at = at + 1
            mode = "close"
          else
            mode = "key"
          end
        elseif here == 91 then
          if depth >= max_depth then
            return fail("nesting deeper than " .. max_depth .. " levels")
          end
          at = at + 1
          depth = depth + 1
          stack[depth] = { kind = "array", count = 0,
                           container = setmetatable({}, array_mt) }
          skip_space()
          if byte(text, at) == 93 then
            at = at + 1
            mode = "close"
          else
            mode = "value"
          end
        else
          local value, scalar_error = read_scalar()
          if value == nil then return fail(scalar_error) end
          store(value)
          mode = "after"
        end
      elseif mode == "key" then
        skip_space()
        if byte(text, at) ~= 34 then return fail("expected a string key") end
        local key, key_error = read_string()
        if key == nil then return fail(key_error) end
        skip_space()
        if byte(text, at) ~= 58 then
          return fail("expected ':' after an object key")
        end
        at = at + 1
        stack[depth].key = key
        mode = "value"
      elseif mode == "close" then
        local frame = stack[depth]
        stack[depth] = nil
        depth = depth - 1
        store(frame.container)
        mode = "after"
      else
        if depth == 0 then break end
        skip_space()
        local here = byte(text, at)
        local frame = stack[depth]
        if frame.kind == "array" then
          if here == 93 then
            at = at + 1
            mode = "close"
          elseif here == 44 then
            at = at + 1
            mode = "value"
          else
            return fail("expected ',' or ']'")
          end
        else
          if here == 125 then
            at = at + 1
            mode = "close"
          elseif here == 44 then
            at = at + 1
            mode = "key"
          else
            return fail("expected ',' or '}'")
          end
        end
      end
    end

    skip_space()
    if at <= length then return fail("trailing data after the value") end
    return result
  end

  wotb.json = json
end
)lua";

// ---------------------------------------------------------------------------
// wotb.timer - after/every/cancel, driven off the frame event
// ---------------------------------------------------------------------------
//
// Not built on wotbmod.async: Read_WotbModV3TimerCreateInfo refuses a Lua
// callback outright ("a callback registered through a dedicated API function"),
// so async.timer_create cannot carry a Lua function and there is no second
// timer slot in the ABI. WOTBMOD_V3_EVENT_FRAME_UPDATE is the only clock a
// script can reach. It *is* exposed as a TOPIC_ constant now - the generator
// homes every WOTBMOD_V3_EVENT_ define on wotb.events, so the topic is
// readable as wotb.events.TOPIC_FRAME_UPDATE - and this module still passes
// subscribe a literal on purpose. The note on FRAME_TOPIC below says why.
//
// This is the module most able to wreck the client, and that is not
// theoretical: docs/API_STATUS_RU.md:139-162 records a shipped FPS collapse
// caused by a mod doing per-frame work on exactly this topic. The costs this
// module is built to hold to, stated so the next change can be checked
// against them:
//
//   * no live timer   -> no subscription at all. Not a subscription whose
//                        handler returns early: no subscription, so the
//                        client's dispatcher never even looks at this script
//                        for this topic.
//   * live but none due -> one Lua call, one integer read from the event
//                        table, one compare against the last element of an
//                        already-ordered array. No allocation, no table walk,
//                        no closure, no string.
//   * k timers due    -> O(k), plus O(log n) binary search per repeating
//                        timer that re-arms. The queue is kept in descending
//                        deadline order so the next timer to fire is the last
//                        element and popping it is O(1).
//
// Wall clock: taken from event.timestamp_ns, which the client stamps with a
// steady clock when it publishes the frame event. It is NOT a frame count and
// no frame rate is assumed anywhere. The frame event carries no delta field -
// RuntimeServicesFramePump publishes it with a null payload - so consecutive
// timestamps are the delta, and that is the honest source rather than a
// guessed 16.6 ms.
//
// If frames stop, timers stop. A timer that came due during a gap fires on the
// first frame after the gap, late by the length of the gap; it is not dropped
// and it is not fired once per missed period. A repeating timer that missed
// several periods fires exactly once and re-bases its next deadline to that
// frame, because the alternative - a catch-up burst - is the frame spike this
// module exists to avoid.
const char kTimerLibrary[] = R"lua(
do
  local timer = {}

  -- WOTBMOD_V3_EVENT_FRAME_UPDATE, spelled out rather than read from
  -- wotb.events.TOPIC_FRAME_UPDATE, which does exist: the generator maps every
  -- WOTBMOD_V3_EVENT_ define onto wotb.events.TOPIC_* and this topic is one of
  -- them. The literal is deliberate, for two reasons that both come down to
  -- *when* this line runs.
  --
  -- It runs at load, and no prelude in this file reads a constant at load
  -- time; lua_preludes.h records that as the rule rather than as an accident,
  -- and lua_bindings.cpp orders RegisterGeneratedConstants before the preludes
  -- only so that the rule could be broken deliberately if one ever had to be.
  -- A load-time read would also bind this module to whatever the global wotb
  -- table held at that instant, in a sandbox where the script owns its own
  -- globals and may replace wotb.events afterwards.
  --
  -- Nothing can drift: the topic string is frozen ABI (events_v1.h), so the
  -- literal and the constant are the same bytes by construction.
  local FRAME_TOPIC = "wotbmod.frame.update"

  local by_id = {}        -- id -> entry, for O(1) cancel
  local pending = {}      -- deadline-DESCENDING; the next to fire is last
  local unbased = {}      -- scheduled before any frame clock was observed
  local live = 0          -- timers a caller can still cancel
  local tombstones = 0    -- cancelled entries still sitting in pending
  local next_id = 1
  local subscription = nil
  local clock_ns = nil

  local function events()
    local value = wotb and wotb.events
    if type(value) ~= "table" or type(value.subscribe) ~= "function" or
       type(value.unsubscribe) ~= "function" then
      return nil, "wotb.events is unavailable"
    end
    return value
  end

  local function insert(entry)
    local low, high = 1, #pending
    while low <= high do
      local mid = (low + high) // 2
      if pending[mid].at > entry.at then low = mid + 1 else high = mid - 1 end
    end
    table.insert(pending, low, entry)
  end

  local function report(id, detail)
    local log = wotb and wotb.log
    if type(log) ~= "table" or type(log.error) ~= "function" then return end
    log.error("timer %d raised: %s", id, tostring(detail))
  end

  local function stop_subscription()
    local token = subscription
    if token == nil then return end
    -- Cleared before the call, not after: unsubscribing from inside our own
    -- handler is legal (EventsUnsubscribe marks the record dead and lets the
    -- in-flight delivery free it on the way out), but a second entry into
    -- this function must not try to unsubscribe the same token twice.
    subscription = nil
    local api = events()
    if api then api.unsubscribe(token) end
  end

  local function tick(event)
    local stamp = event and event.timestamp_ns
    if math.type(stamp) ~= "integer" then
      -- No usable clock on this event, so nothing can honestly be called due.
      -- The next frame that carries a timestamp catches up.
      return
    end
    clock_ns = stamp

    local waiting = #unbased
    if waiting > 0 then
      for index = 1, waiting do
        local entry = unbased[index]
        unbased[index] = nil
        entry.at = stamp + entry.delay
        insert(entry)
      end
    end

    local count = #pending
    if count == 0 then
      if live == 0 then stop_subscription() end
      return
    end
    -- The whole not-due path: one compare, then return. Nothing allocated.
    if pending[count].at > stamp then return end

    -- Bounded by the number of timers that existed when this tick began, so a
    -- callback that schedules a zero-delay timer cannot spin this loop.
    local budget = live
    while budget > 0 do
      count = #pending
      if count == 0 then break end
      local entry = pending[count]
      if entry.at > stamp then break end
      pending[count] = nil
      if entry.cancelled then
        tombstones = tombstones - 1
      else
        budget = budget - 1
        if entry.interval then
          -- Re-armed before the callback runs, so a callback that cancels its
          -- own repeating timer is cancelling something that is already back
          -- in the queue rather than racing the re-arm.
          entry.at = stamp + entry.interval
          insert(entry)
        else
          by_id[entry.id] = nil
          live = live - 1
        end
        -- pcall, because one mod's broken callback must not abort the rest of
        -- this tick or leave the queue half-processed. tostring on an error
        -- object can itself raise (__tostring), so reporting is guarded too.
        local ok, call_error = pcall(entry.fn, entry.id)
        if not ok then pcall(report, entry.id, call_error) end
      end
    end

    if live == 0 then stop_subscription() end
  end

  local function start_subscription(where)
    if subscription ~= nil then return true end
    local api, api_error = events()
    if not api then return nil, where .. ": " .. api_error end
    -- receive_system_events is true because wotbmod.frame.update is published
    -- by the client, not by a mod. PRIORITY_HIGH so that a handler which
    -- stops propagation cannot starve every timer in the script; HIGHEST is
    -- left free for a mod that genuinely must be first.
    local priority = api.PRIORITY_HIGH
    if type(priority) ~= "number" then priority = nil end
    local token, subscribe_error = api.subscribe(FRAME_TOPIC, tick, priority,
                                                 true)
    if token == nil then
      return nil, where .. ": " .. tostring(subscribe_error)
    end
    subscription = token
    return true
  end

  local function schedule(where, delay_ms, fn, repeating)
    if type(delay_ms) ~= "number" or delay_ms ~= delay_ms or delay_ms < 0 or
       delay_ms == math.huge then
      return nil, where ..
          ": argument 1: expected a finite delay in milliseconds, >= 0"
    end
    if type(fn) ~= "function" then
      return nil, where .. ": argument 2: expected a function"
    end
    if repeating and delay_ms < 1 then
      return nil, where ..
          ": argument 1: a repeating interval must be at least 1 ms"
    end
    local delay_ns = math.tointeger(math.floor(delay_ms * 1000000.0))
    if delay_ns == nil then
      return nil, where .. ": argument 1: that delay is too large to schedule"
    end
    -- Subscribed before anything is recorded, so a client that refuses the
    -- subscription leaves no half-scheduled timer behind.
    local started, start_error = start_subscription(where)
    if not started then return nil, start_error end

    local entry = {
      id = next_id,
      fn = fn,
      cancelled = false,
      delay = delay_ns,
      interval = repeating and delay_ns or nil,
    }
    next_id = next_id + 1
    by_id[entry.id] = entry
    live = live + 1
    if clock_ns == nil then
      -- No frame has been seen yet, so there is no "now" to measure from. The
      -- delay is measured from the first frame instead, which is up to one
      -- frame later than the call - and saying so is better than inventing a
      -- start time.
      unbased[#unbased + 1] = entry
    else
      entry.at = clock_ns + delay_ns
      insert(entry)
    end
    return entry.id
  end

  function timer.after(delay_ms, fn)
    return schedule("timer.after", delay_ms, fn, false)
  end

  function timer.every(interval_ms, fn)
    return schedule("timer.every", interval_ms, fn, true)
  end

  function timer.cancel(id)
    if math.type(id) ~= "integer" then
      return nil, "timer.cancel: argument 1: expected a timer id"
    end
    local entry = by_id[id]
    if entry == nil then
      return nil, "timer.cancel: no live timer has id " .. id
    end
    by_id[id] = nil
    entry.cancelled = true
    tombstones = tombstones + 1
    live = live - 1

    if live == 0 then
      for index = #pending, 1, -1 do pending[index] = nil end
      for index = #unbased, 1, -1 do unbased[index] = nil end
      tombstones = 0
      -- The FPS rule, and the reason this branch exists at all: the last
      -- timer going away takes the frame subscription with it.
      stop_subscription()
    elseif tombstones > 16 and tombstones > live then
      -- Cancelled entries are dropped when the queue reaches them, which is
      -- normally soon. A workload that cancels far more than it fires would
      -- otherwise grow the queue without bound, so it is compacted here -
      -- off the per-frame path, and rarely.
      local kept = 0
      for index = 1, #pending do
        local candidate = pending[index]
        if not candidate.cancelled then
          kept = kept + 1
          pending[kept] = candidate
        end
      end
      for index = #pending, kept + 1, -1 do pending[index] = nil end
      tombstones = 0
      for index = 1, #unbased do
        if unbased[index].cancelled then tombstones = tombstones + 1 end
      end
    end
    return true
  end

  function timer.cancel_all()
    local cancelled = live
    for id in next, by_id do by_id[id] = nil end
    for index = #pending, 1, -1 do pending[index] = nil end
    for index = #unbased, 1, -1 do unbased[index] = nil end
    live = 0
    tombstones = 0
    stop_subscription()
    return cancelled
  end

  function timer.count()
    return live
  end

  -- True exactly while this module holds a wotbmod.frame.update subscription.
  -- Exposed so an author - and the host's own tests - can prove the frame
  -- subscription really is dropped when the last timer goes, rather than
  -- taking a comment's word for it.
  function timer.subscribed()
    return subscription ~= nil
  end

  -- The clock this module runs on, in milliseconds, or nil plus a reason
  -- before the first frame event. Never a guess: there is no answer to give
  -- until the client has published one frame.
  function timer.now_ms()
    if clock_ns == nil then
      return nil, "timer.now_ms: no frame update has been delivered yet"
    end
    return clock_ns / 1000000.0
  end

  wotb.timer = timer
end
)lua";

// ---------------------------------------------------------------------------
// wotb.battle - one snapshot instead of eleven hand-written subscriptions
// ---------------------------------------------------------------------------
//
// Aggregated only from topics this host actually publishes - the TOPIC_
// constants in lua_bind_events.cpp - and every field carries where it came
// from and when.
//
// The honesty rule this module is built around, which is project doctrine and
// not a style preference: a field whose source event has not fired is reported
// as UNAVAILABLE. Never 0, never false, never a plausible guess. This codebase
// refuses to synthesise damage_dealt because health ingress has no proven
// attacker source (docs/API_V3_RU.md), and refuses to invent team/name/position
// for unconfirmed entities (wotb.players). The same standard applies here:
//
//   * health is recorded only when the local vehicle's entity id is already
//     known AND the health event names that entity. wotbmod.vehicle.
//     health_changed fires for every vehicle in the battle; before
//     wotbmod.vehicle.local.changed has arrived there is no way to tell whose
//     health changed, so nothing is recorded and the count of events dropped
//     for that reason is reported instead of being hidden.
//   * max_health is permanently unavailable from events. No published payload
//     carries it: WotbModV3VehicleEventData has previous_health and health
//     and nothing else. The field's own reason string says where to get it
//     instead, rather than this module dividing by a number it made up.
//   * damage_dealt is deliberately absent, for the reason the runtime gives.
//   * a subscription the client refuses is reported by topic, so "this field
//     never updates" and "this client does not publish that topic" are
//     different answers.
//   * entering or leaving a battle clears every per-battle observation.
//     Reporting last battle's health as this battle's is the exact failure
//     this rule exists to prevent.
const char kBattleLibrary[] = R"lua(
do
  local battle = {}

  -- group -> the wotb.events.TOPIC_ constant names it needs. Constant *names*
  -- rather than topic strings, so the strings come from the host's own
  -- registration at call time and cannot drift from it here.
  local GROUPS = {
    lifecycle = { "TOPIC_BATTLE_ENTERED", "TOPIC_BATTLE_STARTED",
                  "TOPIC_BATTLE_ENDED", "TOPIC_BATTLE_LEFT" },
    local_vehicle = { "TOPIC_LOCAL_VEHICLE_CHANGED",
                      "TOPIC_LOCAL_VEHICLE_CREATED",
                      "TOPIC_LOCAL_VEHICLE_DESTROYED" },
    health = { "TOPIC_VEHICLE_HEALTH_CHANGED" },
    ammo = { "TOPIC_AMMO_CHANGED" },
    reload = { "TOPIC_RELOAD_STATE_CHANGED" },
    camera = { "TOPIC_CAMERA_MODE_CHANGED", "TOPIC_SNIPER_ENTERED",
               "TOPIC_SNIPER_EXITED" },
    damage = { "TOPIC_DAMAGE_RECEIVED" },
  }

  local KINDS = {
    TOPIC_BATTLE_ENTERED = "entered",
    TOPIC_BATTLE_STARTED = "started",
    TOPIC_BATTLE_ENDED = "ended",
    TOPIC_BATTLE_LEFT = "left",
    TOPIC_LOCAL_VEHICLE_CHANGED = "local_changed",
    TOPIC_LOCAL_VEHICLE_CREATED = "local_created",
    TOPIC_LOCAL_VEHICLE_DESTROYED = "local_destroyed",
    TOPIC_VEHICLE_HEALTH_CHANGED = "health",
    TOPIC_AMMO_CHANGED = "ammo",
    TOPIC_RELOAD_STATE_CHANGED = "reload",
    TOPIC_CAMERA_MODE_CHANGED = "camera",
    TOPIC_SNIPER_ENTERED = "sniper_in",
    TOPIC_SNIPER_EXITED = "sniper_out",
    TOPIC_DAMAGE_RECEIVED = "damage",
  }

  -- Every field this module can ever report, with the reason it is not
  -- available yet. A name missing from here is a name snapshot() will never
  -- mention, which is what keeps "unavailable" exhaustive rather than
  -- best-effort.
  local REASONS = {
    lifecycle = "no wotbmod.battle.* event has been delivered yet",
    battle_id = "no wotbmod.battle.* event with a typed payload yet",
    arena_id = "no wotbmod.battle.* event with a typed payload yet",
    battle_state = "no wotbmod.battle.* event with a typed payload yet",
    winner_team = "no wotbmod.battle.ended event yet",
    end_reason = "no wotbmod.battle.ended event yet",
    local_entity_id = "no wotbmod.vehicle.local.changed/created event yet",
    local_vehicle_alive = "no wotbmod.vehicle.local.* event yet",
    health = "no health event attributable to the local vehicle yet",
    previous_health = "no health event attributable to the local vehicle yet",
    -- Two different causes, one honest answer. Either no ammo event has
    -- arrived, or one has and carried a sentinel: the producer of
    -- wotbmod.gameplay.ammo_changed assigns count = -1 unconditionally on
    -- this client build because no verified shell-count source is bound.
    -- Both mean the same thing to an author - the count is not known - and
    -- neither may be reported as a quantity.
    ammo_count =
        "no wotbmod.gameplay.ammo_changed event with a real count yet; " ..
        "this client build publishes a sentinel, not a shell count",
    shell_id = "no wotbmod.gameplay.ammo_changed event yet",
    previous_shell_id = "no wotbmod.gameplay.ammo_changed event yet",
    reload_state = "no wotbmod.gameplay.reload_state_changed event yet",
    reload_paused = "no wotbmod.gameplay.reload_state_changed event yet",
    reload_duration_seconds =
        "no wotbmod.gameplay.reload_state_changed event yet",
    reload_progress = "no wotbmod.gameplay.reload_state_changed event yet",
    reload_remaining_seconds =
        "no wotbmod.gameplay.reload_state_changed event yet",
    camera_mode = "no wotbmod.gameplay.camera_mode_changed event yet",
    camera_previous_mode = "no wotbmod.gameplay.camera_mode_changed event yet",
    camera_native_mode = "no wotbmod.gameplay.camera_mode_changed event yet",
    sniper = "no wotbmod.gameplay.sniper_entered/exited event yet",
    damage_received = "no wotbmod.gameplay.damage_received event yet",
    max_health =
        "permanently unavailable from events: no published payload carries " ..
        "maximum health. WotbModV3VehicleEventData has previous_health and " ..
        "health and nothing else. Read it from " ..
        "wotb.players.local_player().max_health, which comes from " ..
        "entity_public.enumerate_visible, and treat that as its provenance",
    damage_dealt =
        "deliberately never reported: health ingress has no proven attacker " ..
        "source, so attributing damage to the local player would be a guess. " ..
        "The runtime refuses to publish wotbmod.gameplay.damage_dealt for " ..
        "the same reason",
  }

  -- Cleared whenever a battle is entered or left. lifecycle survives, because
  -- "we just left a battle" is true after leaving one.
  local PER_BATTLE = {
    "battle_id", "arena_id", "battle_state", "winner_team", "end_reason",
    "local_entity_id", "local_vehicle_alive", "health", "previous_health",
    "ammo_count", "shell_id", "previous_shell_id", "reload_state",
    "reload_paused", "reload_duration_seconds", "reload_progress",
    "reload_remaining_seconds", "camera_mode", "camera_previous_mode",
    "camera_native_mode", "sniper", "damage_received",
  }

  local observed = {}          -- name -> { value, topic, at }
  local subscriptions = {}     -- topic string -> subscription token
  local topic_kind = {}        -- topic string -> KINDS value
  local subscribe_errors = {}  -- topic string or constant name -> why
  local active = {}            -- group name -> true
  local tracking = false
  local ignored_health = 0

  local function events()
    local value = wotb and wotb.events
    if type(value) ~= "table" or type(value.subscribe) ~= "function" or
       type(value.unsubscribe) ~= "function" then
      return nil, "wotb.events is unavailable"
    end
    return value
  end

  local function record(name, value, topic, at)
    if value == nil then return end
    observed[name] = { value = value, topic = topic, at = at }
  end

  local function clear_battle()
    for index = 1, #PER_BATTLE do observed[PER_BATTLE[index]] = nil end
    ignored_health = 0
  end

)lua"
/*
 * Seam, not a section break. MSVC caps ONE string literal at 16,380 bytes
 * and TRUNCATES past it rather than failing loudly, so the first symptom is
 * a Lua syntax error in source that looks fine. Adjacent literals are
 * concatenated by the compiler, so the Lua text is unaffected - but the
 * split must land between statements, never inside one.
 */
R"lua(
  local function record_battle_payload(data, topic, at)
    if type(data) ~= "table" then return end
    record("battle_id", data.battle_id, topic, at)
    record("arena_id", data.arena_id, topic, at)
    record("battle_state", data.state, topic, at)
    record("winner_team", data.winner_team, topic, at)
    record("end_reason", data.reason, topic, at)
  end

  local function on_event(event)
    if type(event) ~= "table" then return end
    local topic = event.topic
    local kind = topic_kind[topic]
    if kind == nil then return end
    local data = event.data
    if type(data) ~= "table" then data = nil end
    local at = event.timestamp_ns
    if math.type(at) ~= "integer" then at = nil end

    if kind == "entered" then
      clear_battle()
      record("lifecycle", "entered", topic, at)
      record_battle_payload(data, topic, at)
    elseif kind == "started" then
      record("lifecycle", "started", topic, at)
      record_battle_payload(data, topic, at)
    elseif kind == "ended" then
      record("lifecycle", "ended", topic, at)
      record_battle_payload(data, topic, at)
    elseif kind == "left" then
      clear_battle()
      record("lifecycle", "left", topic, at)
    elseif kind == "local_changed" or kind == "local_created" then
      if data ~= nil then
        local id = data.entity_id
        if id == nil then id = data.primary_entity_id end
        if math.type(id) == "integer" and id ~= 0 then
          record("local_entity_id", id, topic, at)
          record("local_vehicle_alive", true, topic, at)
        elseif math.type(id) == "integer" then
          -- The client said "the local vehicle is now nothing". That is an
          -- answer, not a missing one, so it is recorded as one.
          observed.local_entity_id = nil
          record("local_vehicle_alive", false, topic, at)
        end
      end
    elseif kind == "local_destroyed" then
      record("local_vehicle_alive", false, topic, at)
      if data ~= nil then
        -- Derived by the runtime only for the current local vehicle
        -- (src/wotb_mod_runtime.cpp), so this health really is ours.
        record("previous_health", data.previous_health, topic, at)
        record("health", data.health, topic, at)
      end
    elseif kind == "health" then
      local slot = observed.local_entity_id
      if slot == nil then
        -- No local id yet, so there is no honest way to say whose health this
        -- was. Counted rather than guessed, and the count is reported.
        ignored_health = ignored_health + 1
      elseif data ~= nil then
        local id = data.entity_id
        if id == nil then id = data.primary_entity_id end
        if id == slot.value then
          record("previous_health", data.previous_health, topic, at)
          record("health", data.health, topic, at)
        end
      end
    elseif kind == "ammo" then
      if data ~= nil then
        -- The count is a SENTINEL, not a quantity, on the shipping client:
        -- the producer of wotbmod.gameplay.ammo_changed assigns
        -- payload.count = -1 unconditionally, because no verified shell-count
        -- source is bound. Recording that as an observation would make this
        -- module assert a fact it does not have - the exact failure the rest
        -- of it refuses everywhere else, and the first author to use it read
        -- -1 as a real remaining count.
        --
        -- So a negative count is treated as unobserved, with a reason that
        -- says which of the two it is. When a build starts publishing a real
        -- count this needs no change: a value >= 0 records normally.
        -- Not recording IS how this module says "unobserved": snapshot()
        -- reports every unrecorded field through unavailable[] with the
        -- reason below.
        if type(data.count) == "number" and data.count >= 0 then
          record("ammo_count", data.count, topic, at)
        end
        record("shell_id", data.shell_id, topic, at)
        record("previous_shell_id", data.previous_shell_id, topic, at)
      end
    elseif kind == "reload" then
      if data ~= nil then
        record("reload_state", data.reload_state, topic, at)
        record("reload_paused", data.paused, topic, at)
        record("reload_duration_seconds", data.duration_seconds, topic, at)
        record("reload_progress", data.progress, topic, at)
        record("reload_remaining_seconds", data.remaining_seconds, topic, at)
      end
    elseif kind == "camera" then
      if data ~= nil then
        record("camera_mode", data.mode, topic, at)
        record("camera_previous_mode", data.previous_mode, topic, at)
        record("camera_native_mode", data.native_mode, topic, at)
      end
    elseif kind == "sniper_in" then
      record("sniper", true, topic, at)
    elseif kind == "sniper_out" then
      record("sniper", false, topic, at)
    elseif kind == "damage" then
      if data ~= nil then
        record("damage_received", {
          damage = data.damage,
          previous_health = data.previous_health,
          health = data.health,
          reason_code = data.reason_code,
          source_entity_id = data.source_entity_id,
        }, topic, at)
        -- The runtime derives this topic only when the damaged vehicle is the
        -- current local vehicle, so the health it carries is the local
        -- vehicle's - recorded with this topic as its provenance so the
        -- source is visible rather than merged into the health topic's.
        record("previous_health", data.previous_health, topic, at)
        record("health", data.health, topic, at)
      end
    end
  end

  local function subscribe_group(api, name)
    if active[name] then return end
    for _, constant in ipairs(GROUPS[name]) do
      local topic = api[constant]
      if type(topic) ~= "string" then
        subscribe_errors[constant] =
            "this host publishes no wotb.events." .. constant .. " constant"
      elseif subscriptions[topic] == nil then
        -- PRIORITY_HIGH so a handler that stops propagation cannot blind the
        -- snapshot; receive_system_events because these are client topics.
        local priority = api.PRIORITY_HIGH
        if type(priority) ~= "number" then priority = nil end
        local token, subscribe_error =
            api.subscribe(topic, on_event, priority, true)
        if token == nil then
          subscribe_errors[topic] = tostring(subscribe_error)
        else
          subscriptions[topic] = token
          topic_kind[topic] = KINDS[constant]
          subscribe_errors[topic] = nil
        end
      end
    end
    active[name] = true
  end

  function battle.groups()
    local names = {}
    for name in next, GROUPS do names[#names + 1] = name end
    table.sort(names)
    return names
  end

  -- Lazy: nothing is subscribed until this is called, directly or by the
  -- first snapshot(). names is an array of group names, or nil for all.
  function battle.start(names)
    local api, api_error = events()
    if not api then return nil, "battle.start: " .. api_error end
    local wanted = {}
    if names == nil then
      for name in next, GROUPS do wanted[name] = true end
    else
      if type(names) ~= "table" then
        return nil, "battle.start: argument 1: expected an array of group names"
      end
      for _, name in ipairs(names) do
        if GROUPS[name] == nil then
          return nil, "battle.start: '" .. tostring(name) ..
              "' is not one of this module's groups"
        end
        wanted[name] = true
      end
      -- Health cannot be attributed to the local vehicle without the local
      -- vehicle's entity id, so asking for one asks for the other. Silently
      -- returning unattributable health would be the dishonest alternative.
      if wanted.health then wanted.local_vehicle = true end
      -- Lifecycle is ALWAYS implied, for the same reason and a sharper one.
      -- Every per-battle field is cleared by the battle.entered/left
      -- handlers, and those handlers only exist if the lifecycle group was
      -- subscribed. Ask for {"reload"} alone and last battle's reload
      -- survives into the next one - reported as a current observation, with
      -- a stale timestamp, which is precisely the lie the rest of this
      -- module refuses to tell. The first author to use this module hit it.
      --
      -- It is three cheap subscriptions and it is not optional, so it is not
      -- offered as a choice. battle.groups() reports what was really
      -- subscribed, so nothing is hidden from the caller.
      wanted.lifecycle = true
    end
    for name in next, wanted do subscribe_group(api, name) end
    tracking = true
    return true
  end

  function battle.stop()
    local api = events()
    for topic, token in next, subscriptions do
      if api then api.unsubscribe(token) end
      subscriptions[topic] = nil
      topic_kind[topic] = nil
    end
    for name in next, active do active[name] = nil end
    tracking = false
    return true
  end

  function battle.tracking()
    return tracking
  end

  -- Forgets every observation without touching the subscriptions. A field
  -- goes straight back to unavailable, which is what it honestly is.
  function battle.reset()
    for name in next, observed do observed[name] = nil end
    ignored_health = 0
    return true
  end

  -- The reserved keys on the returned table - no field name may collide with
  -- these: tracking, groups, values, source, updated_ns, unavailable,
  -- subscribe_errors, ignored.
  function battle.snapshot()
    if not tracking then
      local started, start_error = battle.start()
      if not started then return nil, start_error end
    end
    local values = {}
    local source = {}
    local updated_ns = {}
    local unavailable = {}
    for name, reason in next, REASONS do
      local slot = observed[name]
      if slot == nil then
        unavailable[name] = reason
      else
        values[name] = slot.value
        source[name] = slot.topic
        updated_ns[name] = slot.at
      end
    end
    local groups = {}
    for name in next, active do groups[#groups + 1] = name end
    table.sort(groups)
    local errors = {}
    for key, why in next, subscribe_errors do errors[key] = why end

    local snapshot = {
      tracking = tracking,
      groups = groups,
      values = values,
      source = source,
      updated_ns = updated_ns,
      unavailable = unavailable,
      subscribe_errors = errors,
      ignored = { health_without_local_entity_id = ignored_health },
    }
    -- Values are also flattened onto the snapshot so an author can write
    -- snap.health. A name that is not there is not zero: read
    -- snap.unavailable[name] for why.
    for name, value in next, values do snapshot[name] = value end
    return snapshot
  end

)lua"
/*
 * Seam 2: the handler facade below is its own literal for the same MSVC
 * reason as seam 1. Same `do` block, same locals (events, observed, REASONS,
 * tracking) in scope.
 */
R"lua(
  -- ---- the handler facade -----------------------------------------------
  --
  -- battle.on_shot(fn) and friends: one short name per topic, over
  -- wotb.events.subscribe, so an author never types a topic string. The
  -- table is the whole mapping - the documentation and the code are the same
  -- object, and a name that is not in it is not a battle event this module
  -- knows. Constant names rather than topic strings, as in GROUPS above: the
  -- string comes from the host's own registration at call time.
  --
  -- Nothing is subscribed until on() is called, and off() gives the
  -- subscription back; the frame-rate rule wotb.timer states applies here
  -- unchanged.
  local HANDLER_TOPICS = {
    enter = "TOPIC_BATTLE_ENTERED",
    start = "TOPIC_BATTLE_STARTED",
    ["end"] = "TOPIC_BATTLE_ENDED",
    leave = "TOPIC_BATTLE_LEFT",
    shot = "TOPIC_SHOT_FIRED",
    hit = "TOPIC_SHELL_HIT",
    reload = "TOPIC_RELOAD_STATE_CHANGED",
    ammo = "TOPIC_AMMO_CHANGED",
    damage = "TOPIC_DAMAGE_RECEIVED",
    death = "TOPIC_LOCAL_VEHICLE_DESTROYED",
    vehicle_destroyed = "TOPIC_VEHICLE_KILLED",
    spotted = "TOPIC_VEHICLE_SPOTTED",
    unspotted = "TOPIC_VEHICLE_UNSPOTTED",
    camera_changed = "TOPIC_CAMERA_MODE_CHANGED",
    sniper_entered = "TOPIC_SNIPER_ENTERED",
    sniper_exited = "TOPIC_SNIPER_EXITED",
  }

  local handlers = {}      -- handle -> { token = ..., name = ... }
  local next_handle = 1

  local function report_handler(name, detail)
    local log = wotb and wotb.log
    if type(log) ~= "table" or type(log.error) ~= "function" then return end
    log.error("battle.on_%s handler raised: %s", name, tostring(detail))
  end

  -- The names on() accepts, sorted, as a value an author can print.
  function battle.events()
    local names = {}
    for name in next, HANDLER_TOPICS do names[#names + 1] = name end
    table.sort(names)
    return names
  end

  -- battle.on(name, fn [, once]) -> handle | nil, message
  --
  -- fn(data, event): the typed payload first (nil when this build publishes
  -- none for the topic - nothing is synthesised), the whole event second.
  -- Runs under pcall: one broken handler is reported through wotb.log and
  -- the next one still runs, and the client's dispatch never sees a raise.
  function battle.on(name, fn, once)
    local constant = HANDLER_TOPICS[name]
    if constant == nil then
      return nil, "battle.on: '" .. tostring(name) ..
          "' is not a battle event; battle.events() lists them"
    end
    if type(fn) ~= "function" then
      return nil, "battle.on: argument 2: expected a function"
    end
    local api, api_error = events()
    if not api then return nil, "battle.on: " .. api_error end
    local topic = api[constant]
    if type(topic) ~= "string" then
      return nil, "battle.on: this host publishes no wotb.events." ..
          constant .. " constant"
    end
    local handle = next_handle
    next_handle = next_handle + 1
    local function deliver(event)
      -- Before the call, so a once-handler that raises is still gone.
      if once then battle.off(handle) end
      local ok, call_error = pcall(fn, event and event.data, event)
      if not ok then pcall(report_handler, name, call_error) end
    end
    local priority = api.PRIORITY_NORMAL
    if type(priority) ~= "number" then priority = nil end
    local token, subscribe_error =
        api.subscribe(topic, deliver, priority, true)
    if token == nil then
      return nil, "battle.on: " .. tostring(subscribe_error)
    end
    handlers[handle] = { token = token, name = name }
    return handle
  end

  function battle.once(name, fn)
    return battle.on(name, fn, true)
  end

  function battle.off(handle)
    local entry = handlers[handle]
    if entry == nil then
      return nil, "battle.off: no live handler has handle " ..
          tostring(handle)
    end
    handlers[handle] = nil
    local api = events()
    if api then
      local ok, unsubscribe_error = api.unsubscribe(entry.token)
      if not ok then
        return nil, "battle.off: " .. tostring(unsubscribe_error)
      end
    end
    return true
  end

  -- Every handler this module still holds, given back. Answers how many.
  function battle.off_all()
    local count = 0
    for handle in next, handlers do
      if battle.off(handle) then count = count + 1 end
    end
    return count
  end

  -- One closure per name: battle.on_shot(fn) is battle.on("shot", fn).
  for name in next, HANDLER_TOPICS do
    battle["on_" .. name] = function(fn) return battle.on(name, fn) end
  end

  -- Is the player in a battle right now? Read from the context mask, which
  -- needs no subscription: BATTLE or TRAINING. A replay is a recording the
  -- player watches, not a battle they are in, and the hangar is neither.
  function battle.is_active()
    local context = wotb and wotb.context
    if type(context) ~= "table" or type(context.current) ~= "function" then
      return nil, "battle.is_active: wotb.context is unavailable"
    end
    local mask, mask_error = context.current()
    if mask == nil then
      return nil, "battle.is_active: " .. tostring(mask_error)
    end
    local wanted = 0
    if math.type(context.BATTLE) == "integer" then
      wanted = wanted | context.BATTLE
    end
    if math.type(context.TRAINING) == "integer" then
      wanted = wanted | context.TRAINING
    end
    return (mask & wanted) ~= 0
  end

  -- What this module knows without subscribing to anything: the context
  -- mask and whether the player is in a battle; plus, while tracking is on,
  -- the last wotbmod.battle.* lifecycle event seen - absent with its reason
  -- otherwise, exactly as snapshot() reports it.
  function battle.state()
    local active, active_error = battle.is_active()
    local state = { active = active, tracking = tracking }
    if active == nil then state.active_error = active_error end
    local context = wotb and wotb.context
    if type(context) == "table" and type(context.current) == "function" then
      state.context = context.current()
    end
    local slot = observed.lifecycle
    if slot ~= nil then
      state.lifecycle = slot.value
      state.lifecycle_updated_ns = slot.at
    else
      state.lifecycle_unavailable = REASONS.lifecycle
    end
    return state
  end

  wotb.battle = battle
end
)lua";

// ---------------------------------------------------------------------------
// wotb.config - a typed, validated layer over persisted settings
// ---------------------------------------------------------------------------
//
// Named wotb.config rather than added onto wotb.settings. The reason this
// header used to give was the convention every Register* function follows - an
// interface the client did not publish is left *undefined* rather than present
// and always failing, so that `if wotb.settings then` answers the real
// question - and that premise is gone. RegisterGeneratedConstants merges a
// constants table onto wotb.<name> unconditionally, so on a client with no
// settings interface wotb.settings is a live table of 18 numbers and that test
// answers yes to a question it was asked in order to answer no. wotb.available
// below is the primitive written to repair it, and its header carries the
// argument in full.
//
// The conclusion outlived the premise, for the reason the wotb.panel header
// below records: a module that wraps an interface has to outlive the *absence*
// of that interface, and it cannot do that from inside it. On a client with no
// settings the answer an author needs is the sentence "wotb.settings is
// unavailable", and a module reachable only through wotb.settings cannot say
// it. Merging a pure-Lua layer onto an interface table would also add a second
// instance of the defect above - a table made to exist by something other than
// publication. This module reads wotb.settings at call time exactly as
// wotb.players reads wotb.entity_public, and reports its absence as a value.
//
// Two backends, because the two are for different things:
//
//   * "settings" (the default) writes through WotbModV3SettingsApiV1, which
//     is what puts a mod's options in the client's own settings UI.
//   * "storage" keeps one JSON document under a wotb.storage key, which is
//     what a mod wants for state the player never edits.
//
// The rule the task is named for - "defaults must not silently overwrite a
// stored value" - is enforced structurally rather than by care:
//
//   * new() writes nothing, ever. It validates the schema and returns.
//   * get() writes nothing, ever. A missing stored value is answered from the
//     default and *labelled* "default"; the default is not written back.
//   * the storage backend's document is read before it is written, and what
//     is written is the document that was read with the explicitly set keys
//     laid over it. A key nobody set keeps its stored value, and a key
//     outside the schema entirely survives untouched.
//   * a read that fails is an error, never an empty document. wotb.storage.
//     contains() is what distinguishes "nothing has ever been stored" from
//     "the store could not be read", and only the first one is empty.
const char kConfigLibrary[] = R"lua(
do
  local config = {}

  local TYPES = { boolean = true, integer = true, number = true,
                  string = true }

  local SETTINGS_GET = { boolean = "get_bool", integer = "get_int",
                         number = "get_float", string = "get_string" }
  local SETTINGS_SET = { boolean = "set_bool", integer = "set_int",
                         number = "set_float", string = "set_string" }

  local function type_name(value)
    return math.type(value) or type(value)
  end

  local function type_ok(declared, value)
    if declared == "boolean" then return type(value) == "boolean" end
    if declared == "integer" then return math.type(value) == "integer" end
    if declared == "number" then return type(value) == "number" end
    return type(value) == "string"
  end

  local function check_value(name, field, value)
    if not type_ok(field.type, value) then
      return nil, "'" .. name .. "' expects a " .. field.type .. ", not a " ..
          type_name(value)
    end
    if field.type == "integer" or field.type == "number" then
      if value ~= value then
        return nil, "'" .. name .. "' cannot be a nan"
      end
      if field.min ~= nil and value < field.min then
        return nil, "'" .. name .. "' is below its declared minimum " ..
            tostring(field.min)
      end
      if field.max ~= nil and value > field.max then
        return nil, "'" .. name .. "' is above its declared maximum " ..
            tostring(field.max)
      end
    elseif field.type == "string" then
      if field.max_length ~= nil and #value > field.max_length then
        return nil, "'" .. name .. "' is longer than its declared " ..
            tostring(field.max_length) .. " bytes"
      end
      if field.values ~= nil then
        local allowed = false
        for _, candidate in ipairs(field.values) do
          if candidate == value then allowed = true end
        end
        if not allowed then
          return nil, "'" .. name .. "' is not one of its declared values"
        end
      end
    end
    return true
  end

  local function settings_api()
    local value = wotb and wotb.settings
    if type(value) ~= "table" then
      return nil, "wotb.settings is unavailable"
    end
    return value
  end

  local function storage_api()
    local value = wotb and wotb.storage
    if type(value) ~= "table" or type(value.get_json) ~= "function" or
       type(value.set_json) ~= "function" or
       type(value.contains) ~= "function" then
      return nil, "wotb.storage is unavailable"
    end
    return value
  end

  local function json_api()
    local value = wotb and wotb.json
    if type(value) ~= "table" or type(value.decode) ~= "function" or
       type(value.encode) ~= "function" then
      return nil, "wotb.json is unavailable"
    end
    return value
  end

  local Store = {}
  Store.__index = Store

  function config.new(options)
    if type(options) ~= "table" then
      return nil, "config.new: argument 1: expected an options table"
    end
    local schema = options.schema
    if type(schema) ~= "table" then
      return nil, "config.new: options.schema: expected a table of fields"
    end
    local backend = options.backend
    if backend == nil then backend = "settings" end
    if backend ~= "settings" and backend ~= "storage" then
      return nil,
          "config.new: options.backend: expected 'settings' or 'storage'"
    end
    local key = options.key
    if backend == "storage" and (type(key) ~= "string" or key == "") then
      return nil,
          "config.new: options.key: the storage backend needs a document key"
    end
    local autosave = options.autosave
    if autosave == nil then autosave = true end
    if type(autosave) ~= "boolean" then
      return nil, "config.new: options.autosave: expected a boolean"
    end

    local fields = {}
    local order = {}
    for name, field in next, schema do
      if type(name) ~= "string" or name == "" then
        return nil, "config.new: every schema key must be a non-empty string"
      end
      if #name > 95 then
        return nil, "config.new: '" .. name ..
            "' is longer than WOTBMOD_V3_SETTING_KEY_MAX allows"
      end
      if type(field) ~= "table" then
        return nil, "config.new: schema['" .. name .. "']: expected a table"
      end
      if TYPES[field.type] == nil then
        return nil, "config.new: schema['" .. name .. "'].type must be " ..
            "'boolean', 'integer', 'number' or 'string'"
      end
      if field.default == nil then
        return nil, "config.new: schema['" .. name .. "'] needs a default; a " ..
            "field with no default has no honest answer before it is set"
      end
      local copy = {
        type = field.type,
        default = field.default,
        min = field.min,
        max = field.max,
        max_length = field.max_length,
        values = field.values,
      }
      local ok, why = check_value(name, copy, field.default)
      if not ok then
        return nil, "config.new: the declared default for " .. why
      end
      fields[name] = copy
      order[#order + 1] = name
    end
    if #order == 0 then
      return nil, "config.new: options.schema declares no fields"
    end
    table.sort(order)

    return setmetatable({
      fields = fields,
      order = order,
      backend = backend,
      key = key,
      autosave = autosave,
      document = nil,
    }, Store)
  end

  function Store:schema()
    local copy = {}
    for name, field in next, self.fields do
      copy[name] = { type = field.type, default = field.default,
                     min = field.min, max = field.max,
                     max_length = field.max_length, values = field.values }
    end
    return copy
  end

  function Store:defaults()
    local copy = {}
    for name, field in next, self.fields do copy[name] = field.default end
    return copy
  end

  -- Reads the stored document. Never invents an empty one: contains() is what
  -- separates "nothing has ever been written" from "this read failed", and
  -- only the first is answered with an empty document.
  function Store:reload()
    if self.backend ~= "storage" then
      return nil,
          "config.reload: only the storage backend has a document to read"
    end
    local storage, storage_error = storage_api()
    if not storage then return nil, "config.reload: " .. storage_error end
    local json, json_error = json_api()
    if not json then return nil, "config.reload: " .. json_error end

    -- Two returns, and reading them the other way round would be the bug this
    -- whole module exists to avoid: wotb.storage.contains answers
    -- `true, <exists>` on success and `nil, <message>` on failure, so the
    -- first value says whether the call worked and the second says whether
    -- the key is there. Collapsing those into one would turn "this store
    -- could not be read" into "this store is empty", and an empty store is
    -- exactly what makes every default look safe to write.
    local ok, present = storage.contains(self.key)
    if ok == nil then
      self.document = nil
      return nil, "config.reload: " .. tostring(present)
    end
    if present ~= true then
      self.document = {}
      return true
    end
    local text, read_error = storage.get_json(self.key)
    if text == nil then
      self.document = nil
      return nil, "config.reload: " .. tostring(read_error)
    end
    if text == "" then
      self.document = {}
      return true
    end
    local decoded, decode_error = json.decode(text)
    if decoded == nil then
      self.document = nil
      return nil, "config.reload: the stored document is not valid JSON: " ..
          tostring(decode_error)
    end
    if type(decoded) ~= "table" or json.is_array(decoded) then
      self.document = nil
      return nil, "config.reload: the stored document is not a JSON object"
    end
    self.document = decoded
    return true
  end

  local function ensure_document(store)
    if store.document ~= nil then return true end
    return store:reload()
  end

  -- Returns value, "stored" or value, "default" [, note] on success, and
  -- nil, message on failure. A successful get never returns nil as its first
  -- value: every field is required to declare a default, so `nil` from get
  -- always means the read itself failed.
  function Store:get(name)
    local field = self.fields[name]
    if field == nil then
      return nil, "config.get: '" .. tostring(name) ..
          "' is not declared in this schema"
    end
    if self.backend == "storage" then
      local ok, load_error = ensure_document(self)
      if not ok then return nil, load_error end
      local stored = self.document[name]
      if stored == nil then return field.default, "default" end
      if not type_ok(field.type, stored) then
        return field.default, "default",
            "the stored value for '" .. name .. "' is a " ..
            type_name(stored) .. ", not a " .. field.type
      end
      return stored, "stored"
    end

    local settings, settings_error = settings_api()
    if not settings then return nil, "config.get: " .. settings_error end
    local slot_name = SETTINGS_GET[field.type]
    local slot = settings[slot_name]
    if type(slot) ~= "function" then
      return nil, "config.get: wotb.settings." .. slot_name .. " is unavailable"
    end
    local value, get_error = slot(name)
    if value == nil then
      -- No stored value, or the client refused the read. The default is what
      -- the author asked to fall back to; the client's own reason travels
      -- with it rather than being swallowed.
      return field.default, "default", tostring(get_error)
    end
    -- The mirror of the conversion in set: the ABI's boolean is a uint32_t,
    -- so a client that answers 0 or 1 has answered correctly and a strict
    -- type test would throw that answer away. This is the ordinary path and
    -- not a workaround - the generated settings.get_bool pushes that uint32_t
    -- through lua_pushinteger, so an integer 0 or 1 is exactly what a working
    -- client returns. Anything else falls through to the default *with the
    -- reason attached*, never silently.
    if field.type == "boolean" and math.type(value) == "integer" and
       (value == 0 or value == 1) then
      return value == 1, "stored"
    end
    if not type_ok(field.type, value) then
      return field.default, "default",
          "wotb.settings." .. slot_name .. " answered with a " ..
          type_name(value) .. ", not a " .. field.type
    end
    return value, "stored"
  end

  function Store:set(name, value)
    local field = self.fields[name]
    if field == nil then
      return nil, "config.set: '" .. tostring(name) ..
          "' is not declared in this schema"
    end
    local ok, why = check_value(name, field, value)
    if not ok then return nil, "config.set: " .. why end

    if self.backend == "storage" then
      local loaded, load_error = ensure_document(self)
      if not loaded then return nil, load_error end
      self.document[name] = value
      if self.autosave then return self:save() end
      return true
    end

    local settings, settings_error = settings_api()
    if not settings then return nil, "config.set: " .. settings_error end
    local slot_name = SETTINGS_SET[field.type]
    local slot = settings[slot_name]
    if type(slot) ~= "function" then
      return nil, "config.set: wotb.settings." .. slot_name .. " is unavailable"
    end
    -- WotbModV3SettingsApiV1::set_bool takes a uint32_t, not a boolean, and
    -- the generated binding reads argument 2 as an unsigned integer. Handing
    -- it a Lua boolean is a refusal, not a write, so the conversion happens
    -- here where the declared type says it is safe.
    local written, set_error
    if field.type == "boolean" then
      written, set_error = slot(name, value and 1 or 0)
    else
      written, set_error = slot(name, value)
    end
    if not written then return nil, "config.set: " .. tostring(set_error) end
    return true
  end

  -- Forgets an explicit value so the field answers from its default again.
  function Store:reset(name)
    local field = self.fields[name]
    if field == nil then
      return nil, "config.reset: '" .. tostring(name) ..
          "' is not declared in this schema"
    end
    if self.backend == "storage" then
      local loaded, load_error = ensure_document(self)
      if not loaded then return nil, load_error end
      self.document[name] = nil
      if self.autosave then return self:save() end
      return true
    end
    local settings, settings_error = settings_api()
    if not settings then return nil, "config.reset: " .. settings_error end
    if type(settings.reset) ~= "function" then
      return nil, "config.reset: wotb.settings.reset is unavailable"
    end
    local ok, reset_error = settings.reset(name)
    if not ok then return nil, "config.reset: " .. tostring(reset_error) end
    return true
  end

  function Store:save()
    if self.backend ~= "storage" then
      -- The settings backend writes through on every set. Claiming to flush
      -- something would be inventing a step that does not exist.
      return true
    end
    local loaded, load_error = ensure_document(self)
    if not loaded then return nil, load_error end
    local storage, storage_error = storage_api()
    if not storage then return nil, "config.save: " .. storage_error end
    local json, json_error = json_api()
    if not json then return nil, "config.save: " .. json_error end
    local text, encode_error = json.encode(self.document)
    if text == nil then return nil, "config.save: " .. tostring(encode_error) end
    local ok, write_error = storage.set_json(self.key, text)
    if not ok then return nil, "config.save: " .. tostring(write_error) end
    return true
  end

  -- values, source, notes on success; nil, message on the first field that
  -- could not be read at all.
  function Store:all()
    local values = {}
    local source = {}
    local notes = {}
    for _, name in ipairs(self.order) do
      local value, origin, note = self:get(name)
      if value == nil then return nil, origin end
      values[name] = value
      source[name] = origin
      if note ~= nil then notes[name] = note end
    end
    return values, source, notes
  end

  wotb.config = config
end
)lua";

// ---------------------------------------------------------------------------
// wotb.available - "did this client publish that interface", as a value
// ---------------------------------------------------------------------------
//
// A one-function module, and it exists because a documented convention stopped
// being true. docs/LUA_MODS_RU.md:197-202 says "if the client does not publish
// an interface, its table is not created", so every author writes
// `if wotb.settings then`. That was exact: RegisterStorage, RegisterEvents and
// RegisterUi each return early on a null interface pointer, and the generated
// pass wraps every interface table in `if (QueryGenerated(...))`.
//
// RegisterGeneratedConstants broke it. It merges a constants table onto
// wotb.<name> unconditionally (OpenConstantTable, lua_generated_bindings.cpp),
// so on a client with no settings interface wotb.settings is now a live table
// of numbers with not one callable slot behind it, and `if wotb.settings then`
// answers yes to a question it was asked in order to answer no.
//
// Reverting the constants was the alternative and it is the worse one: a nil
// constant on an older client turns `mode == wotb.settings.SCOPE_USER` into
// `mode == nil`, which raises nothing, logs nothing and is simply always
// false. A comparison that silently stops meaning anything is worse than a
// table that exists.
//
// So the constants stay and authors get the primitive the convention was
// standing in for. Living on the wotb table itself rather than on any
// interface table is deliberate: this is the one function whose answer must
// not depend on the shape of the thing it is answering about.
const char kAvailableLibrary[] = R"lua(
do
  -- wotb.available(name) -> boolean, or nil plus a message on a bad argument.
  --
  -- What it promises: wotb.<name> holds at least one callable slot in this
  -- script's state right now. That is the definition of "the client published
  -- this interface" in this host, because publication is exactly what installs
  -- functions - constants are installed either way.
  --
  -- What it does NOT promise: that this script may *call* what it found.
  -- Permission never changes API shape here - the table and its functions
  -- exist whether or not the script holds the grant - so a denial is a value
  -- returned by the call, not a missing name. "Published" and "permitted" are
  -- two different questions and this answers only the first. Code like
  --   if wotb.available('ui') then wotb.ui.create(spec) end
  -- is still obliged to read create's second return.
  --
  -- Works for every interface including the hand-written three: wotb.storage,
  -- wotb.events and wotb.ui are built by lua_newtable + luaL_setfuncs, so
  -- their slots are ordinary Lua function values and are found by the same
  -- walk as a generated table's.
  --
  -- Cost: one raw walk of one table, stopping at the first function. Called
  -- once from the top of a script, never per frame. Nothing is cached, on
  -- purpose: the answer is about the table as it is now, and a cached "yes"
  -- would outlive a table the script had since cleared - which is precisely
  -- how the host's own tests simulate an absent client.
  function wotb.available(name)
    if type(name) ~= "string" or name == "" then
      return nil, "wotb.available: argument 1: expected an interface name"
    end
    -- rawget and next throughout: a script may put a metatable on its own
    -- wotb table or on an interface table, and neither an __index nor a
    -- __pairs may be able to invent an answer to this question.
    local interface = rawget(wotb, name)
    if type(interface) ~= "table" then return false end
    for _, value in next, interface do
      if type(value) == "function" then return true end
    end
    return false
  end
end
)lua";

// ---------------------------------------------------------------------------
// wotb.panel - the high-level wotb.ui framework, built on first touch
// ---------------------------------------------------------------------------
//
// wotb.ui is 74 slots of tree, geometry, text, style, layout and events. It is
// the right shape for a binding and the wrong shape for an author: the two
// shipped panel mods (examples/lua_ally_tracker, examples/lua_battle_telemetry)
// and the F8 lab (examples/lua_ui_framework) contain the same 200 lines three
// times over - remember/create/make_text/set_text/subscribe_ui/read_viewport/
// layout/unsubscribe_ui/unmount/release_screen/rebind_active_screen/mount, plus
// the same context gate and the same four frame cadences. This module is that
// code, written once, with the two mistakes it is easy to make written out of
// it.
//
// Why wotb.panel and not wotb.ui.panel, which is the name the task suggests.
// A module that wraps an interface has to outlive the absence of that
// interface, and it cannot do that from inside it: on a client with no UI the
// answer an author needs is the sentence "wotb.ui is unavailable", and a
// module reachable only through wotb.ui cannot say it. Every other convenience
// module in this file is built the same way for the same reason - wotb.timer
// survives wotb.events being nil, wotb.config survives wotb.storage being nil,
// and wotb.config's own header records the second half of the argument:
// a pure-Lua layer merged onto an interface table makes that table exist on a
// client that never published it, which is the exact defect wotb.available
// above had to be written to repair. This module is not going to add a third
// instance of it.
//
// LAZY, and the laziness is load-bearing rather than tidy. Every prelude is
// parsed and executed for every script at load, inside the same
// 100,000-instruction budget as any other entry into the state
// (RegisterLuaLibrary now goes through LuaScript::ProtectedCall). A framework
// built eagerly is a cost paid by every script that draws nothing. So the
// chunk installs a stub - an empty table behind a metatable - and the ~41
// closures are built by the first index of it and cached.
//
// Measured with the budget's own mechanism as the yardstick, lua_sethook with
// LUA_MASKCOUNT, at count = 1 so the number is exact rather than a bound:
//
//   at load        14 VM instructions   (0.014% of the 100,000 budget)
//   at first touch 127 VM instructions  (0.127%), once, and never again
//
// tests/lua_host_tests.cpp measures the first-touch number a second way and
// inside this host, by binary-searching the largest empty for loop a chunk can
// finish with and without the touch and differencing the two - the budget's own
// unit, measured with the budget's own mechanism, and it prints what it found
// rather than only asserting a bound. It reads 127 against a 99,993-instruction
// idle headroom, which is the same number this comment claims.
//
// For comparison, measured the same way: wotb.log and wotb.timer cost 34
// instructions each to load, wotb.json 46, wotb.config 47 and wotb.battle 142.
// So this framework, which is two and a half times the size of the largest of
// them, is the cheapest module in this file for a script that never draws -
// which is the whole of what deferring bought.
//
// What stays eager is the *parse*: luaL_loadbufferx runs over this whole chunk
// for every script whether or not it ever draws, and this chunk is 45 KB, which
// is about 1.1 ms on this machine. That is C-side work, charged to no budget
// and paid once per script at load rather than per frame, and it is the price
// of keeping the framework readable source rather than a string this module
// load()s on first touch. If prelude parse ever becomes the thing to cut, that
// is the lever - and it would cost a dependency on the mutable global `load`,
// which is why it was not pulled here.
//
// The two constraints this codebase's history imposes, and where each is met:
//
//   * Ownership. Every control, style and subscription a script creates is
//     recorded in its ownership ledger by the C binding, and ~LuaScript revokes
//     them newest-first. This framework does not defeat that: it destroys in
//     the same newest-first order, it drops each handle as it goes so nothing
//     is held after it is destroyed, and it keeps every handle in the panel
//     object - a plain Lua table in this lua_State - so a hot reload, which
//     builds a brand-new state, takes the whole cache with it. There is no
//     host-side cache to invalidate because there is nothing to invalidate.
//   * Cost. docs/API_STATUS_RU.md:139-162 records a shipped mod that collapsed
//     the client's frame rate by doing per-frame work in a frame handler.
//     Redrawing text that has not changed is the same mistake, so set_row
//     compares first and crosses the ABI only on a difference; the compare is
//     one table read and one string compare and it happens before anything
//     else, including the lookup of wotb.ui. update() spends, on a frame with
//     nothing to do, one integer modulo and two field reads.
//
// Split across four adjacent raw literals for the reason kJsonLibrary is: MSVC
// truncates a single string literal past 16,380 bytes with C2026 rather than
// failing the build, so the first symptom would be a Lua syntax error in a
// chunk that looks fine in the editor. Adjacent literals concatenate in
// translation phase 6, so this is one chunk to Lua and two to the compiler.
const char kPanelLibrary[] = R"lua(
do
  local module = {}
  local build

  -- The lazy gate. Three properties, all of them observable from Lua rather
  -- than asserted in a comment:
  --
  --   * before the first touch the table is EMPTY. next(wotb.panel) == nil,
  --     and next is raw, so an __index cannot make it look built.
  --   * the first index builds everything and caches it into this same table,
  --     so the module's identity never changes - a script that captured
  --     `local panel = wotb.panel` before touching it holds the built module
  --     afterwards.
  --   * the gate removes itself. After the build there is no metatable, so a
  --     miss on wotb.panel costs a miss and not a Lua call, and the build
  --     closure is dropped rather than kept alive for a second construction
  --     that can never happen.
  local function realise(_, key)
    setmetatable(module, nil)
    local construct = build
    build = nil
    if construct ~= nil then construct(module) end
    return rawget(module, key)
  end

  build = function(P)
    -- ---- defaults ---------------------------------------------------------
    --
    -- Every one of these is overridable per panel. None of them is a guess
    -- about client state - only about what a panel usually wants. The font is
    -- the face all three shipped examples name.
    local FONT = "~res:/Fonts/WarHeliosCondCBold.ttf"
    local WHITE = { r = 1.0, g = 1.0, b = 1.0, a = 1.0 }
    local BACKGROUND = { r = 0.025, g = 0.045, b = 0.065, a = 0.92 }
    -- Used only until the client answers get_viewport_size, which mount() asks
    -- before it creates anything. Stated rather than hidden: it is the layout
    -- this client ships at, and it is replaced by a measured value one call
    -- later.
    local FALLBACK_WIDTH = 1920
    local FALLBACK_HEIGHT = 1080
    local MAX_ROWS = 64
    local MAX_BUTTONS = 16

    -- anchor -> the fraction of the leftover space that goes before the panel.
    local ANCHOR_X = {
      ["top-left"] = 0.0, ["top-center"] = 0.5, ["top-right"] = 1.0,
      ["center"] = 0.5,
      ["bottom-left"] = 0.0, ["bottom-center"] = 0.5, ["bottom-right"] = 1.0,
    }
    local ANCHOR_Y = {
      ["top-left"] = 0.0, ["top-center"] = 0.0, ["top-right"] = 0.0,
      ["center"] = 0.5,
      ["bottom-left"] = 1.0, ["bottom-center"] = 1.0, ["bottom-right"] = 1.0,
    }

    P.ANCHORS = { "top-left", "top-center", "top-right", "center",
                  "bottom-left", "bottom-center", "bottom-right" }
    P.FONT = FONT

    local Panel = {}
    Panel.__index = Panel

    -- ---- what this module stands on, resolved at call time -----------------

    -- Never captured at load or at build: a module that cached wotb.ui here
    -- would be permanently broken for a client that publishes the interface
    -- later, and permanently useless to a script whose framework was built
    -- before the query succeeded.
    local function ui()
      local api = wotb and wotb.ui
      if type(api) ~= "table" or type(api.create) ~= "function" then
        return nil, "wotb.ui is unavailable on this client"
      end
      return api
    end

    -- A publication check, never a permission check. The distinction matters
    -- and this is the only place it is made: a slot this script may not call
    -- is still a function on the table, and its denial comes back from the
    -- call as nil plus a message which every caller below prefixes and
    -- returns. A slot the *client* never published is not a function at all,
    -- and calling it - or calling the control:method() alias that resolves to
    -- it, which is nil in exactly the same case - is a raise. So every slot is
    -- fetched from the table by name first, and the framework calls the plain
    -- function rather than the alias: one fewer __index per call, and no way
    -- to reach a nil call.
    local function slot(api, name)
      local value = api[name]
      if type(value) ~= "function" then
        return nil, "wotb.ui." .. name .. " is unavailable on this client"
      end
      return value
    end

    -- tostring can itself raise through a __tostring metamethod, and an error
    -- report must never be the thing that kills a frame.
    local function describe(detail)
      local ok, text = pcall(tostring, detail)
      if ok then return text end
      return "<an error whose __tostring raised>"
    end

    -- Where everything this framework does on its own initiative - inside a
    -- frame driver, inside a click handler, inside teardown - puts a failure
    -- it cannot return to anybody. Counted and kept, never printed: a panel
    -- that logs once per frame is the FPS bug wearing a different hat.
    local function report(self, scope, detail)
      self.error_count = self.error_count + 1
      self.last_error = scope .. ": " .. describe(detail)
      local sink = self.on_error
      if sink ~= nil then pcall(sink, scope, self.last_error) end
      local listener = self.listeners and self.listeners.error
      if listener ~= nil then pcall(listener, self, scope, self.last_error) end
    end

    -- panel:on(name, fn) listeners: "mounted", "unmounted", "visibility",
    -- "error". Under pcall, like every callback this framework runs.
    local function fire(self, name, ...)
      local listener = self.listeners and self.listeners[name]
      if listener ~= nil then pcall(listener, self, ...) end
    end

    -- ---- spec validation ---------------------------------------------------

    local function opt_number(where, name, value, fallback)
      if value == nil then return fallback end
      if type(value) ~= "number" or value ~= value then
        return nil, where .. ": " .. name .. ": expected a number"
      end
      return value
    end

    local function opt_integer(where, name, value, fallback, low)
      if value == nil then return fallback end
      if math.type(value) ~= "integer" or value < low then
        return nil, where .. ": " .. name .. ": expected an integer >= " .. low
      end
      return value
    end

    local function opt_string(where, name, value, fallback)
      if value == nil then return fallback end
      if type(value) ~= "string" then
        return nil, where .. ": " .. name .. ": expected a string"
      end
      return value
    end

    -- Shape only. The client owns what a colour means; this refuses a table
    -- that cannot be one rather than deciding what one is.
    local function opt_color(where, name, value, fallback)
      if value == nil then return fallback end
      if type(value) ~= "table" or type(value.r) ~= "number" or
         type(value.g) ~= "number" or type(value.b) ~= "number" then
        return nil, where .. ": " .. name ..
            ": expected a colour table with numeric r, g and b"
      end
      return value
    end

    -- The default gate: battle, training and replay, never while the mod
    -- catalog or a text field owns the screen. Read from wotb.context by name
    -- rather than written here as 4 + 8 + 16, so the numbers stay the host's.
    -- spec.contexts = false turns gating off for a panel that manages its own
    -- visibility.
    local function default_contexts()
      local context = wotb and wotb.context
      if type(context) ~= "table" then return nil end
      local visible = 0
      local blocked = 0
      for _, name in ipairs({ "BATTLE", "TRAINING", "REPLAY" }) do
        if math.type(context[name]) == "integer" then
          visible = visible | context[name]
        end
      end
      for _, name in ipairs({ "MOD_SCREEN", "TEXT_INPUT" }) do
        if math.type(context[name]) == "integer" then
          blocked = blocked | context[name]
        end
      end
      if visible == 0 then return nil end
      return { visible = visible, blocked = blocked }
    end

)lua"
// Seam 1 of 3. See the note above the first literal: MSVC's 16,380-byte
// limit per string literal, not a structural boundary - build() continues.
R"lua(
    -- Creates nothing, reads no interface, needs no permission: new() is
    -- validation and arithmetic. A panel can therefore be declared at the top
    -- of a script on a client that publishes no UI at all, and the absence
    -- surfaces from mount() where it can be reported.
    function P.new(spec)
      local where = "panel.new"
      if type(spec) ~= "table" then
        return nil, where .. ": argument 1: expected a spec table"
      end

      local id, why = opt_string(where, "spec.id", spec.id, "panel")
      if id == nil then return nil, why end
      if id == "" or #id > 24 or string.find(id, "[^%w_%.%-]") ~= nil then
        return nil, where ..
            ": spec.id: expected 1 to 24 bytes of letters, digits, '_', '.' or '-'"
      end
      if type(spec.width) ~= "number" or spec.width <= 0 or
         type(spec.height) ~= "number" or spec.height <= 0 then
        return nil, where ..
            ": spec.width and spec.height are required and must be positive"
      end

      local anchor
      anchor, why = opt_string(where, "spec.anchor", spec.anchor, "top-left")
      if anchor == nil then return nil, why end
      if ANCHOR_X[anchor] == nil then
        return nil, where .. ": spec.anchor: expected one of " ..
            table.concat(P.ANCHORS, ", ")
      end

      local left, right, top, bottom = 0, 0, 0, 0
      local margin = spec.margin
      if type(margin) == "number" then
        left, right, top, bottom = margin, margin, margin, margin
      elseif type(margin) == "table" then
        left, why = opt_number(where, "spec.margin.left", margin.left, 0)
        if left == nil then return nil, why end
        right, why = opt_number(where, "spec.margin.right", margin.right, 0)
        if right == nil then return nil, why end
        top, why = opt_number(where, "spec.margin.top", margin.top, 0)
        if top == nil then return nil, why end
        bottom, why = opt_number(where, "spec.margin.bottom", margin.bottom, 0)
        if bottom == nil then return nil, why end
      elseif margin ~= nil then
        return nil, where ..
            ": spec.margin: expected a number or a table of left/right/top/bottom"
      end

      -- Both or neither. One of the two alone would silently fall back to the
      -- anchor for the other axis, which looks like the framework ignoring
      -- what the author wrote.
      if spec.x ~= nil or spec.y ~= nil then
        if type(spec.x) ~= "number" or type(spec.y) ~= "number" then
          return nil, where .. ": spec.x and spec.y: pass both as numbers to " ..
              "place the panel yourself, or neither and let spec.anchor do it"
        end
      end

      local padding, font, font_size, row_height, row_gap, rows_top
      padding, why = opt_number(where, "spec.padding", spec.padding, 20)
      if padding == nil then return nil, why end
      font, why = opt_string(where, "spec.font", spec.font, FONT)
      if font == nil then return nil, why end
      font_size, why = opt_number(where, "spec.font_size", spec.font_size, 18)
      if font_size == nil then return nil, why end
      row_height, why = opt_number(where, "spec.row_height", spec.row_height, 32)
      if row_height == nil then return nil, why end
      row_gap, why = opt_number(where, "spec.row_gap", spec.row_gap, 4)
      if row_gap == nil then return nil, why end
      rows_top, why = opt_number(where, "spec.rows_top", spec.rows_top, padding)
      if rows_top == nil then return nil, why end

      local color, background
      color, why = opt_color(where, "spec.color", spec.color, WHITE)
      if color == nil then return nil, why end
      background, why =
          opt_color(where, "spec.background", spec.background, BACKGROUND)
      if background == nil then return nil, why end

      local context_frames, rebind_frames, probe_frames, retry_frames
      context_frames, why = opt_integer(
          where, "spec.context_frames", spec.context_frames, 15, 0)
      if context_frames == nil then return nil, why end
      rebind_frames, why = opt_integer(
          where, "spec.rebind_frames", spec.rebind_frames, 30, 0)
      if rebind_frames == nil then return nil, why end
      probe_frames, why = opt_integer(
          where, "spec.probe_frames", spec.probe_frames, 60, 0)
      if probe_frames == nil then return nil, why end
      retry_frames, why = opt_integer(
          where, "spec.retry_frames", spec.retry_frames, 60, 1)
      if retry_frames == nil then return nil, why end

      local contexts = nil
      if spec.contexts == nil then
        contexts = default_contexts()
      elseif spec.contexts ~= false then
        if type(spec.contexts) ~= "table" or
           math.type(spec.contexts.visible) ~= "integer" then
          return nil, where ..
              ": spec.contexts: expected false, or a table with an integer" ..
              " 'visible' mask and an optional integer 'blocked' mask"
        end
        local blocked = spec.contexts.blocked
        if blocked ~= nil and math.type(blocked) ~= "integer" then
          return nil, where .. ": spec.contexts.blocked: expected an integer mask"
        end
        contexts = { visible = spec.contexts.visible, blocked = blocked or 0 }
      end

      if spec.on_error ~= nil and type(spec.on_error) ~= "function" then
        return nil, where .. ": spec.on_error: expected a function"
      end

      -- ---- rows ----
      --
      -- Resolved to absolute geometry here, once, so that mount() is a
      -- straight loop of creates and a remount after a context change repeats
      -- no arithmetic.
      local rows = {}
      local source = spec.rows
      local count
      if source == nil then
        count = 0
      elseif math.type(source) == "integer" then
        count = source
        source = nil
      elseif type(source) == "table" then
        count = #source
      else
        return nil, where ..
            ": spec.rows: expected a row count or an array of row specs"
      end
      if count < 0 or count > MAX_ROWS then
        return nil, where .. ": spec.rows: expected 0 to " .. MAX_ROWS .. " rows"
      end
      local inner = spec.width - padding * 2
      local cursor = rows_top
      for index = 1, count do
        local row = source and source[index] or nil
        if row ~= nil and type(row) ~= "table" then
          return nil, where .. ": spec.rows[" .. index .. "]: expected a table"
        end
        row = row or {}
        local field = where .. ": spec.rows[" .. index .. "]"
        local text, height, size, row_x, row_y, row_width, row_color
        text, why = opt_string(field, "text", row.text, "")
        if text == nil then return nil, why end
        height, why = opt_number(field, "height", row.height, row_height)
        if height == nil then return nil, why end
        size, why = opt_number(field, "size", row.size, font_size)
        if size == nil then return nil, why end
        row_x, why = opt_number(field, "x", row.x, padding)
        if row_x == nil then return nil, why end
        row_y, why = opt_number(field, "y", row.y, cursor)
        if row_y == nil then return nil, why end
        row_width, why = opt_number(field, "width", row.width, inner)
        if row_width == nil then return nil, why end
        row_color, why = opt_color(field, "color", row.color, color)
        if row_color == nil then return nil, why end
        rows[index] = {
          text = text, height = height, size = size, x = row_x, y = row_y,
          width = row_width, color = row_color, visible = row.visible ~= false,
        }
        cursor = row_y + height + row_gap
      end

      -- ---- buttons ----
      local buttons = {}
      local named = {}
      local button_source = spec.buttons
      if button_source ~= nil and type(button_source) ~= "table" then
        return nil, where .. ": spec.buttons: expected an array of button specs"
      end
      for index = 1, (button_source and #button_source or 0) do
        local button = button_source[index]
        local field = where .. ": spec.buttons[" .. index .. "]"
        if type(button) ~= "table" then
          return nil, field .. ": expected a table"
        end
        if index > MAX_BUTTONS then
          return nil, where .. ": spec.buttons: expected at most " ..
              MAX_BUTTONS .. " buttons"
        end
        local name
        name, why = opt_string(field, "id", button.id, nil)
        if name == nil then
          return nil, why or (field .. ": id: a button needs an id")
        end
        -- The control-name space is shared with "root", "frame", "rowN",
        -- "imageN" and "scrollN", and panel:control(name) has to stay
        -- unambiguous.
        if name == "root" or name == "frame" or named[name] ~= nil or
           string.find(name, "^row%d") ~= nil or
           string.find(name, "^image%d") ~= nil or
           string.find(name, "^scroll%d") ~= nil then
          return nil, field .. ": id '" .. name ..
              "' is already taken by this panel"
        end
        if type(button.x) ~= "number" or type(button.y) ~= "number" or
           type(button.width) ~= "number" or type(button.height) ~= "number" then
          return nil, field .. ": x, y, width and height are required numbers"
        end
        if button.on_click ~= nil and type(button.on_click) ~= "function" then
          return nil, field .. ": on_click: expected a function"
        end
        local text, size, button_color, button_background
        text, why = opt_string(field, "text", button.text, "")
        if text == nil then return nil, why end
        size, why = opt_number(field, "size", button.size, font_size)
        if size == nil then return nil, why end
        button_color, why = opt_color(field, "color", button.color, color)
        if button_color == nil then return nil, why end
        button_background, why =
            opt_color(field, "background", button.background, nil)
        if button_background == nil and why ~= nil then return nil, why end
        buttons[index] = {
          id = name, text = text, x = button.x, y = button.y,
          width = button.width, height = button.height, size = size,
          color = button_color, background = button_background,
          on_click = button.on_click,
        }
        named[name] = index
      end

      return setmetatable({
        id = id,
        width = spec.width,
        height = spec.height,
        anchor = anchor,
        margin_left = left, margin_right = right,
        margin_top = top, margin_bottom = bottom,
        fixed_x = spec.x, fixed_y = spec.y,
        x = 0, y = 0,
        background = background,
        font = font,
        rows = rows,
        buttons = buttons,
        named_buttons = named,
        images = {},
        scrolls = {},
        listeners = {},
        destroyed = false,
        controls = {},
        row_controls = {},
        order = {},
        tokens = {},
        screen = nil,
        screen_token = nil,
        is_mounted = false,
        is_visible = spec.visible ~= false,
        contexts = contexts,
        context_mask = 0,
        context_allowed = false,
        context_dirty = true,
        context_frames = context_frames,
        rebind_frames = rebind_frames,
        probe_frames = probe_frames,
        retry_frames = retry_frames,
        retry_at = 0,
        rebind_pending = false,
        viewport_width = FALLBACK_WIDTH,
        viewport_height = FALLBACK_HEIGHT,
        error_count = 0,
        last_error = nil,
        on_error = spec.on_error,
      }, Panel)
    end
)lua"
// Seam 2 of 3.
R"lua(
    -- ---- geometry ----------------------------------------------------------

    -- Where the panel sits inside the current viewport. Explicit spec.x/spec.y
    -- win outright; otherwise the anchor distributes whatever is left after
    -- the margins, and a viewport too small for the panel clamps to the
    -- top-left margin rather than pushing the panel off-screen.
    local function place(self)
      if self.fixed_x ~= nil and self.fixed_y ~= nil then
        return self.fixed_x, self.fixed_y
      end
      local low_x = self.margin_left
      local low_y = self.margin_top
      local high_x = self.viewport_width - self.margin_right - self.width
      local high_y = self.viewport_height - self.margin_bottom - self.height
      local x = low_x + (high_x - low_x) * ANCHOR_X[self.anchor]
      local y = low_y + (high_y - low_y) * ANCHOR_Y[self.anchor]
      if x < low_x then x = low_x end
      if y < low_y then y = low_y end
      return x, y
    end

    local function read_viewport(self, api)
      local get = slot(api, "get_viewport_size")
      if get == nil then return end
      local size = get()
      if type(size) == "table" and type(size.x) == "number" and
         type(size.y) == "number" and size.x > 0 and size.y > 0 then
        self.viewport_width = size.x
        self.viewport_height = size.y
      end
    end

    -- ---- construction ------------------------------------------------------

    -- One create, recorded in two places: `order` is the destruction order
    -- (append here, walk backwards there, so teardown is newest-first exactly
    -- as ~LuaScript's revocation is), `controls` is the name lookup.
    local function make(self, api, name, spec)
      spec.id = self.id .. "." .. name
      local control, create_error = api.create(spec)
      if control == nil then
        return nil, "panel.mount: " .. name .. ": " .. describe(create_error)
      end
      local order = self.order
      order[#order + 1] = control
      self.controls[name] = control
      return control
    end

    local function construct(self, api)
      -- Missing constants are refused rather than replaced with the numbers
      -- from ui_v2.h: a literal here would be a private copy of a value the
      -- client owns. RegisterUi installs all of them whenever it installs
      -- create, so this cannot fire on a client that got past ui() - it fires
      -- on a client that published a UI interface this host does not know.
      if math.type(api.CONTROL_CONTAINER) ~= "integer" or
         math.type(api.CONTROL_TEXT) ~= "integer" then
        return nil, "panel.mount: this wotb.ui publishes no CONTROL_* constants"
      end
      local alignment = api.ALIGN_LEFT
      if math.type(alignment) ~= "integer" then alignment = nil end

      local root, why = make(self, api, "root", {
        type = api.CONTROL_CONTAINER,
        x = 0, y = 0,
        width = self.viewport_width, height = self.viewport_height,
        visible = true, interactable = false,
        parent = self.screen,
      })
      if root == nil then return nil, why end

      local x, y = place(self)
      self.x, self.y = x, y
      local frame
      frame, why = make(self, api, "frame", {
        type = api.CONTROL_CONTAINER,
        x = x, y = y, width = self.width, height = self.height,
        background_color = self.background,
        visible = self.is_visible, interactable = false,
        parent = root,
      })
      if frame == nil then return nil, why end

      -- Scroll containers first: rows, buttons and images may name one as
      -- their parent. CONTROL_SCROLL_VIEW is a constant the client may not
      -- publish, in which case the container is an ordinary container and
      -- says so through report().
      for index = 1, #self.scrolls do
        local scroll = self.scrolls[index]
        local kind = api.CONTROL_SCROLL_VIEW
        if math.type(kind) ~= "integer" then
          kind = api.CONTROL_CONTAINER
          report(self, "scroll" .. index,
                 "this wotb.ui publishes no CONTROL_SCROLL_VIEW; a plain " ..
                 "container stands in")
        end
        local control
        control, why = make(self, api, "scroll" .. index, {
          type = kind,
          x = scroll.x, y = scroll.y, width = scroll.width,
          height = scroll.height,
          background_color = scroll.background,
          visible = scroll.visible, interactable = true,
          parent = frame,
        })
        if control == nil then return nil, why end
      end

      -- A control's parent: the frame, or the scroll container it named.
      local function parent_of(spec)
        if spec.parent ~= nil then
          local container = self.controls[spec.parent]
          if container ~= nil then return container end
        end
        return frame
      end

      -- Created outright rather than cloned from a hidden template, which is
      -- what all three examples do. A template is one more control that stays
      -- alive and invisible for the life of the panel, and cloning still costs
      -- a set_id, a set_parent, a set_position and a set_size afterwards; a
      -- create with a complete descriptor is the same crossings without the
      -- orphan.
      for index = 1, #self.rows do
        local row = self.rows[index]
        local control
        control, why = make(self, api, "row" .. index, {
          type = api.CONTROL_TEXT,
          text = row.text,
          x = row.x, y = row.y, width = row.width, height = row.height,
          font = self.font, font_size = row.size, color = row.color,
          alignment = alignment,
          visible = row.visible, interactable = false,
          parent = parent_of(row),
        })
        if control == nil then return nil, why end
        self.row_controls[index] = control
      end

      for index = 1, #self.images do
        local image = self.images[index]
        local control
        control, why = make(self, api, "image" .. index, {
          type = api.CONTROL_IMAGE,
          texture = image.texture,
          x = image.x, y = image.y, width = image.width, height = image.height,
          visible = image.visible, interactable = false,
          parent = parent_of(image),
        })
        if control == nil then return nil, why end
      end

      local subscribe = nil
      for index = 1, #self.buttons do
        local button = self.buttons[index]
        local control
        control, why = make(self, api, button.id, {
          type = api.CONTROL_BUTTON,
          text = button.text,
          x = button.x, y = button.y,
          width = button.width, height = button.height,
          font = self.font, font_size = button.size, color = button.color,
          background_color = button.background,
          alignment = api.ALIGN_CENTER,
          visible = true, enabled = true, interactable = true,
          parent = parent_of(button),
        })
        if control == nil then return nil, why end
        if button.on_click ~= nil then
          if subscribe == nil then
            subscribe, why = slot(api, "event_subscribe")
            if subscribe == nil then
              return nil, "panel.mount: " .. button.id .. ": " .. why
            end
          end
          local handler = button.on_click
          local name = button.id
          -- pcall around the author's handler: this runs inside the client's
          -- own UI dispatch, and one mod's broken click must not become the
          -- client's problem. The failure is recorded on the panel instead.
          local token, subscribe_error = subscribe(
              control, api.EVENT_CLICK, function()
                local ok, call_error = pcall(handler, self, name)
                if not ok then report(self, "on_click " .. name, call_error) end
              end)
          if token == nil then
            return nil, "panel.mount: " .. name .. ": " ..
                describe(subscribe_error)
          end
          self.tokens[#self.tokens + 1] = token
        end
      end
      return true
    end

    -- ---- screen ownership --------------------------------------------------

    local function release_screen(self, handle)
      if handle == nil then return end
      local handles = wotb and wotb.handles
      if type(handles) ~= "table" or type(handles.release) ~= "function" then
        return
      end
      local ok, release_error = handles.release(handle)
      if not ok then report(self, "screen release", release_error) end
    end

    -- The client can replace the active screen under a mounted panel. Polling
    -- for it every rebind_frames is what the shipped examples do; subscribing
    -- to the screen-changed topic makes the usual case immediate and costs one
    -- subscription, which unmount() gives back. Both paths exist because the
    -- subscription can be refused and the panel still has to recover.
    local function rebind(self)
      self.rebind_pending = false
      local api = ui()
      if api == nil then return false end
      local root = self.controls.root
      if root == nil then return false end
      local get = slot(api, "get_active_screen")
      local set_parent = slot(api, "control_set_parent")
      if get == nil or set_parent == nil then return false end
      local screen, screen_error = get()
      if screen == nil then
        report(self, "replacement screen", screen_error)
        return false
      end
      local ok, parent_error = set_parent(root, screen)
      if not ok then
        release_screen(self, screen)
        report(self, "replacement screen attach", parent_error)
        return false
      end
      local previous = self.screen
      self.screen = screen
      release_screen(self, previous)
      return true
    end

    local function follow_screen(self)
      if self.screen_token ~= nil then return end
      local events = wotb and wotb.events
      if type(events) ~= "table" or type(events.subscribe) ~= "function" or
         type(events.TOPIC_UI_SCREEN_CHANGED) ~= "string" then
        return
      end
      local token, subscribe_error = events.subscribe(
          events.TOPIC_UI_SCREEN_CHANGED,
          function()
            -- Flags only. Nothing is created, destroyed or crossed from inside
            -- a client dispatch; the next update() does the work on the frame
            -- thread where the rest of this panel already runs.
            self.rebind_pending = true
            self.context_dirty = true
          end,
          events.PRIORITY_NORMAL, true)
      if token == nil then
        report(self, "screen-change subscription", subscribe_error)
        return
      end
      self.screen_token = token
    end

    -- ---- the public surface ------------------------------------------------

    function Panel:mounted()
      return self.is_mounted
    end

    function Panel:mount()
      if self.is_mounted then return true end
      local api, api_error = ui()
      if api == nil then return nil, "panel.mount: " .. api_error end
      local get, why = slot(api, "get_active_screen")
      if get == nil then return nil, "panel.mount: " .. why end
      local screen, screen_error = get()
      if screen == nil then
        return nil, "panel.mount: " .. describe(screen_error)
      end
      self.screen = screen
      read_viewport(self, api)
      local ok, construct_error = construct(self, api)
      if not ok then
        -- Rolled back through the same teardown a caller would run, so a
        -- half-built panel leaves exactly as little behind as a torn-down one.
        self:unmount()
        return nil, construct_error
      end
      self.is_mounted = true
      follow_screen(self)
      fire(self, "mounted")
      return true
    end

    -- Idempotent, and safe on a panel that never mounted. Order is the
    -- reverse of construction throughout: subscriptions before the controls
    -- they are attached to, controls newest-first, the screen handle last.
    function Panel:unmount()
      local api = ui()
      local unsubscribe = api and slot(api, "event_unsubscribe") or nil
      local tokens = self.tokens
      for index = #tokens, 1, -1 do
        local token = tokens[index]
        tokens[index] = nil
        if unsubscribe ~= nil then
          local ok, unsubscribe_error = unsubscribe(token)
          if not ok then report(self, "event cleanup", unsubscribe_error) end
        end
      end
      if self.screen_token ~= nil then
        local token = self.screen_token
        self.screen_token = nil
        local events = wotb and wotb.events
        if type(events) == "table" and type(events.unsubscribe) == "function" then
          local ok, unsubscribe_error = events.unsubscribe(token)
          if not ok then report(self, "event cleanup", unsubscribe_error) end
        end
      end

      local destroy = api and slot(api, "control_destroy") or nil
      local order = self.order
      for index = #order, 1, -1 do
        local control = order[index]
        -- Dropped from the panel the moment it is destroyed. Holding a
        -- destroyed control is how a later frame ends up calling a slot with a
        -- handle the client has already reclaimed.
        order[index] = nil
        if destroy ~= nil then
          local ok, destroy_error = destroy(control)
          if not ok then report(self, "control cleanup", destroy_error) end
        end
      end
      for name in next, self.controls do self.controls[name] = nil end
      for index in next, self.row_controls do self.row_controls[index] = nil end

      local screen = self.screen
      self.screen = nil
      release_screen(self, screen)
      local was_mounted = self.is_mounted
      self.is_mounted = false
      self.rebind_pending = false
      if was_mounted then fire(self, "unmounted") end
      -- rows keep their text: it is the model, not a cache of the control's
      -- state, so a panel that leaves battle and comes back is rebuilt with
      -- what it was last showing rather than with the spec's first draft.
      return true
    end

)lua"
// Seam 3 of 3.
R"lua(
    -- The cheap path, and the whole reason this function exists.
    --
    -- Unchanged text costs one table index and one string compare, and
    -- returns before it so much as looks up wotb.ui. Nothing is allocated and
    -- nothing crosses the ABI. Calling this every frame with the same string
    -- is free; calling it every frame with a string built by string.format is
    -- not, because the format allocates before this function is entered - so
    -- an author with an expensive line to build should keep their own
    -- "did anything change" flag as well.
    --
    -- Returns true plus "sent" or true plus "unchanged" on success, and nil
    -- plus a message on a bad index, a bad value, or a client refusal. The
    -- boolean is never the interesting part: `if not ok then` still means the
    -- call failed.
    function Panel:set_row(index, text)
      local row = self.rows[index]
      if row == nil then
        return nil, "panel.set_row: argument 1: this panel has no row " ..
            describe(index)
      end
      if type(text) ~= "string" then
        return nil, "panel.set_row: argument 2: expected a string"
      end
      if row.text == text then return true, "unchanged" end
      row.text = text
      local control = self.row_controls[index]
      if control == nil then return true, "unchanged" end
      local api, api_error = ui()
      if api == nil then return nil, "panel.set_row: " .. api_error end
      local set, why = slot(api, "control_set_text")
      if set == nil then return nil, "panel.set_row: " .. why end
      local ok, set_error = set(control, text)
      if not ok then return nil, "panel.set_row: " .. describe(set_error) end
      return true, "sent"
    end

    -- Rows 1..#list from the list, every row after it hidden. Returns true
    -- plus the number of rows that actually reached the client.
    function Panel:set_rows(list)
      if type(list) ~= "table" then
        return nil, "panel.set_rows: argument 1: expected an array of strings"
      end
      local sent = 0
      for index = 1, #self.rows do
        local text = list[index]
        if text == nil then
          local ok, why = self:set_row_visible(index, false)
          if not ok then return nil, why end
          if why == "sent" then sent = sent + 1 end
        else
          local ok, why = self:set_row(index, text)
          if not ok then return nil, why end
          if why == "sent" then sent = sent + 1 end
          ok, why = self:set_row_visible(index, true)
          if not ok then return nil, why end
          if why == "sent" then sent = sent + 1 end
        end
      end
      return true, sent
    end

    function Panel:set_row_visible(index, on)
      local row = self.rows[index]
      if row == nil then
        return nil, "panel.set_row_visible: argument 1: this panel has no row " ..
            describe(index)
      end
      if type(on) ~= "boolean" then
        return nil, "panel.set_row_visible: argument 2: expected a boolean"
      end
      if row.visible == on then return true, "unchanged" end
      row.visible = on
      local control = self.row_controls[index]
      if control == nil then return true, "unchanged" end
      local api, api_error = ui()
      if api == nil then return nil, "panel.set_row_visible: " .. api_error end
      local set, why = slot(api, "control_set_visible")
      if set == nil then return nil, "panel.set_row_visible: " .. why end
      local ok, set_error = set(control, on)
      if not ok then
        return nil, "panel.set_row_visible: " .. describe(set_error)
      end
      return true, "sent"
    end

    function Panel:row(index)
      local row = self.rows[index]
      if row == nil then
        return nil, "panel.row: argument 1: this panel has no row " ..
            describe(index)
      end
      return row.text, row.visible
    end

    function Panel:row_count()
      return #self.rows
    end

    -- Same compare-first rule as set_row, for the label of a button that
    -- toggles - which is what both shipped panels do with their pause and
    -- collapse buttons.
    function Panel:set_button_text(name, text)
      local index = self.named_buttons[name]
      if index == nil then
        return nil, "panel.set_button_text: argument 1: this panel has no " ..
            "button named " .. describe(name)
      end
      if type(text) ~= "string" then
        return nil, "panel.set_button_text: argument 2: expected a string"
      end
      local button = self.buttons[index]
      if button.text == text then return true, "unchanged" end
      button.text = text
      local control = self.controls[name]
      if control == nil then return true, "unchanged" end
      local api, api_error = ui()
      if api == nil then return nil, "panel.set_button_text: " .. api_error end
      local set, why = slot(api, "control_set_text")
      if set == nil then return nil, "panel.set_button_text: " .. why end
      local ok, set_error = set(control, text)
      if not ok then
        return nil, "panel.set_button_text: " .. describe(set_error)
      end
      return true, "sent"
    end

    function Panel:visible()
      return self.is_visible
    end

    function Panel:set_visible(on)
      if type(on) ~= "boolean" then
        return nil, "panel.set_visible: argument 1: expected a boolean"
      end
      if self.is_visible == on then return true, "unchanged" end
      self.is_visible = on
      local frame = self.controls.frame
      if frame == nil then return true, "unchanged" end
      local api, api_error = ui()
      if api == nil then return nil, "panel.set_visible: " .. api_error end
      local set, why = slot(api, "control_set_visible")
      if set == nil then return nil, "panel.set_visible: " .. why end
      local ok, set_error = set(frame, on)
      if not ok then return nil, "panel.set_visible: " .. describe(set_error) end
      return true, "sent"
    end

    function Panel:show() return self:set_visible(true) end
    function Panel:hide() return self:set_visible(false) end

    -- Re-runs the anchor arithmetic against the current viewport and moves the
    -- panel. Called by mount and by the periodic viewport probe; an author
    -- only needs it after changing the size or the anchor by hand.
    function Panel:layout()
      local root = self.controls.root
      local frame = self.controls.frame
      if root == nil or frame == nil then return true end
      local api, api_error = ui()
      if api == nil then return nil, "panel.layout: " .. api_error end
      local set_size, why = slot(api, "control_set_size")
      if set_size == nil then return nil, "panel.layout: " .. why end
      local set_position
      set_position, why = slot(api, "control_set_position")
      if set_position == nil then return nil, "panel.layout: " .. why end
      local ok, size_error = set_size(root, self.viewport_width,
                                      self.viewport_height)
      if not ok then return nil, "panel.layout: " .. describe(size_error) end
      ok, size_error = set_size(frame, self.width, self.height)
      if not ok then return nil, "panel.layout: " .. describe(size_error) end
      local x, y = place(self)
      self.x, self.y = x, y
      local moved, position_error = set_position(frame, x, y)
      if not moved then
        return nil, "panel.layout: " .. describe(position_error)
      end
      return true
    end

    function Panel:set_size(width, height)
      if type(width) ~= "number" or width <= 0 or
         type(height) ~= "number" or height <= 0 then
        return nil, "panel.set_size: expected two positive numbers"
      end
      self.width, self.height = width, height
      return self:layout()
    end

    function Panel:set_anchor(anchor)
      if ANCHOR_X[anchor] == nil then
        return nil, "panel.set_anchor: argument 1: expected one of " ..
            table.concat(P.ANCHORS, ", ")
      end
      self.anchor = anchor
      self.fixed_x, self.fixed_y = nil, nil
      return self:layout()
    end

    function Panel:set_position(x, y)
      if type(x) ~= "number" or type(y) ~= "number" then
        return nil, "panel.set_position: expected two numbers"
      end
      self.fixed_x, self.fixed_y = x, y
      return self:layout()
    end

    function Panel:position()
      return self.x, self.y
    end

    function Panel:size()
      return self.width, self.height
    end

    function Panel:viewport()
      return self.viewport_width, self.viewport_height
    end

    -- The escape hatch, and the reason this framework is not a ceiling: every
    -- control it made is reachable by name - "root", "frame", "row1".."rowN"
    -- and each button's id - and everything wotb.ui can do to a control it can
    -- do to these. The panel keeps owning it: destroying a control handed out
    -- here leaves the panel holding a dead handle, so let unmount do that.
    function Panel:control(name)
      local control = self.controls[name]
      if control == nil then
        return nil, "panel.control: this panel has no control named " ..
            describe(name)
      end
      return control
    end

    function Panel:errors()
      return self.error_count, self.last_error
    end

    -- ---- the frame driver --------------------------------------------------

    local function refresh_context(self)
      self.context_dirty = false
      local context = wotb and wotb.context
      if type(context) ~= "table" or type(context.current) ~= "function" or
         type(context.should_show) ~= "function" then
        -- Fail closed, the same way wotb.context.should_show itself does. A
        -- panel that cannot prove it is in an allowed context does not draw;
        -- an author who does not want a gate passes contexts = false and gets
        -- this branch skipped entirely.
        self.context_allowed = false
        return false
      end
      local mask, mask_error = context.current()
      if mask == nil then
        report(self, "context", mask_error)
      else
        self.context_mask = mask
      end
      local allowed, why = context.should_show(
          self.contexts.visible, self.contexts.blocked, self.context_mask)
      if allowed == nil then
        report(self, "visibility", why)
        allowed = false
      end
      if allowed ~= self.context_allowed then fire(self, "visibility", allowed) end
      self.context_allowed = allowed
      return allowed
    end

    -- One call per frame from on_frame, and the only one a panel needs.
    --
    -- Returns true while the panel is mounted and the context allows it;
    -- false plus an optional reason when it is not showing; nil plus a message
    -- only for a bad argument. `if panel:update(frame) then` therefore reads
    -- correctly for all three.
    --
    -- What it costs on a frame with nothing to do: one modulo, two field
    -- reads and a return. What it costs periodically, at the cadences the
    -- shipped panels proved live and which every one of these is overridable
    -- from the spec:
    --
    --   context_frames (15) - one wotb.core.get_context, four times a second.
    --   rebind_frames  (30) - get_active_screen + set_parent + release, twice
    --                         a second, and immediately when the screen-changed
    --                         topic says so. Set to 0 to rely on the topic
    --                         alone.
    --   probe_frames   (60) - get_viewport_size plus one control_get_snapshot,
    --                         once a second: the check that notices the client
    --                         took the tree down under the panel.
    --   retry_frames   (60) - how long a failed mount waits before trying
    --                         again, so a client that is refusing does not get
    --                         asked sixty times a second.
    function Panel:update(frame_index)
      if math.type(frame_index) ~= "integer" then
        return nil, "panel.update: argument 1: expected the integer frame " ..
            "index on_frame was handed"
      end
      if self.destroyed then return false, "this panel was destroyed" end
      if self.contexts ~= nil then
        local allowed
        if self.context_dirty or self.context_frames == 0 or
           frame_index % self.context_frames == 0 then
          allowed = refresh_context(self)
        else
          allowed = self.context_allowed
        end
        if not allowed then
          if self.is_mounted then self:unmount() end
          return false
        end
      end

      if self.is_mounted and (self.rebind_pending or
          (self.rebind_frames > 0 and frame_index % self.rebind_frames == 0)) then
        rebind(self)
      end

      if not self.is_mounted then
        if frame_index < self.retry_at then return false end
        -- No layout() afterwards: construct() already placed the frame and
        -- sized the root against the viewport it had just read, and a second
        -- pass would be two more crossings per mount for no change.
        local ok, mount_error = self:mount()
        if not ok then
          -- The deadline is set by a failure and only by a failure. Setting it
          -- on every attempt was the first shape of this and it was wrong: a
          -- panel that unmounts because the player opened the mod catalog, and
          -- mounts again the moment they close it, would have sat blank for up
          -- to retry_frames because a *successful* mount had armed a timer
          -- meant for a refusing client.
          self.retry_at = frame_index + self.retry_frames
          return false, mount_error
        end
      end

      if self.probe_frames > 0 and frame_index % self.probe_frames == 0 then
        local api = ui()
        if api ~= nil then
          local before_width = self.viewport_width
          local before_height = self.viewport_height
          read_viewport(self, api)
          if before_width ~= self.viewport_width or
             before_height ~= self.viewport_height then
            local ok, layout_error = self:layout()
            if not ok then report(self, "layout", layout_error) end
          end
          local snapshot = slot(api, "control_get_snapshot")
          local root = self.controls.root
          if snapshot ~= nil and root ~= nil and snapshot(root) == nil then
            -- The client destroyed this panel's tree - a screen teardown that
            -- took the children with it. Everything the panel still holds is
            -- stale, so it is given back and the next frame remounts.
            self:unmount()
            self.retry_at = frame_index + 1
            return false, "the client destroyed this panel's tree"
          end
        end
      end

      return self.is_visible
    end

)lua"
// Seam 4 of 4: the dynamic controls (label/button/image/scroll, row/column
// layout, on, destroy), still inside build(P).
R"lua(
    -- ---- dynamic controls ------------------------------------------------
    --
    -- label/button/image/scroll add to the spec at any time: a mounted panel
    -- gets the control at once, an unmounted one at its next mount, and the
    -- spec survives an unmount/mount cycle like everything else the panel
    -- models. Each answers the name panel:control() knows the control by.

    local function where_next_row(self)
      local last = self.rows[#self.rows]
      if last == nil then return self.padding_top or 20 end
      return last.y + last.height + 4
    end

    local function check_parent(self, where, opts)
      if opts.parent == nil then return true end
      if type(opts.parent) ~= "string" or
         string.find(opts.parent, "^scroll%d") == nil then
        return nil, where .. ": parent: expected a name panel:scroll() answered"
      end
      return true
    end

    function Panel:label(opts)
      local where = "panel.label"
      if self.destroyed then return nil, where .. ": this panel was destroyed" end
      if type(opts) ~= "table" then
        return nil, where .. ": argument 1: expected an options table"
      end
      local ok, parent_error = check_parent(self, where, opts)
      if not ok then return nil, parent_error end
      local index = #self.rows + 1
      if index > MAX_ROWS then
        return nil, where .. ": this panel already has " .. MAX_ROWS .. " rows"
      end
      local text, why = opt_string(where, "text", opts.text, "")
      if text == nil then return nil, why end
      local inner = self.width - 40
      local height, size, x, y, width, color
      height, why = opt_number(where, "height", opts.height, 32)
      if height == nil then return nil, why end
      size, why = opt_number(where, "size", opts.size, 18)
      if size == nil then return nil, why end
      x, why = opt_number(where, "x", opts.x, 20)
      if x == nil then return nil, why end
      y, why = opt_number(where, "y", opts.y, where_next_row(self))
      if y == nil then return nil, why end
      width, why = opt_number(where, "width", opts.width, inner)
      if width == nil then return nil, why end
      color, why = opt_color(where, "color", opts.color, WHITE)
      if color == nil then return nil, why end
      local row = { text = text, height = height, size = size, x = x, y = y,
                    width = width, color = color, visible = opts.visible ~= false,
                    parent = opts.parent }
      self.rows[index] = row
      if self.is_mounted then
        local api, api_error = ui()
        if api == nil then return nil, where .. ": " .. api_error end
        local parent = self.controls[opts.parent or ""] or self.controls.frame
        local control, make_error = make(self, api, "row" .. index, {
          type = api.CONTROL_TEXT, text = text,
          x = x, y = y, width = width, height = height,
          font = self.font, font_size = size, color = color,
          alignment = api.ALIGN_LEFT,
          visible = row.visible, interactable = false, parent = parent,
        })
        if control == nil then
          self.rows[index] = nil
          return nil, make_error
        end
        self.row_controls[index] = control
      end
      return index
    end

    function Panel:button(opts)
      local where = "panel.button"
      if self.destroyed then return nil, where .. ": this panel was destroyed" end
      if type(opts) ~= "table" then
        return nil, where .. ": argument 1: expected an options table"
      end
      local ok, parent_error = check_parent(self, where, opts)
      if not ok then return nil, parent_error end
      if #self.buttons >= MAX_BUTTONS then
        return nil, where .. ": this panel already has " .. MAX_BUTTONS ..
            " buttons"
      end
      local name, why = opt_string(where, "id", opts.id, nil)
      if name == nil then
        return nil, why or (where .. ": id: a button needs an id")
      end
      if name == "root" or name == "frame" or self.named_buttons[name] ~= nil or
         string.find(name, "^row%d") ~= nil or
         string.find(name, "^image%d") ~= nil or
         string.find(name, "^scroll%d") ~= nil then
        return nil, where .. ": id '" .. name .. "' is already taken by this panel"
      end
      if type(opts.x) ~= "number" or type(opts.y) ~= "number" or
         type(opts.width) ~= "number" or type(opts.height) ~= "number" then
        return nil, where .. ": x, y, width and height are required numbers"
      end
      if opts.on_click ~= nil and type(opts.on_click) ~= "function" then
        return nil, where .. ": on_click: expected a function"
      end
      local text, size, color, background
      text, why = opt_string(where, "text", opts.text, "")
      if text == nil then return nil, why end
      size, why = opt_number(where, "size", opts.size, 18)
      if size == nil then return nil, why end
      color, why = opt_color(where, "color", opts.color, WHITE)
      if color == nil then return nil, why end
      background, why = opt_color(where, "background", opts.background, nil)
      if background == nil and why ~= nil then return nil, why end
      local button = {
        id = name, text = text, x = opts.x, y = opts.y,
        width = opts.width, height = opts.height, size = size,
        color = color, background = background, on_click = opts.on_click,
        parent = opts.parent,
      }
      local index = #self.buttons + 1
      self.buttons[index] = button
      self.named_buttons[name] = index
      if self.is_mounted then
        local api, api_error = ui()
        if api == nil then return nil, where .. ": " .. api_error end
        local parent = self.controls[opts.parent or ""] or self.controls.frame
        local control, make_error = make(self, api, name, {
          type = api.CONTROL_BUTTON, text = text,
          x = opts.x, y = opts.y, width = opts.width, height = opts.height,
          font = self.font, font_size = size, color = color,
          background_color = background, alignment = api.ALIGN_CENTER,
          visible = true, enabled = true, interactable = true, parent = parent,
        })
        if control == nil then
          self.buttons[index] = nil
          self.named_buttons[name] = nil
          return nil, make_error
        end
        if button.on_click ~= nil then
          local subscribe, slot_error = slot(api, "event_subscribe")
          if subscribe == nil then return nil, where .. ": " .. slot_error end
          local handler = button.on_click
          local token, subscribe_error = subscribe(
              control, api.EVENT_CLICK, function()
                local call_ok, call_error = pcall(handler, self, name)
                if not call_ok then report(self, "on_click " .. name, call_error) end
              end)
          if token == nil then
            return nil, where .. ": " .. name .. ": " .. describe(subscribe_error)
          end
          self.tokens[#self.tokens + 1] = token
        end
      end
      return name
    end

    function Panel:image(opts)
      local where = "panel.image"
      if self.destroyed then return nil, where .. ": this panel was destroyed" end
      if type(opts) ~= "table" then
        return nil, where .. ": argument 1: expected an options table"
      end
      local ok, parent_error = check_parent(self, where, opts)
      if not ok then return nil, parent_error end
      if type(opts.texture) ~= "string" or opts.texture == "" then
        return nil, where .. ": texture: expected a texture URI"
      end
      if type(opts.x) ~= "number" or type(opts.y) ~= "number" or
         type(opts.width) ~= "number" or type(opts.height) ~= "number" then
        return nil, where .. ": x, y, width and height are required numbers"
      end
      local index = #self.images + 1
      local image = { texture = opts.texture, x = opts.x, y = opts.y,
                      width = opts.width, height = opts.height,
                      visible = opts.visible ~= false, parent = opts.parent }
      self.images[index] = image
      local name = "image" .. index
      if self.is_mounted then
        local api, api_error = ui()
        if api == nil then return nil, where .. ": " .. api_error end
        local parent = self.controls[opts.parent or ""] or self.controls.frame
        local control, make_error = make(self, api, name, {
          type = api.CONTROL_IMAGE, texture = opts.texture,
          x = opts.x, y = opts.y, width = opts.width, height = opts.height,
          visible = image.visible, interactable = false, parent = parent,
        })
        if control == nil then
          self.images[index] = nil
          return nil, make_error
        end
      end
      return name
    end

    -- A scrollable region inside the frame; rows, buttons and images name
    -- it as `parent`. Their x/y are relative to the region.
    function Panel:scroll(opts)
      local where = "panel.scroll"
      if self.destroyed then return nil, where .. ": this panel was destroyed" end
      if type(opts) ~= "table" then
        return nil, where .. ": argument 1: expected an options table"
      end
      if type(opts.x) ~= "number" or type(opts.y) ~= "number" or
         type(opts.width) ~= "number" or type(opts.height) ~= "number" then
        return nil, where .. ": x, y, width and height are required numbers"
      end
      local background, why = opt_color(where, "background", opts.background, nil)
      if background == nil and why ~= nil then return nil, why end
      local index = #self.scrolls + 1
      local name = "scroll" .. index
      self.scrolls[index] = { x = opts.x, y = opts.y, width = opts.width,
                              height = opts.height, background = background,
                              visible = opts.visible ~= false }
      if self.is_mounted then
        local api, api_error = ui()
        if api == nil then return nil, where .. ": " .. api_error end
        local kind = api.CONTROL_SCROLL_VIEW
        if math.type(kind) ~= "integer" then kind = api.CONTROL_CONTAINER end
        local control, make_error = make(self, api, name, {
          type = kind, x = opts.x, y = opts.y, width = opts.width,
          height = opts.height, background_color = background,
          visible = self.scrolls[index].visible, interactable = true,
          parent = self.controls.frame,
        })
        if control == nil then
          self.scrolls[index] = nil
          return nil, make_error
        end
      end
      return name
    end

    -- row/column: a list of items laid out along one axis from (x, y) with
    -- a gap. Each item is { kind = "label" | "button" | "image", ... } with
    -- the options that kind takes, minus x and y (the layout supplies them;
    -- a label wants width/height, a button and an image always do).
    local function lay_out(self, where, opts, horizontal)
      if self.destroyed then return nil, where .. ": this panel was destroyed" end
      if type(opts) ~= "table" or type(opts.items) ~= "table" then
        return nil, where .. ": argument 1: expected { items = {...} }"
      end
      local x, y, gap = opts.x or 20, opts.y or where_next_row(self), opts.gap or 8
      if type(x) ~= "number" or type(y) ~= "number" or type(gap) ~= "number" then
        return nil, where .. ": x, y and gap must be numbers"
      end
      local names = {}
      local cursor_x, cursor_y = x, y
      for index = 1, #opts.items do
        local item = opts.items[index]
        if type(item) ~= "table" then
          return nil, where .. ": items[" .. index .. "]: expected a table"
        end
        local copy = {}
        for key, value in next, item do copy[key] = value end
        copy.x, copy.y = cursor_x, cursor_y
        copy.parent = copy.parent or opts.parent
        local width = copy.width or (self.width - 40)
        local height = copy.height or 32
        copy.width, copy.height = width, height
        local name, item_error
        if item.kind == "label" or item.kind == nil then
          name, item_error = self:label(copy)
        elseif item.kind == "button" then
          name, item_error = self:button(copy)
        elseif item.kind == "image" then
          name, item_error = self:image(copy)
        else
          return nil, where .. ": items[" .. index .. "].kind: expected " ..
              "'label', 'button' or 'image'"
        end
        if name == nil then return nil, item_error end
        names[#names + 1] = name
        if horizontal then cursor_x = cursor_x + width + gap
        else cursor_y = cursor_y + height + gap end
      end
      return names
    end

    -- panel:row(index) keeps its older meaning - a row's text and
    -- visibility - and panel:row({ items = ... }) lays items out
    -- horizontally; the argument's type says which.
    local read_row = Panel.row
    function Panel:row(arg)
      if type(arg) == "table" then return lay_out(self, "panel.row", arg, true) end
      return read_row(self, arg)
    end
    function Panel:column(opts) return lay_out(self, "panel.column", opts, false) end

    -- panel:on("mounted" | "unmounted" | "visibility" | "error", fn)
    function Panel:on(name, fn)
      if name ~= "mounted" and name ~= "unmounted" and name ~= "visibility" and
         name ~= "error" then
        return nil, "panel.on: argument 1: expected 'mounted', 'unmounted', " ..
            "'visibility' or 'error'"
      end
      if fn ~= nil and type(fn) ~= "function" then
        return nil, "panel.on: argument 2: expected a function or nil"
      end
      self.listeners[name] = fn
      return true
    end

    -- Unmounts and forgets the spec: a destroyed panel creates nothing at a
    -- later mount and update() answers false. The object stays a table so a
    -- stale reference cannot raise.
    function Panel:destroy()
      self:unmount()
      for index = #self.rows, 1, -1 do self.rows[index] = nil end
      for index = #self.buttons, 1, -1 do self.buttons[index] = nil end
      for index = #self.images, 1, -1 do self.images[index] = nil end
      for index = #self.scrolls, 1, -1 do self.scrolls[index] = nil end
      for name in next, self.named_buttons do self.named_buttons[name] = nil end
      self.destroyed = true
      return true
    end
  end

  setmetatable(module, { __index = realise })
  wotb.panel = module
end
)lua";

// ---------------------------------------------------------------------------
// wotb.dava.run_on_main - safe fire-and-forget DAVA execution
// ---------------------------------------------------------------------------
//
// The client owns DAVA objects on its MAIN thread while Lua frame callbacks
// normally arrive on RENDER. Calling a synchronous native DAVA slot from that
// callback can close a RENDER -> MAIN -> RENDER wait cycle inside the game.
// This helper makes the required crossing explicit and releases the temporary
// task handle before user code starts, so an error or instruction-budget trip
// cannot leak it. The generated callback bridge still serialises access to the
// Lua state and applies the script instruction budget.
const char kDavaMainThreadLibrary[] = R"lua(
do
  local dava = wotb and wotb.dava
  if type(dava) == "table" then
    local inside_main = false

    local function guard(table_value, name, prefix)
      local original = table_value and table_value[name]
      if type(original) ~= "function" then return end
      table_value[name] = function(...)
        if not inside_main then
          return nil, prefix .. name ..
              ": call it inside wotb.dava.run_on_main(function() ... end)"
        end
        return original(...)
      end
    end

    local main_only = {
      "class_is_registered", "class_create", "create_material",
      "create_texture", "create_mesh", "create_mesh_consumer",
      "material_set_property", "material_remove_property",
      "material_set_flag", "material_remove_flag", "material_set_texture",
      "material_remove_texture", "material_set_fx", "material_set_quality",
      "material_apply", "mesh_hot_swap", "create_stock_tracer",
    }
    for _, name in ipairs(main_only) do guard(dava, name, "dava.") end

    local loaders = wotb and wotb.loaders
    if type(loaders) == "table" then
      guard(loaders, "load_dava_yaml", "loaders.")
      guard(loaders, "open_dava_archive", "loaders.")
    end

    function dava.run_on_main(callback)
      if type(callback) ~= "function" then
        return nil, "dava.run_on_main: argument 1: expected a function"
      end

      local async = wotb and wotb.async
      if type(async) ~= "table" or
         type(async.dispatch_to_main_thread) ~= "function" then
        return nil, "dava.run_on_main: MAIN dispatcher is unavailable"
      end

      local task
      local function invoke()
        local current = task
        task = nil
        local handles = wotb and wotb.handles
        if current ~= nil and type(handles) == "table" and
           type(handles.release) == "function" then
          handles.release(current)
        end

        inside_main = true
        local ok, callback_error = pcall(callback)
        inside_main = false
        if not ok then
          local message =
              "dava.run_on_main callback failed: " .. tostring(callback_error)
          local log = wotb and wotb.log
          if type(log) == "table" and type(log.error) == "function" then
            log.error("%s", message)
          else
            print(message)
          end
        end
      end

      local queued, queue_error = async.dispatch_to_main_thread(invoke)
      if queued == nil then
        return nil, "dava.run_on_main: " .. tostring(queue_error)
      end
      task = queued
      return true
    end
  end
end
)lua";

// ---------------------------------------------------------------------------
// wotb.mod - the Lua half: capability, info, on_disable
// ---------------------------------------------------------------------------
//
// Extends the table RegisterModLibrary built (id, permissions,
// has_permission) rather than replacing it. wotb.mod is host-owned identity,
// not a client interface, so unlike every wrapper in this file it may exist
// on any client - and it is created here only for a state that was built
// without a script, where the C half was never installed.
//
// on_disable(fn) keeps handlers in Lua. The host reads the author's global
// on_disable through a *raw* lookup (LuaScript::Deactivate, on purpose: a
// metatable on the script's globals must not run code during its own
// teardown), so nothing installed on the globals table could ever wrap it.
// Instead the host calls __run_disable_handlers here by name, before the
// global, and each handler runs under its own pcall: one that raises is
// reported through wotb.log and the next still runs.
const char kModLibrary[] = R"lua(
do
  local mod = wotb.mod
  if type(mod) ~= "table" then
    mod = {}
    wotb.mod = mod
  end

  -- WotbModV3CapabilityStatus, spelled here because base-header enums are
  -- deliberately not published as constants (see wotb.available's header).
  local STATUS_NAMES = {
    [0] = "available",
    [1] = "unavailable",
    [2] = "client_mismatch",
    [3] = "permission_denied",
    [4] = "context_restricted",
    [5] = "degraded",
  }

  local function describe_capability(info)
    local status = STATUS_NAMES[info.status] or
        ("status " .. tostring(info.status))
    return status, {
      name = info.name,
      status = status,
      status_code = info.status,
      reason = info.unavailable_reason,
      interface_version = info.interface_version,
      allowed_contexts = info.allowed_contexts,
      permission_tier = info.permission_tier,
    }
  end

  -- mod.capabilities() -> array of { name, status, reason, ... } | nil, err
  --
  -- Every capability the runtime registers, by name, sorted. Capability
  -- names are the runtime's ("gameplay.tweak.hud", hook- and
  -- interface-derived records), NOT interface ids: "wotbmod.gameplay.hud"
  -- is an interface and has no capability record of its own on the live
  -- client (measured 2026-09-06), so this list is how an author learns
  -- what capability() can be asked.
  function mod.capabilities()
    local caps = wotb and wotb.capabilities
    if type(caps) ~= "table" or type(caps.get_count) ~= "function" or
       type(caps.get_at) ~= "function" then
      return nil, "mod.capabilities: wotb.capabilities is unavailable"
    end
    local count, count_error = caps.get_count()
    if count == nil then
      return nil, "mod.capabilities: " .. tostring(count_error)
    end
    local list = {}
    for index = 0, count - 1 do
      local info = caps.get_at(index)
      if type(info) == "table" then
        local _, record = describe_capability(info)
        list[#list + 1] = record
      end
    end
    table.sort(list, function(left, right) return left.name < right.name end)
    return list
  end

  -- mod.capability(name) -> status_word, info | nil, message
  --
  -- The client's own answer for one registered capability name, as a word
  -- plus the full record. "available" and "degraded" mean there is a
  -- backend; the other words say why a call would be refused, in the
  -- client's words (info.reason). A name the runtime never registered -
  -- an interface id, a typo - answers nil plus the runtime's "capability is
  -- not registered"; mod.capabilities() lists what it knows.
  function mod.capability(name)
    if type(name) ~= "string" or name == "" then
      return nil, "mod.capability: argument 1: expected an interface name"
    end
    local caps = wotb and wotb.capabilities
    if type(caps) ~= "table" or type(caps.query) ~= "function" then
      return nil, "mod.capability: wotb.capabilities.query is unavailable"
    end
    local info, query_error = caps.query(name)
    if info == nil then
      return nil, "mod.capability: " .. tostring(query_error)
    end
    return describe_capability(info)
  end

  -- Everything the script can learn about itself and its host in one table.
  -- host is the Lua host package the script runs inside (its id, state and
  -- tier come from lifecycle.get_info), never the script.
  function mod.info()
    local info = {}
    if type(mod.id) == "function" then info.id = mod.id() end
    if type(mod.permissions) == "function" then
      info.permissions = mod.permissions()
    else
      info.permissions = {}
    end
    local lifecycle = wotb and wotb.lifecycle
    if type(lifecycle) == "table" and type(lifecycle.get_info) == "function" then
      local host, host_error = lifecycle.get_info()
      if host ~= nil then
        info.host = {
          id = host.id,
          state = host.state,
          permission_tier = host.permission_tier,
          hot_reload_supported = host.hot_reload_supported ~= 0,
          hot_reload_reason = host.hot_reload_reason,
        }
      else
        info.host_unavailable = tostring(host_error)
      end
    else
      info.host_unavailable = "wotb.lifecycle.get_info is unavailable"
    end
    return info
  end

  local handlers = {}

  -- mod.on_disable(fn) -> handle. Several may be registered; they run newest
  -- first, before the author's global on_disable, when the script is
  -- disabled, reloaded or unloaded (the host does not distinguish the three
  -- and this module does not pretend to).
  function mod.on_disable(fn)
    if type(fn) ~= "function" then
      return nil, "mod.on_disable: argument 1: expected a function"
    end
    handlers[#handlers + 1] = fn
    return #handlers
  end

  function mod.off_disable(handle)
    if math.type(handle) ~= "integer" or handlers[handle] == nil then
      return nil, "mod.off_disable: no handler has handle " .. tostring(handle)
    end
    handlers[handle] = false
    return true
  end

  -- Called by the host (LuaScript::Deactivate) by name. Each handler runs
  -- once, under pcall, and is dropped whether it returned or raised.
  function mod.__run_disable_handlers()
    for index = #handlers, 1, -1 do
      local fn = handlers[index]
      handlers[index] = nil
      if fn then
        local ok, call_error = pcall(fn)
        if not ok then
          local log = wotb and wotb.log
          if type(log) == "table" and type(log.error) == "function" then
            pcall(log.error, "on_disable handler %d raised: %s", index,
                  tostring(call_error))
          end
        end
      end
    end
  end
end
)lua";

// ---------------------------------------------------------------------------
// wotb.ges extensions - on/off/observe/decode/is_available
// ---------------------------------------------------------------------------
//
// The one prelude that extends an interface table, and the rule that makes
// that legal: it never creates the table. RegisterGes builds wotb.ges only
// when the client published the GES interface, and this chunk adds to it only
// when it finds subscribe there at load - the precedent is wotb.dava's
// run_on_main. On a client without GES nothing here runs and
// wotb.available('ges') keeps answering false. Every function still resolves
// wotb.ges at call time, so a script that clears the table gets "unavailable"
// as a value.
const char kGesExtensionsLibrary[] = R"lua(
do
  local installed = wotb and wotb.ges
  if type(installed) == "table" and type(installed.subscribe) == "function" then
    local function api(where)
      local value = wotb and wotb.ges
      if type(value) ~= "table" or type(value.subscribe) ~= "function" then
        return nil, where .. ": wotb.ges is unavailable"
      end
      return value
    end

    function installed.is_available()
      return api("ges.is_available") ~= nil
    end

    -- on/off: the same subscription as subscribe/unsubscribe under the names
    -- every other facade in this host uses.
    function installed.on(pattern, fn)
      local ges, ges_error = api("ges.on")
      if not ges then return nil, ges_error end
      return ges.subscribe(pattern, fn)
    end

    function installed.off(handle)
      local ges, ges_error = api("ges.off")
      if not ges then return nil, ges_error end
      return ges.unsubscribe(handle)
    end

    -- decode(ev) -> fields, unreadable | nil, message
    --
    -- Every schema field by name, read through ev:field so the runtime's
    -- own bounds and kind checks apply. A field the runtime cannot hand to
    -- Lua (a FastName, raw bytes) is absent from the first table and named
    -- in the second with the reason. No schema, no decode: nil plus why.
    function installed.decode(ev)
      local ok, schema = pcall(function() return ev.schema end)
      if not ok then
        return nil, "ges.decode: argument 1: expected a ges event (" ..
            tostring(schema) .. ")"
      end
      if type(schema) ~= "table" then
        return nil, "ges.decode: this event carries no schema"
      end
      if type(ev.field) ~= "function" then
        return nil, "ges.decode: argument 1: expected a ges event"
      end
      local fields = {}
      local unreadable = nil
      for index = 1, #schema do
        local entry = schema[index]
        local name = type(entry) == "table" and entry.name or nil
        if type(name) == "string" and name ~= "" then
          local value, read_error = ev:field(name)
          if value == nil then
            unreadable = unreadable or {}
            unreadable[name] = tostring(read_error)
          else
            fields[name] = value
          end
        end
      end
      return fields, unreadable
    end

    -- observe(pattern, options, fn) -> handle | nil, message
    --
    -- options.once: give the subscription back after the first delivery.
    -- options.decode: call fn(fields, ev, unreadable) with decode(ev) done,
    -- instead of fn(ev). The event object is still valid only inside fn.
    function installed.observe(pattern, options, fn)
      if type(options) == "function" and fn == nil then
        fn, options = options, nil
      end
      if options ~= nil and type(options) ~= "table" then
        return nil, "ges.observe: argument 2: expected an options table"
      end
      if type(fn) ~= "function" then
        return nil, "ges.observe: expected a handler function"
      end
      local once = options ~= nil and options.once == true
      local decode = options ~= nil and options.decode == true
      local ges, ges_error = api("ges.observe")
      if not ges then return nil, ges_error end
      local handle = nil
      local function deliver(ev)
        if once and handle ~= nil then
          local current = handle
          handle = nil
          ges.unsubscribe(current)
        end
        if decode then
          local fields, unreadable = installed.decode(ev)
          return fn(fields, ev, unreadable)
        end
        return fn(ev)
      end
      local token, subscribe_error = ges.subscribe(pattern, deliver)
      if token == nil then return nil, subscribe_error end
      handle = token
      return token
    end
  end
end
)lua";

// ---------------------------------------------------------------------------
// wotb.hud - the facade over wotb.gameplay_hud, built on first touch
// ---------------------------------------------------------------------------
//
// wotb.gameplay_hud is 34 generated slots that take packed integers: an rgba
// colour as one uint32, a boolean as 0/1, an anchor or a style as an enum
// value. This module is the same 34 slots grouped the way the ABI header
// groups them, taking {r, g, b, a} tables, booleans and names, and answering
// nil, message in one shape. It adds no capability: every call lands on the
// raw slot with the client's own answer passed through, so a slot this build
// refuses (minimap_set_show_last_known, hit_indicator_set_color_pen, the
// DIRECTIONAL style, STAT_KILLS - see docs/API_STATUS_RU.md) refuses here
// with the same words.
//
// Lazy for the reason wotb.panel is: ~45 closures are built by the first
// index and cached, and a script that never touches the HUD pays one table
// and one metatable at load.
//
// Named wotb.hud and not wotb.gameplay_hud.*, for the reason every wrapper in
// this file gives: on a client without the interface the answer an author
// needs is the sentence "wotb.gameplay_hud is unavailable", and a function
// living on that table could not say it - nor may a Lua function make the
// table look published to wotb.available.
const char kHudLibrary[] = R"lua(
do
  local module = {}
  local build

  local function realise(_, key)
    setmetatable(module, nil)
    local construct = build
    build = nil
    if construct ~= nil then construct(module) end
    return rawget(module, key)
  end

  build = function(H)
    -- Every method, by group: { raw slot, argument kind }. The kind is how
    -- the author's argument is converted before the raw slot sees it.
    local SLOTS = {
      reticle = {
        set_texture = { "reticle_set_texture", "uri" },
        set_sniper_texture = { "reticle_set_sniper_texture", "uri" },
        set_color = { "reticle_set_color", "color" },
        set_size = { "reticle_set_size", "scale" },
        set_reloading_indicator = { "reticle_set_reloading_indicator", "flag" },
        set_dispersion_circle = { "reticle_set_dispersion_circle", "flag" },
      },
      damage_log = {
        show = { "damagelog_set_enabled", "on" },
        hide = { "damagelog_set_enabled", "off" },
        set_enabled = { "damagelog_set_enabled", "flag" },
        set_position = { "damagelog_set_position", "anchor" },
        set_max_entries = { "damagelog_set_max_entries", "count" },
        set_show_blocked = { "damagelog_set_show_blocked", "flag" },
        set_show_ricochet = { "damagelog_set_show_ricochet", "flag" },
        set_show_module_damage = { "damagelog_set_show_module_damage", "flag" },
        set_format = { "damagelog_set_format", "text" },
        set_filter_own = { "damagelog_set_filter_own", "flag" },
      },
      session_stats = {
        show = { "session_stats_set_enabled", "on" },
        hide = { "session_stats_set_enabled", "off" },
        set_enabled = { "session_stats_set_enabled", "flag" },
        set_fields = { "session_stats_set_fields", "stats" },
      },
      minimap = {
        set_size = { "minimap_set_size", "scale" },
        set_opacity = { "minimap_set_opacity", "unit" },
        set_show_last_known = { "minimap_set_show_last_known", "flag" },
        set_show_artillery_range = { "minimap_set_show_artillery_range", "flag" },
        set_show_drawing = { "minimap_set_show_drawing", "flag" },
        remove_marker = { "minimap_remove_marker", "count" },
      },
      sixth_sense = {
        set_texture = { "sixth_sense_set_texture", "uri" },
        set_sound = { "sixth_sense_set_sound", "uri" },
        set_scale = { "sixth_sense_set_scale", "scale" },
        set_delay = { "sixth_sense_set_delay_ms", "milliseconds" },
      },
      hit_indicator = {
        set_style = { "hit_indicator_set_style", "style" },
        set_color_hit = { "hit_indicator_set_color_hit", "color" },
        set_color_pen = { "hit_indicator_set_color_pen", "color" },
        set_color_ricochet = { "hit_indicator_set_color_ricochet", "color" },
        set_color_crit = { "hit_indicator_set_color_crit", "color" },
      },
    }
    -- The three slots with a shape of their own, written out below.
    local EXTRA_SLOTS = { "minimap_add_marker", "sixth_sense_set_position",
                          "reset" }

    -- Resolved at call time, never captured (see wotb.panel's ui()).
    local function raw()
      local api = wotb and wotb.gameplay_hud
      if type(api) ~= "table" or type(api.reset) ~= "function" then
        return nil, "wotb.gameplay_hud is unavailable on this client"
      end
      return api
    end

    local function describe(detail)
      local ok, text = pcall(tostring, detail)
      if ok then return text end
      return "<an error whose __tostring raised>"
    end

    -- {r, g, b[, a]} in 0..1, or an integer 0xRRGGBBAA as the ABI takes it.
    local function pack_color(where, value)
      if math.type(value) == "integer" then
        if value < 0 or value > 0xFFFFFFFF then
          return nil, where .. ": argument 1: a colour integer is 0xRRGGBBAA"
        end
        return value
      end
      if type(value) ~= "table" then
        return nil, where .. ": argument 1: expected a colour table " ..
            "{r, g, b[, a]} in 0..1 or an integer 0xRRGGBBAA"
      end
      local channels = { value.r, value.g, value.b, value.a }
      if channels[4] == nil then channels[4] = 1.0 end
      local packed = 0
      for index = 1, 4 do
        local channel = channels[index]
        if type(channel) ~= "number" or channel ~= channel or
           channel < 0 or channel > 1 then
          return nil, where .. ": argument 1: colour channels r, g, b, a " ..
              "are numbers in 0..1"
        end
        packed = packed * 256 + math.floor(channel * 255 + 0.5)
      end
      return packed
    end
    H.rgba = function(value) return pack_color("hud.rgba", value) end

    -- A constant on wotb.gameplay_hud by short name: "top-left" ->
    -- ANCHOR_TOP_LEFT, "compact" -> HIT_STYLE_COMPACT, "damage" ->
    -- STAT_DAMAGE. Integers pass through as the enum values they are.
    local function constant(where, api, prefix, value)
      if math.type(value) == "integer" then
        if value < 0 then
          return nil, where .. ": argument 1: expected a non-negative integer"
        end
        return value
      end
      if type(value) ~= "string" or value == "" then
        return nil, where .. ": argument 1: expected a " .. prefix ..
            "* name or its integer value"
      end
      local key = prefix .. string.upper((string.gsub(value, "[-%s]", "_")))
      local resolved = api[key]
      if math.type(resolved) ~= "integer" then
        return nil, where .. ": '" .. value .. "' is not a wotb.gameplay_hud." ..
            prefix .. "* name on this client"
      end
      return resolved
    end

    local function convert(where, kind, value, api)
      if kind == "on" then return 1 end
      if kind == "off" then return 0 end
      if kind == "flag" then
        if type(value) ~= "boolean" then
          return nil, where .. ": argument 1: expected a boolean"
        end
        return value and 1 or 0
      end
      if kind == "color" then return pack_color(where, value) end
      if kind == "scale" or kind == "milliseconds" or kind == "unit" then
        if type(value) ~= "number" or value ~= value or value < 0 or
           value == math.huge then
          return nil, where .. ": argument 1: expected a finite number >= 0"
        end
        if kind == "unit" and value > 1 then
          return nil, where .. ": argument 1: expected a value in 0..1"
        end
        return value
      end
      if kind == "count" then
        if math.type(value) ~= "integer" or value < 0 then
          return nil, where .. ": argument 1: expected a non-negative integer"
        end
        return value
      end
      if kind == "anchor" then return constant(where, api, "ANCHOR_", value) end
      if kind == "style" then return constant(where, api, "HIT_STYLE_", value) end
      if kind == "stats" then
        -- One name, an array of names, or the mask itself.
        if type(value) == "table" then
          local mask = 0
          for index = 1, #value do
            local bit, bit_error = constant(where, api, "STAT_", value[index])
            if bit == nil then return nil, bit_error end
            mask = mask | bit
          end
          return mask
        end
        return constant(where, api, "STAT_", value)
      end
      if kind == "uri" or kind == "text" then
        if type(value) ~= "string" or value == "" then
          return nil, where .. ": argument 1: expected a non-empty string"
        end
        return value
      end
      return nil, where .. ": unknown argument kind " .. tostring(kind)
    end

    -- The shape every method shares: resolve the interface, resolve the
    -- slot, convert the argument, call, prefix the answer.
    local function method(where, slot, kind)
      return function(value)
        local api, api_error = raw()
        if not api then return nil, where .. ": " .. api_error end
        local fn = api[slot]
        if type(fn) ~= "function" then
          return nil, where .. ": wotb.gameplay_hud." .. slot ..
              " is unavailable on this client"
        end
        local argument, convert_error = convert(where, kind, value, api)
        if argument == nil then return nil, convert_error end
        local ok, call_error = fn(argument)
        if not ok then return nil, where .. ": " .. describe(call_error) end
        return true
      end
    end

    for group_name, methods in next, SLOTS do
      local group = {}
      for method_name, spec in next, methods do
        group[method_name] = method("hud." .. group_name .. "." .. method_name,
                                    spec[1], spec[2])
      end
      H[group_name] = group
    end

    -- minimap.add_marker(world_x, world_z, label, colour) -> marker_id
    function H.minimap.add_marker(world_x, world_z, label, color)
      local where = "hud.minimap.add_marker"
      local api, api_error = raw()
      if not api then return nil, where .. ": " .. api_error end
      if type(api.minimap_add_marker) ~= "function" then
        return nil, where .. ": wotb.gameplay_hud.minimap_add_marker is " ..
            "unavailable on this client"
      end
      if type(world_x) ~= "number" or type(world_z) ~= "number" or
         world_x ~= world_x or world_z ~= world_z then
        return nil, where .. ": arguments 1 and 2: expected world x and z"
      end
      if type(label) ~= "string" then
        return nil, where .. ": argument 3: expected a label string"
      end
      local rgba, color_error = pack_color(where, color == nil and
          { r = 1, g = 1, b = 1, a = 1 } or color)
      if rgba == nil then return nil, color_error end
      local id, add_error = api.minimap_add_marker(world_x, world_z, label, rgba)
      if id == nil then return nil, where .. ": " .. describe(add_error) end
      return id
    end

    -- sixth_sense.set_position(x, y): screen position of the lamp.
    function H.sixth_sense.set_position(x, y)
      local where = "hud.sixth_sense.set_position"
      local api, api_error = raw()
      if not api then return nil, where .. ": " .. api_error end
      if type(api.sixth_sense_set_position) ~= "function" then
        return nil, where .. ": wotb.gameplay_hud.sixth_sense_set_position " ..
            "is unavailable on this client"
      end
      if type(x) ~= "number" or type(y) ~= "number" or x ~= x or y ~= y then
        return nil, where .. ": expected two numbers"
      end
      local ok, set_error = api.sixth_sense_set_position({ x = x, y = y })
      if not ok then return nil, where .. ": " .. describe(set_error) end
      return true
    end

    -- The ABI has one reset for every group; there is no per-group undo,
    -- and this module does not invent one.
    function H.reset()
      local api, api_error = raw()
      if not api then return nil, "hud.reset: " .. api_error end
      local ok, reset_error = api.reset()
      if not ok then return nil, "hud.reset: " .. describe(reset_error) end
      return true
    end

    -- "native" when the interface is published - the runtime publishes
    -- wotb.gameplay_hud only with a backend behind it - and "unavailable"
    -- with the reason otherwise. Not refined through capabilities.query:
    -- the interface has no capability record of its own on the live client
    -- (2026-09-06), and the per-slot answer is the truth for each call
    -- anyway. An overlay mode (mod-owned controls standing in for stock
    -- ones) does not exist and is not claimed.
    function H.mode()
      local api = raw()
      if not api then
        return "unavailable", "wotb.gameplay_hud is not published on this client"
      end
      return "native"
    end

    function H.available()
      return H.mode() == "native"
    end

    -- Which of the 34 raw slots this client published, by name, sorted. A
    -- published slot may still refuse a call; this is shape, not support.
    function H.status()
      local seen = {}
      local names = {}
      for _, methods in next, SLOTS do
        for _, spec in next, methods do
          if not seen[spec[1]] then
            seen[spec[1]] = true
            names[#names + 1] = spec[1]
          end
        end
      end
      for _, name in ipairs(EXTRA_SLOTS) do names[#names + 1] = name end
      table.sort(names)
      local mode, reason = H.mode()
      local api = wotb and wotb.gameplay_hud
      local published = {}
      local missing = {}
      for _, name in ipairs(names) do
        if type(api) == "table" and type(api[name]) == "function" then
          published[#published + 1] = name
        else
          missing[#missing + 1] = name
        end
      end
      return { mode = mode, reason = reason, published = published,
               missing = missing }
    end
  end

  setmetatable(module, { __index = realise })
  wotb.hud = module
end
)lua";

// ---------------------------------------------------------------------------
// wotb.screen - the UI tree by short names, over wotb.ui and wotb.ui_read
// ---------------------------------------------------------------------------
//
// The reading half of the UI as an author needs it: find a control under the
// active screen, read its text (the mirror the mod wrote, or the engine's
// live text), its rectangle and its flags, and change text/visibility with
// the game-owned distinction made before the call. The building half stays
// with wotb.panel, which mount() delegates to. notify/popup are the client's
// own toast and dialog slots, passed through.
//
// Every function takes and gives back the active-screen handle it needs;
// only root() hands one out, and says so.
const char kScreenLibrary[] = R"lua(
do
  local screen = {}

  local function ui(where)
    local api = wotb and wotb.ui
    if type(api) ~= "table" or type(api.get_active_screen) ~= "function" then
      return nil, where .. ": wotb.ui is unavailable on this client"
    end
    return api
  end

  local function slot(api, where, name)
    local fn = api[name]
    if type(fn) ~= "function" then
      return nil, where .. ": wotb.ui." .. name .. " is unavailable on this client"
    end
    return fn
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function release(handle)
    local handles = wotb and wotb.handles
    if handle ~= nil and type(handles) == "table" and
       type(handles.release) == "function" then
      handles.release(handle)
    end
  end

  -- fn(api, root) with the active screen taken and given back around it.
  -- A raise inside fn is a bug in the caller's code; it comes back as a
  -- value here so the screen handle is still released.
  local function with_root(where, fn)
    local api, api_error = ui(where)
    if not api then return nil, api_error end
    local get, why = slot(api, where, "get_active_screen")
    if not get then return nil, why end
    local root, get_error = get()
    if root == nil then return nil, where .. ": " .. describe(get_error) end
    local results = table.pack(pcall(fn, api, root))
    release(root)
    if not results[1] then return nil, where .. ": " .. describe(results[2]) end
    return table.unpack(results, 2, results.n)
  end

  -- The active screen's handle. Owned by the script: give it back with
  -- wotb.handles.release when done.
  function screen.root()
    local api, api_error = ui("screen.root")
    if not api then return nil, api_error end
    local get, why = slot(api, "screen.root", "get_active_screen")
    if not get then return nil, why end
    local root, get_error = get()
    if root == nil then return nil, "screen.root: " .. describe(get_error) end
    return root
  end

  -- find(name): a control id, or a path with '/' between ids, searched
  -- under the active screen. The client's answer for a name that is not
  -- there is nil plus its message; nothing is invented.
  function screen.find(name)
    if type(name) ~= "string" or name == "" then
      return nil, "screen.find: argument 1: expected a control id or path"
    end
    return with_root("screen.find", function(api, root)
      local by_path = string.find(name, "/", 1, true) ~= nil
      local finder, why = slot(api, "screen.find",
          by_path and "control_find_by_path" or "control_find_by_id")
      if not finder then return nil, why end
      local control, find_error = finder(root, name)
      if control == nil then return nil, "screen.find: " .. describe(find_error) end
      return control
    end)
  end

  function screen.children(control)
    local where = "screen.children"
    local api, api_error = ui(where)
    if not api then return nil, api_error end
    local count_of, why = slot(api, where, "control_get_child_count")
    if not count_of then return nil, why end
    local child_at
    child_at, why = slot(api, where, "control_get_child_at")
    if not child_at then return nil, why end
    local count, count_error = count_of(control)
    if count == nil then return nil, where .. ": " .. describe(count_error) end
    local list = {}
    for index = 0, count - 1 do
      local child, child_error = child_at(control, index)
      if child == nil then return nil, where .. ": " .. describe(child_error) end
      list[#list + 1] = child
    end
    return list
  end

  local function read(where, name, control)
    local api = wotb and wotb.ui_read
    if type(api) ~= "table" or type(api[name]) ~= "function" then
      return nil, where .. ": wotb.ui_read." .. name ..
          " is unavailable on this client"
    end
    local text, read_error = api[name](control)
    if text == nil then return nil, where .. ": " .. describe(read_error) end
    return text
  end

  -- The text the mod set through the API (the runtime's mirror).
  function screen.text(control)
    return read("screen.text", "control_get_text", control)
  end

  -- The text the engine draws right now - the only honest answer for a
  -- game-owned control the mod never wrote to.
  function screen.live_text(control)
    return read("screen.live_text", "control_get_live_text", control)
  end

  local function snapshot(where, control)
    local api, api_error = ui(where)
    if not api then return nil, api_error end
    local get, why = slot(api, where, "control_get_snapshot")
    if not get then return nil, why end
    local snap, snap_error = get(control)
    if snap == nil then return nil, where .. ": " .. describe(snap_error) end
    return snap, api
  end

  local function flag(api, snap, name)
    local bit = api[name]
    if math.type(bit) ~= "integer" then return nil end
    return (snap.flags & bit) ~= 0
  end

  function screen.rect(control)
    local snap, api = snapshot("screen.rect", control)
    if snap == nil then return nil, api end
    local g = snap.geometry or {}
    return { x = g.x, y = g.y, width = g.width, height = g.height }
  end

  function screen.visible(control)
    local snap, api = snapshot("screen.visible", control)
    if snap == nil then return nil, api end
    return flag(api, snap, "SNAPSHOT_VISIBLE") == true
  end

  function screen.game_owned(control)
    local snap, api = snapshot("screen.game_owned", control)
    if snap == nil then return nil, api end
    return flag(api, snap, "SNAPSHOT_GAME_OWNED") == true
  end

  -- Everything the snapshot says, with the flag bits as booleans.
  function screen.info(control)
    local snap, api = snapshot("screen.info", control)
    if snap == nil then return nil, api end
    local g = snap.geometry or {}
    return {
      id = snap.id, type = snap.type, z_order = snap.z_order,
      child_count = snap.child_count,
      rect = { x = g.x, y = g.y, width = g.width, height = g.height },
      visible = flag(api, snap, "SNAPSHOT_VISIBLE"),
      enabled = flag(api, snap, "SNAPSHOT_ENABLED"),
      interactable = flag(api, snap, "SNAPSHOT_INTERACTABLE"),
      focused = flag(api, snap, "SNAPSHOT_FOCUSED"),
      game_owned = flag(api, snap, "SNAPSHOT_GAME_OWNED"),
    }
  end

  -- A write to a game-owned control needs ui.modify.game; said before the
  -- call rather than after, in the facade's words, when the script is
  -- known not to hold it.
  local function guard(where, control)
    local snap, api = snapshot(where, control)
    if snap == nil then return nil, api end
    if flag(api, snap, "SNAPSHOT_GAME_OWNED") then
      local mod = wotb and wotb.mod
      if type(mod) == "table" and type(mod.has_permission) == "function" and
         mod.has_permission("ui.modify.game") == false then
        return nil, where .. ": this control is game-owned; ui.modify.game " ..
            "is required and this script does not hold it"
      end
    end
    return api
  end

  function screen.set_text(control, text)
    if type(text) ~= "string" then
      return nil, "screen.set_text: argument 2: expected a string"
    end
    local api, why = guard("screen.set_text", control)
    if not api then return nil, why end
    local set, slot_error = slot(api, "screen.set_text", "control_set_text")
    if not set then return nil, slot_error end
    local ok, set_error = set(control, text)
    if not ok then return nil, "screen.set_text: " .. describe(set_error) end
    return true
  end

  function screen.set_visible(control, on)
    if type(on) ~= "boolean" then
      return nil, "screen.set_visible: argument 2: expected a boolean"
    end
    local api, why = guard("screen.set_visible", control)
    if not api then return nil, why end
    local set, slot_error = slot(api, "screen.set_visible", "control_set_visible")
    if not set then return nil, slot_error end
    local ok, set_error = set(control, on)
    if not ok then return nil, "screen.set_visible: " .. describe(set_error) end
    return true
  end

  -- mount(spec) -> a mounted wotb.panel; unmount(panel) gives it back.
  function screen.mount(spec)
    local panel_api = wotb and wotb.panel
    if type(panel_api) ~= "table" then
      return nil, "screen.mount: wotb.panel is unavailable"
    end
    local panel, new_error = panel_api.new(spec)
    if panel == nil then return nil, "screen.mount: " .. describe(new_error) end
    local ok, mount_error = panel:mount()
    if not ok then return nil, "screen.mount: " .. describe(mount_error) end
    return panel
  end

  function screen.unmount(panel)
    if type(panel) ~= "table" or type(panel.unmount) ~= "function" then
      return nil, "screen.unmount: argument 1: expected a panel"
    end
    return panel:unmount()
  end

  -- A one-line panel at the top of the viewport, taken down by a timer.
  -- This is what notify() draws on a client whose own toast slot answers
  -- NOT_SUPPORTED (11.20 does), so the mod author sees the same thing on
  -- both kinds of client: their text, for `seconds`, then nothing.
  local function overlay(text, seconds)
    local panel_api = wotb and wotb.panel
    local timer_api = wotb and wotb.timer
    if type(panel_api) ~= "table" or type(panel_api.new) ~= "function" or
       type(timer_api) ~= "table" or type(timer_api.after) ~= "function" then
      return nil, "wotb.panel or wotb.timer is unavailable"
    end
    local width = #text * 11 + 48
    if width < 260 then width = 260 end
    if width > 720 then width = 720 end
    local panel, new_error = panel_api.new({
      id = "notify", width = width, height = 56, anchor = "top-center",
      margin = { top = 24 }, padding = 12, font_size = 20, contexts = false,
    })
    if panel == nil then return nil, describe(new_error) end
    local row, label_error = panel:label({ text = text, height = 32 })
    if row == nil then return nil, describe(label_error) end
    local ok, mount_error = panel:mount()
    if not ok then return nil, describe(mount_error) end
    timer_api.after(math.floor(seconds * 1000), function()
      panel:destroy()
    end)
    return true
  end

  -- The client's own toast: text for `seconds` (default 3). A client
  -- without a toast slot gets the overlay above instead; the answer is the
  -- same either way, and `how` says which one drew it ("toast"/"overlay").
  function screen.notify(text, seconds)
    if type(text) ~= "string" or text == "" then
      return nil, "screen.notify: argument 1: expected a non-empty string"
    end
    if seconds == nil then seconds = 3 end
    if type(seconds) ~= "number" or seconds <= 0 then
      return nil, "screen.notify: argument 2: expected seconds > 0"
    end
    local api, api_error = ui("screen.notify")
    if not api then return nil, api_error end
    local toast, why = slot(api, "screen.notify", "toast_show")
    if toast then
      local ok, toast_error = toast(text, seconds)
      if ok then return true, "toast" end
      why = describe(toast_error)
    end
    local drawn, overlay_error = overlay(text, seconds)
    if drawn then return true, "overlay" end
    return nil, "screen.notify: " .. why .. "; overlay: " .. overlay_error
  end

  -- The client's own dialog. options: title, message, accept (label),
  -- cancel (label; its presence makes a confirm dialog), modal.
  function screen.popup(options)
    if type(options) ~= "table" or type(options.message) ~= "string" then
      return nil, "screen.popup: argument 1: expected { message = ... }"
    end
    local api, api_error = ui("screen.popup")
    if not api then return nil, api_error end
    local confirm = options.cancel ~= nil
    local show, why = slot(api, "screen.popup",
        confirm and "confirm_show" or "dialog_show")
    if not show then return nil, why end
    local dialog, show_error = show({
      title = options.title or "", message = options.message,
      accept_label = options.accept or "OK",
      cancel_label = options.cancel or "",
      modal = options.modal ~= false and 1 or 0,
    })
    if dialog == nil then return nil, "screen.popup: " .. describe(show_error) end
    return dialog
  end

  wotb.screen = screen
end
)lua";

// ---------------------------------------------------------------------------
// wotb.vehicle - the local vehicle and skins, over wotb.vehicle_visual
// ---------------------------------------------------------------------------
//
// Client-only, as the interface itself is: a skin changes what this client
// draws and nothing the server knows. Every answer carries what the client
// said, including requires_model_reload - a pack that is applied but not yet
// visible is reported as exactly that.
const char kVehicleLibrary[] = R"lua(
do
  local vehicle = {}

  local function api(where)
    local value = wotb and wotb.vehicle_visual
    if type(value) ~= "table" or
       type(value.get_local_player_vehicle) ~= "function" then
      return nil, where .. ": wotb.vehicle_visual is unavailable on this client"
    end
    return value
  end

  local function slot(api_table, where, name)
    local fn = api_table[name]
    if type(fn) ~= "function" then
      return nil, where .. ": wotb.vehicle_visual." .. name ..
          " is unavailable on this client"
    end
    return fn
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function call(where, name, ...)
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    local fn, why = slot(api_table, where, name)
    if not fn then return nil, why end
    local first, second = fn(...)
    if first == nil then return nil, where .. ": " .. describe(second) end
    return first, second
  end

  -- The local player's vehicle handle. Owned by the script: give it back
  -- with wotb.handles.release when done.
  function vehicle.local_vehicle()
    return call("vehicle.local_vehicle", "get_local_player_vehicle")
  end

  -- The public roster (wotb.players.visible): records with handles.
  function vehicle.visible()
    local players = wotb and wotb.players
    if type(players) ~= "table" or type(players.visible) ~= "function" then
      return nil, "vehicle.visible: wotb.players is unavailable"
    end
    return players.visible()
  end

  function vehicle.is_local(handle)
    local value, why = call("vehicle.is_local", "is_local_player", handle)
    if value == nil then return nil, why end
    return value ~= 0
  end

  function vehicle.is_hangar(handle)
    local value, why = call("vehicle.is_hangar", "is_hangar_vehicle", handle)
    if value == nil then return nil, why end
    return value ~= 0
  end

  function vehicle.position(handle)
    return call("vehicle.position", "get_position", handle)
  end

  -- "unknown" | "loading" | "ready" | "destroyed", from the client's
  -- APPEARANCE_* constants at call time.
  function vehicle.appearance_state(handle)
    local api_table, api_error = api("vehicle.appearance_state")
    if not api_table then return nil, api_error end
    local code, why = call("vehicle.appearance_state", "get_appearance_state", handle)
    if code == nil then return nil, why end
    for _, name in ipairs({ "UNKNOWN", "LOADING", "READY", "DESTROYED" }) do
      if api_table["APPEARANCE_" .. name] == code then
        return string.lower(name), code
      end
    end
    return "state " .. tostring(code), code
  end

  vehicle.skin = {}

  -- register(desc): { id, vehicle_name, assets = { {kind, ...}, ... },
  -- priority, hangar_only }. asset_count is filled from the assets array.
  function vehicle.skin.register(desc)
    if type(desc) ~= "table" or type(desc.id) ~= "string" or desc.id == "" then
      return nil, "vehicle.skin.register: argument 1: expected { id = ..., " ..
          "vehicle_name = ..., assets = {...} }"
    end
    if type(desc.assets) ~= "table" then
      return nil, "vehicle.skin.register: assets: expected an array of assets"
    end
    local pack = {}
    for key, value in next, desc do pack[key] = value end
    pack.asset_count = #desc.assets
    if pack.priority == nil then pack.priority = 0 end
    if pack.hangar_only == nil then pack.hangar_only = 0 end
    if type(pack.hangar_only) == "boolean" then
      pack.hangar_only = pack.hangar_only and 1 or 0
    end
    return call("vehicle.skin.register", "skin_pack_register", pack)
  end

  function vehicle.skin.apply(pack, handle)
    local ok, why = call("vehicle.skin.apply", "skin_pack_apply", pack, handle)
    if not ok then return nil, why end
    return true
  end

  function vehicle.skin.rollback(pack)
    local ok, why = call("vehicle.skin.rollback", "skin_pack_rollback", pack)
    if not ok then return nil, why end
    return true
  end

  -- applied, asset_count, mounted_asset_count, requires_model_reload as a
  -- boolean, vehicle. requires_model_reload == true is the client saying
  -- "applied to resources, not to the model on screen yet".
  function vehicle.skin.state(pack)
    local state, why = call("vehicle.skin.state", "skin_pack_get_state", pack)
    if state == nil then return nil, why end
    return {
      applied = state.applied ~= 0,
      asset_count = state.asset_count,
      mounted_asset_count = state.mounted_asset_count,
      requires_model_reload = state.requires_model_reload ~= 0,
      vehicle = state.vehicle,
    }
  end

  function vehicle.skin.release(pack)
    local ok, why = call("vehicle.skin.release", "skin_pack_release", pack)
    if not ok then return nil, why end
    return true
  end

  vehicle.appearance = {}

  function vehicle.appearance.reset(handle)
    local ok, why = call("vehicle.appearance.reset", "restore_appearance", handle)
    if not ok then return nil, why end
    return true
  end

  function vehicle.set_skin(handle, uri)
    if type(uri) ~= "string" or uri == "" then
      return nil, "vehicle.set_skin: argument 2: expected a texture pack URI"
    end
    local ok, why = call("vehicle.set_skin", "set_custom_skin", handle, uri)
    if not ok then return nil, why end
    return true
  end

  function vehicle.set_camouflage(handle, uri)
    if type(uri) ~= "string" or uri == "" then
      return nil, "vehicle.set_camouflage: argument 2: expected a camouflage URI"
    end
    local ok, why = call("vehicle.set_camouflage", "set_custom_camouflage",
                         handle, uri)
    if not ok then return nil, why end
    return true
  end

  wotb.vehicle = vehicle
end
)lua";

// ---------------------------------------------------------------------------
// wotb.shells - projectiles observed, impacts and tracers drawn
// ---------------------------------------------------------------------------
//
// Observation over wotb.events (the four projectile topics and the local
// shell topic), visuals over wotb.projectile. Nothing here touches a
// trajectory, a shell's physics or the server's result: the interface has
// no such slot and this module invents none. valid_fields is turned into a
// table of booleans so an author never reads a field the client did not
// fill.
const char kShellsLibrary[] = R"lua(
do
  local shells = {}

  local TOPICS = {
    created = "TOPIC_PROJECTILE_CREATED",
    updated = "TOPIC_PROJECTILE_UPDATED",
    impact = "TOPIC_PROJECTILE_IMPACTED",
    destroyed = "TOPIC_PROJECTILE_DESTROYED",
    local_shot = "TOPIC_LOCAL_SHELL_FIRED",
  }
  local FIELDS = {
    { "FIELD_NATIVE_SHOT_ID", "native_shot_id" },
    { "FIELD_PRIMARY_ENTITY", "primary_entity_id" },
    { "FIELD_SECONDARY_ENTITY", "secondary_entity_id" },
    { "FIELD_SHELL_TYPE", "shell_type" },
    { "FIELD_ORIGIN", "origin" },
    { "FIELD_DIRECTION", "visible_direction" },
    { "FIELD_POSITION", "visible_position" },
    { "FIELD_IMPACT_POSITION", "impact_position" },
    { "FIELD_STOCK_SHOT_CODE", "stock_shot_code" },
  }

  local handlers = {}
  local next_handle = 1

  local function events()
    local value = wotb and wotb.events
    if type(value) ~= "table" or type(value.subscribe) ~= "function" or
       type(value.unsubscribe) ~= "function" then
      return nil, "wotb.events is unavailable"
    end
    return value
  end

  local function api(where)
    local value = wotb and wotb.projectile
    if type(value) ~= "table" or type(value.projectile_get_snapshot) ~= "function" then
      return nil, where .. ": wotb.projectile is unavailable on this client"
    end
    return value
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function report(name, detail)
    local log = wotb and wotb.log
    if type(log) ~= "table" or type(log.error) ~= "function" then return end
    log.error("shells.on_%s handler raised: %s", name, describe(detail))
  end

  -- Which snapshot fields the client filled, as booleans by field name.
  local function fields_of(snapshot)
    local api_table = wotb and wotb.projectile
    local mask = snapshot and snapshot.valid_fields
    local result = {}
    if math.type(mask) ~= "integer" or type(api_table) ~= "table" then
      return result
    end
    for _, pair in ipairs(FIELDS) do
      local bit = api_table[pair[1]]
      if math.type(bit) == "integer" then
        result[pair[2]] = (mask & bit) ~= 0
      end
    end
    return result
  end

  -- on(name, fn): created/updated/impact/destroyed give fn(snapshot, data,
  -- event) with snapshot.fields filled in; local_shot gives fn(data, event).
  function shells.on(name, fn)
    local constant = TOPICS[name]
    if constant == nil then
      return nil, "shells.on: '" .. tostring(name) ..
          "' is not one of created, updated, impact, destroyed, local_shot"
    end
    if type(fn) ~= "function" then
      return nil, "shells.on: argument 2: expected a function"
    end
    local events_api, events_error = events()
    if not events_api then return nil, "shells.on: " .. events_error end
    local topic = events_api[constant]
    if type(topic) ~= "string" then
      return nil, "shells.on: this host publishes no wotb.events." .. constant
    end
    local handle = next_handle
    next_handle = next_handle + 1
    local function deliver(event)
      local data = event and event.data
      local ok, call_error
      if name == "local_shot" then
        ok, call_error = pcall(fn, data, event)
      else
        local snapshot = data and data.snapshot
        if type(snapshot) == "table" then snapshot.fields = fields_of(snapshot) end
        ok, call_error = pcall(fn, snapshot, data, event)
      end
      if not ok then pcall(report, name, call_error) end
    end
    local priority = events_api.PRIORITY_NORMAL
    if type(priority) ~= "number" then priority = nil end
    local token, subscribe_error = events_api.subscribe(topic, deliver, priority, true)
    if token == nil then return nil, "shells.on: " .. describe(subscribe_error) end
    handlers[handle] = { token = token, name = name }
    return handle
  end

  for name in next, TOPICS do
    shells["on_" .. name] = function(fn) return shells.on(name, fn) end
  end

  function shells.off(handle)
    local entry = handlers[handle]
    if entry == nil then
      return nil, "shells.off: no live handler has handle " .. tostring(handle)
    end
    handlers[handle] = nil
    local events_api = events()
    if events_api then
      local ok, unsubscribe_error = events_api.unsubscribe(entry.token)
      if not ok then return nil, "shells.off: " .. describe(unsubscribe_error) end
    end
    return true
  end

  function shells.off_all()
    local count = 0
    for handle in next, handlers do
      if shells.off(handle) then count = count + 1 end
    end
    return count
  end

  function shells.snapshot(projectile)
    local api_table, api_error = api("shells.snapshot")
    if not api_table then return nil, api_error end
    local snapshot, snapshot_error = api_table.projectile_get_snapshot(projectile)
    if snapshot == nil then
      return nil, "shells.snapshot: " .. describe(snapshot_error)
    end
    snapshot.fields = fields_of(snapshot)
    return snapshot
  end

  function shells.visual(projectile)
    local api_table, api_error = api("shells.visual")
    if not api_table then return nil, api_error end
    if type(api_table.projectile_get_visual_entity) ~= "function" then
      return nil, "shells.visual: wotb.projectile.projectile_get_visual_entity " ..
          "is unavailable on this client"
    end
    local entity, entity_error = api_table.projectile_get_visual_entity(projectile)
    if entity == nil then return nil, "shells.visual: " .. describe(entity_error) end
    return entity
  end

  local function call(where, name, ...)
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    local fn = api_table[name]
    if type(fn) ~= "function" then
      return nil, where .. ": wotb.projectile." .. name ..
          " is unavailable on this client"
    end
    local first, second = fn(...)
    if first == nil then return nil, where .. ": " .. describe(second) end
    return first
  end

  shells.impact = {}

  -- show(desc): { id, scene_uri, flags, owner_scope_mask, shell_type,
  -- max_instances, lifetime_seconds, uniform_scale, priority } -> handle
  function shells.impact.show(desc)
    if type(desc) ~= "table" or type(desc.id) ~= "string" or
       type(desc.scene_uri) ~= "string" then
      return nil, "shells.impact.show: argument 1: expected { id = ..., " ..
          "scene_uri = ... }"
    end
    return call("shells.impact.show", "impact_visual_register", desc)
  end

  function shells.impact.update(handle, desc)
    if type(desc) ~= "table" then
      return nil, "shells.impact.update: argument 2: expected a descriptor"
    end
    local ok, why = call("shells.impact.update", "impact_visual_update", handle, desc)
    if not ok then return nil, why end
    return true
  end

  function shells.impact.hide(handle)
    local ok, why = call("shells.impact.hide", "impact_visual_unregister", handle)
    if not ok then return nil, why end
    return true
  end

  shells.tracer = {}

  -- register(desc): { id, texture_uri, color = {r,g,b,a}, width,
  -- lifetime_seconds, fade_start, priority } -> handle
  function shells.tracer.register(desc)
    if type(desc) ~= "table" or type(desc.id) ~= "string" then
      return nil, "shells.tracer.register: argument 1: expected { id = ... }"
    end
    return call("shells.tracer.register", "tracer_style_register", desc)
  end

  function shells.tracer.unregister(handle)
    local ok, why = call("shells.tracer.unregister", "tracer_style_unregister", handle)
    if not ok then return nil, why end
    return true
  end

  wotb.shells = shells
end
)lua";

// ---------------------------------------------------------------------------
// wotb.view - the camera, over wotb.camera, wotb.gameplay_camera and
// wotb.camera_state
// ---------------------------------------------------------------------------
//
// get() reads what the client publishes and lists what it refused under
// `unavailable`; set_fov/reset go through the gameplay camera; project and
// unproject take the active camera and give it back. Free, postmortem and
// cinematic modes are not offered: the client publishes no way to enter
// them, and a facade that pretended otherwise would be the thing this host
// refuses to be.
const char kViewLibrary[] = R"lua(
do
  local view = {}

  local function camera(where)
    local api = wotb and wotb.camera
    if type(api) ~= "table" or type(api.get_active) ~= "function" then
      return nil, where .. ": wotb.camera is unavailable on this client"
    end
    return api
  end

  local function gameplay(where)
    local api = wotb and wotb.gameplay_camera
    if type(api) ~= "table" or type(api.set_fov) ~= "function" then
      return nil, where .. ": wotb.gameplay_camera is unavailable on this client"
    end
    return api
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function release(handle)
    local handles = wotb and wotb.handles
    if handle ~= nil and type(handles) == "table" and
       type(handles.release) == "function" then
      handles.release(handle)
    end
  end

  local function with_camera(where, fn)
    local api, api_error = camera(where)
    if not api then return nil, api_error end
    local handle, get_error = api.get_active()
    if handle == nil then return nil, where .. ": " .. describe(get_error) end
    local results = table.pack(pcall(fn, api, handle))
    release(handle)
    if not results[1] then return nil, where .. ": " .. describe(results[2]) end
    return table.unpack(results, 2, results.n)
  end

  local function mode_name(api, code)
    for _, name in ipairs({ "UNKNOWN", "HANGAR", "ARCADE", "SNIPER",
                            "POSTMORTEM", "REPLAY", "FREE", "CINEMATIC" }) do
      if api["MODE_" .. name] == code then return string.lower(name) end
    end
    return "mode " .. tostring(code)
  end

  -- Everything the client will say about the active camera. A read it
  -- refuses is absent and named in `unavailable` with its reason.
  function view.get()
    return with_camera("view.get", function(api, handle)
      local result = { unavailable = {} }
      local function take(name, slot_name, convert)
        local fn = api[slot_name]
        if type(fn) ~= "function" then
          result.unavailable[name] = "wotb.camera." .. slot_name ..
              " is unavailable on this client"
          return
        end
        local value, why = fn(handle)
        if value == nil then
          result.unavailable[name] = describe(why)
        else
          result[name] = convert and convert(value) or value
        end
      end
      take("mode", "get_mode", function(code)
        result.mode_code = code
        return mode_name(api, code)
      end)
      take("transform", "get_transform")
      take("fov", "get_fov")
      take("near_plane", "get_near_plane")
      take("far_plane", "get_far_plane")
      local state = wotb and wotb.camera_state
      if type(state) == "table" and type(state.get_observed_state) == "function" then
        local observed, why = state.get_observed_state()
        if observed ~= nil then
          result.observed = {
            animation_state = observed.animation_state_valid ~= 0 and
                observed.animation_state or nil,
            view_mode = observed.view_mode_valid ~= 0 and observed.view_mode or nil,
          }
        else
          result.unavailable.observed = describe(why)
        end
      else
        result.unavailable.observed = "wotb.camera_state is unavailable on this client"
      end
      return result
    end)
  end

  -- The gameplay FOV: set_fov(degrees) for the current context, or
  -- set_fov(degrees, "hangar" | "battle" | "sniper").
  function view.set_fov(degrees, context)
    if type(degrees) ~= "number" or degrees ~= degrees or degrees <= 0 or
       degrees >= 180 then
      return nil, "view.set_fov: argument 1: expected degrees in (0, 180)"
    end
    local api, api_error = gameplay("view.set_fov")
    if not api then return nil, api_error end
    local slot_name = "set_fov"
    if context ~= nil then
      if context ~= "hangar" and context ~= "battle" and context ~= "sniper" then
        return nil, "view.set_fov: argument 2: expected 'hangar', 'battle' " ..
            "or 'sniper'"
      end
      slot_name = "set_fov_" .. context
    end
    local fn = api[slot_name]
    if type(fn) ~= "function" then
      return nil, "view.set_fov: wotb.gameplay_camera." .. slot_name ..
          " is unavailable on this client"
    end
    local ok, set_error = fn(degrees)
    if not ok then return nil, "view.set_fov: " .. describe(set_error) end
    return true
  end

  function view.fov()
    local api, api_error = gameplay("view.fov")
    if not api then return nil, api_error end
    if type(api.get_fov) ~= "function" then
      return nil, "view.fov: wotb.gameplay_camera.get_fov is unavailable"
    end
    local value, why = api.get_fov()
    if value == nil then return nil, "view.fov: " .. describe(why) end
    return value
  end

  function view.reset()
    local api, api_error = gameplay("view.reset")
    if not api then return nil, api_error end
    if type(api.reset_fov) ~= "function" then
      return nil, "view.reset: wotb.gameplay_camera.reset_fov is unavailable"
    end
    local ok, why = api.reset_fov()
    if not ok then return nil, "view.reset: " .. describe(why) end
    return true
  end

  local function vec3(where, point)
    if type(point) ~= "table" or type(point.x) ~= "number" or
       type(point.y) ~= "number" or type(point.z) ~= "number" then
      return nil, where .. ": argument 1: expected { x = ..., y = ..., z = ... }"
    end
    return point
  end

  -- World -> screen through the active camera; screen -> world back.
  function view.project(point)
    local checked, why = vec3("view.project", point)
    if not checked then return nil, why end
    return with_camera("view.project", function(api, handle)
      local result, project_error = api.world_to_screen(handle, checked)
      if result == nil then return nil, "view.project: " .. describe(project_error) end
      return result
    end)
  end

  function view.unproject(point)
    local checked, why = vec3("view.unproject", point)
    if not checked then return nil, why end
    return with_camera("view.unproject", function(api, handle)
      local result, project_error = api.screen_to_world(handle, checked)
      if result == nil then
        return nil, "view.unproject: " .. describe(project_error)
      end
      return result
    end)
  end

  -- transition({ target = { position, rotation, scale }, duration,
  -- easing, preserve_game_control })
  function view.transition(desc)
    if type(desc) ~= "table" or type(desc.target) ~= "table" then
      return nil, "view.transition: argument 1: expected { target = { position " ..
          "= ..., rotation = ... } }"
    end
    return with_camera("view.transition", function(api, handle)
      local ok, why = api.transition_to(handle, {
        target = desc.target,
        duration_seconds = desc.duration or desc.duration_seconds or 0,
        easing = desc.easing or 0,
        preserve_game_control = desc.preserve_game_control == false and 0 or 1,
      })
      if not ok then return nil, "view.transition: " .. describe(why) end
      return true
    end)
  end

  -- shake({ amplitude, frequency, duration, falloff })
  function view.shake(desc)
    if type(desc) ~= "table" or type(desc.amplitude) ~= "number" then
      return nil, "view.shake: argument 1: expected { amplitude = ..., " ..
          "frequency = ..., duration = ... }"
    end
    return with_camera("view.shake", function(api, handle)
      local ok, why = api.add_shake(handle, {
        amplitude = desc.amplitude,
        frequency = desc.frequency or 1,
        duration_seconds = desc.duration or desc.duration_seconds or 0.5,
        falloff = desc.falloff or 1,
      })
      if not ok then return nil, "view.shake: " .. describe(why) end
      return true
    end)
  end

  -- on_changed(fn): fn(data, event) on every camera mode change, and on
  -- sniper enter/exit.
  local handlers = {}
  local next_handle = 1

  function view.on_changed(fn)
    if type(fn) ~= "function" then
      return nil, "view.on_changed: argument 1: expected a function"
    end
    local events = wotb and wotb.events
    if type(events) ~= "table" or type(events.subscribe) ~= "function" then
      return nil, "view.on_changed: wotb.events is unavailable"
    end
    local tokens = {}
    local function deliver(event)
      local ok, call_error = pcall(fn, event and event.data, event)
      if not ok then
        local log = wotb and wotb.log
        if type(log) == "table" and type(log.error) == "function" then
          pcall(log.error, "view.on_changed handler raised: %s", describe(call_error))
        end
      end
    end
    local priority = events.PRIORITY_NORMAL
    if type(priority) ~= "number" then priority = nil end
    for _, constant in ipairs({ "TOPIC_CAMERA_MODE_CHANGED", "TOPIC_SNIPER_ENTERED",
                                "TOPIC_SNIPER_EXITED" }) do
      local topic = events[constant]
      if type(topic) == "string" then
        local token, subscribe_error = events.subscribe(topic, deliver, priority, true)
        if token == nil then
          for _, made in ipairs(tokens) do events.unsubscribe(made) end
          return nil, "view.on_changed: " .. describe(subscribe_error)
        end
        tokens[#tokens + 1] = token
      end
    end
    if #tokens == 0 then
      return nil, "view.on_changed: this host publishes no camera topics"
    end
    local handle = next_handle
    next_handle = next_handle + 1
    handlers[handle] = tokens
    return handle
  end

  function view.off(handle)
    local tokens = handlers[handle]
    if tokens == nil then
      return nil, "view.off: no live handler has handle " .. tostring(handle)
    end
    handlers[handle] = nil
    local events = wotb and wotb.events
    if type(events) == "table" and type(events.unsubscribe) == "function" then
      for _, token in ipairs(tokens) do events.unsubscribe(token) end
    end
    return true
  end

  wotb.view = view
end
)lua";

// ---------------------------------------------------------------------------
// wotb.sound - play a file, replace a game sound, over wotb.audio
// ---------------------------------------------------------------------------
//
// play() is create + play with the options an author reaches for (loop,
// volume, bus, on_finished) and answers the audio handle; replace() is the
// sound_override slot with the token remembered by event name so reset()
// can take a name. What the client refuses (a semantic override it cannot
// prove, a bus it does not have) comes back in its own words.
const char kSoundLibrary[] = R"lua(
do
  local sound = {}
  local overrides = {}

  local function api(where)
    local value = wotb and wotb.audio
    if type(value) ~= "table" or type(value.create) ~= "function" then
      return nil, where .. ": wotb.audio is unavailable on this client"
    end
    return value
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function call(where, name, ...)
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    local fn = api_table[name]
    if type(fn) ~= "function" then
      return nil, where .. ": wotb.audio." .. name .. " is unavailable on this client"
    end
    local first, second = fn(...)
    if first == nil then return nil, where .. ": " .. describe(second) end
    return first, second
  end

  local function unit(where, name, value, fallback)
    if value == nil then return fallback end
    if type(value) ~= "number" or value ~= value or value < 0 or value > 1 then
      return nil, where .. ": " .. name .. ": expected a number in 0..1"
    end
    return value
  end

  -- play(uri, { loop, spatial, volume, pitch, bus, priority, on_finished })
  -- -> handle. The handle is the author's to stop and release.
  function sound.play(uri, options)
    local where = "sound.play"
    if type(uri) ~= "string" or uri == "" then
      return nil, where .. ": argument 1: expected a sound URI"
    end
    if options ~= nil and type(options) ~= "table" then
      return nil, where .. ": argument 2: expected an options table"
    end
    options = options or {}
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    local flags = 0
    if options.loop and math.type(api_table.CREATE_LOOP) == "integer" then
      flags = flags | api_table.CREATE_LOOP
    end
    if options.spatial and math.type(api_table.CREATE_SPATIAL) == "integer" then
      flags = flags | api_table.CREATE_SPATIAL
    end
    local volume, why = unit(where, "volume", options.volume, 1.0)
    if volume == nil then return nil, why end
    if options.on_finished ~= nil and type(options.on_finished) ~= "function" then
      return nil, where .. ": on_finished: expected a function"
    end
    local handle, create_error = api_table.create({
      uri = uri, flags = flags, priority = options.priority or 0,
      volume = volume, pitch = options.pitch or 1.0, bus = options.bus or "",
    })
    if handle == nil then return nil, where .. ": " .. describe(create_error) end
    if options.on_finished ~= nil and type(api_table.on_finished) == "function" then
      local fn = options.on_finished
      local token, hook_error = api_table.on_finished(handle, function()
        local ok, call_error = pcall(fn, handle)
        if not ok then
          local log = wotb and wotb.log
          if type(log) == "table" and type(log.error) == "function" then
            pcall(log.error, "sound.play on_finished raised: %s", describe(call_error))
          end
        end
      end)
      if token == nil then
        api_table.destroy(handle)
        return nil, where .. ": on_finished: " .. describe(hook_error)
      end
    end
    local ok, play_error = api_table.play(handle)
    if not ok then
      api_table.destroy(handle)
      return nil, where .. ": " .. describe(play_error)
    end
    return handle
  end

  function sound.stop(handle, fade_seconds)
    if fade_seconds ~= nil and (type(fade_seconds) ~= "number" or fade_seconds < 0) then
      return nil, "sound.stop: argument 2: expected fade seconds >= 0"
    end
    local ok, why = call("sound.stop", "stop", handle, fade_seconds or 0)
    if not ok then return nil, why end
    return true
  end

  function sound.release(handle)
    local ok, why = call("sound.release", "destroy", handle)
    if not ok then return nil, why end
    return true
  end

  function sound.set_volume(handle, volume)
    local checked, why = unit("sound.set_volume", "argument 2", volume, nil)
    if checked == nil then
      return nil, why or "sound.set_volume: argument 2: expected a number in 0..1"
    end
    local ok, set_error = call("sound.set_volume", "set_volume", handle, checked)
    if not ok then return nil, set_error end
    return true
  end

  function sound.is_playing(handle)
    local value, why = call("sound.is_playing", "is_playing", handle)
    if value == nil then return nil, why end
    return value ~= 0
  end

  function sound.on_finished(handle, fn)
    if type(fn) ~= "function" then
      return nil, "sound.on_finished: argument 2: expected a function"
    end
    return call("sound.on_finished", "on_finished", handle, function()
      local ok, call_error = pcall(fn, handle)
      if not ok then
        local log = wotb and wotb.log
        if type(log) == "table" and type(log.error) == "function" then
          pcall(log.error, "sound.on_finished handler raised: %s", describe(call_error))
        end
      end
    end)
  end

  -- replace(event_name, uri [, priority]) -> token; reset(event_name or
  -- token) gives it back. The client decides which events it can override
  -- and answers for the rest.
  function sound.replace(event_name, uri, priority)
    if type(event_name) ~= "string" or event_name == "" then
      return nil, "sound.replace: argument 1: expected a sound event name"
    end
    if type(uri) ~= "string" or uri == "" then
      return nil, "sound.replace: argument 2: expected a replacement URI"
    end
    if priority ~= nil and math.type(priority) ~= "integer" then
      return nil, "sound.replace: argument 3: expected an integer priority"
    end
    local token, why = call("sound.replace", "sound_override_register",
                            event_name, uri, priority or 0)
    if token == nil then return nil, why end
    overrides[event_name] = token
    return token
  end

  function sound.reset(target)
    local token = target
    if type(target) == "string" then
      token = overrides[target]
      if token == nil then
        return nil, "sound.reset: no override was registered for '" .. target .. "'"
      end
    end
    local ok, why = call("sound.reset", "sound_override_unregister", token)
    if not ok then return nil, why end
    for name, known in next, overrides do
      if known == token then overrides[name] = nil end
    end
    return true
  end

  function sound.reset_all()
    local count = 0
    for name in next, overrides do
      if sound.reset(name) then count = count + 1 end
    end
    return count
  end

  wotb.sound = sound
end
)lua";

// ---------------------------------------------------------------------------
// wotb.keys - a keybind manager over wotb.input, namespaced per script
// ---------------------------------------------------------------------------
//
// bind(id, desc) registers an input action whose ABI id is "<script id>.<id>"
// - two mods that both bind "toggle" never touch each other's action, and
// nobody intercepts another mod's binding by name. Bindings are given back
// by unbind()/unbind_all(); a subscription is an input token handle,
// released through wotb.handles (the input interface has no unsubscribe
// slot of its own).
const char kKeysLibrary[] = R"lua(
do
  local keys = {}
  local actions = {}

  -- Virtual-key codes by name, for desc.key. Letters and digits are
  -- computed; anything else is desc.code as a number.
  local KEY_CODES = {
    F1 = 0x70, F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74, F6 = 0x75,
    F7 = 0x76, F8 = 0x77, F9 = 0x78, F10 = 0x79, F11 = 0x7A, F12 = 0x7B,
    SPACE = 0x20, TAB = 0x09, ENTER = 0x0D, ESCAPE = 0x1B, BACKSPACE = 0x08,
    INSERT = 0x2D, DELETE = 0x2E, HOME = 0x24, END = 0x23,
    PAGEUP = 0x21, PAGEDOWN = 0x22,
    LEFT = 0x25, UP = 0x26, RIGHT = 0x27, DOWN = 0x28,
    CAPSLOCK = 0x14, NUMLOCK = 0x90, PAUSE = 0x13, PRINTSCREEN = 0x2C,
    TILDE = 0xC0, MINUS = 0xBD, PLUS = 0xBB, COMMA = 0xBC, PERIOD = 0xBE,
  }
  local MODIFIER_NAMES = { shift = "MOD_SHIFT", ctrl = "MOD_CONTROL",
                           control = "MOD_CONTROL", alt = "MOD_ALT",
                           meta = "MOD_META" }

  local function api(where)
    local value = wotb and wotb.input
    if type(value) ~= "table" or type(value.register_action) ~= "function" then
      return nil, where .. ": wotb.input is unavailable on this client"
    end
    return value
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function namespace(id)
    local mod = wotb and wotb.mod
    local owner = nil
    if type(mod) == "table" and type(mod.id) == "function" then owner = mod.id() end
    if type(owner) ~= "string" or owner == "" then owner = "script" end
    return owner .. "." .. id
  end

  function keys.key_code(name)
    if type(name) ~= "string" or name == "" then return nil end
    local upper = string.upper(name)
    if KEY_CODES[upper] ~= nil then return KEY_CODES[upper] end
    if #upper == 1 then
      local byte = string.byte(upper)
      if (byte >= 65 and byte <= 90) or (byte >= 48 and byte <= 57) then
        return byte
      end
    end
    return nil
  end

  local function binding_from(where, api_table, desc)
    local code = desc.code
    if desc.key ~= nil then
      code = keys.key_code(desc.key)
      if code == nil then
        return nil, where .. ": key: '" .. tostring(desc.key) ..
            "' is not a key name this module knows; pass code = <virtual key>"
      end
    end
    if math.type(code) ~= "integer" or code <= 0 then
      return nil, where .. ": expected key = 'F7' (a key name) or code = <integer>"
    end
    local modifiers = 0
    if desc.modifiers ~= nil then
      if type(desc.modifiers) ~= "table" then
        return nil, where .. ": modifiers: expected { shift = true, ctrl = true, ... }"
      end
      for name, on in next, desc.modifiers do
        local constant = MODIFIER_NAMES[name]
        if constant == nil then
          return nil, where .. ": modifiers: '" .. tostring(name) ..
              "' is not shift, ctrl, alt or meta"
        end
        if on and math.type(api_table[constant]) == "integer" then
          modifiers = modifiers | api_table[constant]
        end
      end
    end
    local device = desc.device
    if device == nil then device = api_table.DEVICE_KEYBOARD end
    if math.type(device) ~= "integer" then device = 0 end
    return { device = device, code = code, modifiers = modifiers, scale = desc.scale or 1.0 }
  end

  -- bind(id, { key = "F7" | code = 0x76, modifiers = {...}, contexts,
  -- display_name, description, axis }) -> action handle
  function keys.bind(id, desc)
    local where = "keys.bind"
    if type(id) ~= "string" or id == "" or string.find(id, "[^%w_%.%-]") then
      return nil, where .. ": argument 1: expected an id of letters, digits, '_', '.' or '-'"
    end
    if type(desc) ~= "table" then
      return nil, where .. ": argument 2: expected a binding table"
    end
    if actions[id] ~= nil then
      return nil, where .. ": '" .. id .. "' is already bound; unbind it first"
    end
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    local binding, why = binding_from(where, api_table, desc)
    if binding == nil then return nil, why end
    local contexts = desc.contexts
    if contexts == nil then
      local context = wotb and wotb.context
      contexts = type(context) == "table" and context.ALL or 255
    end
    local value_type = api_table.VALUE_BUTTON
    if desc.axis then value_type = api_table.VALUE_AXIS end
    if math.type(value_type) ~= "integer" then value_type = 0 end
    local full_id = namespace(id)
    local action, register_error = api_table.register_action({
      id = full_id, value_type = value_type, contexts = contexts,
      display_name = desc.display_name or id, description = desc.description or "",
      default_bindings = { binding }, default_binding_count = 1,
    })
    if action == nil then return nil, where .. ": " .. describe(register_error) end
    actions[id] = { handle = action, full_id = full_id, binding = binding,
                    subscriptions = {} }
    return action
  end

  local function bound(where, id)
    local entry = actions[id]
    if entry == nil then
      return nil, where .. ": '" .. tostring(id) .. "' is not bound; keys.bind it first"
    end
    return entry
  end

  function keys.unbind(id)
    local entry, why = bound("keys.unbind", id)
    if not entry then return nil, why end
    local api_table, api_error = api("keys.unbind")
    if not api_table then return nil, api_error end
    local handles = wotb and wotb.handles
    for _, token in ipairs(entry.subscriptions) do
      if type(handles) == "table" and type(handles.release) == "function" then
        handles.release(token)
      end
    end
    actions[id] = nil
    if type(api_table.unregister_action) == "function" then
      local ok, unregister_error = api_table.unregister_action(entry.handle)
      if not ok then return nil, "keys.unbind: " .. describe(unregister_error) end
    end
    return true
  end

  function keys.unbind_all()
    local count = 0
    for id in next, actions do
      if keys.unbind(id) then count = count + 1 end
    end
    return count
  end

  local function query(where, id, slot_name)
    local entry, why = bound(where, id)
    if not entry then return nil, why end
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    if type(api_table[slot_name]) ~= "function" then
      return nil, where .. ": wotb.input." .. slot_name .. " is unavailable on this client"
    end
    local value, query_error = api_table[slot_name](entry.handle)
    if value == nil then return nil, where .. ": " .. describe(query_error) end
    return value
  end

  -- pressed(id): went down this frame; down(id): held right now.
  function keys.pressed(id)
    local value, why = query("keys.pressed", id, "is_action_pressed")
    if value == nil then return nil, why end
    return value ~= 0
  end

  function keys.down(id)
    local value, why = query("keys.down", id, "is_action_down")
    if value == nil then return nil, why end
    return value ~= 0
  end

  function keys.axis(id)
    return query("keys.axis", id, "get_axis")
  end

  local function subscribe(where, id, fn, want_pressed)
    if type(fn) ~= "function" then
      return nil, where .. ": argument 2: expected a function"
    end
    local entry, why = bound(where, id)
    if not entry then return nil, why end
    local api_table, api_error = api(where)
    if not api_table then return nil, api_error end
    if type(api_table.subscribe) ~= "function" then
      return nil, where .. ": wotb.input.subscribe is unavailable on this client"
    end
    -- The raw callback is fn(action, value, pressed); the action is the one
    -- this subscription was made for and is not passed on.
    local token, subscribe_error = api_table.subscribe(entry.handle,
        function(_, value, pressed)
          local is_pressed = pressed ~= 0
          if is_pressed ~= want_pressed then return end
          local ok, call_error = pcall(fn, id, value)
          if not ok then
            local log = wotb and wotb.log
            if type(log) == "table" and type(log.error) == "function" then
              pcall(log.error, "%s handler for '%s' raised: %s", where, id,
                    describe(call_error))
            end
          end
        end)
    if token == nil then return nil, where .. ": " .. describe(subscribe_error) end
    entry.subscriptions[#entry.subscriptions + 1] = token
    return token
  end

  function keys.on_pressed(id, fn) return subscribe("keys.on_pressed", id, fn, true) end
  function keys.on_released(id, fn) return subscribe("keys.on_released", id, fn, false) end

  -- off(token): a subscription token is a handle; releasing it ends it.
  function keys.off(token)
    local handles = wotb and wotb.handles
    if type(handles) ~= "table" or type(handles.release) ~= "function" then
      return nil, "keys.off: wotb.handles is unavailable"
    end
    local ok, why = handles.release(token)
    if not ok then return nil, "keys.off: " .. describe(why) end
    for _, entry in next, actions do
      for index = #entry.subscriptions, 1, -1 do
        if entry.subscriptions[index] == token then
          table.remove(entry.subscriptions, index)
        end
      end
    end
    return true
  end

  function keys.bindings(id)
    return query("keys.bindings", id, "get_bindings")
  end

  function keys.conflicts(id)
    return query("keys.conflicts", id, "find_conflicts")
  end

  -- capture_begin([contexts]) then capture_end() -> the binding the player
  -- pressed, as { device, code, modifiers, scale }.
  function keys.capture_begin(contexts)
    local api_table, api_error = api("keys.capture_begin")
    if not api_table then return nil, api_error end
    if type(api_table.capture_begin) ~= "function" then
      return nil, "keys.capture_begin: wotb.input.capture_begin is unavailable"
    end
    if contexts == nil then
      local context = wotb and wotb.context
      contexts = type(context) == "table" and context.ALL or 255
    end
    local ok, why = api_table.capture_begin(contexts)
    if not ok then return nil, "keys.capture_begin: " .. describe(why) end
    return true
  end

  function keys.capture_end()
    local api_table, api_error = api("keys.capture_end")
    if not api_table then return nil, api_error end
    if type(api_table.capture_end) ~= "function" then
      return nil, "keys.capture_end: wotb.input.capture_end is unavailable"
    end
    local binding, why = api_table.capture_end()
    if binding == nil then return nil, "keys.capture_end: " .. describe(why) end
    return binding
  end

  wotb.keys = keys
end
)lua";

// ---------------------------------------------------------------------------
// wotb.store - typed values over wotb.storage and wotb.json
// ---------------------------------------------------------------------------
//
// get/set/delete/has/keys/clear/flush over the JSON slots, with the values
// encoded and decoded here so an author stores a table and gets a table
// back. No new backend: keys() and clear() work from an index document this
// module keeps under one reserved key, which is the honest way to enumerate
// a store the ABI does not enumerate.
const char kStoreLibrary[] = R"lua(
do
  local store = {}
  local INDEX_KEY = "wotb.store.index"

  local function storage(where)
    local value = wotb and wotb.storage
    if type(value) ~= "table" or type(value.get_json) ~= "function" or
       type(value.set_json) ~= "function" or type(value.contains) ~= "function" or
       type(value.erase) ~= "function" then
      return nil, where .. ": wotb.storage is unavailable on this client"
    end
    return value
  end

  local function json(where)
    local value = wotb and wotb.json
    if type(value) ~= "table" or type(value.encode) ~= "function" then
      return nil, where .. ": wotb.json is unavailable"
    end
    return value
  end

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function check_key(where, key)
    if type(key) ~= "string" or key == "" or key == INDEX_KEY then
      return nil, where .. ": argument 1: expected a non-empty key"
    end
    return key
  end

  local function read_index(where, storage_api, json_api)
    local ok, present = storage_api.contains(INDEX_KEY)
    if ok == nil then return nil, where .. ": " .. describe(present) end
    if present ~= true then return {} end
    local text, read_error = storage_api.get_json(INDEX_KEY)
    if text == nil then return nil, where .. ": " .. describe(read_error) end
    local list = json_api.decode(text)
    if type(list) ~= "table" then return {} end
    return list
  end

  local function write_index(where, storage_api, json_api, list)
    local text, encode_error = json_api.encode(json_api.as_array(list))
    if text == nil then return nil, where .. ": " .. describe(encode_error) end
    local ok, write_error = storage_api.set_json(INDEX_KEY, text)
    if not ok then return nil, where .. ": " .. describe(write_error) end
    return true
  end

  -- get(key [, default]) -> value, "stored" | default, "default" | nil, err
  function store.get(key, default)
    local where = "store.get"
    local checked, why = check_key(where, key)
    if not checked then return nil, why end
    local storage_api, storage_error = storage(where)
    if not storage_api then return nil, storage_error end
    local json_api, json_error = json(where)
    if not json_api then return nil, json_error end
    local ok, present = storage_api.contains(checked)
    if ok == nil then return nil, where .. ": " .. describe(present) end
    if present ~= true then return default, "default" end
    local text, read_error = storage_api.get_json(checked)
    if text == nil then return nil, where .. ": " .. describe(read_error) end
    local value, decode_error = json_api.decode(text)
    if value == nil then
      return nil, where .. ": the stored value is not valid JSON: " ..
          describe(decode_error)
    end
    return value, "stored"
  end

  function store.set(key, value)
    local where = "store.set"
    local checked, why = check_key(where, key)
    if not checked then return nil, why end
    if value == nil then
      return nil, where .. ": argument 2: use store.delete to remove a key"
    end
    local storage_api, storage_error = storage(where)
    if not storage_api then return nil, storage_error end
    local json_api, json_error = json(where)
    if not json_api then return nil, json_error end
    local text, encode_error = json_api.encode(value)
    if text == nil then return nil, where .. ": " .. describe(encode_error) end
    local list, index_error = read_index(where, storage_api, json_api)
    if list == nil then return nil, index_error end
    local ok, write_error = storage_api.set_json(checked, text)
    if not ok then return nil, where .. ": " .. describe(write_error) end
    local known = false
    for _, name in ipairs(list) do
      if name == checked then known = true end
    end
    if not known then
      list[#list + 1] = checked
      local indexed, why_index = write_index(where, storage_api, json_api, list)
      if not indexed then return nil, why_index end
    end
    return true
  end

  function store.delete(key)
    local where = "store.delete"
    local checked, why = check_key(where, key)
    if not checked then return nil, why end
    local storage_api, storage_error = storage(where)
    if not storage_api then return nil, storage_error end
    local json_api, json_error = json(where)
    if not json_api then return nil, json_error end
    local ok, erase_error = storage_api.erase(checked)
    if not ok then return nil, where .. ": " .. describe(erase_error) end
    local list, index_error = read_index(where, storage_api, json_api)
    if list == nil then return nil, index_error end
    local kept = {}
    for _, name in ipairs(list) do
      if name ~= checked then kept[#kept + 1] = name end
    end
    if #kept ~= #list then
      local indexed, why_index = write_index(where, storage_api, json_api, kept)
      if not indexed then return nil, why_index end
    end
    return true
  end

  function store.has(key)
    local checked, why = check_key("store.has", key)
    if not checked then return nil, why end
    local storage_api, storage_error = storage("store.has")
    if not storage_api then return nil, storage_error end
    local ok, present = storage_api.contains(checked)
    if ok == nil then return nil, "store.has: " .. describe(present) end
    return present == true
  end

  -- The keys written through this module, in insertion order.
  function store.keys()
    local storage_api, storage_error = storage("store.keys")
    if not storage_api then return nil, storage_error end
    local json_api, json_error = json("store.keys")
    if not json_api then return nil, json_error end
    return read_index("store.keys", storage_api, json_api)
  end

  function store.clear()
    local list, why = store.keys()
    if list == nil then return nil, why end
    local storage_api = storage("store.clear")
    local count = 0
    for _, name in ipairs(list) do
      if storage_api.erase(name) then count = count + 1 end
    end
    storage_api.erase(INDEX_KEY)
    return count
  end

  function store.flush()
    local storage_api, storage_error = storage("store.flush")
    if not storage_api then return nil, storage_error end
    if type(storage_api.flush) ~= "function" then
      return nil, "store.flush: wotb.storage.flush is unavailable on this client"
    end
    local ok, why = storage_api.flush()
    if not ok then return nil, "store.flush: " .. describe(why) end
    return true
  end

  wotb.store = store
end
)lua";

// ---------------------------------------------------------------------------
// wotb.files - read a file, load a resource, over loaders/vfs/resources
// ---------------------------------------------------------------------------
//
// read_text/read_json go through the bounded loaders; read_yaml answers a
// document object navigated by path, because the YAML interface cannot
// enumerate a map's keys and a "whole tree as a table" would have to guess
// them. load_texture/audio/scene are resources.load with the expected type
// filled in. watch() is the VFS watch plus the invalidation topic.
// ---------------------------------------------------------------------------
// wotb.session - the login cluster of the current region
// ---------------------------------------------------------------------------
//
// Over wotb.session_cluster (raw) and wotb.events. The client already
// implements the switch; this module only names it: clusters() lists the
// region, cluster() is where the client is now, change_cluster() asks the
// client to reconnect (by id, by name, or "auto" for its own choice) and
// on_cluster_changed() reports queued/started/connected/failed from the
// runtime's typed event. Refusals come back as values in the client's words.
const char kSessionLibrary[] = R"lua(
do
  local session = {}
  local STATUS = { [1] = "queued", [2] = "started", [3] = "connected", [4] = "failed" }
  local TOPIC = "wotbmod.session.cluster.changed"

  local function api(where)
    local value = wotb and rawget(wotb, "session_cluster")
    if type(value) ~= "table" or type(value.enumerate) ~= "function" then
      return nil, where .. ": wotb.session_cluster is unavailable on this client"
    end
    return value
  end

  local function record(item)
    return { id = item.cluster_id, name = item.name, current = item.current ~= 0,
             alive = item.alive ~= 0, allowed = item.allowed ~= 0, ccu = item.ccu }
  end

  -- clusters() -> { {id, name, current, alive, allowed, ccu}, ... } sorted by id
  function session.clusters()
    local raw, why = api("session.clusters")
    if not raw then return nil, why end
    local items, err = raw.enumerate()
    if items == nil then return nil, "session.clusters: " .. tostring(err) end
    local list = {}
    for index = 1, #items do list[index] = record(items[index]) end
    table.sort(list, function(a, b) return a.id < b.id end)
    return list
  end

  -- cluster() -> the record the client is connected to
  function session.cluster()
    local raw, why = api("session.cluster")
    if not raw then return nil, why end
    local item, err = raw.get_current()
    if item == nil then return nil, "session.cluster: " .. tostring(err) end
    return record(item)
  end

  local function resolve(target)
    if target == "auto" or target == -1 then return -1 end
    if math.type(target) == "integer" then return target end
    if type(target) == "string" then
      local list, why = session.clusters()
      if not list then return nil, why end
      for _, item in ipairs(list) do
        if item.name == target then return item.id end
      end
      return nil, "session.change_cluster: no cluster named " .. target
    end
    return nil, "session.change_cluster: argument 1: expected a cluster id, a name or 'auto'"
  end

  -- change_cluster(id | "EU_C3" | "auto") -> true | nil, why
  function session.change_cluster(target)
    local raw, why = api("session.change_cluster")
    if not raw then return nil, why end
    local mod = wotb and wotb.mod
    if type(mod) == "table" and type(mod.has_permission) == "function" and
       not mod.has_permission("session.cluster.change") then
      return nil, "permission denied: session.cluster.change"
    end
    local id, resolve_error = resolve(target)
    if id == nil then return nil, resolve_error end
    if type(raw.change) ~= "function" then
      return nil, "session.change_cluster: wotb.session_cluster.change is unavailable"
    end
    local ok, err = raw.change(id)
    if not ok then return nil, "session.change_cluster: " .. tostring(err) end
    return true
  end

  local handlers, token, next_handle = {}, nil, 1

  local function deliver(event)
    local data = event and event.data
    if type(data) ~= "table" or data.kind ~= "cluster_changed" then return end
    local payload = { from = data.from_cluster_id, to = data.to_cluster_id,
                      status = STATUS[data.status] or ("status " .. tostring(data.status)) }
    for _, fn in pairs(handlers) do
      local ok, call_error = pcall(fn, payload)
      if not ok then
        local log = wotb and wotb.log
        if type(log) == "table" and type(log.error) == "function" then
          pcall(log.error, "on_cluster_changed handler raised: %s", tostring(call_error))
        end
      end
    end
  end

  -- on_cluster_changed(fn) -> handle; fn({ from, to, status })
  function session.on_cluster_changed(fn)
    if type(fn) ~= "function" then
      return nil, "session.on_cluster_changed: argument 1: expected a function"
    end
    local events = wotb and wotb.events
    if type(events) ~= "table" or type(events.subscribe) ~= "function" then
      return nil, "session.on_cluster_changed: wotb.events is unavailable"
    end
    if token == nil then
      local topic = events.TOPIC_SESSION_CLUSTER_CHANGED or TOPIC
      local t, err = events.subscribe(topic, deliver, events.PRIORITY_NORMAL, true)
      if t == nil then return nil, "session.on_cluster_changed: " .. tostring(err) end
      token = t
    end
    local handle = next_handle
    next_handle = next_handle + 1
    handlers[handle] = fn
    return handle
  end

  function session.off_cluster_changed(handle)
    if handlers[handle] == nil then
      return nil, "session.off_cluster_changed: no handler " .. tostring(handle)
    end
    handlers[handle] = nil
    if next(handlers) == nil and token ~= nil then
      local events = wotb and wotb.events
      if type(events) == "table" and type(events.unsubscribe) == "function" then
        events.unsubscribe(token)
      end
      token = nil
    end
    return true
  end

  function session.off_all()
    for handle in pairs(handlers) do session.off_cluster_changed(handle) end
    return true
  end

  wotb.session = session
end
)lua";

const char kFilesLibrary[] = R"lua(
do
  local files = {}
  local DEFAULT_MAX = 4 * 1024 * 1024
  local INVALIDATED_TOPIC = "wotbmod.vfs.invalidated"

  local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
  end

  local function table_api(where, name, probe)
    local value = wotb and wotb[name]
    if type(value) ~= "table" or type(value[probe]) ~= "function" then
      return nil, where .. ": wotb." .. name .. " is unavailable on this client"
    end
    return value
  end

  local function check_uri(where, uri)
    if type(uri) ~= "string" or uri == "" then
      return nil, where .. ": argument 1: expected a URI such as mod://self/file.txt"
    end
    return uri
  end

  local function check_max(where, max_bytes)
    if max_bytes == nil then return DEFAULT_MAX end
    if math.type(max_bytes) ~= "integer" or max_bytes <= 0 then
      return nil, where .. ": argument 2: expected a positive byte limit"
    end
    return max_bytes
  end

  function files.read_text(uri, max_bytes)
    local where = "files.read_text"
    local checked, why = check_uri(where, uri)
    if not checked then return nil, why end
    local limit, limit_error = check_max(where, max_bytes)
    if not limit then return nil, limit_error end
    local loaders, api_error = table_api(where, "loaders", "load_text_utf8")
    if not loaders then return nil, api_error end
    local text, read_error = loaders.load_text_utf8(checked, limit)
    if text == nil then return nil, where .. ": " .. describe(read_error) end
    return text
  end

  function files.read_binary(uri, max_bytes)
    local where = "files.read_binary"
    local checked, why = check_uri(where, uri)
    if not checked then return nil, why end
    local limit, limit_error = check_max(where, max_bytes)
    if not limit then return nil, limit_error end
    local loaders, api_error = table_api(where, "loaders", "load_binary")
    if not loaders then return nil, api_error end
    local bytes, read_error = loaders.load_binary(checked, limit)
    if bytes == nil then return nil, where .. ": " .. describe(read_error) end
    return bytes
  end

  function files.read_json(uri, max_bytes)
    local text, why = files.read_text(uri, max_bytes)
    if text == nil then return nil, (string.gsub(why, "^files%.read_text", "files.read_json")) end
    local json = wotb and wotb.json
    if type(json) ~= "table" or type(json.decode) ~= "function" then
      return nil, "files.read_json: wotb.json is unavailable"
    end
    local value, decode_error = json.decode(text)
    if value == nil then return nil, "files.read_json: " .. describe(decode_error) end
    return value
  end

  -- A YAML document navigated by path: doc:get("a.b[2].c") answers the
  -- scalar there (string, boolean, integer or number), a table
  -- { kind = "map" | "sequence", size = n } for a container, or nil plus
  -- why. doc:release() gives the handle back.
  local Document = {}
  Document.__index = Document

  local function yaml_api(where)
    return table_api(where, "yaml", "get_root")
  end

  local function split_path(path)
    local steps = {}
    for piece in string.gmatch(path, "[^%.]+") do
      local key, rest = string.match(piece, "^([^%[]*)(.*)$")
      if key ~= "" then steps[#steps + 1] = { key = key } end
      for index in string.gmatch(rest, "%[(%d+)%]") do
        steps[#steps + 1] = { index = tonumber(index) }
      end
    end
    return steps
  end

  function Document:get(path)
    local where = "files.yaml.get"
    if self.handle == nil then return nil, where .. ": this document was released" end
    if path ~= nil and type(path) ~= "string" then
      return nil, where .. ": argument 1: expected a path such as 'a.b[1]'"
    end
    local yaml, api_error = yaml_api(where)
    if not yaml then return nil, api_error end
    local node, root_error = yaml.get_root(self.handle)
    if node == nil then return nil, where .. ": " .. describe(root_error) end
    for _, step in ipairs(split_path(path or "")) do
      local child, step_error
      if step.key ~= nil then
        child, step_error = yaml.map_get(self.handle, node, step.key)
      else
        child, step_error = yaml.sequence_get(self.handle, node, step.index - 1)
      end
      if child == nil then return nil, where .. ": " .. describe(step_error) end
      node = child
    end
    local kind, kind_error = yaml.get_type(self.handle, node)
    if kind == nil then return nil, where .. ": " .. describe(kind_error) end
    -- WotbModV3YamlNodeType: 0 null, 1 map, 2 sequence, 3 string, 4 bool,
    -- 5 int, 6 float (yaml_v1.h).
    if kind == 0 then return nil, where .. ": the node is null" end
    if kind == 1 or kind == 2 then
      local size = yaml.get_size(self.handle, node)
      return { kind = kind == 1 and "map" or "sequence", size = size or 0 }
    end
    local reader = ({ [3] = "get_string", [4] = "get_bool", [5] = "get_int",
                      [6] = "get_float" })[kind]
    if reader == nil then return nil, where .. ": node type " .. tostring(kind) end
    local value, read_error = yaml[reader](self.handle, node)
    if value == nil then return nil, where .. ": " .. describe(read_error) end
    if kind == 4 then return value ~= 0 end
    return value
  end

  function Document:release()
    local handles = wotb and wotb.handles
    if self.handle ~= nil and type(handles) == "table" and
       type(handles.release) == "function" then
      handles.release(self.handle)
    end
    self.handle = nil
    return true
  end

  function files.read_yaml(uri, limits)
    local where = "files.read_yaml"
    local checked, why = check_uri(where, uri)
    if not checked then return nil, why end
    local loaders, api_error = table_api(where, "loaders", "load_yaml")
    if not loaders then return nil, api_error end
    local handle, load_error = loaders.load_yaml(checked, limits or {
      max_depth = 32, max_nodes = 100000, max_bytes = DEFAULT_MAX })
    if handle == nil then return nil, where .. ": " .. describe(load_error) end
    return setmetatable({ handle = handle, uri = checked }, Document)
  end

  local function load_resource(where, uri, type_name)
    local checked, why = check_uri(where, uri)
    if not checked then return nil, why end
    local resources, api_error = table_api(where, "resources", "load")
    if not resources then return nil, api_error end
    local expected = resources[type_name]
    if math.type(expected) ~= "integer" then
      return nil, where .. ": wotb.resources." .. type_name .. " is not published"
    end
    local handle, load_error = resources.load({
      expected_type = expected, flags = resources.LOAD_DEFAULT or 0,
      max_bytes = DEFAULT_MAX, uri = checked, group = "", expected_sha256 = "",
    })
    if handle == nil then return nil, where .. ": " .. describe(load_error) end
    return handle
  end

  function files.load_texture(uri) return load_resource("files.load_texture", uri, "IMAGE") end
  function files.load_audio(uri) return load_resource("files.load_audio", uri, "AUDIO") end
  function files.load_scene(uri) return load_resource("files.load_scene", uri, "MODEL") end

  function files.info(resource)
    local resources, api_error = table_api("files.info", "resources", "get_info")
    if not resources then return nil, api_error end
    local info, why = resources.get_info(resource)
    if info == nil then return nil, "files.info: " .. describe(why) end
    return info
  end

  function files.release(resource)
    local resources, api_error = table_api("files.release", "resources", "release")
    if not resources then return nil, api_error end
    local ok, why = resources.release(resource)
    if not ok then return nil, "files.release: " .. describe(why) end
    return true
  end

  -- exists(uri) -> true | false, reason: the client's stat answer, with
  -- its reason when it says no.
  function files.exists(uri)
    local checked, why = check_uri("files.exists", uri)
    if not checked then return nil, why end
    local vfs, api_error = table_api("files.exists", "vfs", "stat")
    if not vfs then return nil, api_error end
    local stat, stat_error = vfs.stat(checked)
    if stat == nil then return false, describe(stat_error) end
    return true
  end

  function files.stat(uri)
    local checked, why = check_uri("files.stat", uri)
    if not checked then return nil, why end
    local vfs, api_error = table_api("files.stat", "vfs", "stat")
    if not vfs then return nil, api_error end
    local stat, stat_error = vfs.stat(checked)
    if stat == nil then return nil, "files.stat: " .. describe(stat_error) end
    return stat
  end

  function files.list(uri)
    local checked, why = check_uri("files.list", uri)
    if not checked then return nil, why end
    local vfs, api_error = table_api("files.list", "vfs", "list")
    if not vfs then return nil, api_error end
    local entries, list_error = vfs.list(checked)
    if entries == nil then return nil, "files.list: " .. describe(list_error) end
    return entries
  end

  -- watch(uri, fn): the VFS watch on uri plus a subscription to the
  -- invalidation topic; fn(event) for every invalidation the client
  -- publishes (the payload carries the URI when this build publishes one -
  -- nothing is filtered here). Answers { token, subscription }.
  function files.watch(uri, fn)
    local where = "files.watch"
    local checked, why = check_uri(where, uri)
    if not checked then return nil, why end
    if type(fn) ~= "function" then
      return nil, where .. ": argument 2: expected a function"
    end
    local vfs, api_error = table_api(where, "vfs", "watch")
    if not vfs then return nil, api_error end
    local events = wotb and wotb.events
    if type(events) ~= "table" or type(events.subscribe) ~= "function" then
      return nil, where .. ": wotb.events is unavailable"
    end
    local token, watch_error = vfs.watch(checked)
    if token == nil then return nil, where .. ": " .. describe(watch_error) end
    local subscription, subscribe_error = events.subscribe(INVALIDATED_TOPIC,
        function(event)
          local ok, call_error = pcall(fn, event)
          if not ok then
            local log = wotb and wotb.log
            if type(log) == "table" and type(log.error) == "function" then
              pcall(log.error, "files.watch handler raised: %s", describe(call_error))
            end
          end
        end, events.PRIORITY_NORMAL, true)
    if subscription == nil then
      local handles = wotb and wotb.handles
      if type(handles) == "table" and type(handles.release) == "function" then
        handles.release(token)
      end
      return nil, where .. ": " .. describe(subscribe_error)
    end
    return { token = token, subscription = subscription }
  end

  function files.unwatch(watch)
    if type(watch) ~= "table" then
      return nil, "files.unwatch: argument 1: expected what files.watch answered"
    end
    local events = wotb and wotb.events
    if watch.subscription ~= nil and type(events) == "table" and
       type(events.unsubscribe) == "function" then
      events.unsubscribe(watch.subscription)
    end
    local handles = wotb and wotb.handles
    if watch.token ~= nil and type(handles) == "table" and
       type(handles.release) == "function" then
      handles.release(watch.token)
    end
    watch.subscription, watch.token = nil, nil
    return true
  end

  wotb.files = files
end
)lua";

const LuaPreludeSource kPreludes[] = {
    {"@wotb.log", kLogLibrary, sizeof(kLogLibrary) - 1u},
    {"@wotb.json", kJsonLibrary, sizeof(kJsonLibrary) - 1u},
    {"@wotb.timer", kTimerLibrary, sizeof(kTimerLibrary) - 1u},
    {"@wotb.battle", kBattleLibrary, sizeof(kBattleLibrary) - 1u},
    {"@wotb.config", kConfigLibrary, sizeof(kConfigLibrary) - 1u},
    {"@wotb.available", kAvailableLibrary, sizeof(kAvailableLibrary) - 1u},
    {"@wotb.panel", kPanelLibrary, sizeof(kPanelLibrary) - 1u},
    {"@wotb.dava-main", kDavaMainThreadLibrary,
     sizeof(kDavaMainThreadLibrary) - 1u},
    {"@wotb.mod", kModLibrary, sizeof(kModLibrary) - 1u},
    {"@wotb.ges-ext", kGesExtensionsLibrary,
     sizeof(kGesExtensionsLibrary) - 1u},
    {"@wotb.hud", kHudLibrary, sizeof(kHudLibrary) - 1u},
    {"@wotb.screen", kScreenLibrary, sizeof(kScreenLibrary) - 1u},
    {"@wotb.vehicle", kVehicleLibrary, sizeof(kVehicleLibrary) - 1u},
    {"@wotb.shells", kShellsLibrary, sizeof(kShellsLibrary) - 1u},
    {"@wotb.view", kViewLibrary, sizeof(kViewLibrary) - 1u},
    {"@wotb.sound", kSoundLibrary, sizeof(kSoundLibrary) - 1u},
    {"@wotb.keys", kKeysLibrary, sizeof(kKeysLibrary) - 1u},
    {"@wotb.store", kStoreLibrary, sizeof(kStoreLibrary) - 1u},
    {"@wotb.files", kFilesLibrary, sizeof(kFilesLibrary) - 1u},
    {"@wotb.session", kSessionLibrary, sizeof(kSessionLibrary) - 1u},
};

}  // namespace

const LuaPreludeSource* ConveniencePreludes(size_t* out_count) noexcept {
    if (out_count) *out_count = sizeof(kPreludes) / sizeof(kPreludes[0]);
    return kPreludes;
}

}  // namespace lua
}  // namespace wotbmod
