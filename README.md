# UniXNU aston

Device support for the OnePlus Ace 3 (PJE110, OnePlus board `aston`, project 23801), Qualcomm SM8550, 16 GiB / 512 GiB.
It is derived from [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) `689400177f93219a38bd7cf5ae2e9f5782adc594`
(Xiaomi Pad 6S Pro, the reference SM8550 board) and follows its iPadOS 27.0.1 (24A446) chain. The package keeps the
reference kit name `sheng` and the `sheng-*` boot argument names, which the shared scripts address.

The first phone boot reached SpringBoard with touch on the CPU display path, as recorded in `board.toml`.
The current CPU and Data changes still need a phone test. See the [fix review](reports/cpu-storage-fix.md) and
[porting record](porting.toml); inherited sheng results do not qualify aston hardware.

## OS profile

The board targets iPadOS 27.0.1 (24A446). The reference board's iPadOS 17 chain cannot be rebuilt for another board: its
display and HID builders need prelinked candidate kernels qualified on sheng hardware
(`scripts/audit-mobile-graphics-legacy.py` accepts one candidate hash). The 24A446 kernel (xnu-12377) uses Apple clang's
four-argument `__ptrauth` qualifier, so it builds with Xcode only: `.github/workflows/package.yml` builds the kernel and the
payload on a `macos-26` runner. A Linux host runs the board checks below.

## What differs from the reference board

| Area | Ace 3 |
| --- | --- |
| Storage | ShengUfs matches only the GPT partition `unixnu` (`drivers/ShengUfs/RootLayout.h`), the 32 GiB carved from the end of userdata (LBA 115424000..123812607). Android's `userdata` is never opened. |
| Touch | Synaptics S3910 (TouchComm, from the OnePlus 13T port) on QUP1 SE4 SPI: `package/TouchProbe.cpp`, `S3910.c`, `TouchBoard.h`. Reset GPIO24, ATTN GPIO25, AVDD GPIO10, VDD ldob4 through RPMh. |
| Keys | Volume up/down swapped against sheng (`package/ButtonProbe.cpp`, `sheng-buttons-swap=0` restores). |
| Battery | `sheng-battery=2`: the pack's TI bq28z610 fuel gauge (2S, I2C 0x55 on QUP2 SE0 `i2c@880000`, gpio0/1) read directly, without the audio DSP (`package/AstonGaugeBus.c`, `AstonGauge.c`, `ShengBattery.cpp`). Read only; capacities are reported for both cells, the voltage for one. |
| GPU | The ZAP shader is the phone's own (`scripts/pull-aston-gpu-firmware.sh`, pin in `scripts/data/aston-gpu-inputs.json`); without it the kernel carries an empty ZAP image and the GPU stops at its ZAP step. GMU and SQE images are the reference ones. |
| Firmware | Mu-Silicium `aston`, built and packed by the sibling `Ace3-UniXNU` repository; `[firmware]` here is unused. |

Not ported yet: DCS brightness through DSI0 (the panel stays at the boot loader's level), the audio DSP and its users
(audio, DSP battery manager), Wi-Fi, Bluetooth and USB.

## Checks on a Linux host

From the UniXNU checkout, with an iOS 17 tree whose bootstrap pass produced the compile records
(`./unixnu defconfig` of any iOS 17 configuration, then `./unixnu build kernel` until it reaches the archives):

```sh
python3 ../UniXNU-aston/scripts/check-aston-board-objects.py --tree out/<device>/ipados-17.7.11-21H461/<config>/tree --device ../UniXNU-aston
python3 ../UniXNU-aston/scripts/check-aston-gauge-host.py
python3 out/<...>/tree/scripts/check-touch-host.py      # from a build tree
```

The first compiles the board sources with the kernel's recorded flags and `-Werror`; the iOS 17 records carry the same IOKit
and arm64 kernel interfaces these sources use.

## Build on macOS

1. Push this repository to GitHub and add the secret `UNIXNU_UNIQUE_CHIP_ID` (the platform seed). Optionally add
   `ASTON_A740_ZAP_B64`, the base64 of the phone's ZAP image, after filling its pin.
2. Run the `package` workflow with `init_data` for a first installation. Its artifacts are `kernel.macho`, `payload.img`,
   `payload.json` and the installation seed.

The driver checkouts listed in `drivers/submodules.lock` are cloned by `scripts/fetch-aston-drivers.sh` (some carry file
names Windows cannot hold).

## Boot arguments and CPU/Data checks

`board.toml` `[payload] kernel_args` enables S3910 touch, keys, all CPUs, CPU frequency, persistent Data, the fuel gauge,
GPU framebuffer presentation and the log on `unixnu`. The package menu also offers the CPU display path.
`armcpu-edge=1` completes the patched Edge scheduler setup. Touch boosts last 80 ms; the frame-driven CPU floor is disabled.

The first archive step applies the reviewed RamBlock patch to an isolated build-tree copy. It verifies the original pin
and source hash, rejects tracked source overrides, and writes `build/aston-ramblock/source.json`. Both storage recipes
use this staged source. Persistent Data is attached before APFS can probe it; an unavailable writable store stops Data
startup. Reuse the installation's original Data seed when updating. Creating a new seed or selecting Install and boot
is not part of this fix.

After applying all selected XNU patches, run the native host fixtures (Windows, macOS or Linux):

```sh
python scripts/check-aston-cpu-storage.py --xnu /path/to/patched/xnu --ramblock /path/to/original/RamBlock --cc clang
```

The RamBlock checkout must be `4be2d7eb05ef5a98126c91e0302c149140c0e3e1`. Windows can use `--cc /path/to/zig.exe cc`.
The fixtures exercise policy and publication decisions with API stand-ins. They do not build XNU or execute APFS.
