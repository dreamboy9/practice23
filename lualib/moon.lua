require("base.io")
require("base.os")
require("base.string")
require("base.table")
require("base.math")
require("base.util")
require("base.class")

---@type core
local core = require("mooncore")
local json = require("json")
local seri = require("seri")

local pairs = pairs
local type = type
local error = error
local assert = assert
local setmetatable = setmetatable
local tremove = table.remove
local tointeger = math.tointeger
local traceback = debug.traceback

local jencode = json.encode

local co_create = coroutine.create
local co_running = coroutine.running
local co_yield = coroutine.yield
local co_resume = coroutine.resume
local co_close = coroutine.close

local _send = core.send
local _now = core.now
local _addr = core.id
local _remove_timer = core.remove_timer
local _repeated = core.repeated
local _newservice = core.new_service
local _queryservice = core.queryservice
local _decode = core.decode

local unpack = seri.unpack
local pack = seri.pack

local PTYPE_SYSTEM = 1
local PTYPE_TEXT = 2
local PTYPE_LUA = 3
local PTYPE_SOCKET = 4
local PTYPE_ERROR = 5
local PTYPE_SOCKET_WS = 6
local PTYPE_DEBUG = 7
local PTYPE_SHUTDOWN = 8
local PTYPE_TIMER = 9

local LOG_ERROR = 1
local LOG_WARN = 2
local LOG_INFO = 3
local LOG_DEBUG = 4

---@class moon : core
local moon =  core

moon.PTYPE_TEXT = PTYPE_TEXT
moon.PTYPE_LUA = PTYPE_LUA
moon.PTYPE_SOCKET = PTYPE_SOCKET
moon.PTYPE_SOCKET_WS = PTYPE_SOCKET_WS

--moon.codecache = require("codecache")

moon.error = function(...) core.log(LOG_ERROR,...) end
moon.warn = function(...) core.log(LOG_WARN,...) end
moon.info = function(...) core.log(LOG_INFO,...) end
moon.debug = function(...) core.log(LOG_DEBUG,...) end

moon.pack = pack
moon.unpack = unpack

--export global variable
local _g = _G

---rewrite lua print
_g["print"] = moon.info

moon.DEBUG = function ()
    return core.get_loglevel() == 4 -- LOG_DEBUG
end

moon.exports = {}
setmetatable(
    moon.exports,
    {
        __newindex = function(_, name, value)
            rawset(_g, name, value)
        end,
        __index = function(_, name)
            return rawget(_g, name)
        end
    }
)

-- disable create unexpected global variable
setmetatable(
    _g,
    {
        __newindex = function(_, name,value)
            if name:sub(1,4)~='sol.' then --ignore sol2 registed library
                local msg = string.format('USE "moon.exports.%s = <value>" INSTEAD OF SET GLOBAL VARIABLE', name)
                print(traceback(msg, 2))
                print("")
            else
                rawset(_g, name, value)
            end
        end
    }
)

local uuid = 0
local session_id_coroutine = {}
local protocol = {}
local session_watcher = {}

local function remove_all_timer(t)
    for k,_ in pairs(t) do
        core.remove_timer(k)
    end
end

local timer_cb = setmetatable({},{
	__gc = remove_all_timer,
})

local function coresume(co, ...)
    local ok, err = co_resume(co, ...)
    if not ok then
        err = traceback(co, err)
        co_close(co)
        error(err)
    end
    return ok, err
end

---make map<coroutine,sessionid>
local function make_response(receiver)
    uuid = uuid + 1
    if uuid == 0x7FFFFFFF then
        uuid = 1
    end

    assert(nil == session_id_coroutine[uuid])

    if receiver then
        session_watcher[uuid] = receiver
    end

    session_id_coroutine[uuid] = co_running()
    return uuid
end

--- ????????????session?????????
function moon.cancel_session(sessionid)
    session_id_coroutine[sessionid] = false
end

moon.make_response = make_response

