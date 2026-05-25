"""Regression tests for the callback-holder, argv-buffer and atom-cache
optimizations.

These pin down behaviour that the old implementations did not provide:

* Opt 1 (callback holder): the Python callable held by a JS function is
  released exactly when the JS function is garbage-collected, not when the
  Context is destroyed. The previous dict-based registry kept every
  callable alive for the Context's lifetime.
* Opt 2 (argv buffer): calls with >8 arguments must still work (heap
  fallback path), and tuple/list/generator inputs must all behave
  identically.
* Opt 4 (atom cache + cached global): cached atoms must survive a GC
  cycle and the cached global must remain valid across many ctx.get/
  ctx.set calls (no use-after-free in the dup path).
"""

from __future__ import annotations

import gc
import weakref

import pytest

import quickjs

# ----------------------------------------------------------------------
# Opt 1: callback memory ownership
# ----------------------------------------------------------------------


class _Tag:
    """A trivial heap object so weakref can observe its collection."""

    def __init__(self, n: int) -> None:
        self.n = n

    def __call__(self) -> int:
        return self.n


def test_callback_released_when_js_function_collected(ctx):
    """Dropping the JS function (and running the JS GC) must release the
    underlying Python callable. The pre-optimization registry kept it
    alive for the lifetime of the Context."""
    cb = _Tag(7)
    ref = weakref.ref(cb)
    ctx.set("cb", cb)
    assert ctx.eval("cb()") == 7
    # Drop both the Python-side and JS-side references to the callable.
    del cb
    ctx.eval("cb = null;")
    ctx.runtime.run_gc()
    gc.collect()
    assert ref() is None


def test_callbacks_do_not_leak_in_hot_loop(ctx):
    """Repeatedly install short-lived JS functions; the JS GC must reclaim
    them so the engine's object count returns near baseline."""
    ctx.set("mk", lambda: lambda x: x + 1)
    ctx.eval("var keep = null;")
    ctx.runtime.run_gc()
    baseline = ctx.runtime.compute_memory_usage()["obj_count"]
    for _ in range(2000):
        ctx.eval("(function () { let f = mk(); f(0); })()")
    ctx.runtime.run_gc()
    after = ctx.runtime.compute_memory_usage()["obj_count"]
    # A leak would add ~2000 host objects + 2000 functions.
    assert after - baseline < 100, (baseline, after)


def test_callback_still_works_after_runtime_gc(ctx):
    """Holding a JS function via the global keeps the holder alive across
    explicit GC cycles."""
    ctx.set("cb", lambda x: x * 3)
    for _ in range(10):
        ctx.runtime.run_gc()
        assert ctx.eval("cb(4)") == 12


def test_many_distinct_callbacks(ctx):
    """The old implementation used a per-context monotonically increasing
    id; the new one needs none. Either way, many distinct callbacks must
    not collide."""
    for i in range(500):
        ctx.set(f"f{i}", lambda i=i: i * 2)
    total = ctx.eval(
        "(function(){let s=0;for(let i=0;i<500;i++)s+=this['f'+i]();return s;}).call(globalThis)"
    )
    assert total == sum(i * 2 for i in range(500))


# ----------------------------------------------------------------------
# Opt 2: argv stack buffer + sequence specialisation
# ----------------------------------------------------------------------


@pytest.fixture
def fn_sum(ctx):
    return ctx.eval("(function () { let s = 0; for (const a of arguments) s += a; return s; })")


def test_call_zero_args(fn_sum):
    assert fn_sum() == 0


def test_call_small_args_fits_in_stack_buffer(fn_sum):
    # JS_ARGV_STACK == 8: this fits exactly.
    assert fn_sum(1, 2, 3, 4, 5, 6, 7, 8) == 36


def test_call_large_args_uses_heap_fallback(fn_sum):
    # Strictly larger than the stack buffer: exercises PyMem_Malloc path.
    args = tuple(range(50))
    assert fn_sum(*args) == sum(args)


