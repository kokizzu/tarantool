/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "box/lua/error.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include <fiber.h>
#include <errinj.h>

#include "lua/utils.h"
#include "lua/msgpack.h"
#include "box/error.h"
#include "mpstream/mpstream.h"
#include "box/lua/tuple.h"

/**
 * Set payload field of the `error' for the key `key` to value at stack index
 * `index`. If the field with given key existed before, it is overwritten.
 * The Lua value is encoded to MsgPack.
 */
static void
luaT_error_payload_set(lua_State *L, struct error *error, const char *key,
		       int index)
{
	struct region *gc = &fiber()->gc;
	size_t used = region_used(gc);
	struct mpstream stream;
	mpstream_init(&stream, gc, region_reserve_cb, region_alloc_cb,
		      luamp_error, L);
	if (luamp_encode(L, luaL_msgpack_default, &stream, index) != 0) {
		region_truncate(gc, used);
		return;
	}
	mpstream_flush(&stream);
	size_t size = region_used(gc) - used;
	const char *mp_value = (const char *)xregion_join(gc, size);
	error_set_mp(error, key, mp_value, size);
	region_truncate(gc, used);
}

/** Return whether key `key` is built-in field. */
static bool
luaT_error_is_builtin_field(const char *key)
{
	static const char *const ignore_keys[] = {
		"type", "message", "trace", "prev", "base_type",
		"code", "reason", "errno", "custom_type"
	};
	for (size_t i = 0; i < lengthof(ignore_keys); i++) {
		if (strcmp(key, ignore_keys[i]) == 0)
			return true;
	}
	return false;
}

/**
 * In case the error is constructed from a table, retrieves the reason.
 *
 * @param L Lua state
 * @param index table index on Lua stack
 * @param code error code
 * @param custom_type custom type name or NULL if not specified (ClientError)
 *
 * @return reason
 * @retval "" failed to retrieve reason
 */
static const char *
error_create_table_case_get_reason(lua_State *L, int index,
				   uint32_t code, const char *custom_type)
{
	lua_rawgeti(L, index, 1);
	const char *reason = lua_tostring(L, -1);
	if (reason != NULL)
		return reason;
	lua_getfield(L, index, "message");
	reason = lua_tostring(L, -1);
	if (reason != NULL)
		return reason;
	lua_getfield(L, index, "reason");
	reason = lua_tostring(L, -1);
	if (reason != NULL)
		return reason;
	/* If ClientError has no reason - take description by code. */
	if (custom_type == NULL)
		reason = tnt_errcode_desc(code);
	return reason != NULL ? reason : "";
}

/**
 * Parse Lua arguments (they can come as single table or as
 * separate members) and construct struct error with given values.
 *
 * Can be used either the 'code' (numeric) for create a ClientError
 * error with corresponding message (the format is predefined)
 * and type or the 'type' (string) for create a CustomError error
 * with custom type and desired message.
 *
 *     box.error(code, reason args[, level])
 *     box.error({code = num, reason = string, ...}[, level])
 *     box.error(type, reason format string, reason args)
 *     box.error({type = string, code = num, reason = string, ...}[, level])
 *
 * In case one of arguments is missing its corresponding field
 * in struct error is filled with default value.
 *
 * The optional 'level' argument has the same meaning as in the built-in Lua
 * function 'error' - it specifies how to get the error location (file, line),
 * which is stored in the 'trace' payload field. With level 1 (the default),
 * the error location is where 'box.error' was called. Level 2 points the error
 * to where the function that called 'box.error' was called, and so on. Passing
 * level 0 avoids addition of location information to the error payload.
 */
