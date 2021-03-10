// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this tendis open source
// project for additional information.

#include <math.h>
#include <memory>
#include <string>
#include <vector>
#include "tendisplus/script/lua_state.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/scopeguard.h"

extern "C" {
#include "tendisplus/script/sha1.h"
}

namespace tendisplus {

const char *redisProtocolToLuaType_Int(lua_State *lua, const char *reply);
const char *redisProtocolToLuaType_Bulk(lua_State *lua, const char *reply);
const char *redisProtocolToLuaType_Status(lua_State *lua, const char *reply);
const char *redisProtocolToLuaType_Error(lua_State *lua, const char *reply);
const char *redisProtocolToLuaType_MultiBulk(lua_State *lua, const char *reply);

/* ---------------------------------------------------------------------------
* Redis reply to Lua type conversion functions.
* ------------------------------------------------------------------------- */

/* Take a Redis reply in the Redis protocol format and convert it into a
* Lua type. Thanks to this function, and the introduction of not connected
* clients, it is trivial to implement the redis() lua function.
*
* Basically we take the arguments, execute the Redis command in the context
* of a non connected client, then take the generated reply and convert it
* into a suitable Lua type. With this trick the scripting feature does not
* need the introduction of a full Redis internals API. The script
* is like a normal client that bypasses all the slow I/O paths.
*
* Note: in this function we do not do any sanity check as the reply is
* generated by Redis directly. This allows us to go faster.
*
* Errors are returned as a table with a single 'err' field set to the
* error string.
*/

const char *redisProtocolToLuaType(lua_State *lua, const char* reply) {
  const char *p = reply;

  switch (*p) {
    case ':': p = redisProtocolToLuaType_Int(lua, reply); break;
    case '$': p = redisProtocolToLuaType_Bulk(lua, reply); break;
    case '+': p = redisProtocolToLuaType_Status(lua, reply); break;
    case '-': p = redisProtocolToLuaType_Error(lua, reply); break;
    case '*': p = redisProtocolToLuaType_MultiBulk(lua, reply); break;
  }
  return p;
}

int string2ll(const char *s, size_t slen, int64_t *value) {
  auto expt = ::tendisplus::stoll(string(s, slen));
  if (!expt.ok()) {
    DLOG(INFO) << "string2ll failed:" << value;
    return 0;
  }
  *value = expt.value();
  return 1;
}

const char *redisProtocolToLuaType_Int(lua_State *lua, const char *reply) {
  const char *p = strchr(reply+1, '\r');
  int64_t value;

  string2ll(reply+1, p-reply-1, &value);
  lua_pushnumber(lua, (lua_Number)value);
  return p+2;
}

const char *redisProtocolToLuaType_Bulk(lua_State *lua, const char *reply) {
  const char *p = strchr(reply+1, '\r');
  int64_t bulklen;

  string2ll(reply+1, p-reply-1, &bulklen);
  if (bulklen == -1) {
    lua_pushboolean(lua, 0);
    return p+2;
  } else {
    lua_pushlstring(lua, p+2, bulklen);
    return p+2+bulklen+2;
  }
}

const char *redisProtocolToLuaType_Status(lua_State *lua, const char *reply) {
  const char *p = strchr(reply+1, '\r');

  lua_newtable(lua);
  lua_pushstring(lua, "ok");
  lua_pushlstring(lua, reply+1, p-reply-1);
  lua_settable(lua, -3);
  return p+2;
}

const char *redisProtocolToLuaType_Error(lua_State *lua, const char *reply) {
  const char *p = strchr(reply+1, '\r');

  lua_newtable(lua);
  lua_pushstring(lua, "err");
  lua_pushlstring(lua, reply+1, p-reply-1);
  lua_settable(lua, -3);
  return p+2;
}

const char *redisProtocolToLuaType_MultiBulk(lua_State *lua,
      const char *reply) {
  const char *p = strchr(reply+1, '\r');
  int64_t mbulklen;
  int j = 0;

  string2ll(reply+1, p-reply-1, &mbulklen);
  p += 2;
  if (mbulklen == -1) {
    lua_pushboolean(lua, 0);
    return p;
  }
  lua_newtable(lua);
  for (j = 0; j < mbulklen; j++) {
    lua_pushnumber(lua, j+1);
    p = redisProtocolToLuaType(lua, p);
    lua_settable(lua, -3);
  }
  return p;
}

/* This function is used in order to push an error on the Lua stack in the
 * format used by redis.pcall to return errors, which is a lua table
 * with a single "err" field set to the error string. Note that this
 * table is never a valid reply by proper commands, since the returned
 * tables are otherwise always indexed by integers, never by strings. */
void luaPushError(lua_State *lua, const char *error) {
  lua_Debug dbg;

  // TODO(takenliu): lua debug
  /* If debugging is active and in step mode, log errors resulting from
   * Redis commands. */

  lua_newtable(lua);
  lua_pushstring(lua, "err");

  /* Attempt to figure out where this function was called, if possible */
  if (lua_getstack(lua, 1, &dbg) && lua_getinfo(lua, "nSl", &dbg)) {
    string msg = string(dbg.source) + ": " + std::to_string(dbg.currentline)
          + ": " + string(error);
    LOG(INFO) << "luaPushError:" << msg;
    lua_pushstring(lua, msg.c_str());
  } else {
    LOG(INFO) << "luaPushError:" << error;
    lua_pushstring(lua, error);
  }
  lua_settable(lua, -3);
}

/* In case the error set into the Lua stack by luaPushError() was generated
 * by the non-error-trapping version of redis.pcall(), which is redis.call(),
 * this function will raise the Lua error so that the execution of the
 * script will be halted. */
int luaRaiseError(lua_State *lua) {
  lua_pushstring(lua, "err");
  lua_gettable(lua, -2);
  return lua_error(lua);
}

/* Sort the array currently in the stack. We do this to make the output
 * of commands like KEYS or SMEMBERS something deterministic when called
 * from Lua (to play well with AOf/replication).
 *
 * The array is sorted using table.sort itself, and assuming all the
 * list elements are strings. */
void luaSortArray(lua_State *lua) {
  /* Initial Stack: array */
  lua_getglobal(lua, "table");
  lua_pushstring(lua, "sort");
  lua_gettable(lua, -2);       /* Stack: array, table, table.sort */
  lua_pushvalue(lua, -3);      /* Stack: array, table, table.sort, array */
  if (lua_pcall(lua, 1, 0, 0)) {
    /* Stack: array, table, error */

    /* We are not interested in the error, we assume that the problem is
     * that there are 'false' elements inside the array, so we try
     * again with a slower function but able to handle this case, that
     * is: table.sort(table, __redis__compare_helper) */
    lua_pop(lua, 1);             /* Stack: array, table */
    lua_pushstring(lua, "sort"); /* Stack: array, table, sort */
    lua_gettable(lua, -2);       /* Stack: array, table, table.sort */
    lua_pushvalue(lua, -3);      /* Stack: array, table, table.sort, array */
    lua_getglobal(lua, "__redis__compare_helper");
    /* Stack: array, table, table.sort, array, __redis__compare_helper */
    lua_call(lua, 2, 0);
  }
  /* Stack: array (sorted), table */
  lua_pop(lua, 1);             /* Stack: array (sorted) */
}

Expected<std::string> LuaState::luaCreateFunction(lua_State *lua,
      const std::string& body) {
  char funcname[43];

  funcname[0] = 'f';
  funcname[1] = '_';
  sha1hex(funcname+2, const_cast<char*>(body.c_str()), body.length());

  std::string funcdef;
  funcdef += "function ";
  funcdef += funcname;
  funcdef += "() ";
  funcdef += body;
  funcdef += "\nend";

  if (luaL_loadbuffer(lua, funcdef.c_str(), funcdef.length(),
    "@user_script")) {
    string err = "Error compiling script (new function):"
      + string(lua_tostring(lua, -1));
    DLOG(ERROR) << err;

    lua_pop(lua, 1);
    return {ErrorCodes::ERR_LUA, err};
  }

  if (lua_pcall(lua, 0, 0, 0)) {
    string err = "Error running script (new function):"
      + string(lua_tostring(lua, -1));
    DLOG(ERROR) << err;
    lua_pop(lua, 1);
    return {ErrorCodes::ERR_LUA, err};
  }

  return std::string(funcname);
}

/* Set an array of Redis String Objects as a Lua array (table) stored into a
* global variable. */
void luaSetGlobalArray(lua_State *lua, const string& var,
    const std::vector<std::string>& args, int start, int num) {
  int j;

  lua_newtable(lua);
  for (j = 0; j < num; j++) {
    lua_pushlstring(lua, args[start+j].c_str(), args[start+j].length());
    lua_rawseti(lua, -2, j+1);
  }
  lua_setglobal(lua, var.c_str());
}

/* ---------------------------------------------------------------------------
 * Redis provided math.random
 * ------------------------------------------------------------------------- */

/* We replace math.random() with our implementation that is not affected
 * by specific libc random() implementations and will output the same sequence
 * (for the same seed) in every arch. */

/* The following implementation is the one shipped with Lua itself but with
 * rand() replaced by redisLrand48(). */
int LuaState::redis_math_random(lua_State *L) {
  LuaState* ls = getLuaStateFromLua(L);

  /* the `%' avoids the (rare) case of r==1, and is needed also because on
     some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
  lua_Number r = (lua_Number)(ls->_rand.redisLrand48()%REDIS_LRAND48_MAX) /
                 (lua_Number)REDIS_LRAND48_MAX;
  switch (lua_gettop(L)) {  /* check number of arguments */
    case 0: {  /* no arguments */
      lua_pushnumber(L, r);  /* Number between 0 and 1 */
      break;
    }
    case 1: {  /* only upper limit */
      int u = luaL_checkint(L, 1);
      luaL_argcheck(L, 1 <= u, 1, "interval is empty");
      lua_pushnumber(L, floor(r*u)+1);  /* int between 1 and `u' */
      break;
    }
    case 2: {  /* lower and upper limits */
      int l = luaL_checkint(L, 1);
      int u = luaL_checkint(L, 2);
      luaL_argcheck(L, l <= u, 2, "interval is empty");
      lua_pushnumber(L, floor(r*(u-l+1))+l);  /* int between `l' and `u' */
      break;
    }
    default: return luaL_error(L, "wrong number of arguments");
  }
  return 1;
}

