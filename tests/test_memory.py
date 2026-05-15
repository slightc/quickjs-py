"""Memory / refcount regression tests.

These exercise the ownership chain (Value -> Context -> Runtime) and the
JS reference held by every Value, checking that repeated create/destroy
cycles do not leak JS objects or Python references.
"""

import sys

import quickjs


def test_compute_memory_usage_shape(ctx):
    usage = ctx.runtime.compute_memory_usage()
    assert usage["malloc_size"] > 0
    assert usage["obj_count"] > 0
    assert set(usage) >= {"malloc_size", "obj_count", "atom_count"}


def test_values_do_not_leak_js_objects(ctx):
    ctx.runtime.run_gc()
    baseline = ctx.runtime.compute_memory_usage()["obj_count"]
    for _ in range(2000):
        obj = ctx.eval("({a: 1, b: [1, 2, 3]})")
        del obj
    ctx.runtime.run_gc()
    after = ctx.runtime.compute_memory_usage()["obj_count"]
    # A handful of objects may legitimately remain; a leak would add ~2000.
    assert after - baseline < 100


def test_value_holds_one_python_ref(ctx):
    value = ctx.eval("({})")
    assert sys.getrefcount(value) == 2  # local + getrefcount argument


def test_context_kept_alive_by_value():
    # The Value must keep its Context (and Runtime) alive even after the
    # local Context reference is dropped.
    value = quickjs.Context().eval("({n: 7})")
    assert value["n"] == 7


def test_callback_release_after_context_gc():
    calls = []
    ctx = quickjs.Context()
    ctx.set("cb", lambda: calls.append(1))
    ctx.eval("cb()")
    del ctx
    assert calls == [1]


def test_host_object_releases_python_object(ctx):
    import weakref

    class Holder:
        pass

    holder = Holder()
    ref = weakref.ref(holder)
    ctx.set("h", ctx.new_host_object(holder))
    del holder
    # Still referenced by the JS side.
    assert ref() is not None
    ctx.eval("h = null;")
    ctx.runtime.run_gc()
    assert ref() is None


def test_many_contexts_one_runtime(rt):
    for _ in range(200):
        c = rt.new_context()
        assert c.eval("Math.sqrt(16)") == 4
        del c
    rt.run_gc()
