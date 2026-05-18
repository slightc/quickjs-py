"""Driving the promise / microtask job queue and running async functions."""

import asyncio

import quickjs

ctx = quickjs.Context()

# Promise callbacks are queued as jobs; they do not run until pumped.
ctx.eval(
    """
    globalThis.log = [];
    Promise.resolve(1)
        .then(v => { log.push(v); return v + 1; })
        .then(v => { log.push(v); });
    """
)

print("before pumping:", ctx.eval("log").to_python())  # []

# Run queued jobs until the queue drains.
while ctx.has_pending_job:
    ctx.execute_pending_job()

print("after pumping:", ctx.eval("log").to_python())  # [1, 2]

# A Promise value can be inspected and settled directly.
promise = ctx.eval("Promise.resolve(7)")
print("is_promise:", promise.is_promise, promise.promise_state)  # True fulfilled
print("result():", promise.result())  # 7

# async_eval evaluates code that may use top-level `await`, pumps the job
# queue, and returns the settled value (rejections raise quickjs.JSError).
ctx.eval("async function add(a, b) { return await Promise.resolve(a + b); }")
print("async_eval:", ctx.async_eval("add(2, 3)"))  # 5
print("top-level await:", ctx.async_eval("await Promise.resolve(41) + 1"))  # 42

# await_promise bridges QuickJS promises into asyncio: it pumps the job queue
# cooperatively so other asyncio tasks keep running.
ctx.eval(
    """
    async function sumRange(n) {
        let total = 0;
        for (let i = 0; i < n; i++) total += await Promise.resolve(i);
        return total;
    }
    """
)


async def main():
    result = await ctx.await_promise(ctx.eval("sumRange(5)"))
    print("await_promise:", result)  # 10


asyncio.run(main())
