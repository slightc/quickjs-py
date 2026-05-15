"""JS exceptions surface as Python exceptions."""

import quickjs

ctx = quickjs.Context()

# A thrown JS error becomes a quickjs.JSError.
try:
    ctx.eval('throw new Error("boom")')
except quickjs.JSError as exc:
    print("caught:", exc)  # caught: Error: boom
    print("stack:", exc.js_stack)

# Syntax errors raise too.
try:
    ctx.eval("this is not ) valid (")
except quickjs.JSError as exc:
    print("syntax:", exc)


# Exceptions raised by a Python callback propagate back through JS.
def explode():
    raise ValueError("from python")


ctx.set("explode", explode)
try:
    ctx.eval("explode()")
except Exception as exc:  # noqa: BLE001
    print("python error round-tripped:", type(exc).__name__, exc)