int LuaState::redis_math_randomseed(lua_State *L) {
  LuaState* ls = getLuaStateFromLua(L);
  ls->_rand.redisSrand48(luaL_checkint(L, 1));
  return 0;
}

/* redis.call() redis.pcall()*/
int LuaState::luaRedisGenericCommand(lua_State *lua, int raise_error) {
  LuaState* ls = getLuaStateFromLua(lua);
  int j, argc = lua_gettop(lua);

  ls->updateFakeClient();

  // TODO(takenliu): fix MULTI loggical.
  if (ls->_sess->getCtx()->isInMulti()) {
    ls->_fakeSess->getSession()->getCtx()->setMulti();
  } else {
    ls->_fakeSess->getSession()->getCtx()->resetMulti();
  }

  /* By using Lua debug hooks it is possible to trigger a recursive call
   * to luaRedisGenericCommand(), which normally should never happen.
   * To make this function reentrant is futile and makes it slower, but
   * we should at least detect such a misuse, and abort. */
  if (ls->inuse) {
    const char* recursion_warning =
            "luaRedisGenericCommand() recursive call detected. "
            "Are you doing funny stuff with Lua debug hooks?";
    LOG(WARNING) << recursion_warning;
    luaPushError(lua, recursion_warning);
    return 1;
  }
  ls->inuse++;

  /* Require at least one argument */
  if (argc == 0) {
    luaPushError(lua,
                 "Please specify at least one argument for redis.call()");
    ls->inuse--;
    DLOG(INFO) << "Please specify at least one argument for redis.call()";
    return raise_error ? luaRaiseError(lua) : 1;
  }
  std::vector<string> args;

  for (j = 0; j < argc; j++) {
    char *obj_s;
    size_t obj_len;
    char dbuf[64];

    if (lua_type(lua, j+1) == LUA_TNUMBER) {
      /* We can't use lua_tolstring() for number -> string conversion
       * since Lua uses a format specifier that loses precision. */
      lua_Number num = lua_tonumber(lua, j+1);

      obj_len = snprintf(dbuf, sizeof(dbuf), "%.17g", static_cast<double>(num));
      obj_s = dbuf;
    } else {
      obj_s = const_cast<char*>(lua_tolstring(lua, j+1, &obj_len));
      if (obj_s == NULL) break; /* Not a string. */
    }

    args.push_back(string(obj_s, obj_len));
  }

  /* Check if one of the arguments passed by the Lua script
   * is not a string or an integer (lua_isstring() return true for
   * integers as well). */
  if (j != argc) {
    luaPushError(lua,
                 "Lua redis() command arguments must be strings or integers");
    ls->inuse--;
    DLOG(INFO) << "Lua redis() command arguments must be strings or integers";
    return raise_error ? luaRaiseError(lua) : 1;
  }

  auto guard = MakeGuard([ls, &raise_error, lua] {
    if (raise_error) {
      /* If we are here we should have an error in the stack, in the
       * form of a table with an "err" field. Extract the string to
       * return the plain error. */
      LOG(INFO) << "luaRedisGenericCommand MakeGuard raise_error";
      ls->inuse--;
      luaRaiseError(lua);
      return;
    }
    ls->inuse--;
  });

  /* Setup our fake client for command execution */
  ls->_fakeSess->getSession()->setArgs(args);

  // TODO(takenliu):
  /* Log the command if debugging is active. */

  std::string ret_value;
  auto expCmdName = Command::precheck(ls->_fakeSess->getSession());
  if (!expCmdName.ok()) {
    // luaPushError(lua, const_cast<char*>(
    //   expCmdName.status().toString().c_str()));
    // luaPushError(lua, "ERR unknown command 'nosuchcommand'");
    redisProtocolToLuaType(lua, expCmdName.status().toString().c_str());
    DLOG(INFO) << "Command::precheck failed:"<< expCmdName.status().toString();
    return 1;
  }

  /* There are commands that are not allowed inside scripts. */
  std::string commandName = toLower(args[0]);
  auto command = commandMap().find(commandName);
  if (command->second->getFlags() & CMD_NOSCRIPT) {
    luaPushError(lua, "This Redis command is not allowed from scripts");
    DLOG(INFO) << "Command flags CMD_NOSCRIPT" << args[0];  // takenliu:log here
    return 1;
  }

  /* Write commands are forbidden against read-only slaves, or if a
   * command marked as non-deterministic was already called in the context
   * of this script. */
  if (command->second->getFlags() &  CMD_WRITE) {
    if (ls->lua_random_dirty && !ls->lua_replicate_commands) {
      luaPushError(lua,
         "Write commands not allowed after non deterministic commands."
         "Call redis.replicate_commands() at the start of your script "
         "in order to switch to single commands replication mode.");
      return 1;
    }
  }
  if (command->second->getFlags() & CMD_RANDOM) {
    ls->lua_random_dirty = 1;
  }
  if (command->second->getFlags() & CMD_WRITE) {
    ls->lua_write_dirty = 1;
  }

  /* If this is a Redis Cluster node, we need to make sure Lua is not
   * trying to access non-local keys, with the exception of commands
   * received from our master or when loading the AOF back in memory. */
  // TODO(takenliu): CLIENT_MASTER, master will send lua script to slave.
  if (ls->_svr->isClusterEnabled() && ls->_svr->isRunning() &&
    !ls->_sess->getCtx()->getFlags()) {
    uint32_t flags = ls->_fakeSess->getSession()->getCtx()->getFlags();
    // NOTO(takenliu): hasnot CLIENT_ASKING
    flags &= ~(CLIENT_READONLY);
    flags |= ls->_sess->getCtx()->getFlags() & (CLIENT_READONLY);
    ls->_fakeSess->getSession()->getCtx()->setFlags(flags);

    // TODO(takenliu) : command will check key whether belong this node.
    //    is here need check???
  }

  // TODO(takenliu) : for cur node,can push multi before commands,
  //  and push exec after commands. for slave, need be atomic too.

  auto expect = Command::runSessionCmd(ls->_fakeSess->getSession());
  // LOG(INFO) << "Command::runSessionCmd rsp status:"
  //   <<expect.status().toString()
  //   <<" value:"<<expect.value().c_str() << " raise_error:" << raise_error;
  if (!expect.ok()) {  // TODO(takenliu) do what ???
    // luaPushError(lua, expect.status().toString().c_str());
    redisProtocolToLuaType(lua, expect.status().toString().c_str());
    // ls->has_command_error = true;
    return 1;
  }

  /* Convert the result of the Redis command into a suitable Lua type.
   * The first thing we need is to create a single string from the client
   * output buffers. */

  if (raise_error && expect.value().size() > 0 && expect.value()[0] != '-') {
    raise_error = 0;
  }
  redisProtocolToLuaType(lua, expect.value().c_str());

  // TODO(takenliu) debugger
  /* If the debugger is active, log the reply from Redis. */


  /* Sort the output array if needed, assuming it is a non-null multi bulk
   * reply as expected. */
  if ((command->second->getFlags() & CMD_SORT_FOR_SCRIPT) &&
      (ls->lua_replicate_commands == 0) &&
      expect.value().size() > 1 && expect.value()[0] == '*' &&
      expect.value()[1] != '-') {
    luaSortArray(lua);
  }
  // NOTO(takenliu) :_fakeSess args will reset next time, response is no use.

  return 1;
}

