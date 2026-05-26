/*
 * _quickjs - low-level CPython extension wrapping Bellard's QuickJS engine.
 *
 * Exposes three object types:
 *   Runtime  -> JSRuntime
 *   Context  -> JSContext
 *   Value    -> a single owned JSValue
 *
 * The extension owns exactly one JS reference per Value object and frees it
 * on deallocation. Primitive JS values are converted to native Python objects
 * on the way out; objects/functions/symbols stay wrapped as Value.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "quickjs.h"

/* ------------------------------------------------------------------ */
/* Type declarations                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    PyObject_HEAD
    JSRuntime *rt;
    PyObject *interrupt_cb; /* callable or NULL */
    /* Common atoms cached for the lifetime of the runtime to avoid the
     * per-call JS_NewAtom("length") / JS_FreeAtom roundtrip in hot paths.
     * Initialised lazily on first Context creation; JS_ATOM_NULL until
     * then. Freed via JS_FreeAtomRT in Runtime_dealloc. */
    JSAtom atom_length;
    JSAtom atom_name;
    JSAtom atom_stack;
    JSAtom atom_value;
    JSAtom atom_BigInt;
} RuntimeObject;

typedef struct {
    PyObject_HEAD
    JSContext *ctx;
    RuntimeObject *runtime;
    PyObject *module_loader; /* callable(name) -> source str, or NULL */
    PyObject *module_normalizer; /* callable(base, name) -> str, or NULL */
    /* Cached global object: JS_GetGlobalObject otherwise dups + frees on
     * every ctx.get/ctx.set. JS_UNDEFINED until first use. */
    JSValue global;
} ContextObject;

typedef struct {
    PyObject_HEAD
    JSValue val;
    ContextObject *context;
} ValueObject;

static PyTypeObject Runtime_Type;
static PyTypeObject Context_Type;
static PyTypeObject Value_Type;
static PyTypeObject Undefined_Type;

static PyObject *make_context(RuntimeObject *runtime);
static PyObject *make_py_host(ContextObject *context, PyObject *obj);

static PyObject *QuickJSError;  /* base exception */
static PyObject *JSError;       /* raised when JS throws */
static PyObject *JSUndefined;   /* the singleton JS `undefined` */

/* Class id for objects that opaquely embed a Python object inside JS.
 * Shared process-wide; the class is registered into every Runtime. */
static JSClassID py_host_class_id;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static PyObject *js_to_py(ContextObject *context, JSValueConst val);
static int py_to_js(ContextObject *context, PyObject *obj, JSValue *out);
static PyObject *Value_wrap(ContextObject *context, JSValue val);
static PyObject *make_js_function(ContextObject *context, PyObject *callable,
                                  const char *name, int length);
static JSValueConst context_global(ContextObject *self);

/* ------------------------------------------------------------------ */
/* Error helpers                                                        */
/* ------------------------------------------------------------------ */

