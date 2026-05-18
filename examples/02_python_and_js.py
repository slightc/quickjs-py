"""Moving values across the Python/JS boundary."""

import quickjs

ctx = quickjs.Context()

# Python values flow into JS automatically.
ctx.set("data", {"name": "Ada", "scores": [90, 95, 100]})
print(ctx.eval("data.name"))  # Ada
print(ctx.eval("data.scores.reduce((a, b) => a + b, 0)"))  # 285

# bytes become an ArrayBuffer; read raw bytes back with to_bytes().
buf = ctx.new_array_buffer(b"\x01\x02\x03")
ctx.set("buf", buf)
print(ctx.eval("new Uint8Array(buf).length"))  # 3
print(ctx.eval("new Uint8Array([4, 5, 6])").to_bytes())  # b'\x04\x05\x06'

# JS null -> None, JS undefined -> the distinct Undefined sentinel.
print(ctx.eval("null") is None)  # True
print(ctx.eval("undefined") is quickjs.Undefined)  # True