---@param msg lightuserdata @message*
---@param PTYPE string
local function _default_dispatch(msg, PTYPE)
    local p = protocol[PTYPE]
    if not p then
        error(string.format( "handle unknown PTYPE: %s. sender %u",PTYPE, _decode(msg, "S")))
    end

    local sessionid = _decode(msg, "E")
    if sessionid > 0 and PTYPE ~= PTYPE_ERROR then
        session_watcher[sessionid] = nil
        local co = session_id_coroutine[sessionid]
        if co then
            session_id_coroutine[sessionid] = nil
            --print(coroutine.status(co))
            if p.unpack then
                coresume(co, p.unpack(_decode(msg,"C")))
            else
                coresume(co, msg)
            end
            --print(coroutine.status(co))
            return
        end

        if co ~= false then
            error(string.format( "%s: response [%u] can not find co.",moon.name, sessionid))
        end
	else
        if not p.dispatch then
			error(string.format( "[%s] dispatch PTYPE [%u] is nil",moon.name, p.PTYPE))
			return
        end
        p.dispatch(msg, p.unpack)
    end
end

core.callback(_default_dispatch)

---
---???????????????????????????,?????????????????????????????????????????????
---@param PTYPE string @????????????
---@param receiver integer @???????????????id
---@return boolean
function moon.send(PTYPE, receiver, ...)
    local p = protocol[PTYPE]
    if not p then
        error(string.format("moon send unknown PTYPE[%s] message", PTYPE))
    end

    _send(receiver, p.pack(...), "", 0, p.PTYPE)
    return true
end

---???????????????????????????????????????????????????????????????
---@param PTYPE string @????????????
---@param receiver integer @???????????????id
---@param header string @Message Header
---@param data string|userdata @????????????
---@param sessionid integer
---@return boolean
function moon.raw_send(PTYPE, receiver, header, data, sessionid)
	local p = protocol[PTYPE]
    if not p then
        error(string.format("moon send unknown PTYPE[%s] message", PTYPE))
    end

    header = header or ''
    sessionid = sessionid or 0
    _send(receiver, data, header, sessionid, p.PTYPE)
	return true
end

---?????????????????????id
---@return integer
function moon.addr()
    return _addr
end

---async ????????????????????????
---@param stype string @????????????????????????????????????????????????????????? 'lua'
---@param config table @????????????????????????????????????table, ?????????????????????????????????
---@param unique boolean @default false, ?????????????????????????????????????????????moon.queryservice(name)????????????id
---@param workerid integer @default 0 ,?????????????????????????????????????????????????????????????????????0,???????????????????????????????????????
---@return integer @????????????id
function moon.new_service(stype, config, unique, workerid)
    unique = unique or false
    workerid = workerid or 0
    config = jencode(config)
    local sessionid = make_response()
    _newservice(stype, config, unique, workerid, sessionid)
    return tointeger(co_yield())
end

---???????????????????????????
---@param addr integer @??????id
---@param iswait boolean @????????????true????????? ??????????????????????????????
---@return string
function moon.remove_service(addr, iswait)
    if iswait then
        local sessionid = make_response()
        core.kill(addr, sessionid)
        return co_yield()
    else
        core.kill(addr, 0)
    end
end

---?????????????????????
function moon.quit()
    local running = co_running()
    for k, co in pairs(session_id_coroutine) do
		if type(co) == "thread" and co ~= running then
            co_close(co)
            session_id_coroutine[k] = false
		end
    end

    for k, co in pairs(timer_cb) do
        if type(co) == "thread" and co ~= running then
            co_close(co)
            timer_cb[k] = false
		end
    end

    moon.remove_service(_addr)
end

---????????????name????????????id,?????????????????????????????????unique=true?????????
---@param name string
---@return integer @ 0 ?????????????????????
function moon.queryservice(name)
	if type(name)=='string' then
		return _queryservice(name)
	end
	return name
end

function moon.set_env_pack(name, ...)
    return core.set_env(name, seri.packs(...))
end

function moon.get_env_unpack(name)
    return seri.unpack(core.get_env(name))
end

