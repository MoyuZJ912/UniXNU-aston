#!/usr/bin/env python3
"""Give UniXNU's patches/mesa/turnip-frame-diagnostics.patch the "new file mode" line its new header lacks, in a UniXNU checkout.

  fix-upstream-mesa-patch.py UNIXNU_CHECKOUT

The patch creates src/freedreno/vulkan/tu_frame_diagnostic.h with a git header ("diff --git", "--- /dev/null") but no
"new file mode 100644". git apply then reads "/dev/null" as a path ("error: dev/null: No such file or directory"), so a FRESH
.cache/mesa-work cannot be patched (build-boot-runtime.py build_plugin), and the three later patches that edit the header fail
with it. A Mesa checkout that already exists is not patched again, which is why only a fresh build (CI, run 42462688043) meets
this. Checked in WSL (git 2.43) on Mesa 85fda3b3: the upstream file fails the same way; the fixed one applies all 17 recorded
patches in order, and mesa_patch_identity.validate_recorded_patches passes on the result (it passes with either file, since
its reverse application does not need the mode line). The tree's patches/mesa links to the UniXNU checkout, so the fix is
made there, before `unixnu build`.

Only this exact file (UniXNU 5dc00d201496a1e7a72d1f5db23f041eb94ace1c) is changed, by that one line: the patched Mesa source
is the same. A file that is already fixed is left alone; any other content stops with an error."""
import hashlib
import sys
from pathlib import Path

NAME = 'patches/mesa/turnip-frame-diagnostics.patch'
HEADER = 'diff --git a/src/freedreno/vulkan/tu_frame_diagnostic.h b/src/freedreno/vulkan/tu_frame_diagnostic.h\n'
UPSTREAM = '5eb8dda6e381f10fa48b62a9a497fb74a8b08c7999dea5468585c9848b2af966'
FIXED = 'cd15bbb8f932d564a99300e63d3d6c5a6a714b311596e80619092c5d3fa5e25a'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = Path(sys.argv[1]) / NAME
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest == FIXED:
        print(f'{NAME}: already fixed')
        return
    if digest != UPSTREAM:
        sys.exit(f'{NAME}: sha256 {digest} is neither the known upstream nor the fixed file; check whether UniXNU fixed it')
    text = data.decode()
    if text.count(HEADER + '--- /dev/null\n') != 1:
        sys.exit(f'{NAME}: the new-file header is not where it is expected')
    text = text.replace(HEADER, HEADER + 'new file mode 100644\n', 1)
    if hashlib.sha256(text.encode()).hexdigest() != FIXED:
        sys.exit(f'{NAME}: the fixed file differs from the one checked')
    path.write_text(text)
    print(f'{NAME}: added "new file mode 100644" ({UPSTREAM[:12]} -> {FIXED[:12]})')


if __name__ == '__main__':
    main()