def test_call_with_list_argv(ctx):
    """Internally call(*args) packs into a tuple, but constructor calls and
    user-driven sequences must also work."""
    fn = ctx.eval("(function (a, b, c) { return [a, b, c]; })")
    # Force the tuple path via *args:
    assert fn(*[10, 20, 30]).to_python() == [10, 20, 30]


def test_call_constructor_large_argv(ctx):
    ctor = ctx.eval("(function (...xs) { this.xs = xs; })")
    inst = ctor.call_constructor(*range(20))
    assert inst["xs"].to_python() == list(range(20))


def test_call_argv_error_releases_partial(ctx):
    """Argument conversion failure mid-build must free the JSValues built
    so far. Run many times; a leak would blow up the engine heap."""
    fn = ctx.eval("(function () { return 1; })")
    sentinel = object()  # not convertible
    ctx.runtime.run_gc()
    baseline = ctx.runtime.compute_memory_usage()["obj_count"]
    for _ in range(500):
        with pytest.raises(TypeError):
            fn(1, 2, 3, sentinel)
    ctx.runtime.run_gc()
    after = ctx.runtime.compute_memory_usage()["obj_count"]
    assert after - baseline < 50


# ----------------------------------------------------------------------
# Opt 4: atom cache + cached global
# ----------------------------------------------------------------------


def test_cached_global_survives_many_get_set(ctx):
    """ctx.get/ctx.set used to JS_GetGlobalObject + JS_FreeValue per call.
    The cache must remain valid across many operations and across GC."""
    for i in range(1000):
        ctx.set("x", i)
        ctx.runtime.run_gc() if i % 100 == 0 else None
        assert ctx.get("x") == i


def test_cached_global_identity(ctx):
    """get_global must still return a usable Value, with each call yielding
    a fresh wrapper (Value identity is independent of the JS-level cache)."""
    g1 = ctx.get_global()
    g2 = ctx.get_global()
    # Same underlying JS object.
    assert g1 == g2
    # Both wrappers usable independently.
    g1["marker"] = 1
    assert g2["marker"] == 1


def test_value_length_uses_atom_cache(ctx):
    arr = ctx.eval("Array.from({length: 1000}, (_, i) => i)")
    # Many length lookups: the atom-cache path must not leak atoms.
    ctx.runtime.run_gc()
    baseline = ctx.runtime.compute_memory_usage()["atom_count"]
    for _ in range(5000):
        assert len(arr) == 1000
    ctx.runtime.run_gc()
    after = ctx.runtime.compute_memory_usage()["atom_count"]
    assert after == baseline


def test_atom_cache_shared_across_contexts(rt):
    """Atoms are per-runtime in QuickJS, so the cache is on Runtime and
    must be reused by every Context the runtime spawns."""
    c1 = rt.new_context()
    c2 = rt.new_context()
    rt.run_gc()
    baseline = rt.compute_memory_usage()["atom_count"]
    for _ in range(1000):
        len(c1.eval("[1,2,3]"))
        len(c2.eval("[4,5,6]"))
    rt.run_gc()
    after = rt.compute_memory_usage()["atom_count"]
    assert after == baseline


def test_error_stack_uses_atom_cache(ctx):
    """raise_js_exception used to JS_NewAtom('stack') per error. Many
    raised errors must not grow the atom table."""
    ctx.runtime.run_gc()
    baseline = ctx.runtime.compute_memory_usage()["atom_count"]
    for _ in range(500):
        with pytest.raises(quickjs.JSError):
            ctx.eval("throw new Error('boom')")
    ctx.runtime.run_gc()
    after = ctx.runtime.compute_memory_usage()["atom_count"]
    assert after == baseline


def test_bigint_round_trip_through_cached_global(ctx):
    """py_to_js falls back to globalThis.BigInt for huge ints; the cached
    global and the BigInt atom must keep working call after call."""
    huge = 2**200 + 1
    for _ in range(50):
        ctx.set("n", huge)
        assert ctx.eval("typeof n") == "bigint"
        assert ctx.get("n") == huge
