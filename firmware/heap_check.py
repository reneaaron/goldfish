# PlatformIO post-build script for the board: fails the build when the heap
# left after the static data is smaller than include/heap_budget.h promises
# (APP_HEAP_BYTES for the app, which the simulator gives it exactly, plus
# CORE_HEAP_BYTES for the Arduino core). Static data that grows past that has
# to lower APP_HEAP_BYTES, and the simulator then shows whether the app still
# fits: no more finding out on a board, as "Out of memory".
import os
import re
import subprocess

Import("env")  # noqa: F821


def budget(name):
    text = open(os.path.join(env["PROJECT_DIR"], "include", "heap_budget.h")).read()  # noqa: F821
    return int(re.search(r"#define %s \((\d+) \* 1024\)" % name, text).group(1)) * 1024


def check(target, source, env):
    cc = env.subst("$CC")
    head, _, tail = cc.rpartition("gcc")   # arm-none-eabi-gcc -> arm-none-eabi-nm, wherever it lives
    nm = head + "nm" + tail
    syms = {}
    elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    for line in subprocess.check_output([nm, elf], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("end", "__HeapLimit"):
            syms[parts[2]] = int(parts[0], 16)
    heap = syms["__HeapLimit"] - syms["end"]
    need = budget("APP_HEAP_BYTES") + budget("CORE_HEAP_BYTES")
    print("Heap: %d bytes (app %d + core %d needed, %d to spare)"
          % (heap, budget("APP_HEAP_BYTES"), budget("CORE_HEAP_BYTES"), heap - need))
    if heap < need:
        print("Error: static data leaves %d bytes of heap, %d short of include/heap_budget.h" % (heap, need - heap))
        # no image to flash by hand from the BOOT drive (the next build links again)
        for ext in ("uf2", "bin", "elf"):
            path = env.subst("$BUILD_DIR/${PROGNAME}.") + ext
            if os.path.exists(path):
                os.remove(path)
        env.Exit(1)


env.AddPostAction("buildprog", check)  # every build, also when nothing was relinked  # noqa: F821