/* redis.call() */
int LuaState::luaRedisCallCommand(lua_State *lua) {
  return luaRedisGenericCommand(lua, 1);
}

/* redis.pcall() */
int LuaState::luaRedisPCallCommand(lua_State *lua) {
  return luaRedisGenericCommand(lua, 0);
}


/* This adds redis.sha1hex(string) to Lua scripts using the same hashing
 * function used for sha1ing lua scripts. */
int LuaState::luaRedisSha1hexCommand(lua_State *lua) {
  int argc = lua_gettop(lua);
  char digest[41];
  size_t len;
  char *s;

  if (argc != 1) {
    lua_pushstring(lua, "wrong number of arguments");
    return lua_error(lua);
  }

  s = const_cast<char*>(lua_tolstring(lua, 1, &len));
  sha1hex(digest, s, len);
  lua_pushstring(lua, digest);
  return 1;
}

/* Returns a table with a single field 'field' set to the string value
 * passed as argument. This helper function is handy when returning
 * a Redis Protocol error or status reply from Lua:
 *
 * return redis.error_reply("ERR Some Error")
 * return redis.status_reply("ERR Some Error")
 */
int luaRedisReturnSingleFieldTable(lua_State *lua, const char *field) {
  if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
    luaPushError(lua, "wrong number or type of arguments");
    return 1;
  }

  lua_newtable(lua);
  lua_pushstring(lua, field);
  lua_pushvalue(lua, -3);
  lua_settable(lua, -3);
  return 1;
}