/* Pull the pending JS exception, raise it as a Python JSError, return NULL. */
/* Raise a Python JSError from the JS value `exc`, which is consumed. */
static PyObject *raise_js_from_value(ContextObject *context, JSValue exc)
{
    JSContext *ctx = context->ctx;
    PyObject *message = NULL;
    PyObject *stack = NULL;

    const char *msg = JS_ToCString(ctx, exc);
    if (msg) {
        message = PyUnicode_DecodeUTF8(msg, strlen(msg), "surrogatepass");
        JS_FreeCString(ctx, msg);
    }
    if (JS_IsError(ctx, exc)) {
        JSValue st = JS_GetProperty(ctx, exc, context->runtime->atom_stack);
        if (!JS_IsUndefined(st) && !JS_IsException(st)) {
            const char *s = JS_ToCString(ctx, st);
            if (s) {
                stack = PyUnicode_DecodeUTF8(s, strlen(s), "surrogatepass");
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, exc);

    if (!message) {
        PyErr_Clear();
        message = PyUnicode_FromString("<unknown JS error>");
    }
    PyObject *args = PyTuple_Pack(1, message ? message : Py_None);
    PyObject *err = NULL;
    if (args) {
        err = PyObject_CallObject(JSError, args);
        Py_DECREF(args);
    }
    if (err) {
        if (stack) {
            PyObject_SetAttrString(err, "js_stack", stack);
        }
        PyErr_SetObject(JSError, err);
        Py_DECREF(err);
    }
    Py_XDECREF(message);
    Py_XDECREF(stack);
    return NULL;
}

/* Raise a Python JSError from the context's pending exception. */
static PyObject *raise_js_exception(ContextObject *context)
{
    return raise_js_from_value(context, JS_GetException(context->ctx));
}

/* ------------------------------------------------------------------ */
/* Value object                                                        */
/* ------------------------------------------------------------------ */

/* Create a Value that takes ownership of `val` (one JS reference). */
static PyObject *Value_wrap(ContextObject *context, JSValue val)
{
    ValueObject *self = PyObject_New(ValueObject, &Value_Type);
    if (!self) {
        JS_FreeValue(context->ctx, val);
        return NULL;
    }
    self->val = val;
    self->context = context;
    Py_INCREF(context);
    return (PyObject *)self;
}

static void Value_dealloc(ValueObject *self)
{
    if (self->context) {
        JS_FreeValue(self->context->ctx, self->val);
        Py_DECREF(self->context);
    }
    PyObject_Free(self);
}

/* ------------------------------------------------------------------ */
/* Conversion: JS -> Python                                             */
/* ------------------------------------------------------------------ */

static PyObject *js_string_to_py(JSContext *ctx, JSValueConst val)
{
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, val);
    if (!s) {
        return NULL;
    }
    PyObject *res = PyUnicode_DecodeUTF8(s, (Py_ssize_t)len, "surrogatepass");
    JS_FreeCString(ctx, s);
    return res;
}

static PyObject *js_bigint_to_py(ContextObject *context, JSValueConst val)
{
    /* JS_ToBigInt64 silently truncates out-of-range values, so convert via
     * the exact decimal string produced by BigInt.prototype.toString(). */
    PyObject *s = js_string_to_py(context->ctx, val);
    if (!s) {
        return NULL;
    }
    PyObject *res = PyLong_FromUnicodeObject(s, 10);
    Py_DECREF(s);
    return res;
}

/* Convert a borrowed JSValueConst to a Python object. */
static PyObject *js_to_py(ContextObject *context, JSValueConst val)
{
    JSContext *ctx = context->ctx;
    int tag = JS_VALUE_GET_NORM_TAG(val);

    switch (tag) {
    case JS_TAG_INT:
        return PyLong_FromLong(JS_VALUE_GET_INT(val));
    case JS_TAG_BOOL:
        return PyBool_FromLong(JS_VALUE_GET_BOOL(val));
    case JS_TAG_NULL:
        Py_RETURN_NONE;
    case JS_TAG_UNDEFINED:
        Py_INCREF(JSUndefined);
        return JSUndefined;
    case JS_TAG_FLOAT64:
        return PyFloat_FromDouble(JS_VALUE_GET_FLOAT64(val));
    case JS_TAG_STRING:
        return js_string_to_py(ctx, val);
    case JS_TAG_BIG_INT:
    case JS_TAG_SHORT_BIG_INT:
        return js_bigint_to_py(context, val);
    case JS_TAG_EXCEPTION:
        return raise_js_exception(context);
    default:
        /* A host object carries an embedded Python object: return it. */
        if (tag == JS_TAG_OBJECT) {
            PyObject *host = JS_GetOpaque(val, py_host_class_id);
            if (host) {
                Py_INCREF(host);
                return host;
            }
        }
        /* other objects, functions, symbols: keep a wrapped reference */
        return Value_wrap(context, JS_DupValue(ctx, val));
    }
}

/* Like js_to_py but consumes `*val` (used for owned results). */
static PyObject *js_to_py_consume(ContextObject *context, JSValue *val)
{
    JSContext *ctx = context->ctx;
    int tag = JS_VALUE_GET_NORM_TAG(*val);
    if (tag == JS_TAG_EXCEPTION) {
        JS_FreeValue(ctx, *val);
        return raise_js_exception(context);
    }
    if (tag == JS_TAG_OBJECT) {
        PyObject *host = JS_GetOpaque(*val, py_host_class_id);
        if (host) {
            Py_INCREF(host);
            JS_FreeValue(ctx, *val);
            return host;
        }
        return Value_wrap(context, *val);
    }
    if (tag == JS_TAG_SYMBOL || tag == JS_TAG_FUNCTION_BYTECODE ||
        tag == JS_TAG_MODULE) {
        return Value_wrap(context, *val);
    }
    PyObject *res = js_to_py(context, *val);
    JS_FreeValue(ctx, *val);
    return res;
}

/* Pump the job queue until `promise` settles. On fulfilment, store the
 * fulfilled JS value in *out (the caller takes ownership) and return 0. On
 * rejection raise JSError and return -1. Also returns -1 (with a Python
 * exception set) if `promise` is not a Promise, or if it stays pending after
 * the queue drains. `promise` is borrowed (the caller still owns it). */
static int settle_promise_value(ContextObject *context, JSValueConst promise,
                                JSValue *out)
{
    JSContext *ctx = context->ctx;
    JSRuntime *rt = context->runtime->rt;
    if (JS_PromiseState(ctx, promise) == (JSPromiseStateEnum)-1) {
        PyErr_SetString(PyExc_TypeError, "value is not a Promise");
        return -1;
    }
    while (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
        JSContext *jctx;
        int r = JS_ExecutePendingJob(rt, &jctx);
        if (r < 0) {
            raise_js_exception(context);
            return -1;
        }
        if (r == 0) {
            PyErr_SetString(QuickJSError,
                            "promise is still pending and the job queue "
                            "is empty");
            return -1;
        }
    }
    JSValue result = JS_PromiseResult(ctx, promise);
    if (JS_PromiseState(ctx, promise) == JS_PROMISE_REJECTED) {
        raise_js_from_value(context, result);
        return -1;
    }
    *out = result;
    return 0;
}

/* As above, but return the fulfilled value as a Python object. */
static PyObject *settle_promise(ContextObject *context, JSValueConst promise)
{
    JSValue result;
    if (settle_promise_value(context, promise, &result) < 0) {
        return NULL;
    }
    return js_to_py_consume(context, &result);
}

/* Recursively convert a JS value: arrays -> list, plain objects -> dict.
 * Functions and symbols stay wrapped as Value. `seen` tracks visited
 * objects so circular references raise instead of recursing forever. */
static PyObject *js_to_py_deep(ContextObject *context, JSValueConst val,
                               PyObject *seen)
{
    JSContext *ctx = context->ctx;
    if (JS_VALUE_GET_NORM_TAG(val) != JS_TAG_OBJECT) {
        return js_to_py(context, val);
    }
    if (JS_IsFunction(ctx, val)) {
        return Value_wrap(context, JS_DupValue(ctx, val));
    }

    PyObject *ptr = PyLong_FromVoidPtr(JS_VALUE_GET_PTR(val));
    if (!ptr) {
        return NULL;
    }
    if (PyDict_GetItemWithError(seen, ptr)) {
        Py_DECREF(ptr);
        PyErr_SetString(PyExc_ValueError, "circular reference in JS value");
        return NULL;
    }
    if (PyErr_Occurred() || PyDict_SetItem(seen, ptr, Py_None) < 0) {
        Py_DECREF(ptr);
        return NULL;
    }

    PyObject *result = NULL;
    if (JS_IsArray(ctx, val)) {
        JSValue lenv = JS_GetProperty(ctx, val, context->runtime->atom_length);
        int64_t n = 0;
        JS_ToInt64(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        result = PyList_New((Py_ssize_t)n);
        for (int64_t i = 0; result && i < n; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, val, (uint32_t)i);
            PyObject *p;
            if (JS_IsException(item)) {
                raise_js_exception(context);
                p = NULL;
            } else {
                p = js_to_py_deep(context, item, seen);
            }
            JS_FreeValue(ctx, item);
            if (!p) {
                Py_CLEAR(result);
                break;
            }
            PyList_SET_ITEM(result, (Py_ssize_t)i, p);
        }
    } else {
        result = PyDict_New();
        JSPropertyEnum *tab = NULL;
        uint32_t count = 0;
        if (result && JS_GetOwnPropertyNames(ctx, &tab, &count, val,
                          JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                JSValue pv = JS_GetProperty(ctx, val, tab[i].atom);
                const char *ks = JS_AtomToCString(ctx, tab[i].atom);
                PyObject *key = ks ? PyUnicode_DecodeUTF8(ks, strlen(ks),
                                                          "surrogatepass")
                                   : NULL;
                if (ks) {
                    JS_FreeCString(ctx, ks);
                }
                PyObject *pyval = NULL;
                if (key && !JS_IsException(pv)) {
                    pyval = js_to_py_deep(context, pv, seen);
                } else if (JS_IsException(pv)) {
                    raise_js_exception(context);
                }
                JS_FreeValue(ctx, pv);
                if (!key || !pyval || PyDict_SetItem(result, key, pyval) < 0) {
                    Py_XDECREF(key);
                    Py_XDECREF(pyval);
                    Py_CLEAR(result);
                    break;
                }
                Py_DECREF(key);
                Py_DECREF(pyval);
            }
            JS_FreePropertyEnum(ctx, tab, count);
        } else if (result) {
            Py_CLEAR(result);
            raise_js_exception(context);
        }
    }

    PyDict_DelItem(seen, ptr);
    Py_DECREF(ptr);
    return result;
}

/* ------------------------------------------------------------------ */
/* Conversion: Python -> JS                                             */
/* ------------------------------------------------------------------ */

/* Fill *out with a new JS reference. Returns 0 on success, -1 on error. */
static int py_to_js(ContextObject *context, PyObject *obj, JSValue *out)
{
    JSContext *ctx = context->ctx;

    if (obj == Py_None) {
        *out = JS_NULL;
        return 0;
    }
    if (obj == JSUndefined) {
        *out = JS_UNDEFINED;
        return 0;
    }
    if (PyObject_TypeCheck(obj, &Value_Type)) {
        ValueObject *v = (ValueObject *)obj;
        *out = JS_DupValue(ctx, v->val);
        return 0;
    }
    if (PyBool_Check(obj)) {
        *out = JS_NewBool(ctx, obj == Py_True);
        return 0;
    }
    if (PyLong_Check(obj)) {
        int overflow;
        long long ll = PyLong_AsLongLongAndOverflow(obj, &overflow);
        if (ll == -1 && PyErr_Occurred()) {
            return -1;
        }
        if (overflow == 0) {
            *out = JS_NewInt64(ctx, ll);
            return 0;
        }
        unsigned long long ull = PyLong_AsUnsignedLongLong(obj);
        if (!PyErr_Occurred()) {
            *out = JS_NewBigUint64(ctx, ull);
            return 0;
        }
        PyErr_Clear();
        /* fall back to decimal-string BigInt via JS */
        PyObject *s = PyObject_Str(obj);
        if (!s) {
            return -1;
        }
        const char *cs = PyUnicode_AsUTF8(s);
        JSValue jss = JS_NewString(ctx, cs);
        Py_DECREF(s);
        JSValueConst global = context_global(context);
        JSValue bigint = JS_GetProperty(ctx, global,
                                        context->runtime->atom_BigInt);
        JSValue res = JS_Call(ctx, bigint, JS_UNDEFINED, 1, &jss);
        JS_FreeValue(ctx, bigint);
        JS_FreeValue(ctx, jss);
        if (JS_IsException(res)) {
            raise_js_exception(context);
            return -1;
        }
        *out = res;
        return 0;
    }
    if (PyFloat_Check(obj)) {
        *out = JS_NewFloat64(ctx, PyFloat_AsDouble(obj));
        return 0;
    }
    if (PyUnicode_Check(obj)) {
        PyObject *enc = PyUnicode_AsEncodedString(obj, "utf-8", "surrogatepass");
        if (!enc) {
            return -1;
        }
        *out = JS_NewStringLen(ctx, PyBytes_AS_STRING(enc),
                               PyBytes_GET_SIZE(enc));
        Py_DECREF(enc);
        return 0;
    }
    if (PyBytes_Check(obj)) {
        *out = JS_NewArrayBufferCopy(ctx, (const uint8_t *)PyBytes_AS_STRING(obj),
                                     PyBytes_GET_SIZE(obj));
        return 0;
    }
    if (PyList_Check(obj) || PyTuple_Check(obj)) {
        Py_ssize_t n = PySequence_Fast_GET_SIZE(obj);
        JSValue arr = JS_NewArray(ctx);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PySequence_Fast_GET_ITEM(obj, i);
            JSValue jv;
            if (py_to_js(context, item, &jv) < 0) {
                JS_FreeValue(ctx, arr);
                return -1;
            }
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, jv);
        }
        *out = arr;
        return 0;
    }
    if (PyDict_Check(obj)) {
        JSValue o = JS_NewObject(ctx);
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            PyObject *kstr = PyObject_Str(key);
            if (!kstr) {
                JS_FreeValue(ctx, o);
                return -1;
            }
            PyObject *kenc = PyUnicode_AsEncodedString(kstr, "utf-8",
                                                       "surrogatepass");
            Py_DECREF(kstr);
            if (!kenc) {
                JS_FreeValue(ctx, o);
                return -1;
            }
            JSValue jv;
            if (py_to_js(context, value, &jv) < 0) {
                Py_DECREF(kenc);
                JS_FreeValue(ctx, o);
                return -1;
            }
            JS_SetPropertyStr(ctx, o, PyBytes_AS_STRING(kenc), jv);
            Py_DECREF(kenc);
        }
        *out = o;
        return 0;
    }
    if (PyCallable_Check(obj)) {
        JSValue fn;
        PyObject *wrapped = make_js_function(context, obj, "", 0);
        if (!wrapped) {
            return -1;
        }
        fn = JS_DupValue(ctx, ((ValueObject *)wrapped)->val);
        Py_DECREF(wrapped);
        *out = fn;
        return 0;
    }

    PyErr_Format(PyExc_TypeError,
                 "cannot convert Python object of type '%s' to a JS value",
                 Py_TYPE(obj)->tp_name);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Python callable -> JS function (trampoline)                          */
/* ------------------------------------------------------------------ */

/* The Python callable is embedded in func_data[0] as a py-host JS object,
 * so trampoline dispatch is a single JS_GetOpaque (O(1), no Python dict),
 * and the callable's lifetime is tied to the JS function's: when the
 * function is GC'd, func_data is freed, the holder's refcount drops to
 * zero, and py_host_finalizer Py_DECREFs the callable. Fixes both the
 * per-call dict lookup and the unbounded growth of the old callbacks
 * registry. */
static JSValue js_trampoline(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv,
                             int magic, JSValue *func_data)
{
    ContextObject *context = (ContextObject *)JS_GetContextOpaque(ctx);
    PyObject *callable = JS_GetOpaque(func_data[0], py_host_class_id);
    if (!callable) {
        return JS_ThrowInternalError(ctx, "quickjs-py: callback holder lost");
    }

    PyObject *args = PyTuple_New(argc);
    if (!args) {
        PyErr_Clear();
        return JS_ThrowOutOfMemory(ctx);
    }
    for (int i = 0; i < argc; i++) {
        PyObject *a = js_to_py(context, argv[i]);
        if (!a) {
            PyErr_Clear();
            Py_DECREF(args);
            return JS_ThrowInternalError(ctx, "quickjs-py: argument conversion failed");
        }
        PyTuple_SET_ITEM(args, i, a);
    }

    PyObject *result = PyObject_CallObject(callable, args);
    Py_DECREF(args);

    if (!result) {
        PyObject *type, *value, *tb;
        PyErr_Fetch(&type, &value, &tb);
        PyObject *msg = value ? PyObject_Str(value) : NULL;
        const char *cmsg = msg ? PyUnicode_AsUTF8(msg) : "Python callback raised";
        JSValue err = JS_ThrowInternalError(ctx, "%s", cmsg ? cmsg : "Python error");
        Py_XDECREF(msg);
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
        return err;
    }

    JSValue jv;
    if (py_to_js(context, result, &jv) < 0) {
        PyObject *value = PyErr_Occurred() ? PyObject_Str(PyErr_Occurred()) : NULL;
        PyErr_Clear();
        Py_XDECREF(value);
        Py_DECREF(result);
        return JS_ThrowInternalError(ctx, "quickjs-py: return value conversion failed");
    }
    Py_DECREF(result);
    return jv;
}

