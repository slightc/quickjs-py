# CLAUDE.md

This file guides Claude Code (and human contributors) when working in this repository.

## Project: quickjs-py

Python bindings for [QuickJS](https://github.com/quickjs-ng/quickjs), a small and
embeddable JavaScript engine. The goal is to expose the **full QuickJS C API** to
Python, while also providing an ergonomic high-level interface.

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
│   └── quickjs/         # vendored QuickJS source (quickjs-ng), git submodule
├── src/
│   └── quickjs/
│       ├── __init__.py  # public package API
│       ├── _quickjs.*.so / .pyd   # compiled C extension (built)
│       ├── _quickjs.c   # C extension: raw 1:1 bindings to QuickJS C API
│       ├── runtime.py   # high-level Runtime wrapper
│       ├── context.py   # high-level Context wrapper
│       ├── errors.py     # JSException and friends
│       └── _typeconv.py # Python <-> JSValue conversion helpers
└── tests/
    ├── test_raw_api.py  # raw C API coverage
    ├── test_runtime.py
    ├── test_context.py
    ├── test_eval.py
    ├── test_conversions.py
    └── test_callbacks.py
```

### Layers

- **Raw layer** (`_quickjs` C extension): thin 1:1 wrappers around QuickJS C
  functions. Names mirror QuickJS (`JS_NewRuntime`, `JS_Eval`, ...). Memory and
  refcount semantics are exposed; callers are responsible for `JS_FreeValue`.
- **High-level layer** (`runtime.py`, `context.py`): RAII-style objects that own
  runtimes/contexts, handle refcounting, convert types automatically, and raise
  `JSException` on JS errors.

## Engine choice

Use **quickjs-ng** (the actively maintained fork). Vendor it as a git submodule
under `vendor/quickjs` pinned to a known-good tag.

## Build

- Build backend: `setuptools` with a C extension declared in `setup.py`.
- The extension compiles the vendored QuickJS sources directly (no system dep).
- `pip install -e .` for development builds.

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