static struct error *
luaT_error_create(lua_State *L, int top_base)
{
	uint32_t code = 0;
	const char *custom_type = NULL;
	const char *reason = NULL;
	int level = 1;
	struct error *prev = NULL;
	int top = lua_gettop(L);
	int top_type = lua_type(L, top_base);
	const struct errcode_record *record = NULL;
	if (top >= top_base && (top_type == LUA_TNUMBER ||
				top_type == LUA_TSTRING)) {
		/* Shift of the "reason args". */
		int shift = 1;
		if (top_type == LUA_TNUMBER) {
			code = lua_tonumber(L, top_base);
			record = tnt_errcode_record(code);
			reason = record->errdesc;

			int level_pos = top_base + record->errfields_count + 1;
			if (!lua_isnoneornil(L, level_pos)) {
				if (lua_type(L, level_pos) != LUA_TNUMBER)
					return NULL;
				level = lua_tointeger(L, level_pos);
			}
		} else {
			custom_type = lua_tostring(L, top_base);
			/*
			 * For the CustomError, the message format
			 * must be set via a function argument.
			 */
			if (lua_type(L, top_base + 1) != LUA_TSTRING)
				return NULL;
			reason = lua_tostring(L, top_base + 1);
			shift = 2;
		}
		if (top > top_base) {
			/* Call string.format(reason, ...) to format message */
			lua_getglobal(L, "string");
			if (lua_isnil(L, -1))
				goto raise;
			lua_getfield(L, -1, "format");
			if (lua_isnil(L, -1))
				goto raise;
			lua_pushstring(L, reason);
			int nargs = 1;
			for (int i = top_base + shift; i <= top; ++i, ++nargs)
				lua_pushvalue(L, i);
			lua_call(L, nargs, 1);
			reason = lua_tostring(L, -1);
		} else if (strchr(reason, '%') != NULL) {
			/* Missing arguments to format string */
			return NULL;
		}
	} else if (top >= top_base && top_type == LUA_TTABLE) {
		if (top > top_base + 1)
			return NULL;
		if (!lua_isnoneornil(L, top_base + 1)) {
			if (lua_type(L, top_base + 1) != LUA_TNUMBER)
				return NULL;
			level = lua_tointeger(L, top_base + 1);
		}
		lua_getfield(L, top_base, "code");
		if (!lua_isnil(L, -1))
			code = lua_tonumber(L, -1);
		lua_getfield(L, top_base, "type");
		if (!lua_isnil(L, -1))
			custom_type = lua_tostring(L, -1);
		reason = error_create_table_case_get_reason(L, top_base,
							    code, custom_type);
		lua_getfield(L, top_base, "prev");
		if (!lua_isnil(L, -1)) {
			prev = luaL_iserror(L, -1);
			if (prev == NULL) {
				diag_set(IllegalParams,
					 "Invalid argument 'prev' (error "
					 "expected, got %s)",
					 lua_typename(L, lua_type(L, -1)));
				luaT_error(L);
			}
		}
	} else {
		return NULL;
	}

raise:
	struct error *error;
	error = box_error_new(NULL, 0, code, custom_type, "%s", reason);
	luaT_error_set_trace(L, level, error);
	/*
	 * Set the previous error, if it was specified.
	 */
	if (prev != NULL) {
		/* Cycle is not possible for the newly created error. */
		VERIFY(error_set_prev(error, prev) == 0);
	}
	/*
	 * Add custom payload fields to the `error' if any.
	 */
	if (top_type == LUA_TTABLE) {
		/*
		 * Table is in the stack at index `top', push the first key for
		 * iteration over the table.
		 */
		lua_pushnil(L);
		while (lua_next(L, top_base) != 0) {
			int key_type = lua_type(L, -2);
			if (key_type == LUA_TSTRING) {
				const char *key = lua_tostring(L, -2);
				/* Ignore built-in error fields. */
				if (!luaT_error_is_builtin_field(key))
					luaT_error_payload_set(L, error, key,
							       -1);
			}
			/* Remove the value, keep the key for next iteration. */
			lua_pop(L, 1);
		}
		/* Remove the table. */
		lua_pop(L, 1);
	}
	if (record != NULL) {
		int argidx = 0;
		for (int i = top_base + 1;
		     i <= top && argidx < record->errfields_count;
		     i++, argidx++) {
			const char *name = record->errfields[argidx].name;
			if (name[0] != '\0')
				luaT_error_payload_set(L, error, name, i);
		}
		assert(strncmp("ER_", record->errstr, 3) == 0);
		error_set_str(error, "name", record->errstr + 3);
	}
	return error;
}

static int
luaT_error_call(lua_State *L)
{
	int top = lua_gettop(L);
	if (top <= 1) {
		/* Re-throw saved exceptions if any. */
		if (box_error_last())
			return luaT_error(L);
		return 0;
	}
	struct error *e = luaL_iserror(L, 2);
	if (e != NULL) {
		if (top > 3)
			goto bad_arg;
		/* Update the error location if the level is specified. */
		if (!lua_isnoneornil(L, 3)) {
			if (lua_type(L, 3) != LUA_TNUMBER)
				goto bad_arg;
			int level = lua_tointeger(L, 3);
			luaT_error_set_trace(L, level, e);
		}
	} else {
		e = luaT_error_create(L, 2);
		if (e == NULL)
			goto bad_arg;
	}
	diag_set_error(&fiber()->diag, e);
	return luaT_error_at(L, 0);
bad_arg:
	diag_set(IllegalParams, "box.error(): bad arguments");
	return luaT_error(L);
}

static int
luaT_error_last(lua_State *L)
{
	if (lua_gettop(L) >= 1) {
		diag_set(IllegalParams, "box.error.last(): bad arguments");
		luaT_error(L);
	}

	struct error *e = box_error_last();
	if (e == NULL) {
		lua_pushnil(L);
		return 1;
	}

	luaT_pusherror(L, e);
	return 1;
}

static int
luaT_error_new(lua_State *L)
{
	struct error *e = luaT_error_create(L, 1);
	if (e == NULL) {
		diag_set(IllegalParams, "box.error.new(): bad arguments");
		return luaT_error(L);
	}
	lua_settop(L, 0);
	luaT_pusherror(L, e);
	return 1;
}

