import pytest

import quickjs


def test_eval_arithmetic(ctx):
    assert ctx.eval("1 + 2") == 3
    assert ctx.eval("2 ** 10") == 1024
    assert ctx.eval("7 / 2") == 3.5


def test_eval_string(ctx):
    assert ctx.eval('"foo" + "bar"') == "foobar"
    assert ctx.eval('"héllo".length') == 5


def test_eval_boolean_and_null(ctx):
    assert ctx.eval("true") is True
    assert ctx.eval("1 < 2") is True
    assert ctx.eval("null") is None
    assert ctx.eval("undefined") is quickjs.Undefined


def test_eval_persists_state(ctx):
    ctx.eval("var counter = 0;")
    ctx.eval("counter += 5;")
    ctx.eval("counter += 5;")
    assert ctx.eval("counter") == 10


def test_eval_syntax_error_raises(ctx):
    with pytest.raises(quickjs.JSError):
        ctx.eval("this is not valid )(")


def test_eval_runtime_error_has_message(ctx):
    with pytest.raises(quickjs.JSError) as exc:
        ctx.eval('throw new Error("kaboom")')
    assert "kaboom" in str(exc.value)


def test_module_eval(ctx):
    # module code runs in strict mode and supports top-level declarations
    result = ctx.eval("export const x = 42;", module=True)
    assert result is None or hasattr(result, "tag")


def test_top_level_eval_helper():
    assert quickjs.eval("3 * 14") == 42
