import pytest

import quickjs


def test_simple_callback(ctx):
    ctx.set("add", lambda a, b: a + b)
    assert ctx.eval("add(2, 3)") == 5


def test_callback_returns_string(ctx):
    ctx.set("greet", lambda name: f"hi {name}")
    assert ctx.eval('greet("world")') == "hi world"


def test_callback_with_collections(ctx):
    ctx.set("make", lambda: {"items": [1, 2, 3]})
    assert ctx.eval("make().items[2]") == 3


def test_callback_exception_propagates(ctx):
    def boom():
        raise RuntimeError("python side failure")

    ctx.set("boom", boom)
    with pytest.raises(quickjs.JSError) as exc:
        ctx.eval("boom()")
    assert "python side failure" in str(exc.value)


def test_new_function_explicit(ctx):
    fn = ctx.new_function(lambda x: x * 10, name="times10")
    assert fn.is_function
    assert fn(4) == 40


def test_callback_invoked_from_js_higher_order(ctx):
    # Array.prototype.map invokes the callback with (element, index, array)
    ctx.set("double", lambda x, *rest: x * 2)
    assert ctx.eval("[1, 2, 3].map(double)").to_python() == [2, 4, 6]


def test_python_function_passed_as_argument(ctx):
    apply_twice = ctx.eval("(f, x) => f(f(x))")
    assert apply_twice(lambda v: v + 1, 10) == 12
