local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(g)
    g.server = server:new()
    g.server:start()
end)

g.after_all(function(g)
    g.server:drop()
end)

g.after_each(function(g)
    g.server:exec(function()
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

g.test_invalid_space_format = function(cg)
    cg.server:exec(function()
        --
        -- Test box.schema.space_create().
        --
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "scale is not specified",
        }, box.schema.create_space, 'test', {
            format = {{'a', 'unsigned'}, {'b', 'decimal32'}},
        })
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "scale makes sense only for fixed-point decimals",
        }, box.schema.create_space, 'test', {
            format = {{'a', 'unsigned'}, {'b', 'unsigned', scale = 10}},
        })
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "'scale' must be integer",
        }, box.schema.create_space, 'test', {
            format = {{'a', 'unsigned'}, {'b', 'decimal32', scale = 'string'}},
        })
        --
        -- Test box.space._space:insert.
        --
        local empty_map = setmetatable({}, {__serialize = 'map'})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "scale is not specified",
        }, box.space._space.insert, box.space._space, {
            512, 1, 'test', 'memtx', 2, empty_map,
            {{name = 'a', type = 'unsigned'}, {name = 'b', type = 'decimal32'}},
        })
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "scale makes sense only for fixed-point decimals",
        }, box.space._space.insert, box.space._space, {
            512, 1, 'test', 'memtx', 2, empty_map,
            {
                {name = 'a', type = 'unsigned'},
                {name = 'b', type = 'unsigned', scale = 10},
            },
        })
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_SPACE_FORMAT,
            fieldno = 2,
            details = "'scale' must be integer",
        }, box.space._space.insert, box.space._space, {
            512, 1, 'test', 'memtx', 2, empty_map,
            {
                {name = 'a', type = 'unsigned'},
                {name = 'b', type = 'decimal32', scale = 'string'},
            },
        })
    end)
end

g.test_invalid_index_format = function(cg)
    cg.server:exec(function()
        local s = box.schema.create_space('test', {format = {
            {'a', 'unsigned'}, {'b', 'decimal32', scale = 2},
        }})
        -- XXX Unfortunately difference in scale that causes
        -- error is not visible.
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FORMAT_MISMATCH_INDEX_PART,
            field = "2 (b)",
        }, s.create_index, s, 'pk', {parts = {2, scale = 3}})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_INDEX_PARTS,
            partno = 1,
            details = 'scale makes sense only for fixed-point decimals'
        }, s.create_index, s, 'pk', {parts = {1, scale = 3}})
        s:drop()

        s = box.schema.create_space('test')
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_INDEX_PARTS,
            partno = 1,
            details = 'scale is not specified',
        }, s.create_index, s, 'pk', {
            parts = {2, 'decimal32', sort_order = 'desc'}
        })
        -- A bit different from above. Here we have 166 format.
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.WRONG_INDEX_PARTS,
            partno = 1,
            details = 'scale is not specified',
        }, s.create_index, s, 'pk', {parts = {2, 'decimal32'}})

        -- Now check if first index is added and then format is set.
        s:create_index('pk', {parts = {2, 'decimal32', scale = 2}})
        -- XXX Unfortunately difference in scale that causes
        -- error is not visible.
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FORMAT_MISMATCH_INDEX_PART,
            field = "2 (b)",
        }, s.format, s, {{'a', 'unsigned'}, {'b', 'decimal32', scale = 3}})
    end)
end

g.test_no_format = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local s = box.schema.create_space('test')
        s:create_index('pk', {parts = {1, 'decimal32', scale = 2}})
        s:insert({decimal.new(1.01)})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            value = decimal.new(1.001),
        }, s.insert, s, {decimal.new(1.001)})
    end)
end

g.test_index_key_validation = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local s = box.schema.create_space('test', {format = {
            {'a', 'decimal32', scale = 2}, {'b', 'unsigned'},
        }})
        s:create_index('pk')
        s:insert({decimal.new(1.01), 1})
        s:insert({decimal.new(2.02), 2})
        t.assert_equals(s:get({decimal.new(2.02)}), {decimal.new(2.02), 2})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.KEY_PART_IRREPRESENTABLE_VALUE,
            partno = 0,
            value = decimal.new(2.002),
        }, s.get, s, decimal.new(2.002))
    end)
end

g.test_precision = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local check = function(decimal_type, precision, scale)
            local s = box.schema.create_space('test', {format = {
                {'a', 'unsigned'}, {'b', decimal_type, scale = scale},
            }})
            s:create_index('pk')
            local max = decimal.new(string.rep('9', precision) .. 'e' .. -scale)
            local min = -max;
            s:insert({1, max})
            s:insert({2, min})
            t.assert_equals(s:get({1}), {1, max})
            t.assert_equals(s:get({2}), {2, min})
            local max_next = decimal.new('1' .. string.rep('0', precision) ..
                                         'e' .. -scale)
            t.assert_error_covers({
                type = 'ClientError',
                code = box.error.FIELD_IRREPRESENTABLE_VALUE,
                field = '2 (b)',
                value = max_next,
            }, s.insert, s, {3, max_next})
            local min_prev = -max_next
            t.assert_error_covers({
                type = 'ClientError',
                code = box.error.FIELD_IRREPRESENTABLE_VALUE,
                field = '2 (b)',
                value = min_prev,
            }, s.insert, s, {3, min_prev})
            s:drop()
        end
        local scales = {-500, -20, -10, -9, -8, 0, 8, 9, 10, 20, 500}
        for _, scale in ipairs(scales) do
            check('decimal32', 9, scale)
            check('decimal64', 18, scale)
            check('decimal128', 38, scale)
            check('decimal256', 76, scale)
        end
    end)
