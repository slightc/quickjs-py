# Examples

Runnable examples for `quickjs-py`. Install the package first
(`pip install -e .` from the repository root), then run any file directly:

```sh
python examples/01_basic_eval.py
```

| File | Shows |
|------|-------|
| `01_basic_eval.py` | One-off `eval` and a reusable `Context`. |
| `02_python_and_js.py` | Passing values both ways, calling Python from JS. |
| `03_callbacks.py` | Exposing Python callables and JS functions. |
| `04_errors.py` | Handling JS exceptions as Python exceptions. |
| `05_modules.py` | A custom ES module loader. |
| `06_bytecode.py` | Compiling once and serialising to bytecode. |
| `07_limits.py` | Memory limits and interrupt handlers. |
| `08_promises.py` | Promise/job queue, `async_eval` and the `await_promise` asyncio bridge. |
| `09_benchmark_bytecode_vs_eval.py` | Benchmark: `read_object` rebuild vs. direct `eval`. |
| `10_js_classes.py` | Defining, instantiating and subclassing JS `class`es. |
