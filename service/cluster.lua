local moon = require("moon")
local seri = require("seri")

local conf = ...

local pack = seri.pack
local packs = seri.packs
local co_yield = coroutine.yield

local NODE = math.tointeger(moon.get_env("NODE"))

local function cluster_service()

    local json = require("json")
    local buffer = require("buffer")
    local socket = require("moon.socket")

    local print = print
    local assert = assert
    local pairs = pairs
    local strfmt = string.format

    local unpack_one = seri.unpack_one
    local queryservice = moon.queryservice
    local wfront = buffer.write_front

    local redirect = moon.redirect

    local clusters = {}

    local close_watch = {}
    local send_watch = {}

    local function add_send_watch( fd, sender, sessionid )
        local senders = send_watch[fd]
        if not senders then
            senders = {}
            send_watch[fd] = senders
        end
        local key = (sessionid<<32)|sender
        --print("addkey", key, sessionid)
        senders[key] = moon.time()
    end

    local function remove_send_watch( fd, sender, sessionid )
        local senders = send_watch[fd]
        if not senders then
            assert(false)
            return
        end

        local key = (sessionid<<32)|sender
        --print("remove key", key, sessionid)
        senders[key] = nil
        return true
    end

    local services_address = {}
    local function get_service_address(sname)
        local addr = services_address[sname]
        if not addr then
            addr = queryservice(sname)
            assert(addr>0, tostring(sname))
            services_address[sname] = addr
        end
        return addr
    end

    socket.on("connect",function(fd, msg)
        print("connect", fd, moon.decode(msg, "Z"))
        socket.set_enable_chunked(fd,"wr")
    end)

    socket.on("accept",function(fd, msg)
        socket.settimeout(fd, 180)
        socket.set_enable_chunked(fd,"wr")
        print("accept", fd, moon.decode(msg, "Z"))
    end)

    socket.on("message", function(fd, msg)
        local buf = moon.decode(msg, "B")
        ---@type cluster_header
        local header = unpack_one(buf, true)
        if header.ping then
            socket.write(fd, pack({pong = true}))
            return
        elseif header.pong then
            return
        end

        if header.session < 0 then -- receive call message
            moon.async(function ()
                local address = get_service_address(header.to_sname)
                assert(address>0, tostring(header.to_sname))
                local session = moon.make_response(address)
                redirect(msg, "", address, moon.PTYPE_LUA, moon.addr(), -session)
                local res = {co_yield()}
                header.session = -header.session
                socket.write(fd, pack(header, table.unpack(res)))
            end)
        elseif header.session > 0 then --receive response message
            if remove_send_watch(fd, header.from_addr, header.session) then
                redirect(msg, "", header.from_addr, moon.PTYPE_LUA, moon.addr(), header.session)
            end
        else-- receive send message
            local address = get_service_address(header.to_sname)
            redirect(msg, "", address, moon.PTYPE_LUA)
        end
    end)

    socket.on("close", function(fd, msg)
        print("close", moon.decode(msg, "Z"))
        for k, v in pairs(close_watch) do
            if v == fd then
                close_watch[k] = false
            end
        end

        local senders = send_watch[fd]
        if senders then
            for key in pairs(senders) do
                local sender = key&0xFFFFFFFF
                local sessionid = (key>>32)
                print("close response", sender, sessionid)
                moon.response("lua", sender, -sessionid, false, "cluster:socket disconnect")
            end
        end
        send_watch[fd] = nil

        for _,v in pairs(clusters) do
            if v.fd == fd then
                v.fd = false
                break
            end
        end
    end)

    socket.on("error", function(fd, msg)
        print("socket error",fd, moon.decode(msg, "Z"))
    end)

    local function connect(node)
        local c = clusters[node]
        local fd, err = socket.connect(c.host, c.port, moon.PTYPE_SOCKET, 1000)
        if not fd then
            moon.error(err)
            return
        end
        socket.set_enable_chunked(fd, "wr")
        return fd
    end

    local command = {}

    local send_queue = require("moon.queue").new()

    function command.Start()
        if conf.host and conf.port then
            if not clusters[NODE] then
                print("unconfig cluster node:".. moon.name)
                return false
            end
            local host, port = conf.host, conf.port
            local listenfd = socket.listen(host, port,moon.PTYPE_SOCKET)
            socket.start(listenfd)
            print(strfmt("cluster run at %s %d", host, port))
            setmetatable(clusters, {__gc=function()
                socket.close(listenfd)
            end})
        end
        return true
    end

    function command.Request(msg)
        ---@type cluster_header
        local header = unpack_one(moon.decode(msg, "B"))

        local c = clusters[header.to_node]
        if not c then
            moon.response("lua", header.from_addr, header.session, false, "target not run cluster")
            return
        end

        if c.fd and socket.write_message(c.fd, msg) then
            if header.session < 0 then
                --??????mode-call?????????????????????????????????????????????
                add_send_watch(c.fd, header.from_addr, -header.session)
            end
            return
        end

        local data = moon.decode(msg, "Z")

        send_queue:run(function()
            if not c.fd then
                c.fd = connect(header.to_node)
            end

            if c.fd and socket.write(c.fd, data) then
                if header.session < 0 then
                    --??????mode-call?????????????????????????????????????????????
                    add_send_watch(c.fd, header.from_addr, -header.session)
                end
            else
                c.fd = false
            end

            if not c.fd then
                if header.session == 0 then
                    moon.error("not connected cluster")
                else
                    --CASE1:connect failed, mode-call, ??????????????????
                    moon.response("lua", header.from_addr, header.session, false, "connect failed")
                end
            end
        end)
    end

    local function load_config()
        local function find_host_port(config_params)
            if not config_params then
                return nil
            end

            local host,port
            for k, v in pairs(config_params) do
                if k == "cluster_host" then
                    host = v
                elseif k == "cluster_port" then
                    port = v
                end
            end
            return host,port
        end

        local content = moon.get_env("CONFIG")
        local js = json.decode(content)
        local game_list = {}
        for _,c in ipairs(js) do
            local host,port = find_host_port(c.params)
            if host and port then
                if not clusters[c.node] then
                    clusters[c.node]={host = host, port = port, fd = false}
                end
            end
            if c.params and c.params.server_game then
                table.insert(game_list, c.node)
            end
        end
        -- moon.set_env("MAX_CLUSTER_NODE", tostring(max_cluster_node))
        moon.set_env("GAME_NODE_LIST", json.encode(game_list))
    end

    load_config()

    moon.async(function()
        while true do
            moon.sleep(5000)
            for _,senders in pairs(send_watch) do
                for key, t in pairs(senders) do
                    if moon.time() - t > 10 then
                        local sender = key&0xFFFFFFFF
                        local sessionid = (key>>32)
                        moon.response("lua", sender, -sessionid, false, "cluster:socket read timeout")
                        senders[key] = nil
                    end
                end
            end

            for _, v in pairs(clusters) do
                if v.fd then
                    socket.write(v.fd, pack({ping = true}))
                end
            end
        end
    end)

    moon.dispatch("lua",function(msg)
        local sender, sessionid, buf = moon.decode(msg, "SEB")
        local cmd = unpack_one(buf, true)
        local fn = command[cmd]
        if fn then
            moon.async(function()
                if sessionid ~= 0 then
                    local unsafe_buf = pack(xpcall(fn, debug.traceback, msg))
                    local ok = unpack_one(unsafe_buf, true)
                    if not ok then
                        wfront(unsafe_buf, packs(false))
                    end
                    moon.raw_send("lua", sender, "", unsafe_buf, sessionid)
                else
                    fn(msg)
                end
            end)
        else
            moon.error(moon.name, "recv unknown cmd "..tostring(cmd))
        end
    end)

    moon.shutdown(function()
        moon.quit()
    end)
end

if conf and conf.name then
    cluster_service()
end

local cluster = {}

local cluster_address

---@class cluster_header
---@field public to_node integer @ ??????????????????ID
---@field public to_sname string @ ???????????????name
---@field public from_node integer @ ??????????????????ID
---@field public from_addr integer @ ???????????????ID
---@field public session integer @ ??????session


function cluster.send(receiver_node, receiver_sname, ...)
    if not cluster_address then
        cluster_address = moon.queryservice("cluster")
        assert(cluster_address>0)
    end

    local header = {
        to_node = receiver_node,
        to_sname = receiver_sname,
        from_node = NODE,
        from_addr = moon.addr(),
        session = 0
    }

    moon.send("lua", cluster_address, "Request", header, ...)
end

function cluster.call(receiver_node, receiver_sname, ...)
    if not cluster_address then
        cluster_address = moon.queryservice("cluster")
        assert(cluster_address>0)
    end

    local sessionid = moon.make_response(cluster_address)

    local header = {
        to_node = receiver_node,
        to_sname = receiver_sname,
        from_node = NODE,
        from_addr = moon.addr(),
        session = -sessionid
    }

    moon.raw_send("lua", cluster_address, "", pack("Request", header, ...))
    return co_yield()
end

return cluster