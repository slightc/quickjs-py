# TODO

Task tracker for quickjs-py. Check items off as completed.

## Phase 0 — Project scaffolding
- [x] Write `CLAUDE.md` and `TODO.md`
- [x] Add `pyproject.toml` (build-system, project metadata, deps, tool config)
- [x] Add `setup.py` with C extension definition
- [x] Create `src/quickjs/` package skeleton with `__init__.py`
- [x] Add QuickJS (Bellard upstream) as a git submodule under `vendor/quickjs`
- [x] Verify the extension builds and imports

## Phase 1 — QuickJS API coverage (`_quickjs.c`)
Done — exposed through the `Runtime` / `Context` / `Value` wrappers:
- [x] Runtime: create/free, memory limit, max stack size, GC threshold, run GC
- [x] Context: create/free, bound to a runtime, global object access
- [x] Value lifetime: one owned JS reference per `Value`, freed on dealloc
- [x] Type predicates: object/function/array/string/number/bool/null/
      undefined/symbol/error/constructor
- [x] Conversions: to int/float/bool/str, deep `to_python()`
- [x] Property access: get/set/has/delete/keys, indexing, `length`
- [x] Eval: `JS_Eval` with global/module/strict/compile-only flags
- [x] Function calls: `JS_Call`, `JS_CallConstructor`, `this` binding
- [x] Exceptions: JS throws surface as Python `JSError` (+ `js_stack`)
- [x] JSON: `JS_ParseJSON`, `JS_JSONStringify`
- [x] Promises/jobs: `JS_ExecutePendingJob`, `JS_IsJobPending`
- [x] Interrupt handler: `JS_SetInterruptHandler`
- [x] C functions: Python callables exposed to JS via `JS_NewCFunctionData`

- [x] Classes / opaque pointers / finalizers: host objects embed arbitrary
      Python objects in JS (`Context.new_host_object`, `JS_NewClass`)
- [x] TypedArrays / ArrayBuffer: `Value.to_bytes`, `Context.new_array_buffer`
- [x] Modules: custom ES module loader hook (`Context.set_module_loader`)
- [x] Bytecode: `Context.compile` / `eval_function` / `read_object`,
      `Value.write_object` (`JS_ReadObject` / `JS_WriteObject`)
- [x] `Context.get_exception` on the public API
- [x] `JS_DefineProperty*` with data/getter/setter descriptors
      (`Value.define_property`)
- [x] Memory usage reporting (`Runtime.compute_memory_usage`)

Intentionally kept internal:
- [ ] Atoms (`JS_NewAtom`, ...) - all public APIs accept plain `str`/`int`
      keys, so a raw atom surface with manual lifetime is not exposed.

## Phase 2 — High-level ergonomics
- [x] `Runtime` / `Context` / `Value` object model
- [x] Automatic Python <-> JS type conversion
      (None/bool/int/float/str/bytes/list/dict/callable)
- [x] JS functions usable as callable Python objects
- [x] Python callables exposed to JS (callbacks)
- [x] `JSError` mapping JS errors, with stack trace attribute
- [x] Convenience top-level `eval()`
- [x] Distinct `Undefined` sentinel (null -> None, undefined -> Undefined)
- [x] Module import support via the custom module loader
- [x] Context manager (`with`) support on `Runtime` and `Context`

## Phase 3 — Packaging
- [x] `pyproject.toml` metadata
- [x] Type stubs (`py.typed`, `_quickjs.pyi`)
- [x] MANIFEST so vendored sources ship in the sdist
- [x] `cibuildwheel` config for Linux/macOS/Windows wheels
- [ ] Publish to PyPI

## Phase 4 — Testing & CI
- [x] pytest suite (eval, conversions, callbacks, context, runtime, advanced)
- [x] Memory-leak / refcount regression tests
- [x] GitHub Actions: build matrix, test, lint, wheel build
- [x] `ruff` lint + format checks in CI
- [x] Coverage reporting

## Phase 5 — Docs & release
- [x] README usage docs
- [ ] API reference docs
- [ ] Examples directory
- [ ] CHANGELOG
- [ ] Tag and publish first release to PyPI
