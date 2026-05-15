# quickjs-py

Python bindings for [QuickJS](https://github.com/bellard/quickjs), Fabrice
Bellard's small and embeddable JavaScript engine.

The QuickJS engine is vendored and compiled directly into the extension, so
there is no external dependency on a system QuickJS install.

## Installation

```sh
pip install -e .
```

A C compiler and the Python development headers are required to build the
extension.

## Quick start

```python
import quickjs

# One-off evaluation
print(quickjs.eval("1 + 2"))            # 3

# A reusable context
ctx = quickjs.Context()
ctx.eval("var counter = 0;")
ctx.eval("counter += 10;")
print(ctx.eval("counter"))              # 10

# Pass Python values into JS
ctx.set("data", {"name": "Ada", "scores": [90, 95]})
print(ctx.eval("data.scores[1]"))       # 95

# Expose Python callables to JS
ctx.set("greet", lambda name: f"hi {name}")
print(ctx.eval('greet("world")'))       # hi world

# Convert JS values back to Python
result = ctx.eval("({a: 1, b: [2, 3]})")
print(result.to_python())               # {'a': 1, 'b': [2, 3]}

# JS errors surface as Python exceptions
try:
    ctx.eval('throw new Error("boom")')
except quickjs.JSError as exc:
    print(exc)                          # Error: boom
```

## Architecture

* `quickjs._quickjs` - a C extension wrapping the QuickJS C API
  (`Runtime`, `Context`, `Value`).
* `quickjs` - the public package re-exporting those types plus convenience
  helpers.

See `CLAUDE.md` for the full design and `TODO.md` for the roadmap.

## Type conversion

| Python              | JavaScript            |
|---------------------|-----------------------|
| `None`              | `null`                |
| `bool`              | `boolean`             |
| `int`               | `number` / `bigint`   |
| `float`             | `number`              |
| `str`               | `string`              |
| `bytes`             | `ArrayBuffer`         |
| `list` / `tuple`    | `Array`               |
| `dict`              | `Object`              |
| callable            | `function`            |

JS objects, arrays and functions returned to Python are wrapped as
`quickjs.Value`. Call `.to_python()` to recursively convert arrays/objects
into native `list`/`dict` structures.

## Testing

```sh
pip install pytest
pytest -q
```

## License

MIT. The vendored QuickJS sources are under the MIT license; see
`vendor/quickjs/LICENSE`.
