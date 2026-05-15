import pytest

import quickjs


def test_global_get_set(ctx):
    ctx.set("x", 99)
    assert ctx.get("x") == 99


def test_get_global_object(ctx):
    g = ctx.get_global()
    assert g.is_object
    g["answer"] = 42
    assert ctx.eval("answer") == 42


def test_new_object_and_array(ctx):
    obj = ctx.new_object()
    obj["k"] = "v"
    assert obj["k"] == "v"
    arr = ctx.new_array()
    arr[0] = "first"
    assert arr["length"] == 1


def test_parse_json(ctx):
    val = ctx.parse_json('{"a": 1, "b": [2, 3]}')
    assert val.to_python() == {"a": 1, "b": [2, 3]}


def test_json_stringify(ctx):
    val = ctx.eval("({x: 1, y: 2})")
    assert val.json() == '{"x":1,"y":2}'


def test_property_helpers(ctx):
    obj = ctx.eval("({a: 1})")
    assert obj.has("a") is True
    assert obj.has("missing") is False
    obj.set("b", 2)
    assert obj.get("b") == 2
    obj.delete("a")
    assert obj.has("a") is False
    assert set(obj.keys()) == {"b"}


def test_call_method_with_this(ctx):
    obj = ctx.eval("({n: 7, get() { return this.n; }})")
    method = obj["get"]
    assert method(this=obj) == 7


def test_call_constructor(ctx):
    ctor = ctx.eval("(function Point(x) { this.x = x; })")
    instance = ctor.call_constructor(5)
    assert instance["x"] == 5


def test_pending_jobs_run_promises(ctx):
    ctx.eval("var result = null; Promise.resolve(7).then(v => { result = v; });")
    assert ctx.has_pending_job
    while ctx.execute_pending_job():
        pass
    assert ctx.get("result") == 7


def test_shared_runtime_across_contexts(rt):
    c1 = rt.new_context()
    c2 = rt.new_context()
    assert c1.runtime is rt
    assert c2.runtime is rt
    c1.eval("var a = 1;")
    with pytest.raises(quickjs.JSError):
        c2.eval("a")  # globals are not shared between contexts
