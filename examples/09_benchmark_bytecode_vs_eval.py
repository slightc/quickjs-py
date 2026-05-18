"""Benchmark: rebuilding from bytecode (read_object) vs. direct eval.

Two ways to run the same JavaScript repeatedly:

  * direct eval        -- ctx.eval(code) parses + compiles + runs every time
  * bytecode rebuild   -- compile once, then for each run
                          ctx.read_object(blob) + ctx.eval_function(fn)

The bytecode path skips lexing/parsing/compilation on every iteration, so it
should win as the source grows. This script quantifies the gap.
"""

import time

import quickjs


def _objects_snippet(count):
    """Build an IIFE declaring `count` object literals; cheap to run, costly
    to parse."""
    pad = "v" * 20
    body = "\n".join(
        f"  const o{i} = {{ a: {i}, b: '{pad}', fn: function (x) {{ return x + {i}; }} }};"
        for i in range(count)
    )
    return f"(function () {{\n{body}\n  return o0.fn(o{count - 1}.a);\n}})()"


# Snippets of increasing parse cost. The runtime work is deliberately small so
# the measurement is dominated by parse/compile, which is what bytecode skips.
SNIPPETS = {
    "tiny": "1 + 1",
    "small": "(function () { let s = 0; for (let i = 0; i < 10; i++) s += i; return s; })()",
    "medium": "\n".join(f"function f{i}(x) {{ return x * {i} + {i}; }}" for i in range(40))
    + "\nf0(1) + f39(2);",
    "large": _objects_snippet(150),
    "xl": _objects_snippet(1100),
}

# Iterations per case: heavier snippets run fewer times so the whole
# benchmark stays a few seconds rather than minutes.
ITERATIONS = {
    "tiny": 20_000,
    "small": 20_000,
    "medium": 5_000,
    "large": 1_000,
    "xl": 100,
}


def bench(label, fn, iterations):
    fn()  # warm up
    start = time.perf_counter()
    for _ in range(iterations):
        fn()
    elapsed = time.perf_counter() - start
    per_call_us = elapsed / iterations * 1e6
    print(f"  {label:<22} {elapsed:8.4f} s total   {per_call_us:9.3f} us/call")
    return elapsed


def main():
    print(f"quickjs version: {quickjs.quickjs_version}\n")

    header = f"{'case':<10} {'eval':>12} {'bytecode':>12} {'speedup':>10} {'blob':>8}"
    print(header)
    print("-" * len(header))

    details = []
    for name, code in SNIPPETS.items():
        ctx = quickjs.Context()

        # One-time setup for the bytecode path: compile and serialise.
        blob = ctx.compile(code).write_object()

        def run_eval(ctx=ctx, code=code):
            return ctx.eval(code)

        def run_bytecode(ctx=ctx, blob=blob):
            return ctx.eval_function(ctx.read_object(blob))

        # Sanity check: both paths must produce the same result.
        assert run_eval() == run_bytecode(), f"result mismatch for {name!r}"

        iterations = ITERATIONS[name]
        print(f"\n[{name}]  source = {len(code)} bytes, {iterations} iterations")
        eval_t = bench("direct eval", run_eval, iterations)
        bc_t = bench("read_object + eval_fn", run_bytecode, iterations)
        details.append((name, eval_t / iterations, bc_t / iterations, len(blob)))

    print("\n" + header)
    print("-" * len(header))
    for name, eval_us, bc_us, blob_len in details:
        speedup = eval_us / bc_us if bc_us else float("inf")
        print(
            f"{name:<10} {eval_us * 1e6:9.2f}us {bc_us * 1e6:9.2f}us {speedup:9.2f}x {blob_len:7d}B"
        )

    print(
        "\nNote: the bytecode path still pays read_object deserialisation on every\n"
        "call. Its win comes purely from skipping lexing/parsing/compilation."
    )


if __name__ == "__main__":
    main()