static PyObject *make_js_function(ContextObject *context, PyObject *callable,
                                  const char *name, int length)
{
    JSContext *ctx = context->ctx;

    /* The host object owns one Python reference to `callable`; its
     * finalizer Py_DECREFs when the JS GC reclaims it. */
    PyObject *holder = make_py_host(context, callable);
    if (!holder) {
        return NULL;
    }
    JSValue data = JS_DupValue(ctx, ((ValueObject *)holder)->val);
    Py_DECREF(holder);
    JSValue fn = JS_NewCFunctionData(ctx, js_trampoline, length, 0, 1, &data);
    JS_FreeValue(ctx, data);
    if (JS_IsException(fn)) {
        return raise_js_exception(context);
    }
    if (name && name[0]) {
        JSValue nv = JS_NewString(ctx, name);
        JS_DefinePropertyValueStr(ctx, fn, "name", nv, JS_PROP_CONFIGURABLE);
    }
    return Value_wrap(context, fn);
}

/* ------------------------------------------------------------------ */
/* Host objects: embed an arbitrary Python object inside a JS object    */
/* ------------------------------------------------------------------ */

static void py_host_finalizer(JSRuntime *rt, JSValue val)
{
    PyObject *obj = JS_GetOpaque(val, py_host_class_id);
    if (obj) {
        /* Finalizers run while we hold the GIL (GC is driven from Python). */
        Py_DECREF(obj);
    }
}

static JSClassDef py_host_class_def = {
    "PyObject",
    .finalizer = py_host_finalizer,
};

/* Create a JS object that opaquely owns a reference to `obj`. */
static PyObject *make_py_host(ContextObject *context, PyObject *obj)
{
    JSValue host = JS_NewObjectClass(context->ctx, (int)py_host_class_id);
    if (JS_IsException(host)) {
        return raise_js_exception(context);
    }
    Py_INCREF(obj);
    JS_SetOpaque(host, obj);
    return Value_wrap(context, host);
}

/* ------------------------------------------------------------------ */
/* ES module loader trampoline                                          */
/* ------------------------------------------------------------------ */

static char *js_module_normalize(JSContext *ctx, const char *base_name,
                                 const char *name, void *opaque)
{
    ContextObject *context = (ContextObject *)JS_GetContextOpaque(ctx);
    PyObject *res = PyObject_CallFunction(context->module_normalizer, "ss",
                                          base_name, name);
    if (!res) {
        PyObject *value = NULL, *type = NULL, *tb = NULL;
        PyErr_Fetch(&type, &value, &tb);
        PyObject *msg = value ? PyObject_Str(value) : NULL;
        const char *cmsg = msg ? PyUnicode_AsUTF8(msg) : NULL;
        JS_ThrowReferenceError(ctx,
                               "module normalizer failed for '%s' (base '%s'): %s",
                               name, base_name, cmsg ? cmsg : "error");
        Py_XDECREF(msg);
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
        return NULL;
    }
    if (!PyUnicode_Check(res)) {
        Py_DECREF(res);
        JS_ThrowTypeError(ctx, "module normalizer must return a string");
        return NULL;
    }
    Py_ssize_t out_len;
    const char *out = PyUnicode_AsUTF8AndSize(res, &out_len);
    if (!out) {
        Py_DECREF(res);
        PyErr_Clear();
        JS_ThrowTypeError(ctx, "module normalizer returned invalid utf-8");
        return NULL;
    }
    char *dup = js_malloc(ctx, (size_t)out_len + 1);
    if (!dup) {
        Py_DECREF(res);
        return NULL;
    }
    memcpy(dup, out, (size_t)out_len);
    dup[out_len] = '\0';
    Py_DECREF(res);
    return dup;
}

static JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque)
{
    ContextObject *context = (ContextObject *)JS_GetContextOpaque(ctx);
    if (!context || !context->module_loader) {
        JS_ThrowReferenceError(ctx, "no module loader set for '%s'",
                               module_name);
        return NULL;
    }
    PyObject *res = PyObject_CallFunction(context->module_loader, "s",
                                          module_name);
    if (!res) {
        PyObject *value = NULL, *type = NULL, *tb = NULL;
        PyErr_Fetch(&type, &value, &tb);
        PyObject *msg = value ? PyObject_Str(value) : NULL;
        const char *cmsg = msg ? PyUnicode_AsUTF8(msg) : NULL;
        JS_ThrowReferenceError(ctx, "module loader failed for '%s': %s",
                               module_name, cmsg ? cmsg : "error");
        Py_XDECREF(msg);
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
        return NULL;
    }
    if (res == Py_None) {
        Py_DECREF(res);
        JS_ThrowReferenceError(ctx, "module '%s' not found", module_name);
        return NULL;
    }
    PyObject *enc = PyUnicode_Check(res)
                        ? PyUnicode_AsEncodedString(res, "utf-8", "surrogatepass")
                        : NULL;
    Py_DECREF(res);
    if (!enc) {
        PyErr_Clear();
        JS_ThrowTypeError(ctx, "module loader must return a string");
        return NULL;
    }
    JSValue func = JS_Eval(ctx, PyBytes_AS_STRING(enc), PyBytes_GET_SIZE(enc),
                           module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    Py_DECREF(enc);
    if (JS_IsException(func)) {
        return NULL;
    }
    JSModuleDef *m = JS_VALUE_GET_PTR(func);
    JS_FreeValue(ctx, func);
    return m;
}

/* ------------------------------------------------------------------ */
/* Property key helpers                                                 */
/* ------------------------------------------------------------------ */

/* Returns a JSAtom (must be freed) for a Python str/int key, or JS_ATOM_NULL. */
static JSAtom py_key_to_atom(ContextObject *context, PyObject *key)
{
    JSContext *ctx = context->ctx;
    if (PyUnicode_Check(key)) {
        PyObject *enc = PyUnicode_AsEncodedString(key, "utf-8", "surrogatepass");
        if (!enc) {
            return JS_ATOM_NULL;
        }
        JSAtom atom = JS_NewAtomLen(ctx, PyBytes_AS_STRING(enc),
                                    PyBytes_GET_SIZE(enc));
        Py_DECREF(enc);
        return atom;
    }
    if (PyLong_Check(key)) {
        PyObject *s = PyObject_Str(key);
        if (!s) {
            return JS_ATOM_NULL;
        }
        const char *cs = PyUnicode_AsUTF8(s);
        JSAtom atom = cs ? JS_NewAtom(ctx, cs) : JS_ATOM_NULL;
        Py_DECREF(s);
        return atom;
    }
    PyErr_SetString(PyExc_TypeError, "property key must be str or int");
    return JS_ATOM_NULL;
}

/* ------------------------------------------------------------------ */
/* Value methods                                                        */
/* ------------------------------------------------------------------ */

#define VALUE_CTX(self) ((self)->context->ctx)

static PyObject *Value_get_tag(ValueObject *self, void *closure)
{
    return PyLong_FromLong(JS_VALUE_GET_NORM_TAG(self->val));
}

#define VALUE_PRED(slot, expr)                                            \
    static PyObject *Value_##slot(ValueObject *self, void *closure)        \
    {                                                                     \
        JSContext *ctx = VALUE_CTX(self);                                 \
        (void)ctx;                                                        \
        return PyBool_FromLong(expr);                                     \
    }

VALUE_PRED(is_object, JS_IsObject(self->val))
VALUE_PRED(is_function, JS_IsFunction(ctx, self->val))
VALUE_PRED(is_array, JS_IsArray(ctx, self->val))
VALUE_PRED(is_string, JS_IsString(self->val))
VALUE_PRED(is_number, JS_IsNumber(self->val))
VALUE_PRED(is_bool, JS_IsBool(self->val))
VALUE_PRED(is_null, JS_IsNull(self->val))
VALUE_PRED(is_undefined, JS_IsUndefined(self->val))
VALUE_PRED(is_symbol, JS_IsSymbol(self->val))
VALUE_PRED(is_error, JS_IsError(ctx, self->val))
VALUE_PRED(is_constructor, JS_IsConstructor(ctx, self->val))
VALUE_PRED(is_promise,
           JS_PromiseState(ctx, self->val) != (JSPromiseStateEnum)-1)

static PyObject *Value_promise_state(ValueObject *self, void *closure)
{
    switch (JS_PromiseState(VALUE_CTX(self), self->val)) {
    case JS_PROMISE_PENDING:
        return PyUnicode_FromString("pending");
    case JS_PROMISE_FULFILLED:
        return PyUnicode_FromString("fulfilled");
    case JS_PROMISE_REJECTED:
        return PyUnicode_FromString("rejected");
    default:
        Py_RETURN_NONE;
    }
}

static PyObject *Value_result(ValueObject *self, PyObject *args)
{
    return settle_promise(self->context, self->val);
}

static PyObject *Value_to_python(ValueObject *self, PyObject *args,
                                 PyObject *kwds)
{
    static char *kwlist[] = {"deep", NULL};
    int deep = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p:to_python", kwlist,
                                     &deep)) {
        return NULL;
    }
    if (!deep) {
        return js_to_py(self->context, self->val);
    }
    PyObject *seen = PyDict_New();
    if (!seen) {
        return NULL;
    }
    PyObject *res = js_to_py_deep(self->context, self->val, seen);
    Py_DECREF(seen);
    return res;
}

