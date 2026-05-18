# API reference

`quickjs-py` exposes three engine objects — `Runtime`, `Context` and `Value` —
plus a few module-level helpers. This document is the reference for the public
surface. For a guided introduction see `README.md`; for runnable code see the
`examples/` directory.

## Module: `quickjs`

| Name | Kind | Description |
|------|------|-------------|
| `Runtime` | class | A QuickJS runtime: owns the GC heap and engine limits. |
| `Context` | class | A JavaScript execution context bound to a `Runtime`. |
| `Value` | class | A wrapper around a single owned JS reference. |
| `JSError` | exception | Raised when JavaScript code throws. |
| `QuickJSError` | exception | Base class for all engine errors; parent of `JSError`. |
| `Undefined` | singleton | The distinct sentinel for JS `undefined`. |
| `UndefinedType` | class | The type of `Undefined`. |
| `eval(code, **kwargs)` | function | Evaluate a snippet in a throwaway context. |
| `quickjs_version` | str | Version string of the vendored QuickJS engine. |
| `__version__` | str | Version of the `quickjs-py` package. |

### `quickjs.eval(code, **kwargs)`

Evaluate a snippet of JavaScript in a fresh, throwaway `Context` and return the
result. Extra keyword arguments are forwarded to `Context.eval`. Convenient for
one-off use; create and reuse a `Context` for repeated evaluation.

```python
import quickjs
quickjs.eval("1 + 2")            # -> 3
quickjs.eval("export const x = 1", module=True)
```

## Type conversion

Values cross the Python/JS boundary automatically.

| Python | JavaScript |
|--------|------------|
| `None` | `null` |
| `quickjs.Undefined` | `undefined` |
| `bool` | `boolean` |
| `int` | `number` / `bigint` |
| `float` | `number` |
| `str` | `string` |
| `bytes` | `ArrayBuffer` |
| `list` / `tuple` | `Array` |
| `dict` | `Object` |
| callable | `function` |

JS objects, arrays and functions returned to Python are wrapped as `Value`.
Primitives (numbers, strings, booleans, `null`, `undefined`) are converted to
their native Python equivalents directly. Call `Value.to_python()` to
recursively convert a wrapped array/object into native `list`/`dict`.

JS `null` becomes `None`; JS `undefined` becomes the distinct
`quickjs.Undefined` singleton (which is falsy but is not `None`).

## Class: `Runtime`

A runtime owns the garbage-collected JS heap and engine-wide limits. A single
runtime can host multiple contexts, but values and contexts cannot be shared
across runtimes.

### `Runtime()`

Construct a new runtime.

### `Runtime.new_context()`

Create and return a new `Context` bound to this runtime.

### `Runtime.set_memory_limit(limit)`

Cap the total bytes the runtime may allocate. Allocations past the limit cause
JS code to throw, surfacing as `JSError`.

### `Runtime.set_max_stack_size(size)`

Set the maximum JS call-stack size in bytes. Guards against unbounded
recursion.

### `Runtime.set_gc_threshold(threshold)`

Set the allocation threshold (in bytes) that triggers an automatic GC cycle.

### `Runtime.run_gc()`

Force a garbage-collection cycle immediately.

### `Runtime.set_interrupt_handler(handler)`

Register a callable invoked periodically while JS runs. Returning a truthy
value interrupts execution (surfacing as `JSError`). Pass `None` to clear.

```python
import time
deadline = time.time() + 1.0
rt.set_interrupt_handler(lambda: time.time() > deadline)
```

### `Runtime.compute_memory_usage()`

Return a `dict[str, int]` of engine memory counters (allocated bytes, object
counts, etc.).

### Context manager

`Runtime` supports `with`; the runtime is freed on exit.

## Class: `Context`

A context holds the global object and is where JavaScript actually runs.

### `Context(runtime=None)`

Construct a context. If `runtime` is omitted a fresh `Runtime` is created and
owned by the context.

### `Context.eval(code, filename="<eval>", module=False, strict=False, compile_only=False)`

Evaluate `code`. Returns the result, converted to Python (primitives) or
wrapped in a `Value`. With `module=True` the code is parsed as an ES module
(strict mode, supports `import`/`export`). With `compile_only=True` the code is
compiled but not run, returning a compiled `Value` (see `eval_function`).

### `Context.get_global()`

Return the global object as a `Value`.

### `Context.get(name)` / `Context.set(name, value)`

Read or write a property on the global object. `set` converts Python values to
JS automatically.

### `Context.parse_json(text, filename="<json>")`

Parse a JSON string into a JS value (`JS_ParseJSON`).

### `Context.new_object()` / `Context.new_array()`

Create an empty JS object or array, returned as a `Value`.

### `Context.new_function(callable, name="", length=0)`

Wrap a Python callable as a JS function `Value`. `length` sets the reported
arity.

### `Context.new_array_buffer(data)`

Create a JS `ArrayBuffer` from a `bytes` object.

