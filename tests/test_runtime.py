import pytest

import quickjs


def test_create_runtime_and_context():
    rt = quickjs.Runtime()
    ctx = rt.new_context()
    assert ctx.eval("1 + 1") == 2


def test_memory_limit_enforced():
    rt = quickjs.Runtime()
    rt.set_memory_limit(128 * 1024)
    ctx = rt.new_context()
    with pytest.raises(quickjs.JSError):
        ctx.eval("var a = []; while (true) { a.push(new Array(1000)); }")


def test_run_gc_does_not_crash(rt):
    ctx = rt.new_context()
    ctx.eval("var junk = []; for (let i = 0; i < 1000; i++) junk.push({i});")
    ctx.eval("junk = null;")
    rt.run_gc()


def test_gc_threshold(rt):
    rt.set_gc_threshold(64 * 1024)
    ctx = rt.new_context()
    assert ctx.eval("({}).constructor.name") == "Object"


def test_interrupt_handler_stops_infinite_loop(rt):
    calls = {"n": 0}

    def handler():
        calls["n"] += 1
        return calls["n"] > 1000

    rt.set_interrupt_handler(handler)
    ctx = rt.new_context()
    with pytest.raises(quickjs.JSError):
        ctx.eval("while (true) {}")
    assert calls["n"] > 1000


def test_interrupt_handler_can_be_cleared(rt):
    rt.set_interrupt_handler(lambda: False)
    rt.set_interrupt_handler(None)
    ctx = rt.new_context()
    assert ctx.eval("41 + 1") == 42


def test_quickjs_version_exposed():
    assert isinstance(quickjs.quickjs_version, str)
    assert quickjs.quickjs_version
