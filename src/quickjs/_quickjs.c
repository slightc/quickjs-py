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
} RuntimeObject;

typedef struct {
    PyObject_HEAD
    JSContext *ctx;
    RuntimeObject *runtime;
    PyObject *callbacks;    /* dict: int id -> python callable */
    long next_cb_id;
} ContextObject;

typedef struct {
    PyObject_HEAD
    JSValue val;
    ContextObject *context;
} ValueObject;

static PyTypeObject Runtime_Type;
static PyTypeObject Context_Type;
static PyTypeObject Value_Type;

static PyObject *make_context(RuntimeObject *runtime);

static PyObject *QuickJSError;  /* base exception */
static PyObject *JSError;       /* raised when JS throws */

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static PyObject *js_to_py(ContextObject *context, JSValueConst val);
static int py_to_js(ContextObject *context, PyObject *obj, JSValue *out);
static PyObject *Value_wrap(ContextObject *context, JSValue val);
static PyObject *make_js_function(ContextObject *context, PyObject *callable,
                                  const char *name, int length);

/* ------------------------------------------------------------------ */
/* Error helpers                                                        */
/* ------------------------------------------------------------------ */

/* Pull the pending JS exception, raise it as a Python JSError, return NULL. */
static PyObject *raise_js_exception(ContextObject *context)
{
    JSContext *ctx = context->ctx;
    JSValue exc = JS_GetException(ctx);
    PyObject *message = NULL;
    PyObject *stack = NULL;

    const char *msg = JS_ToCString(ctx, exc);
    if (msg) {
        message = PyUnicode_DecodeUTF8(msg, strlen(msg), "surrogatepass");
        JS_FreeCString(ctx, msg);
    }
    if (JS_IsError(ctx, exc)) {
        JSValue st = JS_GetPropertyStr(ctx, exc, "stack");
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
    case JS_TAG_UNDEFINED:
        Py_RETURN_NONE;
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
        /* objects, functions, symbols: keep a wrapped reference */
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
    if (tag == JS_TAG_OBJECT || tag == JS_TAG_SYMBOL ||
        tag == JS_TAG_FUNCTION_BYTECODE || tag == JS_TAG_MODULE) {
        return Value_wrap(context, *val);
    }
    PyObject *res = js_to_py(context, *val);
    JS_FreeValue(ctx, *val);
    return res;
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
        JSValue lenv = JS_GetPropertyStr(ctx, val, "length");
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
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue bigint = JS_GetPropertyStr(ctx, global, "BigInt");
        JS_FreeValue(ctx, global);
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

static JSValue js_trampoline(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv,
                             int magic, JSValue *func_data)
{
    ContextObject *context = (ContextObject *)JS_GetContextOpaque(ctx);
    int32_t id = 0;
    JS_ToInt32(ctx, &id, func_data[0]);

    PyObject *key = PyLong_FromLong(id);
    PyObject *callable = key ? PyDict_GetItemWithError(context->callbacks, key)
                             : NULL;
    Py_XDECREF(key);
    if (!callable) {
        return JS_ThrowInternalError(ctx, "quickjs-py: callback %d not found", id);
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
    long id = context->next_cb_id++;
    PyObject *key = PyLong_FromLong(id);
    if (!key) {
        return NULL;
    }
    if (PyDict_SetItem(context->callbacks, key, callable) < 0) {
        Py_DECREF(key);
        return NULL;
    }
    Py_DECREF(key);

    JSContext *ctx = context->ctx;
    JSValue data = JS_NewInt32(ctx, (int32_t)id);
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

/* Build argv from a Python sequence. Caller frees with free_js_argv. */
static JSValue *build_js_argv(ContextObject *context, PyObject *seq, int *argc)
{
    Py_ssize_t n = PySequence_Size(seq);
    if (n < 0) {
        return NULL;
    }
    JSValue *argv = PyMem_Malloc(sizeof(JSValue) * (n ? n : 1));
    if (!argv) {
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_GetItem(seq, i);
        if (!item || py_to_js(context, item, &argv[i]) < 0) {
            Py_XDECREF(item);
            for (Py_ssize_t j = 0; j < i; j++) {
                JS_FreeValue(context->ctx, argv[j]);
            }
            PyMem_Free(argv);
            return NULL;
        }
        Py_DECREF(item);
    }
    *argc = (int)n;
    return argv;
}

static void free_js_argv(ContextObject *context, JSValue *argv, int argc)
{
    for (int i = 0; i < argc; i++) {
        JS_FreeValue(context->ctx, argv[i]);
    }
    PyMem_Free(argv);
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
    JSValue *argv = build_js_argv(self->context, args, &argc);
    if (!argv) {
        JS_FreeValue(ctx, js_this);
        return NULL;
    }
    JSValue res = JS_Call(ctx, self->val, js_this, argc, argv);
    JS_FreeValue(ctx, js_this);
    free_js_argv(self->context, argv, argc);
    if (JS_IsException(res)) {
        return raise_js_exception(self->context);
    }
    return js_to_py_consume(self->context, &res);
}

static PyObject *Value_call_constructor(ValueObject *self, PyObject *args)
{
    JSContext *ctx = VALUE_CTX(self);
    int argc = 0;
    JSValue *argv = build_js_argv(self->context, args, &argc);
    if (!argv) {
        return NULL;
    }
    JSValue res = JS_CallConstructor(ctx, self->val, argc, argv);
    free_js_argv(self->context, argv, argc);
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

static Py_ssize_t Value_length(ValueObject *self)
{
    JSContext *ctx = VALUE_CTX(self);
    JSValue len = JS_GetPropertyStr(ctx, self->val, "length");
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

/* Create a ContextObject bound to `runtime` (new reference). */
static PyObject *make_context(RuntimeObject *runtime)
{
    JSContext *ctx = JS_NewContext(runtime->rt);
    if (!ctx) {
        PyErr_SetString(QuickJSError, "failed to create QuickJS context");
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
    context->next_cb_id = 0;
    context->callbacks = PyDict_New();
    if (!context->callbacks) {
        Py_DECREF(context);
        return NULL;
    }
    JS_SetContextOpaque(ctx, context);
    return (PyObject *)context;
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
        JS_FreeContext(self->ctx);
    }
    Py_XDECREF(self->callbacks);
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

static PyObject *Context_get_global(ContextObject *self, PyObject *args)
{
    return Value_wrap(self, JS_GetGlobalObject(self->ctx));
}

static PyObject *Context_get(ContextObject *self, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s:get", &name)) {
        return NULL;
    }
    JSValue global = JS_GetGlobalObject(self->ctx);
    JSValue v = JS_GetPropertyStr(self->ctx, global, name);
    JS_FreeValue(self->ctx, global);
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
    JSValue global = JS_GetGlobalObject(self->ctx);
    int r = JS_SetPropertyStr(self->ctx, global, name, jv);
    JS_FreeValue(self->ctx, global);
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
    {"execute_pending_job", (PyCFunction)Context_execute_pending_job,
     METH_NOARGS, "Run one pending job; returns True if a job ran."},
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
    JS_SetRuntimeOpaque(self->rt, self);
    return (PyObject *)self;
}

static void Runtime_dealloc(RuntimeObject *self)
{
    Py_XDECREF(self->interrupt_cb);
    if (self->rt) {
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

static PyMethodDef Runtime_methods[] = {
    {"new_context", (PyCFunction)Runtime_new_context, METH_NOARGS,
     "Create a new Context bound to this runtime."},
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
        PyType_Ready(&Value_Type) < 0) {
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
    PyModule_AddObject(m, "QuickJSError", QuickJSError);
    PyModule_AddObject(m, "JSError", JSError);
    PyModule_AddObject(m, "Runtime", (PyObject *)&Runtime_Type);
    PyModule_AddObject(m, "Context", (PyObject *)&Context_Type);
    PyModule_AddObject(m, "Value", (PyObject *)&Value_Type);

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
