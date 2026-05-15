# TODO

Task tracker for quickjs-py. Check items off as completed.

## Phase 0 — Project scaffolding
- [ ] Write `CLAUDE.md` and `TODO.md`
- [ ] Add `pyproject.toml` (build-system, project metadata, deps, tool config)
- [ ] Add `setup.py` with C extension definition
- [ ] Create `src/quickjs/` package skeleton with `__init__.py`
- [ ] Vendor QuickJS (quickjs-ng) under `vendor/quickjs` as a git submodule,
      pinned to a stable tag
- [ ] Verify a trivial extension builds and imports

## Phase 1 — Raw C API bindings (`_quickjs.c`)
Expose the full QuickJS public API. Group by area:
- [ ] Runtime: `JS_NewRuntime`, `JS_FreeRuntime`, `JS_SetMemoryLimit`,
      `JS_SetMaxStackSize`, `JS_SetGCThreshold`, `JS_RunGC`, `JS_SetRuntimeInfo`,
      `JS_UpdateStackTop`
- [ ] Context: `JS_NewContext`, `JS_FreeContext`, `JS_DupContext`,
      `JS_GetRuntime`, `JS_NewContextRaw`, intrinsic registration funcs
- [ ] Value lifetime: `JS_DupValue`, `JS_FreeValue`, refcount helpers
- [ ] Value constructors: `JS_NewBool`, `JS_NewInt32/64`, `JS_NewFloat64`,
      `JS_NewString`, `JS_NewObject`, `JS_NewArray`, `JS_NewBigInt64`, etc.
- [ ] Type predicates: `JS_IsNumber`, `JS_IsString`, `JS_IsObject`,
      `JS_IsFunction`, `JS_IsArray`, `JS_IsError`, `JS_IsException`, ...
- [ ] Conversions: `JS_ToInt32/64`, `JS_ToFloat64`, `JS_ToBool`,
      `JS_ToString`, `JS_ToCString`, `JS_FreeCString`
- [ ] Property access: `JS_GetPropertyStr`, `JS_SetPropertyStr`,
      `JS_GetProperty`, `JS_SetProperty`, `JS_DefineProperty*`,
      `JS_HasProperty`, `JS_DeleteProperty`, `JS_GetOwnPropertyNames`
- [ ] Eval: `JS_Eval`, `JS_EvalThis`, `JS_EvalFunction`,
      `JS_GetGlobalObject`, module eval flags
- [ ] Function calls: `JS_Call`, `JS_CallConstructor`, `JS_Invoke`,
      `JS_NewCFunction`, `JS_NewCFunctionData`
- [ ] Exceptions: `JS_Throw`, `JS_GetException`, `JS_HasException`,
      `JS_ThrowTypeError`, `JS_ThrowRangeError`, `JS_ThrowSyntaxError`,
      `JS_ThrowInternalError`, `JS_ThrowReferenceError`
- [ ] Classes: `JS_NewClass`, `JS_NewClassID`, `JS_NewObjectClass`,
      `JS_SetOpaque`, `JS_GetOpaque`, finalizers
- [ ] Arrays/typed arrays: `JS_NewArrayBuffer`, `JS_GetArrayBuffer`,
      `JS_NewTypedArray`, `JS_GetTypedArrayBuffer`
- [ ] JSON: `JS_ParseJSON`, `JS_JSONStringify`
- [ ] Modules: `JS_NewCModule`, `JS_AddModuleExport`, `JS_SetModuleExport`,
      module loader hook
- [ ] Promises/jobs: `JS_NewPromiseCapability`, `JS_ExecutePendingJob`,
      `JS_IsJobPending`
- [ ] Interrupt handler: `JS_SetInterruptHandler`
- [ ] Bytecode: `JS_ReadObject`, `JS_WriteObject`, `JS_EvalFunction`
- [ ] Atoms: `JS_NewAtom`, `JS_FreeAtom`, `JS_AtomToString`, `JS_ValueToAtom`

## Phase 2 — High-level API
- [ ] `Runtime` class (memory limit, stack size, GC, interrupt callback)
- [ ] `Context` class (`eval`, `get`/`set` globals, `call`)
- [ ] Automatic Python <-> JS type conversion (`_typeconv.py`):
      None/bool/int/float/str/bytes/list/dict <-> JS equivalents
- [ ] Expose JS functions as callable Python objects
- [ ] Expose Python callables to JS (callbacks)
- [ ] `JSException` mapping JS errors with stack traces
- [ ] Module import support
- [ ] Async/Promise integration with the pending-job loop

## Phase 3 — Packaging
- [ ] Finalize `pyproject.toml` metadata (name, version, classifiers, urls)
- [ ] MANIFEST/package-data so vendored sources ship in the sdist
- [ ] `cibuildwheel` config for Linux/macOS/Windows wheels
- [ ] Type stubs (`py.typed`, `.pyi`) for the C extension
- [ ] README usage docs and install instructions

## Phase 4 — Testing & CI
- [ ] pytest suite covering raw API + high-level API
- [ ] Memory-leak / refcount regression tests
- [ ] GitHub Actions: build matrix, test, lint, wheel build
- [ ] `ruff` lint + format checks in CI
- [ ] Coverage reporting

## Phase 5 — Docs & release
- [ ] API reference docs
- [ ] Examples directory
- [ ] CHANGELOG
- [ ] Tag and publish first release to PyPI
