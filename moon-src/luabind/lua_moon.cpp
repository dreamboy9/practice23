#include "lua.hpp"
#include "common/time.hpp"
#include "common/timer.hpp"
#include "common/log.hpp"
#include "common/md5.hpp"
#include "common/byte_convert.hpp"
#include "common/lua_utility.hpp"
#include "message.hpp"
#include "server.h"
#include "worker.h"
#include "services/lua_service.h"

using namespace moon;

static void* get_ptr(lua_State* L, const char* key) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, key) == LUA_TNIL) {
        luaL_error(L, "'%s' is not register", key);
        return NULL;
    }
    void* v = lua_touserdata(L, -1);
    if (v == NULL) {
        luaL_error(L, "Invalid %s", key);
        return NULL;
    }
    lua_pop(L, 1);
    return v;
}

moon::buffer_ptr_t moon_to_buffer(lua_State* L, int index)
{
    int t = lua_type(L, index);
    switch (t)
    {
    case LUA_TNIL:
    {
        return nullptr;
    }
    case LUA_TSTRING:
    {
        std::size_t len;
        auto str = lua_tolstring(L, index, &len);
        auto buf = moon::message::create_buffer(len);
        buf->write_back(str, len);
        return buf;
    }
    case LUA_TLIGHTUSERDATA:
    {
        moon::buffer* p = static_cast<moon::buffer*>(lua_touserdata(L, index));
        return moon::buffer_ptr_t(p);
    }
    default:
        luaL_error(L, "expected nil or a  lightuserdata(buffer*) or a string");
    }
    return nullptr;
}

static int lmoon_microseconds(lua_State* L)
{
    lua_pushinteger(L, time::microsecond());
    return 1;
}

static int lmoon_md5(lua_State* L)
{
    size_t size;
    const char* s = luaL_checklstring(L, 1, &size);
    uint8_t buf[md5::DIGEST_BYTES] = { 0 };
    md5::md5_context ctx;
    md5::init(ctx);
    md5::update(ctx, s, size);
    md5::finish(ctx, buf);

    char res[md5::DIGEST_BYTES * 2];
    for (size_t i = 0; i < 16; i++) {
        int t = buf[i];
        int a = t / 16;
        int b = t % 16;
        res[i * 2] = md5::HEX[a];
        res[i * 2 + 1] = md5::HEX[b];
    }
    lua_pushlstring(L, res, sizeof(res));
    return 1;
}

static int lmoon_tostring(lua_State* L)
{
    const char* data = (const char*)lua_touserdata(L, 1);
    if (nullptr == data)
    {
        return luaL_error(L, "need char* lightuserdata");
    }
    size_t len = luaL_checkinteger(L, 2);
    lua_pushlstring(L, data, len);
    return 1;
}

static int lmoon_localtime(lua_State* L)
{
    time_t t = luaL_checkinteger(L, 1);
    std::tm local_tm;
    time::localtime(&t, &local_tm);
    lua_createtable(L, 0, 9);
    luaL_rawsetfield(L, -3, "year", lua_pushinteger(L, (lua_Integer)local_tm.tm_year + 1900));
    luaL_rawsetfield(L, -3, "month", lua_pushinteger(L, (lua_Integer)local_tm.tm_mon + 1));
    luaL_rawsetfield(L, -3, "day", lua_pushinteger(L, local_tm.tm_mday));
    luaL_rawsetfield(L, -3, "hour", lua_pushinteger(L, local_tm.tm_hour));
    luaL_rawsetfield(L, -3, "min", lua_pushinteger(L, local_tm.tm_min));
    luaL_rawsetfield(L, -3, "sec", lua_pushinteger(L, local_tm.tm_sec));
    luaL_rawsetfield(L, -3, "weekday", lua_pushinteger(L, local_tm.tm_wday));
    luaL_rawsetfield(L, -3, "yearday", lua_pushinteger(L, local_tm.tm_yday));
    luaL_rawsetfield(L, -3, "isdst", lua_pushboolean(L, local_tm.tm_isdst));
    return 1;
}

static int lmoon_timer_repeated(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    int32_t interval = (int32_t)luaL_checkinteger(L, 1);
    int32_t times = (int32_t)luaL_checkinteger(L, 2);
    moon::timer_t timerid = S->get_worker()->repeat(interval, times, S->id());
    lua_pushinteger(L, timerid);
    return 1;
}

static int lmoon_timer_remove(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    moon::timer_t id = (moon::timer_t)luaL_checkinteger(L, 1);
    S->get_worker()->remove_timer(id);
    return 0;
}

