/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2022, Tarantool AUTHORS, please see AUTHORS file.
 */
#include <string.h>

/*
 * This file is supposed to be autogenerated in a future.
 *
 * Now it contains several internal symbols we want to offer
 * outside of the module API: no guarantees are provided
 * regarding them.
 *
 * fiber_channel_*() and ipc_value_*() symbols are used in
 * the Rust module [1], because the symbols were exported in
 * Tarantool 2.8.
 *
 * [1]: https://github.com/picodata/tarantool-module
 *
 * fiber_lua_state() is used in [2] to eliminate dependency from
 * fiber.h. See gh-8025.
 *
 * [2]: test/box-tap/check_merge_source.c
 */

extern void
box_lua_find(void);
extern void
fiber_channel_close(void);
extern void
fiber_channel_create(void);
extern void
fiber_channel_delete(void);
extern void
fiber_channel_destroy(void);
extern void
fiber_channel_get_msg_timeout(void);
extern void
fiber_channel_get_timeout(void);
extern void
fiber_channel_has_readers(void);
extern void
fiber_channel_has_writers(void);
extern void
fiber_channel_new(void);
extern void
fiber_channel_put_msg_timeout(void);
extern void
fiber_channel_put_timeout(void);
extern void
ipc_value_delete(void);
extern void
ipc_value_new(void);
extern void
fiber_lua_state(void);

/** Symbol definition. */
struct symbol_def {
	/** Name of the symbol. */
	const char *name;
	/** Address of the symbol. */
	void *addr;
};

static struct symbol_def symbols[] = {
	{"box_lua_find", box_lua_find},
	{"fiber_channel_close", fiber_channel_close},
	{"fiber_channel_create", fiber_channel_create},
	{"fiber_channel_delete", fiber_channel_delete},
	{"fiber_channel_destroy", fiber_channel_destroy},
	{"fiber_channel_get_msg_timeout", fiber_channel_get_msg_timeout},
	{"fiber_channel_get_timeout", fiber_channel_get_timeout},
	{"fiber_channel_has_readers", fiber_channel_has_readers},
	{"fiber_channel_has_writers", fiber_channel_has_writers},
	{"fiber_channel_new", fiber_channel_new},
	{"fiber_channel_put_msg_timeout", fiber_channel_put_msg_timeout},
	{"fiber_channel_put_timeout", fiber_channel_put_timeout},
	{"ipc_value_delete", ipc_value_delete},
	{"ipc_value_new", ipc_value_new},
	{"fiber_lua_state", fiber_lua_state},
	{NULL, NULL}
};

void *
tnt_internal_symbol(const char *name)
{
	struct symbol_def *def = &symbols[0];
	while (def->name != NULL) {
		if (strcmp(name, def->name) == 0) {
			return def->addr;
		}
		++def;
	}
	return NULL;
}