static PyObject *Value_str(ValueObject *self)
{
    JSContext *ctx = VALUE_CTX(self);
    JSValue s = JS_ToString(ctx, self->val);
    if (JS_IsException(s)) {
        return raise_js_exception(self->context);
    }
    PyObject *res = js_string_to_py(ctx, s);
    JS_FreeValue(ctx, s);
    return res;
}

static PyObject *Value_repr(ValueObject *self)
{
    PyObject *s = Value_str(self);
    if (!s) {
        PyErr_Clear();
        return PyUnicode_FromString("<quickjs.Value>");
    }
    PyObject *res = PyUnicode_FromFormat("<quickjs.Value %R>", s);
    Py_DECREF(s);
    return res;
}

static PyObject *Value_int(ValueObject *self)
{
    int64_t v;
    if (JS_ToInt64Ext(VALUE_CTX(self), &v, self->val) < 0) {
        return raise_js_exception(self->context);
    }
    return PyLong_FromLongLong(v);
}

static PyObject *Value_float(ValueObject *self)
{
    double v;
    if (JS_ToFloat64(VALUE_CTX(self), &v, self->val) < 0) {
        return raise_js_exception(self->context);
    }
    return PyFloat_FromDouble(v);
}

static int Value_bool(ValueObject *self)
{
    return JS_ToBool(VALUE_CTX(self), self->val);
}

static PyObject *Value_getprop(ValueObject *self, PyObject *key)
{
    JSContext *ctx = VALUE_CTX(self);
    JSAtom atom = py_key_to_atom(self->context, key);
    if (atom == JS_ATOM_NULL) {
        return NULL;
    }
    JSValue res = JS_GetProperty(ctx, self->val, atom);
    JS_FreeAtom(ctx, atom);
    if (JS_IsException(res)) {
        return raise_js_exception(self->context);
    }
    return js_to_py_consume(self->context, &res);
}

static int Value_setprop(ValueObject *self, PyObject *key, PyObject *value)
{
    JSContext *ctx = VALUE_CTX(self);
    JSAtom atom = py_key_to_atom(self->context, key);
    if (atom == JS_ATOM_NULL) {
        return -1;
    }
    if (value == NULL) {
        int r = JS_DeleteProperty(ctx, self->val, atom, JS_PROP_THROW);
        JS_FreeAtom(ctx, atom);
        if (r < 0) {
            raise_js_exception(self->context);
            return -1;
        }
        return 0;
    }
    JSValue jv;
    if (py_to_js(self->context, value, &jv) < 0) {
        JS_FreeAtom(ctx, atom);
        return -1;
    }
    int r = JS_SetPropertyInternal(ctx, self->val, atom, jv, self->val,
                                   JS_PROP_THROW);
    JS_FreeAtom(ctx, atom);
    if (r < 0) {
        raise_js_exception(self->context);
        return -1;
    }
    return 0;
}

static PyObject *Value_get(ValueObject *self, PyObject *args)
{
    PyObject *key;
    if (!PyArg_ParseTuple(args, "O:get", &key)) {
        return NULL;
    }
    return Value_getprop(self, key);
}

static PyObject *Value_set(ValueObject *self, PyObject *args)
{
    PyObject *key, *value;
    if (!PyArg_ParseTuple(args, "OO:set", &key, &value)) {
        return NULL;
    }
    if (Value_setprop(self, key, value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *Value_has(ValueObject *self, PyObject *args)
{
    PyObject *key;
    if (!PyArg_ParseTuple(args, "O:has", &key)) {
        return NULL;
    }
    JSContext *ctx = VALUE_CTX(self);
    JSAtom atom = py_key_to_atom(self->context, key);
    if (atom == JS_ATOM_NULL) {
        return NULL;
    }
    int r = JS_HasProperty(ctx, self->val, atom);
    JS_FreeAtom(ctx, atom);
    if (r < 0) {
        return raise_js_exception(self->context);
    }
    return PyBool_FromLong(r);
}

static PyObject *Value_delete(ValueObject *self, PyObject *args)
{
    PyObject *key;
    if (!PyArg_ParseTuple(args, "O:delete", &key)) {
        return NULL;
    }
    if (Value_setprop(self, key, NULL) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *Value_keys(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    JSPropertyEnum *tab = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &count, self->val,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return raise_js_exception(self->context);
    }
    PyObject *list = PyList_New(count);
    if (!list) {
        JS_FreePropertyEnum(ctx, tab, count);
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        const char *s = JS_AtomToCString(ctx, tab[i].atom);
        PyObject *str = s ? PyUnicode_DecodeUTF8(s, strlen(s), "surrogatepass")
                          : NULL;
        if (s) {
            JS_FreeCString(ctx, s);
        }
        if (!str) {
            Py_DECREF(list);
            JS_FreePropertyEnum(ctx, tab, count);
            return NULL;
        }
        PyList_SET_ITEM(list, i, str);
    }
    JS_FreePropertyEnum(ctx, tab, count);
    return list;
}

/* Most JS calls have very few arguments; a small stack buffer skips the
 * PyMem_Malloc/Free pair for the common case. Tuples and lists short-circuit
 * to borrowed-item access (PyTuple_GET_ITEM / PyList_GET_ITEM) so we also
 * avoid the per-item PySequence_GetItem incref/decref pair. */
#define JS_ARGV_STACK 8

static JSValue *build_js_argv(ContextObject *context, PyObject *seq,
                              JSValue *stackbuf, int stack_cap, int *argc)
{
    Py_ssize_t n;
    int is_tuple = PyTuple_CheckExact(seq);
    int is_list = !is_tuple && PyList_CheckExact(seq);
    if (is_tuple) {
        n = PyTuple_GET_SIZE(seq);
    } else if (is_list) {
        n = PyList_GET_SIZE(seq);
    } else {
        n = PySequence_Size(seq);
        if (n < 0) {
            return NULL;
        }
    }
    JSValue *argv;
    if (n <= stack_cap) {
        argv = stackbuf;
    } else {
        argv = PyMem_Malloc(sizeof(JSValue) * (size_t)n);
        if (!argv) {
            PyErr_NoMemory();
            return NULL;
        }
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item;
        int needs_decref;
        if (is_tuple) {
            item = PyTuple_GET_ITEM(seq, i);  /* borrowed */
            needs_decref = 0;
        } else if (is_list) {
            item = PyList_GET_ITEM(seq, i);   /* borrowed */
            needs_decref = 0;
        } else {
            item = PySequence_GetItem(seq, i); /* new ref */
            needs_decref = 1;
        }
        int rc = item ? py_to_js(context, item, &argv[i]) : -1;
        if (needs_decref) {
            Py_XDECREF(item);
        }
        if (rc < 0) {
            for (Py_ssize_t j = 0; j < i; j++) {
                JS_FreeValue(context->ctx, argv[j]);
            }
            if (argv != stackbuf) {
                PyMem_Free(argv);
            }
            return NULL;
        }
    }
    *argc = (int)n;
    return argv;
}

static void free_js_argv(ContextObject *context, JSValue *argv,
                         JSValue *stackbuf, int argc)
{
    for (int i = 0; i < argc; i++) {
        JS_FreeValue(context->ctx, argv[i]);
    }
    if (argv != stackbuf) {
        PyMem_Free(argv);
    }
}

static PyObject *Value_call(ValueObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"this", NULL};
    PyObject *this_obj = Py_None;
    /* args is the positional tuple of JS arguments; `this` only via kwargs */
    if (kwds) {
        PyObject *t = PyDict_GetItemString(kwds, "this");
        if (t) {
            this_obj = t;
        }
    }
    (void)kwlist;

    JSContext *ctx = VALUE_CTX(self);
    JSValue js_this;
    if (py_to_js(self->context, this_obj, &js_this) < 0) {
        return NULL;
    }
    int argc = 0;
    JSValue stackbuf[JS_ARGV_STACK];
    JSValue *argv = build_js_argv(self->context, args, stackbuf,
                                  JS_ARGV_STACK, &argc);
    if (!argv) {
        JS_FreeValue(ctx, js_this);
        return NULL;
    }
    JSValue res = JS_Call(ctx, self->val, js_this, argc, argv);
    JS_FreeValue(ctx, js_this);
    free_js_argv(self->context, argv, stackbuf, argc);
    if (JS_IsException(res)) {
        return raise_js_exception(self->context);
    }
    return js_to_py_consume(self->context, &res);
}

static PyObject *Value_call_constructor(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    int argc = 0;
    JSValue stackbuf[JS_ARGV_STACK];
    JSValue *argv = build_js_argv(self->context, args, stackbuf,
                                  JS_ARGV_STACK, &argc);
    if (!argv) {
        return NULL;
    }
    JSValue res = JS_CallConstructor(ctx, self->val, argc, argv);
    free_js_argv(self->context, argv, stackbuf, argc);
    if (JS_IsException(res)) {
        return raise_js_exception(self->context);
    }
    return js_to_py_consume(self->context, &res);
}

static PyObject *Value_json(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    JSValue s = JS_JSONStringify(ctx, self->val, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) {
        return raise_js_exception(self->context);
    }
    PyObject *res = JS_IsUndefined(s) ? (Py_INCREF(Py_None), Py_None)
                                      : js_string_to_py(ctx, s);
    JS_FreeValue(ctx, s);
    return res;
}

/* Read an ArrayBuffer or TypedArray's bytes into a Python bytes object. */
static PyObject *Value_to_bytes(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    size_t size = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, self->val);
    if (buf) {
        return PyBytes_FromStringAndSize((const char *)buf, (Py_ssize_t)size);
    }
    /* Not an ArrayBuffer: clear the pending error and try as a TypedArray. */
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t off = 0, len = 0, bpe = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, self->val, &off, &len, &bpe);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        PyErr_SetString(PyExc_TypeError,
                        "value is not an ArrayBuffer or TypedArray");
        return NULL;
    }
    uint8_t *tbuf = JS_GetArrayBuffer(ctx, &size, ab);
    PyObject *res = NULL;
    if (tbuf) {
        res = PyBytes_FromStringAndSize((const char *)(tbuf + off),
                                        (Py_ssize_t)len);
    } else {
        raise_js_exception(self->context);
    }
    JS_FreeValue(ctx, ab);
    return res;
}