static int lmoon_log(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    auto level = (moon::LogLevel)luaL_checkinteger(L, 1);
    int n = lua_gettop(L);  /* number of arguments */
    int i;
    buffer buf;
    for (i = 2; i <= n; i++) {  /* for each argument */
        size_t l;
        const char* s = luaL_tolstring(L, i, &l);  /* convert it to string */
        if (i > 2)  /* not the first element? */
            buf.write_back("\t", 1);  /* add a tab before it */
        buf.write_back(s, l);  /* print it */
        lua_pop(L, 1);  /* pop result */
    }

    lua_Debug ar;
    if (lua_getstack(L, 2, &ar))
    {
        if (lua_getinfo(L, "Sl", &ar))
        {
            buf.write_back("\t(", 2);
            if (ar.source != nullptr && ar.source[0] != '\0')
                buf.write_back(ar.source + 1, std::strlen(ar.source) - 1);
            buf.write_back(":", 1);
            auto line = std::to_string(ar.currentline);
            buf.write_back(line.data(), line.size());
            buf.write_back(")", 1);
        }
    }

    S->logger()->logstring(true, level, std::string_view{ buf.data(), buf.size() }, S->id());
    return 0;
}

static int lmoon_set_loglevel(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    S->logger()->set_level(moon::luaL_check_stringview(L, 1));
    return 0;
}

static int lmoon_get_loglevel(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    lua_pushinteger(L, (lua_Integer)S->logger()->get_level());
    return 1;
}

static int lmoon_cpu(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    lua_pushinteger(L, (lua_Integer)S->cpu_cost());
    return 1;
}

static int lmoon_make_prefab(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    uint32_t id = S->get_worker()->make_prefab(moon_to_buffer(L, 1));
    lua_pushinteger(L, id);
    return 1;
}

static int lmoon_send_prefab(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    uint32_t receiver = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t prefabid = (uint32_t)luaL_checkinteger(L, 2);
    std::string_view header = luaL_check_stringview(L, 3);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 4);
    int8_t type = (int8_t)luaL_checkinteger(L, 5);
    S->get_worker()->send_prefab(S->id(), receiver, prefabid, header, sessionid, type);
    return 0;
}

static int lmoon_send(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    uint32_t receiver = (uint32_t)luaL_checkinteger(L, 1);
    if (receiver == 0)
        return luaL_error(L, "moon.send 'receiver' must >0");
    buffer_ptr_t buf = moon_to_buffer(L, 2);
    std::string_view header = luaL_check_stringview(L, 3);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 4);
    int8_t type = (int8_t)luaL_checkinteger(L, 5);
    S->get_router()->send(S->id(), receiver, std::move(buf), header, sessionid, type);
    return 0;
}

static int lmoon_new_service(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    std::string_view type = luaL_check_stringview(L, 1);
    std::string_view conf = luaL_check_stringview(L, 2);
    bool isunique = luaL_checkboolean(L, 3);
    int32_t workerid = (int32_t)luaL_checkinteger(L, 4);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 5);
    S->get_router()->new_service(std::string{ type },std::string{ conf }, isunique, workerid, S->id(), sessionid);
    return 0;
}

static int lmoon_kill(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    uint32_t serviceid = (uint32_t)luaL_checkinteger(L, 1);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 2);
    S->get_router()->remove_service(serviceid, S->id(), sessionid);
    return 0;
}

static int lmoon_runcmd(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    std::string_view cmd = luaL_check_stringview(L, 1);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 2);
    S->get_router()->runcmd(S->id(), std::string{ cmd }, sessionid);
    return 0;
}

static int lmoon_queryservice(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    std::string_view name = luaL_check_stringview(L, 1);
    uint32_t id = S->get_router()->get_unique_service(std::string{ name });
    lua_pushinteger(L, id);
    return 1;
}

static int lmoon_setenv(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    std::string_view name = luaL_check_stringview(L, 1);
    std::string_view value = luaL_check_stringview(L, 2);
    S->get_router()->set_env(std::string{ name }, std::string{ value });
    return 0;
}

