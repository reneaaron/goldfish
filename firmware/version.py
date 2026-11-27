# PlatformIO pre-build script: writes include/version.h with FW_VERSION, the
# version the home screen shows. A build of a tagged commit is that tag
# (v0.1.0; dove-v* tags are the companion's and don't count); anything else
# is the short commit hash. "+" marks uncommitted changes. The file is only
# rewritten when the version changes, so a normal rebuild doesn't recompile
# ui.cpp.
import os
import subprocess

Import("env")  # noqa: F821


def git(*args):
    try:
        return subprocess.check_output(("git",) + args, cwd=env["PROJECT_DIR"],  # noqa: F821
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


version = git("describe", "--tags", "--exact-match", "--match", "v*") or git("rev-parse", "--short=7", "HEAD") or "unknown"
if git("status", "--porcelain", "--untracked-files=no"):
    version += "+"

path = os.path.join(env["PROJECT_DIR"], "include", "version.h")  # noqa: F821
content = '#pragma once\n#define FW_VERSION "%s"\n' % version
if not os.path.exists(path) or open(path).read() != content:
    with open(path, "w") as f:
        f.write(content)
