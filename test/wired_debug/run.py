#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / "input_test"
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root / "lib/hal"),
                    "-I", str(root / "src/debug"), str(Path(__file__).with_name("input_test.cpp")), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    stubs = Path(directory) / "stubs"
    (stubs / "activities").mkdir(parents=True)
    for name in ("GfxRenderer.h", "HalDisplay.h", "HalGPIO.h", "HalPowerManager.h", "Logging.h", "activities/RenderLock.h", "activities/Activity.h", "activities/ActivityManager.h"):
        (stubs / name).write_text('#include "Fakes.h"\n')
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(stubs),
                    "-I", str(root / "test/wired_debug"), "-I", str(root / "lib/hal"), "-I", str(root / "src/debug"),
                    str(root / "src/debug/WiredDebug.cpp"), str(root / "test/wired_debug/handler_test.cpp"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
subprocess.run([sys.executable, str(Path(__file__).with_name("test_host.py"))], check=True)
print("Wired debug: parser, input lifecycle, cancellation, time wrap, gestures, framing, checksum and PNG orientation passed")
