"""Using JavaScript `class` declarations from Python."""

import quickjs

ctx = quickjs.Context()

# A `class` declaration is a statement, so wrap it in parentheses (or assign
# it) to get the class object back out of `eval`.
Point = ctx.eval("""
(class Point {
    constructor(x, y) {
        this.x = x;
        this.y = y;
    }
    norm() {
        return Math.hypot(this.x, this.y);
    }
    static origin() {
        return new Point(0, 0);
    }
    get label() {
        return `(${this.x}, ${this.y})`;
    }
})
""")
print(Point.is_constructor)  # True

# Instantiate it with `new` via call_constructor().
p = Point.call_constructor(3, 4)
print(p["x"], p["y"])  # 3 4

# Methods live on the prototype; fetch one and call it with `this` bound to
# the instance.
print(p["norm"].call(this=p))  # 5

# A getter is read like a plain property.
print(p["label"])  # (3, 4)

# Static methods live on the class object itself.
o = Point["origin"].call(this=Point)
print(o["x"], o["y"])  # 0 0

# Inheritance: a subclass referring to `Point` via `extends` must see it in
# scope, so define both classes together and hand back the subclass.
Point3D = ctx.eval("""
(() => {
    class Point extends Object {
        constructor(x, y) { super(); this.x = x; this.y = y; }
        norm() { return Math.hypot(this.x, this.y); }
    }
    class Point3D extends Point {
        constructor(x, y, z) { super(x, y); this.z = z; }
        norm() { return Math.hypot(super.norm(), this.z); }
    }
    return Point3D;
})()
""")
q = Point3D.call_constructor(2, 3, 6)
print(q["norm"].call(this=q))  # 7

# Pass an instance back into JS and use it there (e.g. with `instanceof`).
ctx.set("p", p)
print(ctx.eval("p instanceof Object"))  # True

# Convert an instance to a plain Python dict. Only own enumerable properties
# are included; methods and getters live on the prototype and are skipped.
print(p.to_python())  # {'x': 3, 'y': 4}
