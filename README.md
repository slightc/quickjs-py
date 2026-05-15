# quickjs-py

Python bindings for [QuickJS](https://github.com/bellard/quickjs), Fabrice
Bellard's small and embeddable JavaScript engine.

The QuickJS engine is included as a git submodule and compiled directly into
the extension, so there is no external dependency on a system QuickJS install.

## Installation

The engine lives in a git submodule, so initialise it first:

```sh
git submodule update --init
pip install -e .
```

A C compiler and the Python development headers are required to build the
extension.

QuickJS uses GCC extensions and POSIX headers and cannot be compiled with
MSVC. On **Windows** install [mingw-w64](https://www.mingw-w64.org/) (for
example `choco install mingw`) and make sure `gcc` is on `PATH`; the build
selects the mingw toolchain automatically. Linux and macOS use the system
GCC/Clang.

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

JS `null` becomes Python `None`; JS `undefined` becomes the distinct
`quickjs.Undefined` singleton.

## More features

```python
import quickjs

ctx = quickjs.Context()

# Compile once, run many times (and serialise to bytecode)
compiled = ctx.compile("Math.PI * r * r")
blob = compiled.write_object()
restored = ctx.read_object(blob)

# ArrayBuffer / TypedArray access
buf = ctx.new_array_buffer(b"\x01\x02\x03")
ctx.eval("new Uint8Array([4, 5, 6])").to_bytes()      # b'\x04\x05\x06'

# Embed an arbitrary Python object opaquely inside JS
handle = ctx.new_host_object(open("data.txt"))         # round-trips unchanged

# Accessor properties backed by Python callables
obj = ctx.new_object()
obj.define_property("now", get=lambda: __import__("time").time())

# Custom ES module loader
ctx.set_module_loader(lambda name: MODULES.get(name))
ctx.eval("import { x } from 'mod';", module=True)

# Engine memory counters
ctx.runtime.compute_memory_usage()                     # -> dict

# Context manager
with quickjs.Context() as c:
    c.eval("1 + 1")
```

## Testing

```sh
pip install pytest
pytest -q
```

## License

MIT. The vendored QuickJS sources are under the MIT license; see
`vendor/quickjs/LICENSE`.
