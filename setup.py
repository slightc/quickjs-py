"""Build configuration for the quickjs-py C extension.

The QuickJS engine (Bellard's reference implementation,
https://github.com/bellard/quickjs) lives in the ``vendor/quickjs`` git
submodule and is compiled directly into the extension module, so there is
no external dependency on a system QuickJS install.

QuickJS relies on GCC extensions (computed-goto bytecode dispatch) and
POSIX headers, so it cannot be built with MSVC. The whole project is
therefore compiled with a GCC/Clang toolchain on every platform; on
Windows that means mingw-w64, and ``gcc`` must be available on PATH.

Run ``git submodule update --init`` before building from a git checkout.
"""

import os
import sys

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

VENDOR = "vendor/quickjs"

if not os.path.exists(os.path.join(VENDOR, "quickjs.c")):
    raise SystemExit(
        "QuickJS sources not found under vendor/quickjs.\n"
        "Initialise the submodule first: git submodule update --init"
    )

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
# Every platform builds with a GCC/Clang toolchain (mingw-w64 on Windows).
extra_compile_args = ["-std=gnu11", "-Wno-unused-parameter", "-Wno-sign-compare"]
extra_link_args = []

if sys.platform == "darwin":
    extra_compile_args += ["-Wno-implicit-fallthrough"]
elif sys.platform == "win32":
    define_macros += [("WIN32_LEAN_AND_MEAN", None), ("_WIN32_WINNT", "0x0601")]
    # Statically pull in the mingw runtime so the .pyd needs no extra DLLs.
    extra_link_args += ["-static", "-lm", "-lpthread"]
else:
    extra_compile_args += ["-Wno-implicit-fallthrough"]
    extra_link_args += ["-lm", "-lpthread"]


class BuildExt(build_ext):
    """Force the mingw-w64 (GCC) toolchain on Windows.

    MSVC cannot compile QuickJS, so on Windows we always select the
    ``mingw32`` distutils compiler unless the caller explicitly picked one.
    """

    def initialize_options(self):
        super().initialize_options()
        if sys.platform == "win32" and not self.compiler:
            self.compiler = "mingw32"


quickjs_ext = Extension(
    "quickjs._quickjs",
    sources=extension_sources,
    include_dirs=[VENDOR],
    define_macros=define_macros,
    extra_compile_args=extra_compile_args,
    extra_link_args=extra_link_args,
)

setup(ext_modules=[quickjs_ext], cmdclass={"build_ext": BuildExt})
