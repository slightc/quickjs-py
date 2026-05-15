"""Calling Python from JS and JS from Python."""

import quickjs

ctx = quickjs.Context()

# Expose a Python callable to JS.
ctx.set("greet", lambda name: f"hi {name}")
print(ctx.eval('greet("world")'))  # hi world

# Python callables can be passed as arguments and invoked from JS.
ctx.set("log", print)
ctx.eval('["a", "b", "c"].forEach(x => log("item:", x))')

# JS functions returned to Python are callable like Python functions.
add = ctx.eval("(a, b) => a + b")
print(add(3, 4))  # 7

# Bind `this` when calling a JS method.
obj = ctx.eval("({factor: 10, scale(x) { return x * this.factor; }})")
print(obj.get("scale").call(5, this=obj))  # 50