static int lmoon_getenv(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    std::string_view name = luaL_check_stringview(L, 1);
    std::string value = S->get_router()->get_env(std::string{ name });
    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

static int lmoon_wstate(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    int32_t workerid = (int32_t)luaL_checkinteger(L, 1);
    std::string info = S->get_router()->worker_info(workerid);
    lua_pushlstring(L, info.data(), info.size());
    return 1;
}

static int lmoon_exit(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    int32_t code = (int32_t)luaL_checkinteger(L, 1);
    S->get_server()->stop(code);
    return 0;
}

static int lmoon_size(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    uint32_t count = S->get_server()->service_count();
    lua_pushinteger(L, count);
    return 1;
}

static int lmoon_now(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    time_t t = S->get_server()->now();
    lua_pushinteger(L, t);
    return 1;
}

static int lmoon_adjtime(lua_State* L)
{
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    time_t t = luaL_checkinteger(L, 1);
    bool ok = time::offset(t);
    S->get_server()->now(true);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lmoon_callback(lua_State* L) {
    lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, S);
    return 0;
}

static int message_decode(lua_State* L)
{
    message* m = (message*)lua_touserdata(L, 1);
    if (nullptr == m)
    {
        return luaL_error(L, "message info param 1 need userdata");
    }
    size_t len = 0;
    const char* sz = luaL_checklstring(L, 2, &len);
    int top = lua_gettop(L);
    for (size_t i = 0; i < len; ++i)
    {
        switch (sz[i])
        {
        case 'S':
            lua_pushinteger(L, m->sender());
            break;
        case 'R':
            lua_pushinteger(L, m->receiver());
            break;
        case 'E':
            lua_pushinteger(L, m->sessionid());
            break;
        case 'H':
        {
            std::string_view header = m->header();
            if (!header.empty())
            {
                lua_pushlstring(L, header.data(), header.size());
            }
            else
            {
                lua_pushnil(L);
            }
            break;
        }
        case 'Z':
        {
            std::string_view str = m->bytes();
            if (!str.empty())
            {
                lua_pushlstring(L, str.data(), str.size());
            }
            else
            {
                lua_pushnil(L);
            }
            break;
        }
        case 'N':
        {
            lua_pushinteger(L, m->size());
            break;
        }
        case 'B':
        {
            lua_pushlightuserdata(L, m->get_buffer());
            break;
        }
        case 'C':
        {
            buffer* buf = m->get_buffer();
            if (nullptr == buf)
            {
                lua_pushnil(L);
                lua_pushnil(L);
            }
            else
            {
                lua_pushlightuserdata(L, m->get_buffer()->data());
                lua_pushinteger(L, m->get_buffer()->size());
            }
            break;
        }
        default:
            return luaL_error(L, "message decode get unknown cmd %s", sz);
        }
    }
    return lua_gettop(L) - top;
}

static int message_clone(lua_State* L)
{
    message* m = (message*)lua_touserdata(L, 1);
    if (nullptr == m)
    {
        return luaL_error(L, "message clone param 1 need userdata");
    }
    message* nm = new message((const buffer_ptr_t&)*m);
    nm->set_broadcast(m->broadcast());
    nm->set_header(m->header());
    nm->set_receiver(m->receiver());
    nm->set_sender(m->sender());
    nm->set_sessionid(m->sessionid());
    nm->set_type(m->type());
    lua_pushlightuserdata(L, nm);
    return 1;
}

static int message_release(lua_State* L)
{
    message* m = (message*)lua_touserdata(L, 1);
    if (nullptr == m)
    {
        return luaL_error(L, "message release param 1 need userdata");
    }
    delete m;
    return 0;
}

static int message_redirect(lua_State* L)
{
    int top = lua_gettop(L);
    message* m = (message*)lua_touserdata(L, 1);
    if (nullptr == m)
    {
        return luaL_error(L, "message clone param 1 need userdata");
    }
    size_t len = 0;
    const char* sz = luaL_checklstring(L, 2, &len);
    m->set_header(std::string_view{ sz, len });
    m->set_receiver((uint32_t)luaL_checkinteger(L, 3));
    m->set_type((uint8_t)luaL_checkinteger(L, 4));
    if (top > 4)
    {
        m->set_sender((uint32_t)luaL_checkinteger(L, 5));
        m->set_sessionid((int32_t)luaL_checkinteger(L, 6));
    }
    return 0;
}

extern "C"
{
    int LUAMOD_API luaopen_moon(lua_State* L)
    {
        luaL_Reg l[] = {
            { "microseconds", lmoon_microseconds},
            { "md5", lmoon_md5 },
            { "tostring", lmoon_tostring },
            { "localtime", lmoon_localtime },
            { "repeated", lmoon_timer_repeated},
            { "remove_timer", lmoon_timer_remove},
            { "log", lmoon_log},
            { "set_loglevel", lmoon_set_loglevel},
            { "get_loglevel", lmoon_get_loglevel},
            { "cpu", lmoon_cpu},
            { "make_prefab", lmoon_make_prefab},
            { "send_prefab", lmoon_send_prefab},
            { "send", lmoon_send},
            { "new_service", lmoon_new_service},
            { "kill", lmoon_kill},
            { "runcmd", lmoon_runcmd},
            { "queryservice", lmoon_queryservice},
            { "set_env", lmoon_setenv},
            { "get_env", lmoon_getenv},
            { "wstate", lmoon_wstate},
            { "exit", lmoon_exit},
            { "size", lmoon_size},
            { "now", lmoon_now},
            { "adjtime", lmoon_adjtime},
            { "callback", lmoon_callback},
            { "decode", message_decode},
            { "clone", message_clone },
            { "release", message_release },
            { "redirect", message_redirect},
            {NULL,NULL}
        };

        lua_createtable(L, 0, sizeof(l) / sizeof(l[0])  - 1);
        lua_service* S = (lua_service*)get_ptr(L, LMOON_GLOBAL);
        lua_pushstring(L, "id");
        lua_pushinteger(L, S->id());
        lua_rawset(L, -3);
        lua_pushstring(L, "name");
        lua_pushlstring(L, S->name().data(), S->name().size());
        lua_rawset(L, -3);
        lua_pushstring(L, "null");
        lua_pushlightuserdata(L, S);
        lua_rawset(L, -3);
        lua_pushstring(L, "timezone");
        lua_pushinteger(L, moon::time::timezone());
        lua_rawset(L, -3);
        luaL_setfuncs(L, l, 0);
        return 1;
    }
}

static int lasio_try_open(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    std::string_view host = luaL_check_stringview(L, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(L, 2);
    bool ok = S->try_open(std::string{host}, port);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_listen(lua_State* L)
{
    lua_service* LS  = (lua_service*)get_ptr(L, LMOON_GLOBAL);
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    std::string_view host = luaL_check_stringview(L, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(L, 2);
    uint8_t type = (uint8_t)luaL_checkinteger(L, 3);
    uint32_t fd = S->listen(std::string{ host }, port, LS->id(), type);
    lua_pushinteger(L, fd);
    return 1;
}

static int lasio_accept(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (int32_t)luaL_checkinteger(L, 1);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 2);
    uint32_t owner = (uint32_t)luaL_checkinteger(L, 3);
    S->accept(fd, sessionid, owner);
    return 0;
}

static int lasio_connect(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    std::string_view host = luaL_check_stringview(L, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(L, 2);
    uint32_t owner = (uint32_t)luaL_checkinteger(L, 3);
    uint8_t type = (uint8_t)luaL_checkinteger(L, 4);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 5);
    uint32_t timeout = (uint32_t)luaL_checkinteger(L, 6);
    uint32_t fd = S->connect(std::string{ host }, port, owner, type, sessionid, timeout);
    lua_pushinteger(L, fd);
    return 1;
}

static int lasio_read(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t owner = (uint32_t)luaL_checkinteger(L, 2);
    int64_t size = (int64_t)luaL_checkinteger(L, 3);
    std::string_view delim = luaL_check_stringview(L, 4);
    int32_t sessionid = (int32_t)luaL_checkinteger(L, 5);
    S->read(fd, owner, size, delim, sessionid);
    return 0;
}

static int lasio_write(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    auto data = moon_to_buffer(L, 2);
    int flag = (int)luaL_optinteger(L, 3, 0);
    if (flag!=0 && (flag<=0 || flag >=(int)moon::buffer_flag::buffer_flag_max))
    {
        return luaL_error(L, "asio.write param 'flag' invalid");
    }
    bool ok = S->write(fd, data, (moon::buffer_flag)flag);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_write_message(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    moon::message* m =(moon::message*)lua_touserdata(L, 2);
    if (nullptr == m)
    {
        return luaL_error(L, "asio.write_message param 'message' invalid");
    }
    bool ok = S->write(fd, *m);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_close(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    bool ok = S->close(fd);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_settimeout(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    int32_t v = (int32_t)luaL_checkinteger(L, 2);//seconds
    bool ok = S->settimeout(fd, v);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_setnodelay(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    bool ok = S->setnodelay(fd);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_set_enable_chunked(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    std::string_view flag = luaL_check_stringview(L, 2);
    bool ok = S->set_enable_chunked(fd, flag);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_set_send_queue_limit(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t warnsize = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t errorsize = (uint32_t)luaL_checkinteger(L, 3);
    bool ok = S->set_send_queue_limit(fd, warnsize, errorsize);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int lasio_address(lua_State* L)
{
    moon::socket* S = (moon::socket*)get_ptr(L, LASIO_GLOBAL);
    uint32_t fd = (uint32_t)luaL_checkinteger(L, 1);
    std::string addr = S->getaddress(fd);
    lua_pushlstring(L, addr.data(), addr.size());
    return 1;
}

extern "C"
{
    int LUAMOD_API luaopen_asio(lua_State* L)
    {
        luaL_checkversion(L);
        luaL_Reg l[] = {
            { "try_open", lasio_try_open},
            { "listen", lasio_listen },
            { "accept", lasio_accept },
            { "connect", lasio_connect },
            { "read", lasio_read},
            { "write", lasio_write},
            { "write_message", lasio_write_message},
            { "close", lasio_close},
            { "settimeout", lasio_settimeout},
            { "setnodelay", lasio_setnodelay},
            { "set_enable_chunked", lasio_set_enable_chunked},
            { "set_send_queue_limit", lasio_set_send_queue_limit},
            { "getaddress", lasio_address},
            {NULL,NULL}
        };

        luaL_newlib(L, l);
        return 1;
    }
}