---?????????????????????, ???????????? moon.adjtime ????????????
function moon.time()
    return _now()//1000
end

-------------------------??????????????????--------------------------

local co_num = 0

local co_pool = setmetatable({}, {__mode = "kv"})

local function routine(fn)
    local co = co_running()
    while true do
        co_num = co_num + 1
        fn()
        co_num = co_num - 1
        co_pool[#co_pool + 1] = co
        fn = co_yield()
    end
end

---??????????????????
---@param func fun()
---@return thread
function moon.async(func)
    local co = tremove(co_pool)
    if not co then
        co = co_create(routine)
    end
    local _, res = coresume(co, func) --func ?????? routine ?????????
    if res then
        return res
    end
    return co
end

---??????????????????????????????,?????????????????????????????????
function moon.coroutine_num()
    return co_num, #co_pool
end

------------------------------------------

---async
---@param serviceid integer
---@return string
function moon.co_remove_service(serviceid)
    return moon.remove_service(serviceid, true)
end

---@param command string
---@return string
function moon.co_runcmd(command)
    local sessionid = make_response()
    core.runcmd(command, sessionid)
    return co_yield()
end

---RPC???????????????????????????????????????responseid?????????????????????responseid???????????????????????????moon.response??????.
---@param PTYPE string @????????????
---@param receiver integer @???????????????id
---@return any|boolean,string @??????????????????, ?????????????????????????????????????????????????????????false,????????????????????????
function moon.co_call(PTYPE, receiver, ...)
    local p = protocol[PTYPE]
    if not p then
        error(string.format("moon call unknown PTYPE[%s] message", PTYPE))
    end

    if receiver == 0 then
        error("moon co_call receiver == 0")
    end

    local sessionid = make_response(receiver)
	_send(receiver, p.pack(...), "", sessionid, p.PTYPE)
    return co_yield()
end

---??????moon.call
---@param PTYPE string @????????????
---@param receiver integer  @???????????????id
---@param sessionid integer
function moon.response(PTYPE, receiver, sessionid, ...)
    if sessionid == 0 then return end
    local p = protocol[PTYPE]
    if not p then
        error("handle unknown message")
    end

    if receiver == 0 then
        error("moon response receiver == 0")
    end

    _send(receiver, p.pack(...), '', sessionid, p.PTYPE)
end

function moon.wait()
    return co_yield()
end

function moon.wakeup(session, ...)
    local co = session_id_coroutine[session]
    coresume(co, ...)
end

------------------------------------
function moon.register_protocol(t)
    local PTYPE = t.PTYPE
    if protocol[PTYPE] then
        print("Warning attemp register duplicated PTYPE", t.name)
    end
    protocol[PTYPE] = t
    protocol[t.name] = t
end

local reg_protocol = moon.register_protocol


---?????????????????????????????????????????????
---@param PTYPE string
---@param cb fun(msg:userdata,ptype:table)
---@return boolean
function moon.dispatch(PTYPE, cb)
    local p = protocol[PTYPE]
    if cb then
        local ret = p.dispatch
        p.dispatch = cb
        return ret
    else
        return p and p.dispatch
    end
end

reg_protocol {
    name = "lua",
    PTYPE = PTYPE_LUA,
    pack = seri.pack,
    unpack = unpack,
    dispatch = function()
        error("PTYPE_LUA dispatch not implemented")
    end
}

reg_protocol {
    name = "text",
    PTYPE = PTYPE_TEXT,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function()
        error("PTYPE_TEXT dispatch not implemented")
    end
}

reg_protocol {
    name = "error",
    PTYPE = PTYPE_ERROR,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function(msg)
        local sessionid, content, data = _decode(msg,"EHZ")
        if data and #data >0 then
            content = content..":"..data
        end
        local co = session_id_coroutine[sessionid]
        if co then
            session_id_coroutine[sessionid] = nil
            coresume(co, false, content)
            return
        end
    end
}

local system_command = {}

system_command._service_exit = function(sender, msg)
    local data = _decode(msg,"Z")
    for k, v in pairs(session_watcher) do
        if v == sender then
            local co = session_id_coroutine[k]
            if co then
                session_id_coroutine[k] = nil
                coresume(co, false, data)
                return
            end
        end
    end
end

moon.system = function(cmd, fn)
    system_command[cmd] = fn
end

reg_protocol {
    name = "system",
    PTYPE = PTYPE_SYSTEM,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function(msg)
        local sender, header = _decode(msg,"SH")
        local func = system_command[header]
        if func then
            func(sender, msg)
        end
    end
}

reg_protocol{
    name = "socket",
    PTYPE = PTYPE_SOCKET,
    pack = function(...) return ... end,
    dispatch = function()
        error("PTYPE_SOCKET dispatch not implemented")
    end
}

reg_protocol{
    name = "websocket",
    PTYPE = PTYPE_SOCKET_WS,
    pack = function(...) return ... end,
    dispatch = function(_)
        error("PTYPE_SOCKET_WS dispatch not implemented")
    end
}

local cb_shutdown

reg_protocol {
    name = "shutdown",
    PTYPE = PTYPE_SHUTDOWN,
    dispatch = function()
        if cb_shutdown then
            cb_shutdown()
        else
            local name = moon.name
            --- bootstrap or not unique service
            if name == "bootstrap" or 0 == moon.queryservice(moon.name) then
                moon.quit()
            end
        end
    end
}

---??????????????????????????????,??????????????????, ????????????moon.quit, ???????????????????????????
---??????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
---???????????????????????????????????????moon.quit,?????????????????????,??????server??????????????????????????????
---@param callback fun()
function moon.shutdown(callback)
    cb_shutdown = callback
end

--------------------------timer-------------

reg_protocol {
    name = "timer",
    PTYPE = PTYPE_TIMER,
    dispatch = function(msg)
        local timerid, last = _decode(msg, "SR")
        local v = timer_cb[timerid]
        local tp = type(v)
        if tp == "function" then
            v(timerid, last>0)
        elseif tp == "thread" then
            timer_cb[timerid] = nil
            coresume(v, timerid)
            return
        end

        if last>0 then
            timer_cb[timerid] = nil
        end
    end
}

---@param mills integer
---@param times integer
---@param fn function
---@return integer
function moon.repeated(mills, times, fn)
    local timerid = _repeated(mills, times)
    timer_cb[timerid] = fn
    return timerid
end

---@param timerid integer
function moon.remove_timer(timerid)
    timer_cb[timerid] = nil
    _remove_timer(timerid)
end

---async
---???????????? mills ??????
---@param mills integer
---@return integer
function moon.sleep(mills)
    local timerid = _repeated(mills, 1)
    timer_cb[timerid] = co_running()
    return co_yield()
end

--------------------------DEBUG----------------------------

local debug_command = {}

debug_command.gc = function(sender, sessionid)
    collectgarbage("collect")
    moon.response("debug",sender,sessionid, collectgarbage("count"))
end

debug_command.mem = function(sender, sessionid)
    moon.response("debug",sender,sessionid, collectgarbage("count"))
end

debug_command.ping = function(sender, sessionid)
    moon.response("debug",sender,sessionid, "pong")
end

debug_command.state = function(sender, sessionid)
    local running_num, free_num = moon.coroutine_num()
    local s = string.format("co-running %d co-free %d cpu:%d", running_num,free_num, moon.cpu())
    moon.response("debug",sender,sessionid, s)
end

reg_protocol {
    name = "debug",
    PTYPE = PTYPE_DEBUG,
    pack = pack,
    unpack = unpack,
    dispatch = function(msg, unpack_fn)
        local sender, sessionid, sz, len = _decode(msg,"SEC")
        local params = {unpack_fn(sz, len)}
        local func = debug_command[params[1]]
        if func then
            func(sender, sessionid, table.unpack(params,2))
        else
            moon.response("debug",sender,sessionid, "unknow debug cmd "..params[1])
        end
    end
}

return moon
