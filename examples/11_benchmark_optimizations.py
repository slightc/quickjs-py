"""Micro-benchmarks for the optimization work in this branch.

Three hot paths are measured:

  * callback dispatch     -- JS calls a Python callable through the
                             trampoline (Opt 1: PyObject* lives in func_data,
                             no per-call dict lookup).
  * argv marshalling      -- JS function called from Python with small N
                             positional args (Opt 2: small-arg stack buffer,
                             fast tuple item access).
  * property/global access -- repeated ctx.get/set and Value.length
                             (Opt 4: per-runtime atom cache + per-context
                             cached global object).
"""

import time

import quickjs


def bench(label, fn, iterations):
    fn()  # warm up
    start = time.perf_counter()
    for _ in range(iterations):
        fn()
    elapsed = time.perf_counter() - start
    per_call_us = elapsed / iterations * 1e6
    print(f"  {label:<32} {elapsed:7.4f} s   {per_call_us:9.3f} us/call")
    return per_call_us


def bench_callback_dispatch():
    print("\n[callback] JS -> Python trampoline (10k calls per iter)")
    ctx = quickjs.Context()
    ctx.set("add", lambda a, b: a + b)
    code = "(function () { let s = 0; for (let i = 0; i < 10000; i++) s = add(s, 1); return s; })()"
    bench("callback x10000", lambda: ctx.eval(code), 200)


def bench_call_argv_small():
    print("\n[argv] Python -> JS call with small N args")
    ctx = quickjs.Context()
    fn = ctx.eval("(function (a, b, c) { return a + b + c; })")
    bench("call(1,2,3)", lambda: fn(1, 2, 3), 100_000)
    fn8 = ctx.eval("(function (a,b,c,d,e,f,g,h) { return a+b+c+d+e+f+g+h; })")
    bench("call(1..8)", lambda: fn8(1, 2, 3, 4, 5, 6, 7, 8), 100_000)


def bench_global_get_set():
    print("\n[global] ctx.get / ctx.set / Value.length")
    ctx = quickjs.Context()
    ctx.set("x", 42)
    bench("ctx.get('x')", lambda: ctx.get("x"), 200_000)
    bench("ctx.set('x', 42)", lambda: ctx.set("x", 42), 200_000)
    arr = ctx.eval("[1,2,3,4,5,6,7,8,9,10]")
    bench("len(arr)", lambda: len(arr), 500_000)


def bench_callback_leak():
    print("\n[leak] create + drop JS functions in a hot loop")
    ctx = quickjs.Context()
    # Each iteration installs a fresh JS-wrapped Python callable and lets
    # it die. With the dict-based registry these accumulated forever; with
    # the host-object holder the JS function's finalizer evicts them.
    code = "(function () { for (let i = 0; i < 1000; i++) { let f = mk(); f(); } })()"

    def setup():
        ctx.set("mk", lambda: lambda: 1)

    setup()
    bench("install+drop x1000", lambda: ctx.eval(code), 50)


def main():
    print(f"quickjs version: {quickjs.quickjs_version}")
    bench_callback_dispatch()
    bench_call_argv_small()
    bench_global_get_set()
    bench_callback_leak()


if __name__ == "__main__":
    main()
