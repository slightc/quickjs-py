# Changelog

All notable changes to `quickjs-py` are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- `Context.async_eval(code, ...)` and the top-level `quickjs.async_eval()`
  helper: evaluate code that may use top-level `await`, pumping the job queue
  until the result settles. Rejections raise `JSError`; a promise that can
  never settle raises `QuickJSError`.
- `Context.await_promise(value)`: an asyncio-compatible coroutine that drives
  the job queue cooperatively, yielding to the event loop between jobs, so a
  QuickJS promise can be `await`ed from inside an `asyncio` coroutine.
- `Value.is_promise` and `Value.promise_state` for inspecting promises, and
  `Value.result()` to settle a promise synchronously.

## [0.1.0] - 2026-05-15

Initial release. Python bindings for Fabrice Bellard's QuickJS engine,
vendored as a git submodule and compiled directly into the C extension.

### Added

- `Runtime`, `Context` and `Value` engine objects in the `quickjs._quickjs`
  C extension, with one owned JS reference per `Value` freed on deallocation.
- Automatic Python <-> JS type conversion for `None`/`Undefined`, `bool`,
  `int`, `float`, `str`, `bytes`, `list`/`tuple`, `dict` and callables.
- `eval` with global/module/strict/compile-only flags; persistent context
  state across evaluations.
- Function calls with `this` binding, constructor invocation, and JS
  functions usable as callable Python objects.
- Python callables exposed to JS as functions and accessor callbacks.
- JS exceptions surfaced as `JSError` (subclass of `QuickJSError`) with a
  `js_stack` attribute.
- Property access: get/set/has/delete/keys, indexing and `length`.
- JSON parsing and stringification (`Context.parse_json`, `Value.json`).
- Promise/job queue control (`execute_pending_job`, `has_pending_job`).
- Interrupt handler and engine limits (memory limit, max stack size,
  GC threshold, manual GC, memory-usage reporting).
- Host objects embedding arbitrary Python objects (`Context.new_host_object`).
- TypedArray / ArrayBuffer support (`Value.to_bytes`,
  `Context.new_array_buffer`).
- Custom ES module loader hook (`Context.set_module_loader`).
- Bytecode compilation and serialisation (`Context.compile`, `eval_function`,
  `read_object`, `Value.write_object`).
- Full property descriptors via `Value.define_property`.
- Context-manager (`with`) support on `Runtime` and `Context`.
- Top-level `quickjs.eval()` convenience helper and distinct `Undefined`
  sentinel.
- Type stubs (`_quickjs.pyi`, `py.typed`) for the C extension.
- Packaging: `pyproject.toml` metadata, sdist manifest for vendored sources,
  and `cibuildwheel` configuration for Linux/macOS/Windows wheels.
- pytest suite (eval, conversions, callbacks, context, runtime, advanced,
  memory/refcount regression) and GitHub Actions CI with `ruff` lint/format
  checks and coverage reporting.
- Documentation: README, API reference (`docs/api.md`) and runnable
  `examples/`.

[Unreleased]: https://github.com/slightc/quickjs-py/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/slightc/quickjs-py/releases/tag/v0.1.0
