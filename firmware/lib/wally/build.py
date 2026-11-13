# Builds libwally-core from its single-file amalgamation. PlatformIO fetches
# the sources (lib_deps in platformio.ini) but can't build them as they come:
# there is no library.json, and src/ holds tests, bindings and Elements.
import os

from SCons.Script import DefaultEnvironment

Import("env")  # noqa: F821  (this library's build environment)

wally = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "libwally-core")  # noqa: F821
here = os.path.join(env.subst("$PROJECT_DIR"), "lib", "wally")  # noqa: F821

env.Append(  # noqa: F821
    CPPPATH=[
        os.path.join(here, "config"),          # our config.h
        wally,                                  # combined.c includes "src/..."
        os.path.join(wally, "src"),
        os.path.join(wally, "src", "ccan"),
        os.path.join(wally, "src", "secp256k1", "include"),
    ],
    # BUILD_MINIMAL: libwally's small-footprint mode for embedded targets.
    CPPDEFINES=["BUILD_MINIMAL"],
)

# The public headers, for the firmware's own sources.
DefaultEnvironment().Append(CPPPATH=[os.path.join(wally, "include")])
