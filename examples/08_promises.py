"""Driving the promise / microtask job queue."""

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
