import pytest

import quickjs


def test_int_roundtrip(ctx):
    ctx.set("v", 123)
    assert ctx.eval("v") == 123


def test_large_int_becomes_bigint(ctx):
    big = 2**70
    ctx.set("v", big)
    assert ctx.eval("typeof v") == "bigint"
    assert ctx.eval("v") == big


def test_float_roundtrip(ctx):
    ctx.set("v", 3.5)
    assert ctx.eval("v") == 3.5


def test_string_roundtrip(ctx):
    ctx.set("v", "héllo \U0001f600")
    assert ctx.eval("v") == "héllo \U0001f600"


def test_bool_and_none(ctx):
    ctx.set("t", True)
    ctx.set("n", None)
    assert ctx.eval("t") is True
    assert ctx.eval("n") is None


def test_list_to_array(ctx):
    ctx.set("v", [1, 2, 3])
    assert ctx.eval("Array.isArray(v)") is True
    assert ctx.eval("v.length") == 3
    assert ctx.eval("v[1]") == 2


def test_dict_to_object(ctx):
    ctx.set("v", {"a": 1, "b": 2})
    assert ctx.eval("v.a") == 1
    assert ctx.eval("v.b") == 2


def test_deep_conversion_array(ctx):
    assert ctx.eval("[1, [2, 3], 4]").to_python() == [1, [2, 3], 4]


def test_deep_conversion_object(ctx):
    assert ctx.eval("({a: 1, b: {c: 2}})").to_python() == {"a": 1, "b": {"c": 2}}


def test_shallow_conversion_keeps_value(ctx):
    shallow = ctx.eval("[1, 2]").to_python(deep=False)
    assert isinstance(shallow, quickjs.Value)


def test_circular_reference_raises(ctx):
    obj = ctx.eval("var o = {}; o.self = o; o")
    with pytest.raises(ValueError):
        obj.to_python()


def test_unconvertible_type_raises(ctx):
    with pytest.raises(TypeError):
        ctx.set("v", object())
