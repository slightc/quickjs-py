"""Build configuration for the quickjs-py C extension.

The QuickJS engine (Bellard's reference implementation,
https://github.com/bellard/quickjs) is vendored under ``vendor/quickjs``
and compiled directly into the extension module, so there is no external
dependency on a system QuickJS install.
"""

import os
import sys

from setuptools import Extension, setup

VENDOR = "vendor/quickjs"

quickjs_sources = [
    f"{VENDOR}/quickjs.c",
    f"{VENDOR}/libregexp.c",
    f"{VENDOR}/libunicode.c",
    f"{VENDOR}/cutils.c",
    f"{VENDOR}/dtoa.c",
]

extension_sources = ["src/quickjs/_quickjs.c"] + quickjs_sources


def _quickjs_version() -> str:
    try:
        with open(os.path.join(VENDOR, "VERSION")) as fh:
            return fh.read().strip()
    except OSError:
        return "unknown"


define_macros = [
    ("_GNU_SOURCE", None),
    ("CONFIG_VERSION", f'"{_quickjs_version()}"'),
]
extra_compile_args = []
extra_link_args = []

if sys.platform == "win32":
    define_macros += [("WIN32_LEAN_AND_MEAN", None), ("_WIN32_WINNT", "0x0601")]
else:
    extra_compile_args += ["-std=gnu11", "-Wno-unused-parameter", "-Wno-sign-compare"]
    if sys.platform == "darwin":
        extra_compile_args += ["-Wno-implicit-fallthrough"]
    else:
        extra_link_args += ["-lm", "-lpthread"]

quickjs_ext = Extension(
    "quickjs._quickjs",
    sources=extension_sources,
    include_dirs=[VENDOR],
    define_macros=define_macros,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

setup(ext_modules=[quickjs_ext])
