"""Tests for async / Promise execution: async_eval, await_promise, and the
Promise inspection helpers on Value."""

import asyncio

import pytest

import quickjs

# --- Value Promise inspection ------------------------------------------


def test_is_promise(ctx):
    assert ctx.eval("Promise.resolve(1)").is_promise is True
    assert ctx.eval("({})").is_promise is False


def test_promise_state(ctx):
    assert ctx.eval("Promise.resolve(1)").promise_state == "fulfilled"
    assert ctx.eval("Promise.reject(new Error('x'))").promise_state == "rejected"
    assert ctx.eval("new Promise(() => {})").promise_state == "pending"
    assert ctx.eval("({})").promise_state is None


def test_value_result_fulfilled(ctx):
    assert ctx.eval("Promise.resolve(42)").result() == 42


def test_value_result_pumps_pending(ctx):
    ctx.eval("async function f(){ return await Promise.resolve(7) + 1; }")
    promise = ctx.eval("f()")
    assert promise.promise_state == "pending"
    assert promise.result() == 8


def test_value_result_rejected(ctx):
    promise = ctx.eval("Promise.reject(new Error('boom'))")
    with pytest.raises(quickjs.JSError, match="boom"):
        promise.result()


def test_value_result_on_non_promise(ctx):
    with pytest.raises(TypeError):
        ctx.eval("({})").result()


# --- Context.async_eval ------------------------------------------------


def test_async_eval_sync_expression(ctx):
    assert ctx.async_eval("1 + 2") == 3


def test_async_eval_top_level_await(ctx):
    assert ctx.async_eval("const x = await Promise.resolve(20); x * 2") == 40


def test_async_eval_calls_async_function(ctx):
    ctx.eval("async function f(){ return await Promise.resolve(10) + 5; }")
    # Calling f() yields a Promise; async_eval unwraps it like `await f()`.
    assert ctx.async_eval("f()") == 15
    assert ctx.async_eval("await f()") == 15


def test_async_eval_unwraps_nested_promises(ctx):
    assert ctx.async_eval("Promise.resolve(Promise.resolve(8))") == 8


def test_async_eval_object_result(ctx):
    assert ctx.async_eval("({a: 1, b: [2, 3]})").to_python() == {"a": 1, "b": [2, 3]}


def test_async_eval_rejection_raises(ctx):
    with pytest.raises(quickjs.JSError, match="boom"):
        ctx.async_eval("await Promise.reject(new Error('boom'))")


def test_async_eval_sync_throw_raises(ctx):
    with pytest.raises(quickjs.JSError):
        ctx.async_eval("throw new Error('nope')")


def test_async_eval_pending_forever(ctx):
    with pytest.raises(quickjs.QuickJSError, match="pending"):
        ctx.async_eval("await new Promise(() => {})")


def test_top_level_async_eval():
    assert quickjs.async_eval("await Promise.resolve(99)") == 99


# --- Context.await_promise (asyncio bridge) ----------------------------


def test_await_promise_fulfilled(ctx):
    ctx.eval("async function f(){ return await Promise.resolve(10) + 5; }")

    async def main():
        return await ctx.await_promise(ctx.eval("f()"))

    assert asyncio.run(main()) == 15


def test_await_promise_rejected(ctx):
    promise = ctx.eval("Promise.reject(new Error('nope'))")

    async def main():
        await ctx.await_promise(promise)

    with pytest.raises(quickjs.JSError, match="nope"):
        asyncio.run(main())


def test_await_promise_passes_non_promise_through(ctx):
    async def main():
        return await ctx.await_promise(123)

    assert asyncio.run(main()) == 123


def test_await_promise_flattens_nested(ctx):
    promise = ctx.eval("Promise.resolve(Promise.resolve(77))")

    async def main():
        return await ctx.await_promise(promise)

    assert asyncio.run(main()) == 77


def test_await_promise_interleaves_with_event_loop(ctx):
    ctx.eval(
        "async function g(){ let s = 0;"
        " for (let i = 0; i < 4; i++) s += await Promise.resolve(i);"
        " return s; }"
    )
    order = []

    async def other():
        for i in range(4):
            order.append(i)
            await asyncio.sleep(0)

    async def main():
        js, _ = await asyncio.gather(ctx.await_promise(ctx.eval("g()")), other())
        return js

    assert asyncio.run(main()) == 6
    # The plain asyncio task made progress while the JS jobs were pumped.
    assert order == [0, 1, 2, 3]


def test_await_promise_pending_forever(ctx):
    promise = ctx.eval("new Promise(() => {})")

    async def main():
        await ctx.await_promise(promise)

    with pytest.raises(quickjs.QuickJSError, match="pending"):
        asyncio.run(main())
