# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the **Xilinx/AMD Linux kernel fork** (`linux-xlnx`) based on Linux 6.12 LTS, targeting Xilinx Zynq-7000 (ARM32), ZynqMP, and Versal (ARM64) SoC families. The upstream repo is `https://github.com/Xilinx/linux-xlnx.git`. The current branch `local-xlnx_rebase_v6.12_LTS` tracks Xilinx's rebase of mainline v6.12.x with vendor patches on top.

## Build Commands

### ARM64 (ZynqMP/Versal) — primary target

```bash
# Configure
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- xilinx_defconfig

# Build kernel image + modules + device trees
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)

# Build only device trees
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs

# Build a single module (example)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=drivers/gpu/drm/xlnx modules
```

### ARM32 (Zynq-7000)

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- xilinx_zynq_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)
```

### Code Quality

```bash
# Check a patch for kernel style violations
scripts/checkpatch.pl --no-tree -f <file>
scripts/checkpatch.pl <patch-file>

# Check device tree binding schemas
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dt_binding_check
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs_check

# Sparse static analysis on a single file
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- C=1 drivers/gpu/drm/xlnx/xlnx_mixer.o
```

## Contribution Policy

Do NOT submit GitHub Pull Requests. Patches go through mailing lists per:
https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842172/Create+and+Submit+a+Patch

## Architecture — Xilinx-Specific Subsystems

The fork extends mainline Linux with drivers for Xilinx programmable logic (PL) soft IPs. Key vendor-specific subsystems:

### DRM/Display (`drivers/gpu/drm/xlnx/`)
KMS display driver stack: CRTC, framebuffer, GEM, DisplayPort TX, video mixer, HDCP. The mixer driver (`xlnx_mixer.c`) composites multiple video planes in hardware. Bridges connect PL display IPs to the DRM pipeline via `xlnx_bridge.c`.

### Video/Media Pipeline (`drivers/media/platform/xilinx/`)
50+ files implementing a video IP pipeline framework. `xilinx-vipp.c` orchestrates chains of processing blocks (CSI-2 RX, demosaic, gamma correction, scaler, color space conversion) connected via `xilinx-dma.c`. Each processing block is a V4L2 subdevice. HDMI/SDI/DisplayPort receivers have their own subsystem drivers.

### Audio (`sound/soc/xilinx/`)
ASoC drivers for PL audio IPs: I2S, SPDIF, SDI audio, audio DMA formatter. `xlnx_pl_snd_card.c` wraps soft audio IPs into ALSA sound cards.

### FPGA Management (`drivers/fpga/xilinx-*.c`)
FPGA manager for full and partial reconfiguration. Programming interfaces include SPI, SelectMAP, and ICAP (`drivers/char/xilinx_hwicap/`).

### AI Engine (`drivers/misc/xilinx-ai-engine/`)
Driver for the Versal AI Engine tile array — manages tile configuration, DMA, events, and memory. Userspace interface via UIO.

### Firmware/SOC Services (`drivers/firmware/xilinx/`, `drivers/soc/xilinx/`)
Interface to the platform management unit (PMU/PSM) via EEMI (Embedded Energy Management Interface). Handles power domains, clocks, resets, and secure monitor calls.

### Networking
- AXI Ethernet MAC: `drivers/net/ethernet/xilinx/xilinx_axienet_*.c`
- TSN (Time-Sensitive Networking): `drivers/staging/xilinx-tsn/` — IEEE 802.1 switch, QBV scheduling, PTP
- CAN: `drivers/net/can/xilinx_can.c`

### DMA Engines (`drivers/dma/xilinx/`)
General-purpose DMA (`xilinx_dma.c`), DisplayPort DMA (`xilinx_dpdma.c`), and frame buffer DMA (`xilinx_frmbuf.c`).

### Crypto (`drivers/crypto/xilinx/`)
Hardware RSA, ECDSA engines, and TRNG.

## Device Trees

- ARM64 boards: `arch/arm64/boot/dts/xilinx/` (ZynqMP, Versal)
- ARM32 boards: `arch/arm/boot/dts/xilinx/` (Zynq-7000: ZED, ZC702, ZC706, ZYBO, etc.)
- DT binding docs: `Documentation/devicetree/bindings/` (look under `display/xlnx/`, `media/xilinx/`, `soc/xilinx/`, `net/xlnx*`)

## Kernel Coding Conventions

- Follow kernel coding style: tabs for indentation, 80-column soft limit
- Use `scripts/checkpatch.pl` before submitting patches
- `.clang-format` is present for reference but kernel style takes precedence
- Commit messages follow kernel conventions: `subsystem: Short summary` (50-char subject, imperative mood, body wrapped at 72 chars)
- Xilinx driver commits typically use prefixes like `drm: xlnx:`, `ASoC: xlnx:`, `net: xilinx:`, `media: xilinx:`

## Remote Target Debugging

The local settings indicate a remote Kria board at `ibara3.local` used for testing. Dynamic debug can be enabled via:
```bash
ssh ibara3.local "echo 'module xlnx_mixer +p' | sudo tee /sys/kernel/debug/dynamic_debug/control"
```
Deploy modules to the target with `scp`.
