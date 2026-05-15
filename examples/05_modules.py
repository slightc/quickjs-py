"""Custom ES module loader."""

import quickjs

ctx = quickjs.Context()

# An in-memory module registry.
MODULES = {
    "math/area": "export const circle = r => Math.PI * r * r;",
    "greeting": "export default name => `hello, ${name}`;",
}

# The loader is called with a module name and returns its source (or None).
ctx.set_module_loader(lambda name: MODULES.get(name))

ctx.eval(
    """
    import { circle } from 'math/area';
    import greet from 'greeting';
    globalThis.result = { area: circle(2), msg: greet('Ada') };
    """,
    module=True,
)

print(ctx.get("result").to_python())
# {'area': 12.566..., 'msg': 'hello, Ada'}
