# CLAUDE.md - Linux Kernel for HelloTechno HT-PA20

## Project Overview

This is a customized **Linux Kernel 6.1.30** based on Xilinx's linux-xlnx repository, specifically adapted for the **HelloTechno HT-PA20 embedded board** which uses the Zynq-7000 SoC platform.

### Key Information
- **Kernel Version**: 6.1.30 "Curry Ramen"
- **Architecture**: ARM (32-bit)
- **Target Platform**: Xilinx Zynq-7000 SoC
- **Board**: HelloTechno HT-PA20
- **License**: GPL-2.0
- **Repository**: Fork of [Xilinx/linux-xlnx](https://github.com/Xilinx/linux-xlnx.git)

## Repository Structure

### Core Directories
```
├── arch/arm/                    # ARM architecture support
│   ├── boot/dts/zynq-htpa20.dts # HT-PA20 device tree
│   ├── configs/                 # Board configurations
│   └── mach-zynq/              # Zynq platform code
├── drivers/                     # Device drivers
│   ├── gpu/drm/xlnx/           # Xilinx Display drivers
│   ├── remoteproc/             # Zynq remote processor
│   └── [various subsystems]
├── include/                     # Header files
│   └── uapi/misc/xilinx*       # Xilinx userspace APIs
├── Documentation/               # Kernel documentation
├── scripts/                     # Build and development tools
└── fs/, mm/, kernel/, net/     # Core kernel subsystems
```

### Build System
- **Main Makefile**: Cross-compilation setup for ARM
- **Kconfig**: Kernel configuration system
- **Device Tree**: Hardware description for HT-PA20 board
- **Scripts**: Automated build and validation tools

## Hardware Support

### HT-PA20 Board Features
- **SoC**: Xilinx Zynq-7000 (dual-core ARM Cortex-A9)
- **Memory**: 512MB DDR3 RAM
- **Storage**: eMMC with power control
- **Connectivity**:
  - Gigabit Ethernet (Xilinx GEM)
  - Multiple UART interfaces (8 additional serial ports)
  - I2C and SPI buses
  - USB with PHY support

### Serial Interfaces
The HT-PA20 board includes extensive serial communication:
- RS485 interfaces (2x)
- Printer interface
- FeliCa card reader
- Magnetic card reader
- UPS communication
- JVM interfaces (2x)
- MDB (Multi-Drop Bus)

## Development Workflow

### Building the Kernel

```bash
# Configure for HT-PA20
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- hellotechno_htpa20_defconfig

# Build kernel and device tree
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage dtbs

# Build modules
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
```

### Configuration
- **Default Config**: `arch/arm/configs/hellotechno_htpa20_defconfig`
- **Device Tree**: `arch/arm/boot/dts/zynq-htpa20.dts`
- **Cross Compiler**: `arm-linux-gnueabihf-gcc`
- **Load Address**: `0x200000`

### Installation Paths
- **Modules**: `../rootfs/` (relative to kernel source)
- **Boot Files**: `../rootfs/boot/`

## Recent Modifications

Recent commits include HT-PA20-specific improvements:
- Watchdog reset functionality
- eMMC power control for hardware revision E
- USB PHY type optimization
- MMC hardware reset fixes
- Network driver enhancements for high-frequency operation

## Code Quality Standards

### Coding Style
- Follows Linux kernel coding standards (8-character tabs)
- See `Documentation/process/coding-style.rst`
- Use `scripts/checkpatch.pl` for patch validation

### Development Tools
```bash
# Check code style
./scripts/checkpatch.pl --file <filename>

# Generate tags for code navigation
make tags

# Build documentation
make htmldocs
```

## Key Subsystems

### Xilinx-Specific Features
- **DRM/Display**: Hardware-accelerated graphics (`drivers/gpu/drm/xlnx/`)
- **DMA**: Xilinx DMA engine support
- **FPGA**: Programmable logic integration
- **Crypto**: Hardware cryptographic acceleration

### Board-Specific Drivers
- eMMC power management
- Multi-UART support
- Industrial I/O interfaces
- Watchdog timer integration

## Documentation

### Essential Reading
- `Documentation/admin-guide/README.rst` - Getting started
- `Documentation/process/submitting-patches.rst` - Contribution guidelines
- `Documentation/devicetree/` - Device tree documentation
- `Documentation/arm/` - ARM-specific documentation

### Development Process
- Follow Linux kernel development practices
- Use appropriate mailing lists for submissions
- Include proper commit message formatting
- Test thoroughly on target hardware

## Contributing

### Patch Submission
1. Create patches with proper commit messages
2. Test on HT-PA20 hardware
3. Run `scripts/checkpatch.pl` validation
4. Follow kernel submission guidelines

### Git Workflow
```bash
# Remotes configured
git remote -v
# origin: git@github.com:sugioka565/linux-xlnx.git
# xilinx: https://github.com/Xilinx/linux-xlnx.git

# Create feature branch
git checkout -b feature/my-enhancement

# Make changes and commit
git commit -s -m "subsystem: description of change"
```

## Debugging and Testing

### Build Verification
```bash
# Verify build system
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)

# Check for warnings
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1
```

### Hardware Testing
- Boot test on HT-PA20 board
- Verify all serial interfaces
- Test eMMC power management
- Validate Ethernet connectivity
- Check peripheral device functionality

## Troubleshooting

### Common Issues
- **MMC Recovery**: See `memo.txt` for MMC busy state recovery issues
- **Cross-compilation**: Ensure ARM toolchain is properly installed
- **Device Tree**: Validate DTS syntax with `scripts/dtc/dtc`

### Useful Commands
```bash
# Extract configuration from running kernel
zcat /proc/config.gz > current_config

# Compare configurations
./scripts/diffconfig old_config new_config

# Decode oops/panic messages
./scripts/decode_stacktrace.sh vmlinux < oops.txt
```

## Additional Resources

- [Xilinx Linux-xlnx Repository](https://github.com/Xilinx/linux-xlnx)
- [Linux Kernel Documentation](https://www.kernel.org/doc/html/latest/)
- [Zynq-7000 Technical Reference Manual](https://docs.xilinx.com/v/u/en-US/ug585-Zynq-7000-TRM)
- [Device Tree Specification](https://devicetree-specification.readthedocs.io/)