/* redis.error_reply() */
int luaRedisErrorReplyCommand(lua_State *lua) {
  return luaRedisReturnSingleFieldTable(lua, "err");
}

/* redis.status_reply() */
int luaRedisStatusReplyCommand(lua_State *lua) {
  return luaRedisReturnSingleFieldTable(lua, "ok");
}

/* redis.replicate_commands()
 *
 * Turn on single commands replication if the script never called
 * a write command so far, and returns true. Otherwise if the script
 * already started to write, returns false and stick to whole scripts
 * replication, which is our default. */
// int LuaState::luaRedisReplicateCommandsCommand(lua_State *lua) {
//  if (lua_write_dirty) {
//    lua_pushboolean(lua,0);
//  } else {
//    lua_replicate_commands = 1;
//    /* When we switch to single commands replication, we can provide
//     * different math.random() sequences at every call, which is what
//     * the user normally expects. */
//    // TODO(takenliu) rand
//    // redisSrand48(rand());
//    lua_pushboolean(lua,1);
//  }
//  return 1;
//}

/* redis.log() */
int luaLogCommand(lua_State *lua) {
  int j, argc = lua_gettop(lua);
  int level;
  string log;

  if (argc < 2) {
    lua_pushstring(lua, "redis.log() requires two arguments or more.");
    return lua_error(lua);
  } else if (!lua_isnumber(lua, -argc)) {
    lua_pushstring(lua, "First argument must be a number (log level).");
    return lua_error(lua);
  }
  level = lua_tonumber(lua, -argc);
  if (level < LL_DEBUG || level > LL_WARNING) {
    lua_pushstring(lua, "Invalid debug level.");
    return lua_error(lua);
  }

  /* Glue together all the arguments */
  for (j = 1; j < argc; j++) {
    size_t len;
    char *s;

    s = const_cast<char*>(lua_tolstring(lua, (-argc)+j, &len));
    if (s) {
      if (j != 1) log += " ";
      log += string(s, len);
    }
  }
  serverLogNew(level, "%s", log.c_str());
  return 0;
}

