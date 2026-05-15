"""Sandboxing: memory limits and interrupt handlers."""

import time

import quickjs

# A memory limit makes runaway allocation throw instead of exhausting the host.
rt = quickjs.Runtime()
rt.set_memory_limit(256 * 1024)
ctx = rt.new_context()
try:
    ctx.eval("const a = []; while (true) a.push(new Array(1000));")
except quickjs.JSError as exc:
    print("memory limit hit:", exc)

# An interrupt handler stops long-running (or infinite) scripts.
rt2 = quickjs.Runtime()
deadline = time.time() + 0.5
rt2.set_interrupt_handler(lambda: time.time() > deadline)
ctx2 = rt2.new_context()
try:
    ctx2.eval("while (true) {}")
except quickjs.JSError as exc:
    print("interrupted:", exc)

# Engine memory counters.
print(rt2.compute_memory_usage())