/* Serialise this value (typically a compiled function) to a bytecode blob. */
static PyObject *Value_write_object(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    size_t size = 0;
    uint8_t *buf = JS_WriteObject(ctx, &size, self->val, JS_WRITE_OBJ_BYTECODE);
    if (!buf) {
        return raise_js_exception(self->context);
    }
    PyObject *res = PyBytes_FromStringAndSize((const char *)buf,
                                              (Py_ssize_t)size);
    js_free(ctx, buf);
    return res;
}

/* Define a property with an explicit descriptor (data or accessor). */
static PyObject *Value_define_property(ValueObject *self, PyObject *args,
                                       PyObject *kwds)
{
    static char *kwlist[] = {"key", "value", "get", "set",
                             "writable", "enumerable", "configurable", NULL};
    PyObject *key, *value = NULL, *getter = NULL, *setter = NULL;
    int writable = 1, enumerable = 1, configurable = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OOOppp:define_property",
                                     kwlist, &key, &value, &getter, &setter,
                                     &writable, &enumerable, &configurable)) {
        return NULL;
    }
    JSContext *ctx = VALUE_CTX(self);
    JSAtom atom = py_key_to_atom(self->context, key);
    if (atom == JS_ATOM_NULL) {
        return NULL;
    }
    int flags = (enumerable ? JS_PROP_ENUMERABLE : 0) |
                (configurable ? JS_PROP_CONFIGURABLE : 0);
    int r;
    if (getter || setter) {
        JSValue jget = JS_UNDEFINED, jset = JS_UNDEFINED;
        if (getter && py_to_js(self->context, getter, &jget) < 0) {
            JS_FreeAtom(ctx, atom);
            return NULL;
        }
        if (setter && py_to_js(self->context, setter, &jset) < 0) {
            JS_FreeValue(ctx, jget);
            JS_FreeAtom(ctx, atom);
            return NULL;
        }
        r = JS_DefinePropertyGetSet(ctx, self->val, atom, jget, jset, flags);
    } else {
        JSValue jv;
        if (py_to_js(self->context, value ? value : Py_None, &jv) < 0) {
            JS_FreeAtom(ctx, atom);
            return NULL;
        }
        r = JS_DefinePropertyValue(ctx, self->val, atom, jv,
                                   flags | (writable ? JS_PROP_WRITABLE : 0));
    }
    JS_FreeAtom(ctx, atom);
    if (r < 0) {
        return raise_js_exception(self->context);
    }
    Py_RETURN_NONE;
}

static Py_ssize_t Value_length(ValueObject *self)
{
    JSContext *ctx = VALUE_CTX(self);
    JSValue len = JS_GetProperty(ctx, self->val,
                                 self->context->runtime->atom_length);
    if (JS_IsException(len)) {
        raise_js_exception(self->context);
        return -1;
    }
    int64_t n = 0;
    int r = JS_ToInt64(ctx, &n, len);
    JS_FreeValue(ctx, len);
    if (r < 0) {
        raise_js_exception(self->context);
        return -1;
    }
    return (Py_ssize_t)n;
}

static PyObject *Value_subscript(ValueObject *self, PyObject *key)
{
    return Value_getprop(self, key);
}

static int Value_ass_subscript(ValueObject *self, PyObject *key, PyObject *value)
{
    return Value_setprop(self, key, value);
}

static PyObject *Value_richcompare(PyObject *a, PyObject *b, int op)
{
    if ((op != Py_EQ && op != Py_NE) ||
        !PyObject_TypeCheck(a, &Value_Type) ||
        !PyObject_TypeCheck(b, &Value_Type)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    ValueObject *va = (ValueObject *)a;
    ValueObject *vb = (ValueObject *)b;
    int eq = JS_StrictEq(VALUE_CTX(va), JS_DupValue(VALUE_CTX(va), va->val),
                         JS_DupValue(VALUE_CTX(va), vb->val));
    if (op == Py_NE) {
        eq = !eq;
    }
    return PyBool_FromLong(eq);
}

static PyObject *Value_get_context(ValueObject *self, void *closure)
{
    Py_INCREF(self->context);
    return (PyObject *)self->context;
}

static PyGetSetDef Value_getset[] = {
    {"tag", (getter)Value_get_tag, NULL, "raw JS tag", NULL},
    {"context", (getter)Value_get_context, NULL, "owning Context", NULL},
    {"is_object", (getter)Value_is_object, NULL, NULL, NULL},
    {"is_function", (getter)Value_is_function, NULL, NULL, NULL},
    {"is_array", (getter)Value_is_array, NULL, NULL, NULL},
    {"is_string", (getter)Value_is_string, NULL, NULL, NULL},
    {"is_number", (getter)Value_is_number, NULL, NULL, NULL},
    {"is_bool", (getter)Value_is_bool, NULL, NULL, NULL},
    {"is_null", (getter)Value_is_null, NULL, NULL, NULL},
    {"is_undefined", (getter)Value_is_undefined, NULL, NULL, NULL},
    {"is_symbol", (getter)Value_is_symbol, NULL, NULL, NULL},
    {"is_error", (getter)Value_is_error, NULL, NULL, NULL},
    {"is_constructor", (getter)Value_is_constructor, NULL, NULL, NULL},
    {"is_promise", (getter)Value_is_promise, NULL,
     "True if the value is a Promise.", NULL},
    {"promise_state", (getter)Value_promise_state, NULL,
     "'pending'/'fulfilled'/'rejected' for a Promise, else None.", NULL},
    {NULL}
};

static PyMethodDef Value_methods[] = {
    {"to_python", (PyCFunction)Value_to_python, METH_VARARGS | METH_KEYWORDS,
     "to_python(deep=True): convert to native Python objects. With "
     "deep=True arrays become lists and plain objects become dicts."},
    {"get", (PyCFunction)Value_get, METH_VARARGS, "get(key) -> property value"},
    {"set", (PyCFunction)Value_set, METH_VARARGS, "set(key, value)"},
    {"has", (PyCFunction)Value_has, METH_VARARGS, "has(key) -> bool"},
    {"delete", (PyCFunction)Value_delete, METH_VARARGS, "delete(key)"},
    {"keys", (PyCFunction)Value_keys, METH_NOARGS, "own enumerable string keys"},
    {"call", (PyCFunction)Value_call, METH_VARARGS | METH_KEYWORDS,
     "call(*args, this=None) -> result"},
    {"call_constructor", (PyCFunction)Value_call_constructor, METH_VARARGS,
     "call_constructor(*args) -> new instance"},
    {"json", (PyCFunction)Value_json, METH_NOARGS, "JSON.stringify(self)"},
    {"to_bytes", (PyCFunction)Value_to_bytes, METH_NOARGS,
     "Read an ArrayBuffer or TypedArray into a bytes object."},
    {"write_object", (PyCFunction)Value_write_object, METH_NOARGS,
     "Serialise to a QuickJS bytecode blob (bytes)."},
    {"result", (PyCFunction)Value_result, METH_NOARGS,
     "Pump the job queue until this Promise settles, then return its "
     "fulfilled value (or raise JSError if it rejected)."},
    {"define_property", (PyCFunction)Value_define_property,
     METH_VARARGS | METH_KEYWORDS,
     "define_property(key, value=None, get=None, set=None, writable=True, "
     "enumerable=True, configurable=True)"},
    {NULL}
};

static PyNumberMethods Value_as_number = {
    .nb_int = (unaryfunc)Value_int,
    .nb_float = (unaryfunc)Value_float,
    .nb_bool = (inquiry)Value_bool,
};

static PyMappingMethods Value_as_mapping = {
    .mp_length = (lenfunc)Value_length,
    .mp_subscript = (binaryfunc)Value_subscript,
    .mp_ass_subscript = (objobjargproc)Value_ass_subscript,
};

static PyTypeObject Value_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickjs._quickjs.Value",
    .tp_basicsize = sizeof(ValueObject),
    .tp_dealloc = (destructor)Value_dealloc,
    .tp_repr = (reprfunc)Value_repr,
    .tp_str = (reprfunc)Value_str,
    .tp_call = (ternaryfunc)Value_call,
    .tp_as_number = &Value_as_number,
    .tp_as_mapping = &Value_as_mapping,
    .tp_richcompare = Value_richcompare,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A single owned QuickJS value.",
    .tp_methods = Value_methods,
    .tp_getset = Value_getset,
};

/* ------------------------------------------------------------------ */
/* Context object                                                       */
/* ------------------------------------------------------------------ */

/* Lazily populate the runtime-wide atom cache. Atoms are per-runtime in
 * QuickJS; creating them once and reusing the handle replaces the
 * JS_NewAtom/JS_FreeAtom dance in property-access hot paths. */
static int runtime_init_atoms(RuntimeObject *runtime, JSContext *ctx)
{
    if (runtime->atom_length != JS_ATOM_NULL) {
        return 0;
    }
    runtime->atom_length = JS_NewAtom(ctx, "length");
    runtime->atom_name   = JS_NewAtom(ctx, "name");
    runtime->atom_stack  = JS_NewAtom(ctx, "stack");
    runtime->atom_value  = JS_NewAtom(ctx, "value");
    runtime->atom_BigInt = JS_NewAtom(ctx, "BigInt");
    if (runtime->atom_length == JS_ATOM_NULL ||
        runtime->atom_name   == JS_ATOM_NULL ||
        runtime->atom_stack  == JS_ATOM_NULL ||
        runtime->atom_value  == JS_ATOM_NULL ||
        runtime->atom_BigInt == JS_ATOM_NULL) {
        PyErr_SetString(QuickJSError, "failed to intern QuickJS atoms");
        return -1;
    }
    return 0;
}

