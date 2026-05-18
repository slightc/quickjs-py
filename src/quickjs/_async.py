"""Asyncio bridge for QuickJS promises.

QuickJS resolves promises by draining a job (microtask) queue. This module
adapts that queue to :mod:`asyncio` so a JS ``Promise`` can be awaited from a
Python coroutine without freezing the event loop: jobs are pumped one at a
time, yielding control back to the loop between each so other tasks keep
running.
"""

from __future__ import annotations

import asyncio
from typing import Any

from ._quickjs import Context, QuickJSError, Value


async def await_promise(context: Context, value: Any) -> Any:
    """Await a QuickJS ``Promise`` from within an asyncio coroutine.

    ``value`` is normally a :class:`~quickjs.Value` wrapping a JS promise. The
    coroutine pumps ``context``'s job queue until the promise settles, then
    returns its fulfilled value or raises :class:`~quickjs.JSError` if it
    rejected. Non-promise values are returned unchanged, mirroring the
    behaviour of ``await`` on a non-thenable in JavaScript.
    """
    while isinstance(value, Value) and value.is_promise:
        while value.promise_state == "pending":
            if context.has_pending_job:
                context.execute_pending_job()
                await asyncio.sleep(0)
            else:
                raise QuickJSError("promise is still pending and the job queue is empty")
        value = value.result()
    return value
