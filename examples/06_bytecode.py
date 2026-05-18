"""Compile once, serialise to bytecode, run many times."""

import quickjs

ctx = quickjs.Context()

# Compile a snippet without running it.
compiled = ctx.compile("globalThis.r = (globalThis.r || 0) + 1; r")

# Serialise the compiled code to a portable bytecode blob.
blob = compiled.write_object()
print(f"bytecode is {len(blob)} bytes")

# Reload the blob and run it repeatedly in the same context.
restored = ctx.read_object(blob)
print(ctx.eval_function(restored))  # 1
print(ctx.eval_function(ctx.read_object(blob)))  # 2

# compile_only=True via eval gives the same compiled value.
fn = ctx.eval("40 + 2", compile_only=True)
print(ctx.eval_function(fn))  # 42
