"""quickjs-py - Python bindings for the QuickJS JavaScript engine.

The package is split into two layers:

* the :mod:`quickjs._quickjs` C extension exposes the low-level engine objects
  (:class:`Runtime`, :class:`Context`, :class:`Value`);
* this module re-exports them and adds small convenience helpers.

Basic usage::

    import quickjs

    print(quickjs.eval("1 + 2"))          # -> 3

    ctx = quickjs.Context()
    ctx.set("greet", lambda name: f"hi {name}")
    print(ctx.eval("greet('world')"))     # -> 'hi world'
"""

from __future__ import annotations

from ._quickjs import (
    Context,
    JSError,
    QuickJSError,
    Runtime,
    Value,
    quickjs_version,
)

__all__ = [
    "Runtime",
    "Context",
    "Value",
    "JSError",
    "QuickJSError",
    "eval",
    "quickjs_version",
    "__version__",
]

__version__ = "0.1.0"


def eval(code: str, **kwargs: object) -> object:
    """Evaluate a snippet of JavaScript in a throwaway context.

    Convenience wrapper; for repeated evaluation create a :class:`Context`
    and reuse it. Extra keyword arguments are forwarded to
    :meth:`Context.eval`.
    """
    return Context().eval(code, **kwargs)
