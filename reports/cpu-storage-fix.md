# Aston CPU and persistent Data review

Device: OnePlus Ace 3 PJE110, SM8550, 16 GiB / 512 GiB. Target: iPadOS 27.0.1 (24A446).
Implementation revision: `c72ab4e2aa75727569162823cac20100b4c36703`.
Base device revision: `afde8ebc8fa023712e6c5a09a50438476649fbcb`.

The user reports excessive heat, drain, severe UI lag, zero storage capacity in Settings, and crashes in Files and other
applications using local shared storage. No matching crash log, firmware/image hash or current boot-menu selection was
available. These changes fix source defects that can contribute to those symptoms; they do not establish their complete
cause or demonstrate that the symptoms are gone.

## Source findings and changes

ArmCpu `93a5030c5d983d0ef90f0fca93879d6e36ad1ba8` defaults `armcpu-edge` to zero and skips `ml_cpu_init_completed()`.
The board now enables it. The old board policy prohibited P-to-E migration for high QoS buckets but allowed it for lower
buckets, contradicting XNU's non-decreasing QoS strictness check. The new patch restores weighted P-to-E spill for all
buckets and the development/debug assertion. Utility/background groups still prefer E; idle P cores no longer steal E
work. Foreground groups still prefer P. This is the two-cluster policy for the selected source, not a thermal controller.

Absent/offline CPUs previously returned zero idle time and could be counted as fully busy by the frequency governor.
They now return an unavailable sentinel. The governor rebases after unavailable or regressing samples and bounds its
arithmetic. Touch boosts last 80 ms instead of the inherited 1000 ms. Display activity no longer maintains a 60% CPU floor.
The existing maximum L3 vote, WFI-only idle path and missing thermal feedback remain limitations. No frequency, temperature,
energy or frame-time improvement was measured on the phone.

RamBlock `4be2d7eb05ef5a98126c91e0302c149140c0e3e1` published its seed-backed disk before attaching persistent storage.
APFS could therefore probe seed checkpoints, discover a volume UUID, and only then switch to the persisted backing during
volume selection. The patch attaches backing before publication. `package/SourceStorage.cpp` starts UFS first, and
`NativeUfs::start` initializes the store geometry before returning. A missing, mismatched or failed writable store now
prevents publication; source-storage startup reports a Data start failure. Partial read-only restoration also fails closed.
Small explicitly volatile/read-only probe configurations retain their existing behavior.

The original Data seed identity, explicit install/reset rules, GPT selection, partition bounds and overlay format are
unchanged. Reuse the installation seed when updating. This change neither repairs already-corrupt APFS metadata nor
regenerates a seed. An installation with unavailable or mismatched backing can now stop at Data startup instead of
proceeding with a seed-only disk.

`scripts/prepare-aston-ramblock.py` checks the pinned revision, normalized source digest and tracked source overrides,
then patches an isolated copy in the build tree. It runs before both `component:ShengUfs` and `component:RamBlock` and
records source/patch hashes in `build/aston-ramblock/source.json`. Shared checkouts and component/runtime names stay intact.
APFS capacity is queried at successful Data mount and exported in `[data mount capacity]`; failures are recorded without
inventing free space. Exit logging includes Files, fileproviderd and diskspaced. The capacity row is a boot checkpoint,
not a live storage meter.

## Commands and host results

Tests ran natively on Windows. No WSL or QEMU was used. The main checkout is pinned to
`5dc00d201496a1e7a72d1f5db23f041eb94ace1c`, the SM8550 checkout to
`97665e28578483082b674e8375a8fa65f4cf49cf`, and XNU to
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f`.

From the pinned UniXNU checkout, with `DEVICE` naming this repository and `SOC` the unchanged SM8550 pin:

```text
python unixnu defconfig aston_ipados27_defconfig --device DEVICE --local-module soc:sm8550=SOC
python unixnu status --device DEVICE --local-module soc:sm8550=SOC --json
python unixnu tree --device DEVICE --local-module soc:sm8550=SOC
python unixnu build kernel --device DEVICE --local-module soc:sm8550=SOC
```

The first three commands passed. Kernel build stopped with `make not found on PATH (brew install make)`.
The selected arm64e source also requires Xcode's compiler and SDK. No full kernel/object/prelink build is claimed.
The copied preparation script was then run directly in the assembled tree; staging passed against its resolved RamBlock
checkout. Both component build recipes read the staged `drivers/RamBlock/NativeRamBlock.cpp` path.

All 91 selected patches applied in architecture, SoC, OS and device order to a fresh pinned XNU worktree using
`git apply --whitespace=nowarn`. The native fixture command was:

```text
python scripts/check-aston-cpu-storage.py --xnu .cache/xnu-verify --ramblock .cache/RamBlock --cc .cache/toolchain/ziglang/zig.exe cc
```

It passed 30 publication cases, nine load-accounting cases, the actual XNU QoS validator (including rejection of the
previous policy), seven capacity-query cases, repeated staging, source preservation and archive ordering. The compiler
used `-Wall -Wextra -Werror`. These fixtures use API stand-ins; they do not execute APFS or IOKit.

The unmodified upstream `RamBlock/Test/persist.cpp` and `Test/direct.cpp` also passed under native Zig C++ targeting
`x86_64-windows-gnu`, with `-std=c++17 -Wall -Wextra -Werror`. The direct test additionally needs
`-Wno-unused-function` for the upstream header's unused `seedCountNonzero`. They cover overlay resume, stale seed handling,
explicit reset, write failure, direct reads/writes, seed immutability and short copies. The first direct compilation
failed only on that unused helper; the subsequent run passed.

The porting-record checker passes all 16 areas. The base boot argument string is 871 bytes, below the 1000-byte limit.
All four new diagnostic globals are recognized by the source-storage weak-default generator. Review checked patch order,
publication ordering, canonical paths, source isolation, failure behavior, arithmetic and the absence of new reset writes.

## Missing inputs and device checkpoints

The original installation seed, platform seed, qualified firmware/image and a Mac/Xcode build environment are needed to
produce and qualify an update. The user has not supplied device logs for this report. Existing sheng observations in
`porting.toml` are explicitly reference-board evidence, not aston verification.

| Checkpoint | Required observation | Current result |
| --- | --- | --- |
| Boot | Exact firmware/image revision; eight CPUs online; Edge setup without assertion | Pending |
| Storage | Persistence state 2 on an ordinary resumed boot; writable APFS Data; nonzero valid capacity | Pending |
| Files | Open local storage, create/read/rename/delete a file; retain it through reboot | Pending |
| Display | Stable SpringBoard, compare CPU and GPU framebuffer menu entries | Pending |
| Input | Touch, keys, scrolling and wake responsiveness | Pending |
| GPU | Submission/retirement progress and no repeating faults | Pending |
| Power | Idle and interactive frequency/current/temperature samples over a fixed interval | Pending |

If capacity remains zero, compare the capacity status with the Data mount and persistence rows before investigating
diskspaced or Files exit reasons. A valid mount capacity with a zero Settings value points to a separate userspace issue.
If lag remains, collect CPU utilization, GPU progress and process exits rather than assuming scheduling is its only cause.

## Site documentation

The English and Chinese ArmCpu and RamBlock pages were updated in the pinned UniXNU checkout. Their portable diff is
`patches/documentation/cpu-storage-guides.patch`; it is not part of the kernel series. Apply it from that UniXNU revision
with `git apply /path/to/UniXNU-aston/patches/documentation/cpu-storage-guides.patch` for upstream site review.
The device's bilingual generated pages consume the updated `porting.toml`. No upstream site deployment is claimed.