### `Context.new_host_object(obj)`

Embed an arbitrary Python object opaquely inside JS. The object round-trips
through JS unchanged and is kept alive for the lifetime of the JS handle.

### `Context.compile(code, filename="<compile>", module=False)`

Compile `code` to a bytecode `Value` without running it.

### `Context.eval_function(value)`

Run a compiled `Value` (from `compile` or `read_object`) and return its result.

### `Context.read_object(data)` / `Value.write_object()`

Serialise and deserialise compiled bytecode. `write_object` returns `bytes`;
`read_object` reconstructs a `Value` from those bytes.

### `Context.execute_pending_job()`

Run one pending job (a microtask such as a resolved-promise callback). Returns
`True` if a job ran, `False` if the queue was empty.

### `Context.has_pending_job`

Boolean property: whether the job queue is non-empty.

### `Context.get_exception()`

Return the current pending exception as a Python value, or `None`.

### `Context.set_module_loader(loader)`

Install a custom ES module loader. `loader` is called with a module name and
must return the module's source as a `str`, or `None` if unknown. Pass `None`
to remove the loader.

### Context manager

`Context` supports `with`; the context (and an owned runtime) is freed on exit.

## Class: `Value`

A `Value` wraps exactly one owned JS reference and frees it on deallocation —
callers never call `JS_FreeValue`. A `Value` keeps its `Context` (and thus
`Runtime`) alive.

### Attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `tag` | int | The QuickJS type tag (see `JS_TAG_*` constants). |
| `context` | `Context` | The owning context. |
| `is_object` | bool | Whether the value is a JS object. |
| `is_function` | bool | Whether the value is callable. |
| `is_array` | bool | Whether the value is an array. |
| `is_string` | bool | Whether the value is a string. |
| `is_number` | bool | Whether the value is a number. |
| `is_bool` | bool | Whether the value is a boolean. |
| `is_null` | bool | Whether the value is `null`. |
| `is_undefined` | bool | Whether the value is `undefined`. |
| `is_symbol` | bool | Whether the value is a symbol. |
| `is_error` | bool | Whether the value is an `Error`. |
| `is_constructor` | bool | Whether the value can be used with `new`. |

### `Value.to_python(deep=True)`

Convert the value to a native Python object. With `deep=True` arrays and
objects are converted recursively into `list`/`dict`; with `deep=False` only
the top level is converted.

### `Value.get(key)` / `Value.set(key, value)`

Get or set a property. `key` may be a `str` or an `int` index. Also available
as `value[key]` / `value[key] = ...`.

### `Value.has(key)` / `Value.delete(key)`

Test for, or remove, a property.

### `Value.keys()`

Return the value's own enumerable property names as a `list[str]`.

### `Value.call(*args, this=Undefined)`

Call the value as a JS function with the given `this` binding. Also available
as `value(*args, this=...)`.

### `Value.call_constructor(*args)`

Invoke the value as a constructor (`new`), returning the constructed object.

### `Value.json()`

Return the value serialised with `JSON.stringify`, or `None` if not
serialisable.

### `Value.to_bytes()`

Return the raw bytes backing an `ArrayBuffer` or TypedArray.

### `Value.write_object()`

Serialise a compiled value to bytecode `bytes` (see `Context.read_object`).

### `Value.define_property(key, value=..., get=None, set=None, writable=False, enumerable=False, configurable=False)`

Define a property with a full descriptor. Supply either `value` (a data
property) or `get`/`set` (accessor callbacks backed by Python callables).

### Special methods

| Method | Behaviour |
|--------|-----------|
| `len(value)` | The `length` property. |
| `value[key]` | `Value.get(key)`. |
| `value[key] = x` | `Value.set(key, x)`. |
| `value(*args)` | `Value.call(*args)`. |

## Exceptions

### `QuickJSError`

Base class for all errors raised by the bindings.

### `JSError`

Raised when JavaScript code throws. Subclass of `QuickJSError`. The string form
is the JS error message; the `js_stack` attribute holds the JS stack trace when
one is available.

```python
try:
    ctx.eval('throw new Error("boom")')
except quickjs.JSError as exc:
    print(exc)            # Error: boom
    print(exc.js_stack)   # JS stack trace
```

## Constants

The C extension also exposes the raw QuickJS flag and tag constants for
advanced use:

- Eval flags: `JS_EVAL_TYPE_GLOBAL`, `JS_EVAL_TYPE_MODULE`,
  `JS_EVAL_FLAG_STRICT`, `JS_EVAL_FLAG_COMPILE_ONLY`.
- Type tags: `JS_TAG_INT`, `JS_TAG_BOOL`, `JS_TAG_NULL`, `JS_TAG_UNDEFINED`,
  `JS_TAG_OBJECT`, `JS_TAG_STRING`, `JS_TAG_FLOAT64`, `JS_TAG_SYMBOL`,
  `JS_TAG_EXCEPTION`.