/* Create a ContextObject bound to `runtime` (new reference). */
static PyObject *make_context(RuntimeObject *runtime)
{
    JSContext *ctx = JS_NewContext(runtime->rt);
    if (!ctx) {
        PyErr_SetString(QuickJSError, "failed to create QuickJS context");
        return NULL;
    }
    if (runtime_init_atoms(runtime, ctx) < 0) {
        JS_FreeContext(ctx);
        return NULL;
    }
    ContextObject *context = PyObject_New(ContextObject, &Context_Type);
    if (!context) {
        JS_FreeContext(ctx);
        return NULL;
    }
    context->ctx = ctx;
    context->runtime = runtime;
    Py_INCREF(runtime);
    context->module_loader = NULL;
    context->module_normalizer = NULL;
    context->global = JS_UNDEFINED;
    JS_SetContextOpaque(ctx, context);
    return (PyObject *)context;
}

/* Return a borrowed handle to the cached global object (do not free). */
static JSValueConst context_global(ContextObject *self)
{
    if (JS_IsUndefined(self->global)) {
        self->global = JS_GetGlobalObject(self->ctx);
    }
    return self->global;
}

static PyObject *Context_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"runtime", NULL};
    PyObject *runtime = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:Context", kwlist,
                                     &runtime)) {
        return NULL;
    }
    if (runtime && runtime != Py_None) {
        if (!PyObject_TypeCheck(runtime, &Runtime_Type)) {
            PyErr_SetString(PyExc_TypeError,
                            "runtime must be a quickjs Runtime");
            return NULL;
        }
        return make_context((RuntimeObject *)runtime);
    }
    PyObject *rt = PyObject_CallObject((PyObject *)&Runtime_Type, NULL);
    if (!rt) {
        return NULL;
    }
    PyObject *ctx = make_context((RuntimeObject *)rt);
    Py_DECREF(rt);
    return ctx;
}

static void Context_dealloc(ContextObject *self)
{
    if (self->ctx) {
        if (!JS_IsUndefined(self->global)) {
            JS_FreeValue(self->ctx, self->global);
        }
        JS_FreeContext(self->ctx);
    }
    Py_XDECREF(self->module_loader);
    Py_XDECREF(self->module_normalizer);
    Py_XDECREF(self->runtime);
    PyObject_Free(self);
}

static PyObject *Context_eval(ContextObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"code", "filename", "module", "strict",
                             "compile_only", NULL};
    const char *code;
    Py_ssize_t code_len;
    const char *filename = "<input>";
    int module = 0, strict = 0, compile_only = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|sppp:eval", kwlist,
                                     &code, &code_len, &filename,
                                     &module, &strict, &compile_only)) {
        return NULL;
    }
    int flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    if (strict) {
        flags |= JS_EVAL_FLAG_STRICT;
    }
    if (compile_only) {
        flags |= JS_EVAL_FLAG_COMPILE_ONLY;
    }
    JSValue res = JS_Eval(self->ctx, code, (size_t)code_len, filename, flags);
    if (JS_IsException(res)) {
        return raise_js_exception(self);
    }
    return js_to_py_consume(self, &res);
}

static PyObject *Context_async_eval(ContextObject *self, PyObject *args,
                                    PyObject *kwds)
{
    static char *kwlist[] = {"code", "filename", "module", "strict", NULL};
    const char *code;
    Py_ssize_t code_len;
    const char *filename = "<async>";
    int module = 0, strict = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|spp:async_eval", kwlist,
                                     &code, &code_len, &filename,
                                     &module, &strict)) {
        return NULL;
    }
    int flags = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    if (strict) {
        flags |= JS_EVAL_FLAG_STRICT;
    }
    /* JS_EVAL_FLAG_ASYNC wraps the script so it always returns a Promise and
     * top-level `await` works; it is only valid for global (non-module) code,
     * while modules support top-level await natively. */
    if (!module) {
        flags |= JS_EVAL_FLAG_ASYNC;
    }
    JSValue res = JS_Eval(self->ctx, code, (size_t)code_len, filename, flags);
    if (JS_IsException(res)) {
        return raise_js_exception(self);
    }
    if (!module) {
        /* The async-wrapped script resolves to a `{ value: <result> }`
         * object; settle it and unwrap the completion value. */
        JSValue settled;
        if (settle_promise_value(self, res, &settled) < 0) {
            JS_FreeValue(self->ctx, res);
            return NULL;
        }
        JS_FreeValue(self->ctx, res);
        JSValue inner = JS_GetProperty(self->ctx, settled,
                                       self->runtime->atom_value);
        JS_FreeValue(self->ctx, settled);
        if (JS_IsException(inner)) {
            return raise_js_exception(self);
        }
        /* If the completion value is itself a Promise, settle it too, so
         * async_eval("f()") behaves like async_eval("await f()"). */
        while (JS_PromiseState(self->ctx, inner) != (JSPromiseStateEnum)-1) {
            JSValue next;
            if (settle_promise_value(self, inner, &next) < 0) {
                JS_FreeValue(self->ctx, inner);
                return NULL;
            }
            JS_FreeValue(self->ctx, inner);
            inner = next;
        }
        return js_to_py_consume(self, &inner);
    }
    /* Module evaluation may yield a Promise (top-level await) or a value. */
    if (JS_PromiseState(self->ctx, res) != (JSPromiseStateEnum)-1) {
        PyObject *out = settle_promise(self, res);
        JS_FreeValue(self->ctx, res);
        return out;
    }
    return js_to_py_consume(self, &res);
}

/* asyncio bridge: delegate to the pure-Python helper so the raw layer stays
 * dumb. Returns a coroutine; `await` it inside an asyncio coroutine. */
static PyObject *Context_await_promise(ContextObject *self, PyObject *args)
{
    PyObject *value;
    if (!PyArg_ParseTuple(args, "O:await_promise", &value)) {
        return NULL;
    }
    PyObject *mod = PyImport_ImportModule("quickjs._async");
    if (!mod) {
        return NULL;
    }
    PyObject *fn = PyObject_GetAttrString(mod, "await_promise");
    Py_DECREF(mod);
    if (!fn) {
        return NULL;
    }
    PyObject *res =
        PyObject_CallFunctionObjArgs(fn, (PyObject *)self, value, NULL);
    Py_DECREF(fn);
    return res;
}

static PyObject *Context_get_global(ContextObject *self, PyObject *args)
{
    return Value_wrap(self, JS_DupValue(self->ctx, context_global(self)));
}

static PyObject *Context_get(ContextObject *self, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s:get", &name)) {
        return NULL;
    }
    JSValue v = JS_GetPropertyStr(self->ctx, context_global(self), name);
    if (JS_IsException(v)) {
        return raise_js_exception(self);
    }
    return js_to_py_consume(self, &v);
}

static PyObject *Context_set(ContextObject *self, PyObject *args)
{
    const char *name;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "sO:set", &name, &value)) {
        return NULL;
    }
    JSValue jv;
    if (py_to_js(self, value, &jv) < 0) {
        return NULL;
    }
    int r = JS_SetPropertyStr(self->ctx, context_global(self), name, jv);
    if (r < 0) {
        return raise_js_exception(self);
    }
    Py_RETURN_NONE;
}

static PyObject *Context_parse_json(ContextObject *self, PyObject *args)
{
    const char *text;
    Py_ssize_t len;
    const char *filename = "<json>";
    if (!PyArg_ParseTuple(args, "s#|s:parse_json", &text, &len, &filename)) {
        return NULL;
    }
    JSValue v = JS_ParseJSON(self->ctx, text, (size_t)len, filename);
    if (JS_IsException(v)) {
        return raise_js_exception(self);
    }
    return js_to_py_consume(self, &v);
}

static PyObject *Context_new_object(ContextObject *self, PyObject *args)
{
    return Value_wrap(self, JS_NewObject(self->ctx));
}

static PyObject *Context_new_array(ContextObject *self, PyObject *args)
{
    return Value_wrap(self, JS_NewArray(self->ctx));
}

static PyObject *Context_new_function(ContextObject *self, PyObject *args,
                                      PyObject *kwds)
{
    static char *kwlist[] = {"callable", "name", "length", NULL};
    PyObject *callable;
    const char *name = "";
    int length = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|si:new_function", kwlist,
                                     &callable, &name, &length)) {
        return NULL;
    }
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "argument must be callable");
        return NULL;
    }
    return make_js_function(self, callable, name, length);
}

static PyObject *Context_execute_pending_job(ContextObject *self, PyObject *args)
{
    JSContext *jctx;
    int r = JS_ExecutePendingJob(self->runtime->rt, &jctx);
    if (r < 0) {
        return raise_js_exception(self);
    }
    return PyBool_FromLong(r > 0);
}

static PyObject *Context_has_pending_job(ContextObject *self, void *closure)
{
    return PyBool_FromLong(JS_IsJobPending(self->runtime->rt));
}

static PyObject *Context_get_runtime(ContextObject *self, void *closure)
{
    Py_INCREF(self->runtime);
    return (PyObject *)self->runtime;
}

/* Compile code without running it; returns a compiled-function Value. */
static PyObject *Context_compile(ContextObject *self, PyObject *args,
                                 PyObject *kwds)
{
    static char *kwlist[] = {"code", "filename", "module", NULL};
    const char *code;
    Py_ssize_t code_len;
    const char *filename = "<input>";
    int module = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|sp:compile", kwlist,
                                     &code, &code_len, &filename, &module)) {
        return NULL;
    }
    int flags = JS_EVAL_FLAG_COMPILE_ONLY |
                (module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    JSValue res = JS_Eval(self->ctx, code, (size_t)code_len, filename, flags);
    if (JS_IsException(res)) {
        return raise_js_exception(self);
    }
    return Value_wrap(self, res);
}

/* Instantiate and run a compiled function previously produced by compile()
 * or read_object(). */
