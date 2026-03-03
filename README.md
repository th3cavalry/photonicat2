# Photonicat 2 — Open-Source Tools & Documentation

Open-source tools, drivers, and documentation for the **Ariaboard Photonicat 2**, a portable dual-Gigabit Ethernet router based on the Rockchip RK3576 SoC.

## What is the Photonicat 2?

The Photonicat 2 is a compact, battery-powered networking device featuring:

| Component | Detail |
|-----------|--------|
| **SoC** | Rockchip RK3576 (4× Cortex-A72 + 4× Cortex-A53, big.LITTLE) |
| **RAM** | 4GB / 8GB / 16GB LPDDR4X or LPDDR5 |
| **Storage** | 32GB / 64GB / 128GB eMMC + microSD slot |
| **Ethernet** | 2× Gigabit (Realtek RTL8211F PHY, RGMII) |
| **WiFi** | Qualcomm WCN6855 WiFi 6E (M.2 Key E, PCIe Gen2 x1) |
| **Cellular** | Quectel RM520N-GL 5G (M.2 Key B, USB QMI) |
| **NVMe** | M.2 Key M (PCIe Gen1 x1) |
| **USB** | USB 3.0 host via GenesysLogic GL3510 hub |
| **Display** | GC9307 172×320 TFT LCD (SPI) |
| **Battery** | Li-ion with MCU-managed charging |
| **Fan** | Active cooling, MCU-controlled |
| **MCU** | Onboard power manager (UART serial, proprietary protocol) |
| **Console** | UART0 at 1,500,000 baud |

## Project Goals

1. **Full OpenWrt support** — upstream PR submitted: [openwrt/openwrt#22246](https://github.com/openwrt/openwrt/pull/22246)
2. **Open-source MCU driver** — reverse-engineer the proprietary MCU serial protocol and create a clean, documented, open-source replacement
3. **Display application** — configurable status display and management app for the built-in LCD
4. **Complete documentation** — hardware details, protocol specs, and development notes

## Repository Structure

```
├── docs/                    # Documentation
│   ├── hardware.md          # Hardware overview & pinouts
│   ├── mcu-protocol.md      # MCU serial protocol specification
│   └── display.md           # Display hardware & driver details
├── mcu/                     # Open-source MCU communication
│   ├── pcat2-mcu.h          # Protocol definitions & API
│   ├── pcat2-mcu.c          # MCU communication library
│   └── Makefile
├── display/                 # Display application
│   ├── pcat2-display.c      # Status display daemon
│   ├── Makefile
│   └── pcat2-display.init   # procd init script
├── tools/                   # Diagnostic & utility tools
│   └── mcu-dump.c           # Raw MCU protocol dumper
├── progress                 # Development log
└── README.md
```

## MCU Protocol

The Photonicat 2 has an onboard microcontroller (MCU) connected via UART (serial) that manages:

- **Battery** — voltage, current, SoC%, charging status
- **Power** — system power on/off, watchdog, scheduled startup
- **Fan** — speed control (10 levels)
- **RTC** — real-time clock (time sync)
- **Temperature** — board temperature sensor
- **Accelerometer** — motion detection (G-sensor)
- **LED** — power button LED control

The protocol uses a binary framing format with CRC16 checksums. See [docs/mcu-protocol.md](docs/mcu-protocol.md) for the full specification.

## Display

The built-in 172×320 TFT LCD uses a GC9307 (ST7789-compatible) controller on SPI1.0. The display application shows real-time system status including battery, network, WiFi, and system load.

See [docs/display.md](docs/display.md) for hardware details.

## OpenWrt Support

Our upstream OpenWrt PR adds full board support:

- **Fork:** [th3cavalry/openwrt](https://github.com/th3cavalry/openwrt/tree/add-ariaboard-photonicat-2)
- **PR:** [openwrt/openwrt#22246](https://github.com/openwrt/openwrt/pull/22246)
- **Branch:** `add-ariaboard-photonicat-2`

## Building

### Display application (standalone)

```sh
# Cross-compile for aarch64
export CC=aarch64-linux-gnu-gcc
cd display
make
```

### MCU tools (standalone)

```sh
export CC=aarch64-linux-gnu-gcc
cd mcu
make
```

### As OpenWrt packages

The packages are included in our OpenWrt fork. Build with:

```sh
make package/pcat2-display/compile V=s
make package/pcat2-mcu/compile V=s
```

## License

This project is licensed under the **GPL-2.0-or-later** license. See [LICENSE](LICENSE) for details.

## Credits

- **Brandon Cleary** ([@th3cavalry](https://github.com/th3cavalry)) — OpenWrt port, display app, protocol reverse engineering
- **Kyosuke Nekoyashiki** — Original photonicat-pm kernel driver (GPL-2.0, used as reference for protocol analysis)
- **Jonas Karlman** — RK3576 U-Boot mkimage patches
