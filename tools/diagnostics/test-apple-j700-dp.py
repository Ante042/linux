#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production J700 helpers against bounded host register models."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[2]
source = (root / "drivers/gpu/drm/apple/dptxep.c").read_text()
functions = []
for name in ("dptxport_v2_group_call", "dptxport_call_group1", "dptxport_group_call"):
    start = source.index("static int " + name + "(")
    end = source.index("\n}\n", start) + 3
    functions.append(source[start:end])
with tempfile.TemporaryDirectory(prefix="j700-dp-") as work:
    work = Path(work)
    (work / "group-call.h").write_text("\n\n".join(functions))
    for name in ("apple-j700-dp-test", "apple-dptx-training-test"):
        command = shlex.split(os.environ.get("CC", "clang"))
        command += ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += ["-I" + str(root), "-I" + str(work), "-DDPTX_GROUP_TEST"]
        command += [str(root / "tools/diagnostics" / (name + ".c"))]
        command += ["-o", str(work / name)]
        subprocess.run(command, check=True)
        subprocess.run([str(work / name)], check=True)
