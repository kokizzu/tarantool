local t = require('luatest')
local treegen = require('luatest.treegen')
local justrun = require('luatest.justrun')

local g = t.group()

-- Core idea: return something that differs from the corresponding
-- built-in module.
--
-- Print `...` and `arg` to ensure that they have expected
-- value.
local OVERRIDE_SCRIPT_TEMPLATE = [[
print(require('json').encode({
    ['script'] = '<filename>',
    ['...'] = {...},
    ['arg[-1]'] = arg[-1],
    ['arg[0]'] = arg[0],
    ['arg[]'] = setmetatable(arg, {__serialize = 'seq'}),
}))
return {whoami = 'override.<module_name>'}
]]

-- Print a result of the require call.
local MAIN_SCRIPT_TEMPLATE = [[
local json = require('json').new()
json.cfg({encode_use_tostring = true})
print(json.encode({
    ['script'] = '<filename>',
    ['<module_name>'] = require('<module_name>'),
}))
]]

g.before_all(function()
    treegen.add_template('^override/.*%.lua$', OVERRIDE_SCRIPT_TEMPLATE)
    treegen.add_template('^main%.lua$', MAIN_SCRIPT_TEMPLATE)
end)

-- Test oracle.
--
-- Verifies that the override module is actually returned by the
-- require call in main.lua.
--
-- Also holds `...` (without 'override.') and `arg` (the same as
-- in the main script).
local function expected_output(module_name)
    local module_name_as_path = table.concat(module_name:split('.'), '/')
    local override_filename = ('override/%s.lua'):format(module_name_as_path)

    local res = {
        {
            ['script'] = override_filename,
            ['...'] = {module_name},
            ['arg[-1]'] = arg[-1],
            ['arg[0]'] = 'main.lua',
            ['arg[]'] = {},
        },
        {
            ['script'] = 'main.lua',
            [module_name] = {
                whoami = ('override.%s'):format(module_name)
            },
        },
    }

    return {
        exit_code = 0,
        stdout = res,
    }
end

-- A couple of test cases with overriding built-in modules.
--
-- In a general case it is not a trivial task to correctly
-- override a built-in module. However, there are modules that
-- could be overridden with an arbitrary table and it will pass
-- tarantool's initialization successfully.
--
-- We have no guarantee that any module could be overridden. It is
-- more like 'it is generally possible'.
--
-- The list is collected from loaders.builtin and package.loaded
-- keys. Many modules are excluded deliberately:
--
-- - json -- it is used in the test itself
-- - bit, coroutine, debug, ffi, io, jit, jit.*, math, os,
--   package, string, table -- LuaJIT modules
-- - misc -- tarantool's module implemented as part of LuaJIT.
-- - *.lib, internal.*, utils.* and so on -- tarantool internal
--   modules
-- - memprof.*, misc.*, sysprof.*, table.*, timezones -- unclear
--   whether they're public
-- - box, buffer, decimal, errno, fiber, fio, log, merger,
--   msgpackffi, strict, tarantool, yaml, fun -- used during
--   tarantool's initialization in a way that doesn't allow to
--   replace them with an arbitrary table
-- - msgpack -- box.NULL is used in config/instance_config.lua
--   at initialization of the module. If msgpack.NULL is nil, then
--   access to box.NULL leads to the 'Please call box.cfg{} first'
--   error.
local override_cases = {
    'clock',
    'console',
    'crypto',
    'csv',
    'datetime',
    'digest',
    'error',
    'help',
    'http.client',
    'iconv',
    'key_def',
    'luadebug',
    'memprof',
    'net.box',
    'pickle',
    'popen',
    'pwd',
    'socket',
    'swim',
    'sysprof',
    'tap',
    'title',
    'uri',
    'utf8',
    'uuid',
    'xlog',
}

-- Generate a workdir (override/foo.lua and main.lua), run
-- tarantool, check output.
for _, module_name in ipairs(override_cases) do
    local module_name_as_path = table.concat(module_name:split('.'), '/')
    local override_filename = ('override/%s.lua'):format(module_name_as_path)
    local module_name_as_snake = table.concat(module_name:split('.'), '_')
    local case_name = ('test_override_%s'):format(module_name_as_snake)

    g[case_name] = function()
        local scripts = {override_filename, 'main.lua'}
        local replacements = {module_name = module_name}
        local dir = treegen.prepare_directory(scripts, replacements)
        local res = justrun.tarantool(dir, {}, {'main.lua'})
        local exp = expected_output(module_name)
        t.assert_equals(res, exp)
    end
end

local function parse_boolean(str, default)
    if str:lower() == 'false' or str == '0' then
        return false
    end
    if str:lower() == 'true' or str == '1' then
        return true
    end
    return default
end

-- Verify that TT_OVERRIDE_BUILTIN=false and TT_OVERRIDE_BUILTIN=0
-- disable the builtin modules overriding functionality.
for _, envvar in ipairs({'false', 'true', '0', '1', '', 'X'}) do
    local case_slug = envvar == '' and 'empty' or envvar:lower()
    local case_name = ('test_override_onoff_%s'):format(case_slug)
    g[case_name] = function()
        local scripts = {'override/socket.lua', 'main.lua'}
        local replacements = {module_name = 'socket'}
        local dir = treegen.prepare_directory(scripts, replacements)
        local env = {['TT_OVERRIDE_BUILTIN'] = envvar}
        local res = justrun.tarantool(dir, env, {'main.lua'})
        if parse_boolean(envvar, true) then
            local exp = expected_output('socket')
            t.assert_equals(res, exp)
        else
            t.assert_equals(#res.stdout, 1)
            local socket = res.stdout[1].socket
            t.assert_is_not(socket.tcp_connect, nil)
        end
    end
end
