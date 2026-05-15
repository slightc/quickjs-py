# CLAUDE.md

This file guides Claude Code (and human contributors) when working in this repository.

## Project: quickjs-py

Python bindings for [QuickJS](https://github.com/bellard/quickjs), Fabrice
Bellard's small and embeddable JavaScript engine. The goal is to expose the
**full QuickJS C API** to Python, while also providing an ergonomic high-level
interface.

### Goals

1. Expose every public QuickJS C API symbol (`JS_*`, `JSValue`, `JSContext`,
   `JSRuntime`, etc.) to Python.
2. Provide a high-level, Pythonic API (`Runtime`, `Context`) layered on top of
   the raw bindings.
3. Ship as a proper installable package (`pip install quickjs-py`) with wheels
   built for common platforms.
4. Be well tested (pytest) and CI-verified.

## Architecture

```
quickjs-py/
├── CLAUDE.md            # this file
├── TODO.md              # task tracker
├── README.md
├── LICENSE
├── pyproject.toml       # build config (setuptools + build-backend)
├── setup.py             # C extension build definition
├── vendor/
│   └── quickjs/         # QuickJS source (Bellard upstream), git submodule
├── src/
│   └── quickjs/
│       ├── __init__.py  # public package API
│       ├── _quickjs.c   # C extension: Runtime/Context/Value wrappers
│       ├── _quickjs.pyi # type stubs for the C extension
│       ├── _quickjs.*.so / .pyd   # compiled C extension (built)
│       └── py.typed     # PEP 561 marker
└── tests/
    ├── conftest.py
    ├── test_runtime.py
    ├── test_context.py
    ├── test_eval.py
    ├── test_conversions.py
    └── test_callbacks.py
```

### Layers

- **C extension** (`_quickjs`): object-oriented wrappers around the QuickJS C
  API exposed as three types — `Runtime`, `Context`, `Value`. The extension
  owns exactly one JS reference per `Value` and frees it on deallocation, so
  callers never deal with `JS_FreeValue` directly. It performs automatic
  Python <-> JS type conversion and raises `JSError` when JS throws.
- **Public package** (`quickjs/__init__.py`): re-exports the C types and adds
  convenience helpers (e.g. the top-level `eval()`).

Ownership chain: `Value` holds a reference to its `Context`, which holds a
reference to its `Runtime`. This guarantees the engine outlives every value
derived from it, so teardown order is always correct.

## Engine choice

Use **Bellard's upstream QuickJS** (https://github.com/bellard/quickjs). The
engine is included as a git submodule under `vendor/quickjs`, pinned to a
known-good commit. After cloning the repository, run
`git submodule update --init` before building. Compiled translation units:
`quickjs.c`, `libregexp.c`, `libunicode.c`, `cutils.c`, `dtoa.c`. The engine
version is read from `vendor/quickjs/VERSION` and passed as `CONFIG_VERSION`.

## Build

- Build backend: `setuptools` with a C extension declared in `setup.py`.
- The extension compiles the vendored QuickJS sources directly (no system dep).
- `pip install -e .` for development builds.
- QuickJS needs a GCC/Clang toolchain (computed-goto dispatch, POSIX headers)
  and cannot be built with MSVC. On Windows the build forces the `mingw32`
  compiler via a custom `build_ext`; `gcc` (mingw-w64) must be on `PATH`.

## Testing

- Framework: `pytest`.
- Run: `pytest -q`.
- Every QuickJS API wrapper must have at least one test.
- High-level API must cover: eval, type conversion (both directions),
  callbacks/closures, exceptions, modules, memory limits, interrupts.

## Conventions

- C code: follow QuickJS style; keep raw bindings mechanical and predictable.
- Python: type-hinted, `ruff`-clean, formatted with `ruff format`.
- Never leak `JSValue` references — every `JS_Free*` must be paired.
- Keep the raw layer dumb; put ergonomics in the high-level layer.

## Commands

- Build/install dev: `pip install -e .`
- Test: `pytest -q`
- Lint: `ruff check . && ruff format --check .`

## Git

- Develop on branch `claude/quickjs-python-bindings-Nueff`.
- Commit in logical, reviewable chunks.
