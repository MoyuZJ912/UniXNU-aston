#!/usr/bin/env python3
"""Build and run the Ace 3 fuel gauge host checks (package/AstonGaugeTests.c) with the host C compiler."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package'
cc = os.environ.get('CC', 'cc')
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / 'aston-gauge-tests'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(PACKAGE),
                    str(PACKAGE / 'AstonGaugeTests.c'), str(PACKAGE / 'AstonGaugeBus.c'), str(PACKAGE / 'AstonGauge.c'),
                    '-o', str(binary)], check=True)
    sys.exit(subprocess.run([str(binary)]).returncode)
