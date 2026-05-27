import pytest

import quickjs

# --- Undefined ----------------------------------------------------------


def test_undefined_is_singleton(ctx):
    assert ctx.eval("undefined") is quickjs.Undefined
    assert ctx.eval("void 0") is quickjs.Undefined
    assert bool(quickjs.Undefined) is False
    assert repr(quickjs.Undefined) == "Undefined"


def test_undefined_distinct_from_null(ctx):
    assert ctx.eval("null") is None
    assert ctx.eval("null") is not quickjs.Undefined


def test_undefined_roundtrip(ctx):
    ctx.set("u", quickjs.Undefined)
    assert ctx.eval("typeof u") == "undefined"


def test_undefined_type_is_a_singleton():
    # Calling the type returns the one canonical instance, never a new one.
    assert quickjs.UndefinedType() is quickjs.Undefined


# --- ArrayBuffer / TypedArray ------------------------------------------


def test_new_array_buffer_roundtrip(ctx):
    ab = ctx.new_array_buffer(b"binary\x00data")
    assert ab.to_bytes() == b"binary\x00data"


def test_typed_array_to_bytes(ctx):
    ta = ctx.eval("new Uint8Array([0, 1, 2, 255])")
    assert ta.to_bytes() == b"\x00\x01\x02\xff"


def test_array_buffer_visible_in_js(ctx):
    ctx.set("buf", ctx.new_array_buffer(b"abc"))
    assert ctx.eval("new Uint8Array(buf)[0]") == 97


def test_to_bytes_rejects_non_buffer(ctx):
    with pytest.raises(TypeError):
        ctx.eval("({})").to_bytes()


# --- Bytecode -----------------------------------------------------------


def test_compile_and_eval_function(ctx):
    compiled = ctx.compile("3 * 14")
    assert ctx.eval_function(compiled) == 42


def test_bytecode_roundtrip_across_contexts(ctx):
    blob = ctx.compile("'hello'.toUpperCase()").write_object()
    assert isinstance(blob, bytes)
    other = quickjs.Context()
    restored = other.read_object(blob)
    assert other.eval_function(restored) == "HELLO"


# --- Host objects -------------------------------------------------------


def test_host_object_roundtrip(ctx):
    sentinel = object()
    ctx.set("h", ctx.new_host_object(sentinel))
    assert ctx.eval("h") is sentinel


def test_host_object_survives_js_storage(ctx):
    payload = {"complex": [1, 2, 3]}
    ctx.set("h", ctx.new_host_object(payload))
    ctx.eval("var box = { inner: h };")
    assert ctx.eval("box.inner") is payload


def test_host_object_passed_through_callback(ctx):
    received = []
    ctx.set("capture", lambda v: received.append(v))
    marker = object()
    ctx.set("h", ctx.new_host_object(marker))
    ctx.eval("capture(h)")
    assert received == [marker]


# --- define_property ----------------------------------------------------


def test_define_property_data(ctx):
    obj = ctx.new_object()
    obj.define_property("ro", value=10, writable=False)
    ctx.set("o", obj)
    ctx.eval("'use strict'; try { o.ro = 99; } catch (e) {}")
    assert obj["ro"] == 10


def test_define_property_getter(ctx):
    obj = ctx.new_object()
    calls = []

    def getter():
        calls.append(1)
        return len(calls)

    obj.define_property("counter", get=getter)
    assert obj["counter"] == 1
    assert obj["counter"] == 2


def test_define_property_setter(ctx):
    obj = ctx.new_object()
    stored = []
    obj.define_property("sink", set=lambda v: stored.append(v))
    ctx.set("o", obj)
    ctx.eval("o.sink = 'written';")
    assert stored == ["written"]


def test_define_property_non_enumerable(ctx):
    obj = ctx.new_object()
    obj.define_property("hidden", value=1, enumerable=False)
    assert "hidden" not in obj.keys()


# --- ES modules ---------------------------------------------------------


def test_module_loader_resolves_imports(ctx):
    sources = {
        "math": "export function square(x) { return x * x; }",
    }
    ctx.set_module_loader(lambda name: sources.get(name))
    ctx.eval(
        "import { square } from 'math'; globalThis.result = square(9);",
        module=True,
    )
    assert ctx.get("result") == 81


def test_module_loader_missing_module_raises(ctx):
    ctx.set_module_loader(lambda name: None)
    with pytest.raises(quickjs.JSError):
        ctx.eval("import { x } from 'nope';", module=True)


def test_module_loader_can_be_cleared(ctx):
    ctx.set_module_loader(lambda name: "export const v = 1;")
    ctx.set_module_loader(None)
    with pytest.raises(quickjs.JSError):
        ctx.eval("import { v } from 'any';", module=True)


def test_module_normalizer_resolves_relative_names(ctx):
    seen_loads = []

    def normalize(base, name):
        if name.startswith("./"):
            return name[2:]
        return name

    def load(name):
        seen_loads.append(name)
        sources = {
            "math": "export function square(x) { return x * x; }",
        }
        return sources.get(name)

    ctx.set_module_normalizer(normalize)
    ctx.set_module_loader(load)
    ctx.eval(
        "import { square } from './math'; globalThis.result = square(7);",
        module=True,
        filename="entry",
    )
    assert ctx.get("result") == 49
    assert "math" in seen_loads


def test_module_normalizer_receives_base_name(ctx):
    captured = []

    def normalize(base, name):
        captured.append((base, name))
        return name

    ctx.set_module_normalizer(normalize)
    ctx.set_module_loader(lambda name: "export const v = 1;")
    ctx.eval(
        "import { v } from 'lib'; globalThis.v = v;",
        module=True,
        filename="main",
    )
    assert ctx.get("v") == 1
    assert any(name == "lib" for _base, name in captured)


def test_module_normalizer_handles_null_base_name(ctx):
    captured = []

    def normalize(base, name):
        captured.append((base, name))
        return name

    ctx.set_module_normalizer(normalize)
    ctx.set_module_loader(
        lambda name: "import 'inner';" if name == "outer" else "export const x = 1;"
    )
    ctx.eval("import 'outer';", module=True)
    assert captured, "normalizer should have been invoked"
    bases = {base for base, _ in captured}
    assert None in bases or any(isinstance(b, str) for b in bases)


def test_module_normalizer_can_be_cleared(ctx):
    ctx.set_module_normalizer(lambda base, name: "math")
    ctx.set_module_normalizer(None)
    ctx.set_module_loader(lambda name: "export const v = 1;" if name == "lib" else None)
    ctx.eval("import { v } from 'lib'; globalThis.x = v;", module=True)
    assert ctx.get("x") == 1


def test_module_normalizer_error_propagates(ctx):
    def normalize(base, name):
        raise RuntimeError("boom")

    ctx.set_module_normalizer(normalize)
    ctx.set_module_loader(lambda name: "export const v = 1;")
    with pytest.raises(quickjs.JSError):
        ctx.eval("import { v } from 'lib';", module=True)


def test_module_normalizer_requires_callable(ctx):
    with pytest.raises(TypeError):
        ctx.set_module_normalizer(42)


# --- get_exception ------------------------------------------------------


def test_get_exception_returns_none_when_clear(ctx):
    assert ctx.get_exception() is None


# --- context manager ----------------------------------------------------


def test_context_is_a_context_manager():
    with quickjs.Context() as ctx:
        assert ctx.eval("1 + 1") == 2


def test_runtime_is_a_context_manager():
    with quickjs.Runtime() as rt:
        ctx = rt.new_context()
        assert ctx.eval("2 + 2") == 4