void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc) {
  lua_pushcfunction(lua, luafunc);
  lua_pushstring(lua, libname);
  lua_call(lua, 1, 0);
}

extern "C" {
LUALIB_API int (luaopen_cjson)(lua_State *L);
LUALIB_API int (luaopen_struct)(lua_State *L);
LUALIB_API int (luaopen_cmsgpack)(lua_State *L);
LUALIB_API int (luaopen_bit)(lua_State *L);
// LUALIB_API int luaL_loadbuffer (lua_State *L, const char *buff, size_t size,
//    const char *name);
}

void luaLoadLibraries(lua_State *lua) {
  luaLoadLib(lua, "", luaopen_base);
  luaLoadLib(lua, LUA_TABLIBNAME, luaopen_table);
  luaLoadLib(lua, LUA_STRLIBNAME, luaopen_string);
  luaLoadLib(lua, LUA_MATHLIBNAME, luaopen_math);
  luaLoadLib(lua, LUA_DBLIBNAME, luaopen_debug);
  luaLoadLib(lua, "cjson", luaopen_cjson);
  luaLoadLib(lua, "struct", luaopen_struct);
  luaLoadLib(lua, "cmsgpack", luaopen_cmsgpack);
  luaLoadLib(lua, "bit", luaopen_bit);

#if 0 /* Stuff that we don't load currently, for sandboxing concerns. */
  luaLoadLib(lua, LUA_LOADLIBNAME, luaopen_package);
  luaLoadLib(lua, LUA_OSLIBNAME, luaopen_os);
#endif
}

/* Remove a functions that we don't want to expose to the Redis scripting
* environment. */
void LuaState::luaRemoveUnsupportedFunctions(lua_State *lua) {
  lua_pushnil(lua);
  lua_setglobal(lua, "loadfile");
  lua_pushnil(lua);
  lua_setglobal(lua, "dofile");
}

/* This function installs metamethods in the global table _G that prevent
* the creation of globals accidentally.
*
* It should be the last to be called in the scripting engine initialization
* sequence, because it may interact with creation of globals. */
void scriptingEnableGlobalsProtection(lua_State *lua) {
  std::string code;

  /* strict.lua from: http://metalua.luaforge.net/src/lib/strict.lua.html.
   * Modified to be adapted to Redis. */
  code += "local dbg=debug\n";
  code += "local mt = {}\n";
  code += "setmetatable(_G, mt)\n";
  code += "mt.__newindex = function (t, n, v)\n";
  code += "  if dbg.getinfo(2) then\n";
  code += "    local w = dbg.getinfo(2, \"S\").what\n";
  code += "    if w ~= \"main\" and w ~= \"C\" then\n";
  code += "      error(\"Script attempted to create global variable '\"..tostring(n)..\"'\", 2)\n"; // NOLINT
  code += "    end\n";
  code += "  end\n";
  code += "  rawset(t, n, v)\n";
  code += "end\n";
  code += "mt.__index = function (t, n)\n";
  code += "  if dbg.getinfo(2) and dbg.getinfo(2, \"S\").what ~= \"C\" then\n";
  code += "    error(\"Script attempted to access nonexistent global variable '\"..tostring(n)..\"'\", 2)\n"; // NOLINT
  code += "  end\n";
  code += "  return rawget(t, n)\n";
  code += "end\n";
  code += "debug = nil\n";

  luaL_loadbuffer(lua, code.c_str(), code.length(), "@enable_strict_lua");
  lua_pcall(lua, 0, 0, 0);
}


LuaState::LuaState(std::shared_ptr<ServerEntry> svr, uint32_t id) {
  _id = id;
  _lua = initLua(1);
  _svr = svr;
  _scriptMgr = _svr->getScriptMgr();
}

LuaState::~LuaState() {
  lua_close(_lua);
}

