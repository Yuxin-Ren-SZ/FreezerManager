# Intentionally a no-op: the C++ build never needs Node.
#
# The SPA in this directory is built with npm (Vite), not with CMake — see
# doc/dev/web.md and TODO.md G1.1. This file only marks src/web/ as part of the
# source tree, so `cmake --build --preset dev` stays independent of the web
# toolchain and CI can run the `web` job on its own.