static int
luaT_error_clear(lua_State *L)
{
	if (lua_gettop(L) >= 1) {
		diag_set(IllegalParams, "box.error.clear(): bad arguments");
		luaT_error(L);
	}

	box_error_clear();
	return 0;
}

static int
luaT_error_set(struct lua_State *L)
{
	if (lua_gettop(L) == 0) {
		diag_set(IllegalParams, "Usage: box.error.set(error)");
		return luaT_error(L);
	}
	struct error *e = luaT_checkerror(L, 1);
	diag_set_error(&fiber()->diag, e);
	return 0;
}

/** Return whether first argument is box error. */
static int
luaT_error_is(struct lua_State *L)
{
	lua_pushboolean(L, lua_gettop(L) >= 1 && luaL_iserror(L, 1) != NULL);
	return 1;
}

static int
lbox_errinj_set(struct lua_State *L)
{
	char *name = (char *)luaT_checkstring(L, 1);
	struct errinj *errinj;
	errinj = errinj_by_name(name);
	if (errinj == NULL) {
		say_error("%s", name);
		lua_pushfstring(L, "error: can't find error injection '%s'", name);
		return 1;
	}
	switch (errinj->type) {
	case ERRINJ_BOOL:
		errinj->bparam = lua_toboolean(L, 2);
		say_info("%s = %s", name, errinj->bparam ? "true" : "false");
		break;
	case ERRINJ_INT:
		errinj->iparam = luaT_checkint64(L, 2);
		say_info("%s = %lld", name, (long long)errinj->iparam);
		break;
	case ERRINJ_DOUBLE:
		errinj->dparam = lua_tonumber(L, 2);
		say_info("%s = %g", name, errinj->dparam);
		break;
	default:
		lua_pushfstring(L, "error: unknown injection type '%s'", name);
		return 1;
	}

	lua_pushstring(L, "ok");
	return 1;
}

static int
lbox_errinj_push_value(struct lua_State *L, const struct errinj *e)
{
	switch (e->type) {
	case ERRINJ_BOOL:
		lua_pushboolean(L, e->bparam);
		return 1;
	case ERRINJ_INT:
		luaL_pushint64(L, e->iparam);
		return 1;
	case ERRINJ_DOUBLE:
		lua_pushnumber(L, e->dparam);
		return 1;
	default:
		unreachable();
		return 0;
	}
}

static int
lbox_errinj_get(struct lua_State *L)
{
	char *name = (char *)luaT_checkstring(L, 1);
	struct errinj *e = errinj_by_name(name);
	if (e != NULL)
		return lbox_errinj_push_value(L, e);
	lua_pushfstring(L, "error: can't find error injection '%s'", name);
	return 1;
}

static inline int
lbox_errinj_cb(struct errinj *e, void *cb_ctx)
{
	struct lua_State *L = (struct lua_State*)cb_ctx;
	lua_pushstring(L, e->name);
	lua_newtable(L);
	lua_pushstring(L, "state");
	lbox_errinj_push_value(L, e);
	lua_settable(L, -3);
	lua_settable(L, -3);
	return 0;
}

static int
lbox_errinj_info(struct lua_State *L)
{
	lua_newtable(L);
	errinj_foreach(lbox_errinj_cb, L);
	return 1;
}

void
box_lua_error_init(struct lua_State *L) {
	luaL_findtable(L, LUA_GLOBALSINDEX, "box.error", 0);
	for (int i = 0; i < box_error_code_MAX; i++) {
		const char *name = box_error_codes[i].errstr;
		/* Gap is reserved or deprecated error code. */
		if (name == NULL)
			continue;
		assert(strncmp(name, "ER_", 3) == 0);
		lua_pushnumber(L, i);
		/* cut ER_ prefix from constant */
		lua_setfield(L, -2, name + 3);
	}
	lua_newtable(L);
	{
		lua_pushcfunction(L, luaT_error_call);
		lua_setfield(L, -2, "__call");

		lua_newtable(L);
		{
			lua_pushcfunction(L, luaT_error_last);
			lua_setfield(L, -2, "last");
		}
		{
			lua_pushcfunction(L, luaT_error_clear);
			lua_setfield(L, -2, "clear");
		}
		{
			lua_pushcfunction(L, luaT_error_new);
			lua_setfield(L, -2, "new");
		}
		{
			lua_pushcfunction(L, luaT_error_set);
			lua_setfield(L, -2, "set");
		}
		{
			lua_pushcfunction(L, luaT_error_is);
			lua_setfield(L, -2, "is");
		}
		lua_setfield(L, -2, "__index");
	}
	lua_setmetatable(L, -2);

	lua_pop(L, 1);

	static const struct luaL_Reg errinjlib[] = {
		{"info", lbox_errinj_info},
		{"set", lbox_errinj_set},
		{"get", lbox_errinj_get},
		{NULL, NULL}
	};
	luaL_findtable(L, LUA_GLOBALSINDEX, "box.error.injection", 0);
	luaL_setfuncs(L, errinjlib, 0);
	lua_pop(L, 1);
}