/* Initialize the scripting environment.
*
* This function is called the first time at server startup with
* the 'setup' argument set to 1.
*
* It can be called again multiple times during the lifetime of the Redis
* process, with 'setup' set to 0, and following a scriptingRelease() call,
* in order to reset the Lua scripting environment.
*
* However it is simpler to just call scriptingReset() that does just that. */
lua_State* LuaState::initLua(int setup) {
  lua_State *lua = lua_open();

  if (setup) {
      _sess = nullptr;
      _fakeSess = nullptr;
      lua_timedout = 0;
  }

  luaLoadLibraries(lua);
  luaRemoveUnsupportedFunctions(lua);

  /* NOTE(takenliu): dont support lua_scripts dictionary*/

  /* Register the redis commands table and fields */
  lua_newtable(lua);

  /* redis.call */
  lua_pushstring(lua, "call");
  lua_pushcfunction(lua, luaRedisCallCommand);
  lua_settable(lua, -3);

  /* redis.pcall */
  lua_pushstring(lua, "pcall");
  lua_pushcfunction(lua, luaRedisPCallCommand);
  lua_settable(lua, -3);

  /* redis.log and log levels. */
  lua_pushstring(lua, "log");
  lua_pushcfunction(lua, luaLogCommand);
  lua_settable(lua, -3);

  lua_pushstring(lua, "LOG_DEBUG");
  lua_pushnumber(lua, LL_DEBUG);
  lua_settable(lua, -3);

  lua_pushstring(lua, "LOG_VERBOSE");
  lua_pushnumber(lua, LL_VERBOSE);
  lua_settable(lua, -3);

  lua_pushstring(lua, "LOG_NOTICE");
  lua_pushnumber(lua, LL_NOTICE);
  lua_settable(lua, -3);

  lua_pushstring(lua, "LOG_WARNING");
  lua_pushnumber(lua, LL_WARNING);
  lua_settable(lua, -3);

  /* redis.sha1hex */
  lua_pushstring(lua, "sha1hex");
  lua_pushcfunction(lua, luaRedisSha1hexCommand);
  lua_settable(lua, -3);

  /* redis.error_reply and redis.status_reply */
  lua_pushstring(lua, "error_reply");
  lua_pushcfunction(lua, luaRedisErrorReplyCommand);
  lua_settable(lua, -3);
  lua_pushstring(lua, "status_reply");
  lua_pushcfunction(lua, luaRedisStatusReplyCommand);
  lua_settable(lua, -3);

  /* NOTE(takenliu): dont support: */
  /* redis.replicate_commands */
  /* redis.debug */

  /* Finally set the table as 'redis' global var. */
  lua_setglobal(lua, "redis");

  /* Replace math.random and math.randomseed with our implementations. */
  lua_getglobal(lua, "math");

  lua_pushstring(lua, "random");
  lua_pushcfunction(lua, redis_math_random);
  lua_settable(lua, -3);

  lua_pushstring(lua, "randomseed");
  lua_pushcfunction(lua, redis_math_randomseed);
  lua_settable(lua, -3);

  lua_setglobal(lua, "math");

  /* Add a helper function that we use to sort the multi bulk output of non
   * deterministic commands, when containing 'false' elements. */
  {
    std::string compare_func =    "function __redis__compare_helper(a,b)\n"
                                  "  if a == false then a = '' end\n"
                                  "  if b == false then b = '' end\n"
                                  "  return a<b\n"
                                  "end\n";
    luaL_loadbuffer(lua, compare_func.c_str(), compare_func.length(),
      "@cmp_func_def");
    lua_pcall(lua, 0, 0, 0);
  }

  /* Add a helper function we use for pcall error reporting.
   * Note that when the error is in the C function we want to report the
   * information about the caller, that's what makes sense from the point
   * of view of the user debugging a script. */
  {
    std::string errh_func =       "local dbg = debug\n"
                                  "function __redis__err__handler(err)\n"
                                  "  local i = dbg.getinfo(2,'nSl')\n"
                                  "  if i and i.what == 'C' then\n"
                                  "    i = dbg.getinfo(3,'nSl')\n"
                                  "  end\n"
                                  "  if i then\n"
                                  "    return i.source .. ':' .. i.currentline .. ': ' .. err\n" // NOLINT
                                  "  else\n"
                                  "    return err\n"
                                  "  end\n"
                                  "end\n";
    luaL_loadbuffer(lua, errh_func.c_str(), errh_func.length(),
        "@err_handler_def");
    lua_pcall(lua, 0, 0, 0);
  }

  /* Create the (non connected) client that we use to execute Redis commands
   * inside the Lua interpreter.
   * Note: there is no need to create it again when this function is called
   * by scriptingReset(). */
  // if (server.lua_client == NULL) {
  //  server.lua_client = createClient(-1);
  //  server.lua_client->flags |= CLIENT_LUA;
  //}

  /* Lua beginners often don't use "local", this is likely to introduce
   * subtle bugs in their code. To prevent problems we protect accesses
   * to global variables. */

  scriptingEnableGlobalsProtection(lua);

  pushThisToLua(lua);

  return lua;
}

void LuaState::pushThisToLua(lua_State *lua) {
  uint64_t p = reinterpret_cast<uint64_t>(this);
  lua_pushstring(lua, tendisplus::ultos(p).c_str());
  lua_setglobal(lua, "lua_state");
  // LOG(INFO) << "pushThisToLua:" << p;
}

LuaState* LuaState::getLuaStateFromLua(lua_State *lua) {
  lua_getglobal(lua, "lua_state");
  string v = string(lua_tostring(lua, -1), lua_strlen(lua, -1));
  auto addr = tendisplus::stoul(v);
  if (!addr.ok()) {
    LOG(ERROR) << "getLuaStateFromLua failed.";
    return nullptr;
  }
  LuaState* ls = reinterpret_cast<LuaState*>(addr.value());
  lua_pop(lua, 1);
  // LOG(INFO) << "getLuaStateFromLua:" << addr.value();
  return ls;
}

void LuaState::LuaClose() {
  lua_close(_lua);
}