static PyObject *Context_eval_function(ContextObject *self, PyObject *args)
{
    PyObject *value;
    if (!PyArg_ParseTuple(args, "O!:eval_function", &Value_Type, &value)) {
        return NULL;
    }
    ValueObject *v = (ValueObject *)value;
    JSValue res = JS_EvalFunction(self->ctx,
                                  JS_DupValue(self->ctx, v->val));
    if (JS_IsException(res)) {
        return raise_js_exception(self);
    }
    return js_to_py_consume(self, &res);
}

/* Deserialise a bytecode blob written by Value.write_object(). */
static PyObject *Context_read_object(ContextObject *self, PyObject *args)
{
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "y*:read_object", &buf)) {
        return NULL;
    }
    JSValue res = JS_ReadObject(self->ctx, (const uint8_t *)buf.buf,
                                (size_t)buf.len, JS_READ_OBJ_BYTECODE);
    PyBuffer_Release(&buf);
    if (JS_IsException(res)) {
        return raise_js_exception(self);
    }
    return Value_wrap(self, res);
}

/* Return the current pending JS exception (and clear it), or None. */
static PyObject *Context_get_exception(ContextObject *self, PyObject *args)
{
    if (!JS_HasException(self->ctx)) {
        Py_RETURN_NONE;
    }
    JSValue exc = JS_GetException(self->ctx);
    return js_to_py_consume(self, &exc);
}

/* Create a JS ArrayBuffer holding a copy of the given bytes. */
static PyObject *Context_new_array_buffer(ContextObject *self, PyObject *args)
{
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "y*:new_array_buffer", &buf)) {
        return NULL;
    }
    JSValue ab = JS_NewArrayBufferCopy(self->ctx, (const uint8_t *)buf.buf,
                                       (size_t)buf.len);
    PyBuffer_Release(&buf);
    if (JS_IsException(ab)) {
        return raise_js_exception(self);
    }
    return Value_wrap(self, ab);
}

/* Wrap an arbitrary Python object as an opaque JS host object. */
static PyObject *Context_new_host_object(ContextObject *self, PyObject *args)
{
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "O:new_host_object", &obj)) {
        return NULL;
    }
    return make_py_host(self, obj);
}

/* Reinstall the runtime-level module callbacks based on which Python hooks
 * are currently set. The loader can be unset while a normalizer is set; in
 * that case the loader trampoline still runs and reports "no module loader
 * set" with the canonical name produced by the normalizer. */
static void context_refresh_module_hooks(ContextObject *self)
{
    JSModuleNormalizeFunc *nf = self->module_normalizer ? js_module_normalize
                                                        : NULL;
    JSModuleLoaderFunc *lf =
        (self->module_loader || self->module_normalizer) ? js_module_loader
                                                         : NULL;
    if (!nf && !lf) {
        JS_SetModuleLoaderFunc(self->runtime->rt, NULL, NULL, NULL);
    } else {
        JS_SetModuleLoaderFunc(self->runtime->rt, nf, lf, NULL);
    }
}

/* Install a callable(module_name) -> source-string ES module loader. */
static PyObject *Context_set_module_loader(ContextObject *self, PyObject *args)
{
    PyObject *loader;
    if (!PyArg_ParseTuple(args, "O:set_module_loader", &loader)) {
        return NULL;
    }
    if (loader != Py_None && !PyCallable_Check(loader)) {
        PyErr_SetString(PyExc_TypeError, "loader must be callable or None");
        return NULL;
    }
    Py_XDECREF(self->module_loader);
    if (loader == Py_None) {
        self->module_loader = NULL;
    } else {
        Py_INCREF(loader);
        self->module_loader = loader;
    }
    context_refresh_module_hooks(self);
    Py_RETURN_NONE;
}

/* Install a callable(base_name, name) -> str ES module name resolver. */
static PyObject *Context_set_module_normalizer(ContextObject *self,
                                               PyObject *args)
{
    PyObject *normalizer;
    if (!PyArg_ParseTuple(args, "O:set_module_normalizer", &normalizer)) {
        return NULL;
    }
    if (normalizer != Py_None && !PyCallable_Check(normalizer)) {
        PyErr_SetString(PyExc_TypeError,
                        "normalizer must be callable or None");
        return NULL;
    }
    Py_XDECREF(self->module_normalizer);
    if (normalizer == Py_None) {
        self->module_normalizer = NULL;
    } else {
        Py_INCREF(normalizer);
        self->module_normalizer = normalizer;
    }
    context_refresh_module_hooks(self);
    Py_RETURN_NONE;
}

static PyObject *Context_enter(ContextObject *self, PyObject *args)
{
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Context_exit(ContextObject *self, PyObject *args)
{
    Py_RETURN_FALSE;
}

static PyMethodDef Context_methods[] = {
    {"eval", (PyCFunction)Context_eval, METH_VARARGS | METH_KEYWORDS,
     "eval(code, filename='<input>', module=False, strict=False, "
     "compile_only=False) -> result"},
    {"get_global", (PyCFunction)Context_get_global, METH_NOARGS,
     "Return the global object as a Value."},
    {"get", (PyCFunction)Context_get, METH_VARARGS,
     "get(name) -> global property value"},
    {"set", (PyCFunction)Context_set, METH_VARARGS,
     "set(name, value) -> set a global property"},
    {"parse_json", (PyCFunction)Context_parse_json, METH_VARARGS,
     "parse_json(text) -> parsed value"},
    {"new_object", (PyCFunction)Context_new_object, METH_NOARGS,
     "Create an empty JS object."},
    {"new_array", (PyCFunction)Context_new_array, METH_NOARGS,
     "Create an empty JS array."},
    {"new_function", (PyCFunction)Context_new_function,
     METH_VARARGS | METH_KEYWORDS,
     "new_function(callable, name='', length=0) -> JS function"},
    {"async_eval", (PyCFunction)Context_async_eval,
     METH_VARARGS | METH_KEYWORDS,
     "async_eval(code, filename='<async>', module=False, strict=False) -> "
     "result. Evaluates code that may use top-level `await`, pumps the job "
     "queue until the resulting promise settles, and returns its value "
     "(or raises JSError if it rejected)."},
    {"await_promise", (PyCFunction)Context_await_promise, METH_VARARGS,
     "await_promise(value) -> coroutine. Asyncio-compatible: `await` it to "
     "drive the job queue and obtain the Promise's settled value."},
    {"execute_pending_job", (PyCFunction)Context_execute_pending_job,
     METH_NOARGS, "Run one pending job; returns True if a job ran."},
    {"compile", (PyCFunction)Context_compile, METH_VARARGS | METH_KEYWORDS,
     "compile(code, filename='<input>', module=False) -> compiled function"},
    {"eval_function", (PyCFunction)Context_eval_function, METH_VARARGS,
     "eval_function(value) -> run a compiled function"},
    {"read_object", (PyCFunction)Context_read_object, METH_VARARGS,
     "read_object(data) -> Value deserialised from a bytecode blob"},
    {"get_exception", (PyCFunction)Context_get_exception, METH_NOARGS,
     "Return and clear the current pending JS exception, or None."},
    {"new_array_buffer", (PyCFunction)Context_new_array_buffer, METH_VARARGS,
     "new_array_buffer(data) -> JS ArrayBuffer copy of the given bytes"},
    {"new_host_object", (PyCFunction)Context_new_host_object, METH_VARARGS,
     "new_host_object(obj) -> opaque JS object embedding a Python object"},
    {"set_module_loader", (PyCFunction)Context_set_module_loader, METH_VARARGS,
     "set_module_loader(callable_or_None): resolve ES module sources."},
    {"set_module_normalizer", (PyCFunction)Context_set_module_normalizer,
     METH_VARARGS,
     "set_module_normalizer(callable_or_None): map (base_name, name) to a "
     "canonical module name. Replaces QuickJS's default relative-path "
     "resolver."},
    {"__enter__", (PyCFunction)Context_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)Context_exit, METH_VARARGS, NULL},
    {NULL}
};

static PyGetSetDef Context_getset[] = {
    {"has_pending_job", (getter)Context_has_pending_job, NULL,
     "True if the runtime has a pending job.", NULL},
    {"runtime", (getter)Context_get_runtime, NULL, "owning Runtime", NULL},
    {NULL}
};

static PyTypeObject Context_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickjs._quickjs.Context",
    .tp_basicsize = sizeof(ContextObject),
    .tp_dealloc = (destructor)Context_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A QuickJS execution context. Context(runtime=None).",
    .tp_methods = Context_methods,
    .tp_getset = Context_getset,
    .tp_new = Context_new,
};

/* ------------------------------------------------------------------ */
/* Runtime object                                                       */
/* ------------------------------------------------------------------ */

static int js_interrupt_handler(JSRuntime *rt, void *opaque)
{
    RuntimeObject *self = (RuntimeObject *)opaque;
    if (!self->interrupt_cb) {
        return 0;
    }
    PyObject *res = PyObject_CallObject(self->interrupt_cb, NULL);
    if (!res) {
        PyErr_Clear();
        return 1; /* interrupt on callback error */
    }
    int r = PyObject_IsTrue(res);
    Py_DECREF(res);
    return r;
}

static PyObject *Runtime_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    RuntimeObject *self = (RuntimeObject *)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }
    self->rt = JS_NewRuntime();
    if (!self->rt) {
        Py_DECREF(self);
        PyErr_SetString(QuickJSError, "failed to create QuickJS runtime");
        return NULL;
    }
    self->interrupt_cb = NULL;
    self->atom_length = JS_ATOM_NULL;
    self->atom_name   = JS_ATOM_NULL;
    self->atom_stack  = JS_ATOM_NULL;
    self->atom_value  = JS_ATOM_NULL;
    self->atom_BigInt = JS_ATOM_NULL;
    JS_SetRuntimeOpaque(self->rt, self);
    /* Register the host-object class so Python objects can be embedded. */
    JS_NewClass(self->rt, py_host_class_id, &py_host_class_def);
    return (PyObject *)self;
}