end

g.test_incompatible_types = function(cg)
    cg.server:exec(function()
        local uuid = require('uuid')
        local s = box.schema.create_space('test', {format = {
            {'a', 'unsigned'}, {'b', 'decimal32', scale = 5},
        }})
        s:create_index('pk')
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_TYPE,
        }, s.insert, s, {1, 1})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_TYPE,
        }, s.insert, s, {1, 'xxx'})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_TYPE,
        }, s.insert, s, {1, uuid.new()})
    end)
end

g.test_altering_format = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local s = box.schema.create_space('test', {format = {
            {'a', 'unsigned'}, {'b', 'decimal32', scale = 1},
        }})
        s:create_index('pk')
        s:insert({1, decimal.new(0.1)})
        -- Can not change format due to scale does not fit.
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            field = '2 (b)',
            value = decimal.new(0.1),
        }, s.format, s, {{'a', 'unsigned'}, {'b', 'decimal32', scale = 0}})
        s:format({{'a', 'unsigned'}, {'b', 'decimal64', scale = 1}})
        s:format({{'a', 'unsigned'}, {'b', 'decimal32', scale = 1}})
        s:format({{'a', 'unsigned'}, {'b', 'decimal64', scale = 1}})
        s:insert({2, decimal.new(10^10)})
        -- Can not change format due to precision does not fit.
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            field = '2 (b)',
            value = decimal.new(10^10),
        }, s.format, s, {{'a', 'unsigned'}, {'b', 'decimal32', scale = 1}})
    end)
end

g.test_indexed_field = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local check = function(decimal_type)
            local s = box.schema.create_space('test', {format = {
                {'a', 'unsigned'}, {'b', decimal_type, scale = -1},
            }})
            s:create_index('pk')
            local sk = s:create_index('sk', {parts = {'b'}})
            s:insert({1, decimal.new(110)})
            s:insert({2, decimal.new(100)})
            t.assert_equals(sk:select(), {
                {2, decimal.new(100)}, {1, decimal.new(110)}
            })
            s:drop()
        end
        check('decimal32')
        check('decimal64')
        check('decimal128')
        check('decimal256')
    end)
end

g.test_indexed_field_altering = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local s = box.schema.create_space('test', {format = {
            {'a', 'unsigned'}, {'b', 'decimal32', scale = -1},
        }})
        s:create_index('pk')
        local sk = s:create_index('sk', {parts = {'b'}})
        s:insert({1, decimal.new(110)})
        s:insert({2, decimal.new(100)})
        t.assert_equals(sk:select(), {
            {2, decimal.new(100)}, {1, decimal.new(110)}
        })
        sk:alter({parts = {{'b', 'decimal64'}}})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            field = '2 (b)',
            value = decimal.new(10^11),
        }, s.insert, s, {3, decimal.new(10^11)})
        t.assert_equals(sk:select(), {
            {2, decimal.new(100)}, {1, decimal.new(110)}
        })
    end)
end

g.test_update = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        local s = box.schema.create_space('test', {format = {
            {'a', 'unsigned'}, {'b', 'decimal32', scale = 0},
        }})
        s:create_index('pk')
        s:insert({1, decimal.new(999999999)})
        s:insert({2, decimal.new(-999999999)})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            value = decimal.new(1000000000),
        }, s.update, s, {1}, {{'+', 'b', 1}})
        t.assert_error_covers({
            type = 'ClientError',
            code = box.error.FIELD_IRREPRESENTABLE_VALUE,
            value = decimal.new(-1000000000),
        }, s.update, s, {2}, {{'-', 'b', 1}})
        s:update({1}, {{'-', 'b', 1}})
        s:update({2}, {{'+', 'b', 1}})
        t.assert_equals(s:select(), {
            {1, decimal.new(999999998)}, {2, decimal.new(-999999998)},
        })
    end)
end

g.test_index_introspection = function(cg)
    cg.server:exec(function()
        local s = box.schema.create_space('test', {
            engine = 'vinyl',
            format = {{'a', 'unsigned'}, {'b', 'decimal32', scale = 7}}
        })
        s:create_index('pk', {parts = {'b'}})
        t.assert_equals(s.index.pk.parts[1].scale, 7)
    end)
end

-- Vinyl uses key def parts decoding and encoding which has extra
-- paths in case of fixed point decimals.
g.test_key_part_encode_decode = function(cg)
    cg.server:exec(function()
        local decimal = require('decimal')
        -- Test encoding positive scale.
        local s = box.schema.create_space('test', {
            engine = 'vinyl',
            format = {{'a', 'unsigned'}, {'b', 'decimal32', scale = 2}}
        })
        s:create_index('pk', {parts = {'b'}})
        s:insert({1, decimal.new(1.01)})
        s:insert({2, decimal.new(2.02)})
        box.snapshot()
        s:drop()
        -- Test encoding negative scale.
        local s = box.schema.create_space('test', {
            engine = 'vinyl',
            format = {{'a', 'unsigned'}, {'b', 'decimal32', scale = -2}}
        })
        s:create_index('pk', {parts = {'b'}})
        s:insert({1, decimal.new(100)})
        s:insert({2, decimal.new(200)})
        box.snapshot()
    end)
end