Expected<std::string> LuaState::luaReplyToRedisReply(lua_State *lua) {
  int t = lua_type(lua, -1);
  string repl;
  switch (t) {
    case LUA_TSTRING:
      // addReplyBulkCBuffer(c,(char*)lua_tostring(lua,-1),lua_strlen(lua,-1));
      // LOG(INFO) << "this is a tstring.";
      repl = Command::fmtBulk(string(lua_tostring(lua, -1),
              lua_strlen(lua, -1)));
      lua_pop(lua, 1);
      return repl;
      break;
    case LUA_TBOOLEAN:
      // addReply(c,lua_toboolean(lua,-1) ? shared.cone : shared.nullbulk);
      // LOG(INFO) << "this is a tbool.";
      repl = lua_toboolean(lua, -1) ? Command::fmtOne() : Command::fmtNull();
      lua_pop(lua, 1);
      return repl;
      break;
    case LUA_TNUMBER:
      // addReplyLongLong(c,(long long)lua_tonumber(lua,-1));
      // LOG(INFO) << "this is a tnumber.";
      repl = Command::fmtLongLong(lua_tonumber(lua, -1));
      lua_pop(lua, 1);
      return repl;
      break;
    case LUA_TTABLE:
      /* We need to check if it is an array, an error, or a status reply.
       * Error are returned as a single element table with 'err' field.
       * Status replies are returned as single element table with 'ok'
       * field. */
      lua_pushstring(lua, "err");
      lua_gettable(lua, -2);
      t = lua_type(lua, -1);
      if (t == LUA_TSTRING) {
        string err = lua_tostring(lua, -1);
        redis_port::strmapchars(err, "\r\n", "  ", 2);
        lua_pop(lua, 2);
        // return Command::fmtBulk("-" + err + "\r\n");
        return "-" + err + "\r\n";
      }

      lua_pop(lua, 1);
      lua_pushstring(lua, "ok");
      lua_gettable(lua, -2);
      t = lua_type(lua, -1);
      if (t == LUA_TSTRING) {
        string ok = lua_tostring(lua, -1);
        redis_port::strmapchars(ok, "\r\n", "  ", 2);
        lua_pop(lua, 2);
        // return Command::fmtBulk("+" + ok + "\r\n");
        return Command::fmtBulk(ok);
      } else {
        // void *replylen = addDeferredMultiBulkLength(c);
        int j = 1, mbulklen = 0;

        lua_pop(lua, 1); /* Discard the 'ok' field value we popped */
        string rsp;
        while (1) {
          lua_pushnumber(lua, j++);
          lua_gettable(lua, -2);
          t = lua_type(lua, -1);
          if (t == LUA_TNIL) {
            lua_pop(lua, 1);
            break;
          }
          rsp += luaReplyToRedisReply(lua).value();
          mbulklen++;
        }
        std::stringstream ss;
        Command::fmtMultiBulkLen(ss, mbulklen);
        lua_pop(lua, 1);
        return ss.str()+rsp;
        // setDeferredMultiBulkLength(c,replylen,mbulklen);
      }
      INVARIANT(false);
      break;
    default:
      // addReply(c,shared.nullbulk);
      lua_pop(lua, 1);
      return Command::fmtNull();
  }
  INVARIANT(false);
  return Command::fmtOK();
}

void LuaState::sha1hex(char *digest, char *script, size_t len) {
  SHA1_CTX ctx;
  unsigned char hash[20];
  char cset[] = "0123456789abcdef";
  int j;

  SHA1Init(&ctx);
  SHA1Update(&ctx, (unsigned char*)script, len);
  SHA1Final(hash, &ctx);

  for (j = 0; j < 20; j++) {
    digest[j*2] = cset[((hash[j]&0xF0)>>4)];
    digest[j*2+1] = cset[(hash[j]&0xF)];
  }
  digest[40] = '\0';
}

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)  // takenliu:for what?

/* This is the Lua script "count" hook that we use to detect scripts timeout. */
void LuaState::luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
  LuaState* ls = getLuaStateFromLua(lua);

  int64_t elapsed = msSinceEpoch() - ls->lua_time_start;
  UNUSED(ar);
  UNUSED(lua);

  /* Set the timeout condition if not already set and the maximum
   * execution time was reached. */
  if (elapsed >= ls->_svr->getParams()->luaTimeLimit && ls->lua_timedout == 0) {
    serverLog(LL_WARNING, "Lua slow script detected: still in execution "
              "after %" PRId64 " milliseconds. You can try "
              "killing the script using the SCRIPT KILL command.", elapsed);
    ls->lua_timedout = 1;
    /* Once the script timeouts we reenter the event loop to permit others
     * to call SCRIPT KILL or SHUTDOWN NOSAVE if needed. For this reason
     * we need to mask the client executing the script from the event loop.
     * If we don't do that the client may disconnect and could no longer be
     * here when the EVAL command will return. */
    // protectClient(ls->_sess);
  }
  // if (ls->lua_timedout) processEventsWhileBlocked();

  if (ls->_scriptMgr->luaKill()) {
    serverLog(LL_WARNING, "Lua script killed by user with SCRIPT KILL.");
    lua_pushstring(lua, "Script killed by user with SCRIPT KILL...");
    lua_error(lua);
  }
  // NOTE(takenliu): stopped need exit eval
  if (ls->_scriptMgr->stopped()) {
    serverLog(LL_WARNING, "server stopped, Lua script need quit.");
    // lua_pushstring(lua,"server stopped, Lua script need quit.");
    lua_error(lua);
  }
}

Expected<std::string> LuaState::evalCommand(Session *sess) {
  return evalGenericCommand(sess, 0);
}