static void Runtime_dealloc(RuntimeObject *self)
{
    Py_XDECREF(self->interrupt_cb);
    if (self->rt) {
        if (self->atom_length != JS_ATOM_NULL) {
            JS_FreeAtomRT(self->rt, self->atom_length);
            JS_FreeAtomRT(self->rt, self->atom_name);
            JS_FreeAtomRT(self->rt, self->atom_stack);
            JS_FreeAtomRT(self->rt, self->atom_value);
            JS_FreeAtomRT(self->rt, self->atom_BigInt);
        }
        JS_FreeRuntime(self->rt);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Runtime_new_context(RuntimeObject *self, PyObject *args)
{
    return make_context(self);
}

static PyObject *Runtime_set_memory_limit(RuntimeObject *self, PyObject *args)
{
    Py_ssize_t limit;
    if (!PyArg_ParseTuple(args, "n:set_memory_limit", &limit)) {
        return NULL;
    }
    JS_SetMemoryLimit(self->rt, (size_t)limit);
    Py_RETURN_NONE;
}

static PyObject *Runtime_set_max_stack_size(RuntimeObject *self, PyObject *args)
{
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "n:set_max_stack_size", &size)) {
        return NULL;
    }
    JS_SetMaxStackSize(self->rt, (size_t)size);
    Py_RETURN_NONE;
}

static PyObject *Runtime_set_gc_threshold(RuntimeObject *self, PyObject *args)
{
    Py_ssize_t threshold;
    if (!PyArg_ParseTuple(args, "n:set_gc_threshold", &threshold)) {
        return NULL;
    }
    JS_SetGCThreshold(self->rt, (size_t)threshold);
    Py_RETURN_NONE;
}

static PyObject *Runtime_run_gc(RuntimeObject *self, PyObject *args)
{
    JS_RunGC(self->rt);
    Py_RETURN_NONE;
}

static PyObject *Runtime_set_interrupt_handler(RuntimeObject *self, PyObject *args)
{
    PyObject *handler;
    if (!PyArg_ParseTuple(args, "O:set_interrupt_handler", &handler)) {
        return NULL;
    }
    Py_XDECREF(self->interrupt_cb);
    if (handler == Py_None) {
        self->interrupt_cb = NULL;
        JS_SetInterruptHandler(self->rt, NULL, NULL);
    } else {
        if (!PyCallable_Check(handler)) {
            PyErr_SetString(PyExc_TypeError, "handler must be callable or None");
            return NULL;
        }
        Py_INCREF(handler);
        self->interrupt_cb = handler;
        JS_SetInterruptHandler(self->rt, js_interrupt_handler, self);
    }
    Py_RETURN_NONE;
}

static PyObject *Runtime_compute_memory_usage(RuntimeObject *self,
                                              PyObject *args)
{
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(self->rt, &u);
#define MU(name) #name, (long long)u.name
    return Py_BuildValue(
        "{sLsLsLsLsLsLsLsLsLsLsLsLsL}",
        MU(malloc_size), MU(malloc_limit), MU(memory_used_size),
        MU(malloc_count), MU(memory_used_count), MU(atom_count),
        MU(atom_size), MU(str_count), MU(str_size), MU(obj_count),
        MU(obj_size), MU(prop_count), MU(prop_size));
#undef MU
}

static PyObject *Runtime_enter(RuntimeObject *self, PyObject *args)
{
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *Runtime_exit(RuntimeObject *self, PyObject *args)
{
    Py_RETURN_FALSE;
}

static PyMethodDef Runtime_methods[] = {
    {"new_context", (PyCFunction)Runtime_new_context, METH_NOARGS,
     "Create a new Context bound to this runtime."},
    {"compute_memory_usage", (PyCFunction)Runtime_compute_memory_usage,
     METH_NOARGS, "Return a dict of engine memory-usage counters."},
    {"__enter__", (PyCFunction)Runtime_enter, METH_NOARGS, NULL},
    {"__exit__", (PyCFunction)Runtime_exit, METH_VARARGS, NULL},
    {"set_memory_limit", (PyCFunction)Runtime_set_memory_limit, METH_VARARGS,
     "set_memory_limit(bytes)"},
    {"set_max_stack_size", (PyCFunction)Runtime_set_max_stack_size, METH_VARARGS,
     "set_max_stack_size(bytes)"},
    {"set_gc_threshold", (PyCFunction)Runtime_set_gc_threshold, METH_VARARGS,
     "set_gc_threshold(bytes)"},
    {"run_gc", (PyCFunction)Runtime_run_gc, METH_NOARGS, "Force a GC cycle."},
    {"set_interrupt_handler", (PyCFunction)Runtime_set_interrupt_handler,
     METH_VARARGS,
     "set_interrupt_handler(callable_or_None): truthy return interrupts JS."},
    {NULL}
};

static PyTypeObject Runtime_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickjs._quickjs.Runtime",
    .tp_basicsize = sizeof(RuntimeObject),
    .tp_dealloc = (destructor)Runtime_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "A QuickJS runtime (heap + GC).",
    .tp_methods = Runtime_methods,
    .tp_new = Runtime_new,
};

/* ------------------------------------------------------------------ */
/* Undefined singleton                                                  */
/* ------------------------------------------------------------------ */

static PyObject *Undefined_repr(PyObject *self)
{
    return PyUnicode_FromString("Undefined");
}

static int Undefined_bool(PyObject *self)
{
    return 0;
}

static PyObject *Undefined_new(PyTypeObject *type, PyObject *args,
                               PyObject *kwds)
{
    if (JSUndefined) {
        Py_INCREF(JSUndefined);
        return JSUndefined;
    }
    PyErr_SetString(PyExc_TypeError, "cannot create 'UndefinedType' instances");
    return NULL;
}

static PyNumberMethods Undefined_as_number = {
    .nb_bool = Undefined_bool,
};

static PyTypeObject Undefined_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "quickjs._quickjs.UndefinedType",
    .tp_basicsize = sizeof(PyObject),
    .tp_repr = Undefined_repr,
    .tp_as_number = &Undefined_as_number,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Type of the JS `undefined` singleton.",
    .tp_new = Undefined_new,
};

/* ------------------------------------------------------------------ */
/* Module                                                               */
/* ------------------------------------------------------------------ */

static PyModuleDef quickjs_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_quickjs",
    .m_doc = "Low-level Python bindings for the QuickJS JavaScript engine.",
    .m_size = -1,
};

PyMODINIT_FUNC PyInit__quickjs(void)
{
    if (PyType_Ready(&Runtime_Type) < 0 ||
        PyType_Ready(&Context_Type) < 0 ||
        PyType_Ready(&Value_Type) < 0 ||
        PyType_Ready(&Undefined_Type) < 0) {
        return NULL;
    }

    /* Reserve the process-wide class id for host objects. */
    JS_NewClassID(&py_host_class_id);

    /* Create the unique `undefined` singleton. */
    JSUndefined = PyObject_New(PyObject, &Undefined_Type);
    if (!JSUndefined) {
        return NULL;
    }

    PyObject *m = PyModule_Create(&quickjs_module);
    if (!m) {
        return NULL;
    }

    QuickJSError = PyErr_NewException("quickjs._quickjs.QuickJSError", NULL, NULL);
    JSError = PyErr_NewException("quickjs._quickjs.JSError", QuickJSError, NULL);
    if (!QuickJSError || !JSError) {
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(QuickJSError);
    Py_INCREF(JSError);
    Py_INCREF(&Runtime_Type);
    Py_INCREF(&Context_Type);
    Py_INCREF(&Value_Type);
    Py_INCREF(&Undefined_Type);
    Py_INCREF(JSUndefined);
    PyModule_AddObject(m, "QuickJSError", QuickJSError);
    PyModule_AddObject(m, "JSError", JSError);
    PyModule_AddObject(m, "Runtime", (PyObject *)&Runtime_Type);
    PyModule_AddObject(m, "Context", (PyObject *)&Context_Type);
    PyModule_AddObject(m, "Value", (PyObject *)&Value_Type);
    PyModule_AddObject(m, "UndefinedType", (PyObject *)&Undefined_Type);
    PyModule_AddObject(m, "Undefined", JSUndefined);

#ifdef CONFIG_VERSION
    PyModule_AddStringConstant(m, "quickjs_version", CONFIG_VERSION);
#else
    PyModule_AddStringConstant(m, "quickjs_version", "unknown");
#endif

    /* eval flags */
    PyModule_AddIntConstant(m, "JS_EVAL_TYPE_GLOBAL", JS_EVAL_TYPE_GLOBAL);
    PyModule_AddIntConstant(m, "JS_EVAL_TYPE_MODULE", JS_EVAL_TYPE_MODULE);
    PyModule_AddIntConstant(m, "JS_EVAL_FLAG_STRICT", JS_EVAL_FLAG_STRICT);
    PyModule_AddIntConstant(m, "JS_EVAL_FLAG_COMPILE_ONLY", JS_EVAL_FLAG_COMPILE_ONLY);

    /* value tags */
    PyModule_AddIntConstant(m, "JS_TAG_INT", JS_TAG_INT);
    PyModule_AddIntConstant(m, "JS_TAG_BOOL", JS_TAG_BOOL);
    PyModule_AddIntConstant(m, "JS_TAG_NULL", JS_TAG_NULL);
    PyModule_AddIntConstant(m, "JS_TAG_UNDEFINED", JS_TAG_UNDEFINED);
    PyModule_AddIntConstant(m, "JS_TAG_OBJECT", JS_TAG_OBJECT);
    PyModule_AddIntConstant(m, "JS_TAG_STRING", JS_TAG_STRING);
    PyModule_AddIntConstant(m, "JS_TAG_FLOAT64", JS_TAG_FLOAT64);
    PyModule_AddIntConstant(m, "JS_TAG_SYMBOL", JS_TAG_SYMBOL);
    PyModule_AddIntConstant(m, "JS_TAG_EXCEPTION", JS_TAG_EXCEPTION);

    return m;
}
