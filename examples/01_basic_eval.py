"""Basic evaluation: one-off snippets and a reusable context."""

import quickjs

# One-off evaluation in a throwaway context.
print(quickjs.eval("1 + 2"))  # 3
print(quickjs.eval("'ab'.repeat(3)"))  # ababab

# A reusable context keeps state between evals.
ctx = quickjs.Context()
ctx.eval("var counter = 0;")
ctx.eval("counter += 10;")
ctx.eval("counter += 5;")
print(ctx.eval("counter"))  # 15

# Objects come back as quickjs.Value; convert them with to_python().
result = ctx.eval("({a: 1, b: [2, 3]})")
print(result.to_python())  # {'a': 1, 'b': [2, 3]}

# The vendored engine version.
print("QuickJS", quickjs.quickjs_version)