Expected<std::string> LuaState::evalGenericCommand(Session *sess,
      int evalsha) {
  _sess = sess;
  const std::vector<std::string>& args = sess->getArgs();
  int delhook = 0;

  char funcname[43];
  int64_t numkeys;
  int err;

  _rand.redisSrand48(0);

  lua_random_dirty = 0;
  lua_write_dirty = 0;
  int lua_always_replicate_commands = 0;  // TODO(takenliu)
  lua_replicate_commands = lua_always_replicate_commands;
  // has_command_error = false;

  /* Get the number of arguments that are keys */
  Expected<int64_t> eNumkeys = ::tendisplus::stoll(args[2]);
  if (!eNumkeys.ok()) {
    return eNumkeys.status();
  }
  numkeys = eNumkeys.value();
  if (numkeys > static_cast<int>(args.size() - 3)) {
    return {ErrorCodes::ERR_LUA,
      "Number of keys can't be greater than number of args"};
  } else if (numkeys < 0) {
    return {ErrorCodes::ERR_LUA, "Number of keys can't be negative"};
  }
  funcname[0] = 'f';
  funcname[1] = '_';
  if (!evalsha) {
    /* Hash the code if this is an EVAL call */
    sha1hex(funcname + 2, const_cast<char*>(args[1].c_str()),
      args[1].length());
  }
  /* Push the pcall error handler function on the stack. */
  lua_getglobal(_lua, "__redis__err__handler");

  /* Try to lookup the Lua function */
  lua_getglobal(_lua, funcname);
  if (lua_isnil(_lua, -1)) {
    lua_pop(_lua, 1); /* remove the nil from the stack */
    /* Function not defined... let's define it if we have the
     * body of the function. If this is an EVALSHA call we can just
     * return an error. */
    if (evalsha) {
      lua_pop(_lua, 1); /* remove the error handler from the stack. */
      return {ErrorCodes::ERR_LUA,
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"};
    }
    auto ret = luaCreateFunction(_lua, args[1]);
    if (!ret.ok()) {
      lua_pop(_lua, 1); /* remove the error handler from the stack. */
      /* The error is sent to the client by luaCreateFunction()
       * itself when it returns NULL. */
      return {ErrorCodes::ERR_LUA, "luaCreateFunction failed"};;
    }
    /* Now the following is guaranteed to return non nil */
    lua_getglobal(_lua, funcname);
    serverAssert(!lua_isnil(_lua, -1));
  }

  /* Populate the argv and keys table accordingly to the arguments that
   * EVAL received. */
  luaSetGlobalArray(_lua, "KEYS", args, 3, numkeys);
  luaSetGlobalArray(_lua, "ARGV", args, 3+numkeys,
        args.size() - 3 - numkeys);

  updateFakeClient();

  auto guard = MakeGuard([this] {
    _sess = nullptr;
    // NOTE(takenliu): _fakeSess should be reused for better performance
    // _fakeSess = nullptr;
  });

  // lock all keys
  auto server = sess->getServerEntry();
  std::vector<int32_t> keyidx;
  for (uint32_t i = 3; i < 3 + numkeys; ++i) {
    keyidx.push_back(i);
  }
  auto locklist = server->getSegmentMgr()->getAllKeysLocked(
          _fakeSess->getSession(), args, keyidx, mgl::LockMode::LOCK_X);
  if (!locklist.ok()) {
    LOG(ERROR) << "evalGenericCommand getAllKeysLocked failed:"
      << locklist.status().toString();
    lua_pop(_lua, 2); /* remove the Lua function and error handler. */
    return locklist.status();
  }

  lua_time_start = msSinceEpoch();
  if (_svr->getParams()->luaTimeLimit > 0) {
    lua_sethook(_lua, luaMaskCountHook, LUA_MASKCOUNT, 100000);
    delhook = 1;
  }

  /* At this point whether this script was never seen before or if it was
   * already defined, we can call it. We have zero arguments and expect
   * a single return value. */
  err = lua_pcall(_lua, 0, 1, -2);

  if (delhook) lua_sethook(_lua, NULL, 0, 0); /* Disable hook */
  if (lua_timedout) {
    lua_timedout = 0;
    /* Restore the readable handler that was unregistered when the
     * script timeout was detected. */
    // aeCreateFileEvent(server.el,c->fd,AE_READABLE, readQueryFromClient,c);
  }

  // commit all txn
  if (_fakeSess) {
    // if (!has_command_error) {
    //  _fakeSess->getSession()->setInLua(false);
    //  _fakeSess->getSession()->getCtx()->commitAll("lua");
    // } else {
    //  DLOG(INFO) << "has_command_error txns don't commit.";
    // }
  }

  if (err) {
    string errInfo = "Error running script (call to " + string(funcname) + "):"
                 + string(lua_tostring(_lua, -1));
    lua_pop(_lua, 2); /* Consume the Lua reply and remove error handler. */
    return {ErrorCodes::ERR_LUA, errInfo};
  } else {
    /* On success convert the Lua return value into Redis protocol, and
     * send it to * the client. */
    auto ret = luaReplyToRedisReply(_lua); /* Convert and consume the reply. */
    lua_pop(_lua, 1); /* Remove the error handler. */
    return ret;
  }
}

void LuaState::updateFakeClient() {
  if (_fakeSess == nullptr) {
    _fakeSess = std::make_unique<LocalSessionGuard>(_svr.get());
    _fakeSess->getSession()->setInLua(true);
  }
  if (!_fakeSess->getSession()->getCtx()->authed() &&
      _sess->getCtx()->authed()) {
    _fakeSess->getSession()->getCtx()->setAuthed();
  }
  if (_fakeSess->getSession()->getCtx()->getDbId()
    != _sess->getCtx()->getDbId()) {
    _fakeSess->getSession()->getCtx()->setDbId(_sess->getCtx()->getDbId());
  }
}

}  // namespace tendisplus
