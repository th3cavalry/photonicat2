# Photonicat 2 Hardware Reference

## SoC

| Item | Detail |
|------|--------|
| SoC | Rockchip RK3576 |
| CPU | 4× Cortex-A72 (big) + 4× Cortex-A53 (LITTLE) |
| GPU | Mali-G52 MC3 |
| NPU | 6 TOPS |
| Process | 8nm |

## PMIC

| Item | Detail |
|------|--------|
| Chip | RK806 |
| Bus | I2C1 @ address 0x23 |
| Interrupt | GPIO0 pin 6 |
| Regulators | 13+ supply rails (see DTS for full table) |

## Memory & Storage

| Item | Detail |
|------|--------|
| RAM | 4GB / 8GB / 16GB LPDDR4X or LPDDR5 |
| eMMC | 32GB / 64GB / 128GB (HS400, /dev/mmcblk0) |
| microSD | SD card slot (/dev/mmcblk1) |

## Ethernet

| Item | Detail |
|------|--------|
| Controller | 2× GMAC (rk_gmac-dwmac) |
| PHYs | 2× Realtek RTL8211F (PHY ID 0x001cc916) |
| Mode | RGMII-RXID |
| Mapping | GMAC0 = eth0 (WAN), GMAC1 = eth1 (LAN) |
| MAC source | Generated from eMMC CID (`macaddr_generate_from_mmc_cid mmcblk0`) |

### PHY Wiring

- **GMAC0 (WAN):** PHY @ mdio address 1, reset GPIO4_PB3, tx delay 0.21ns / rx delay 0.30ns
- **GMAC1 (LAN):** PHY @ mdio address 1, reset GPIO4_PA7, tx delay 0.26ns / rx delay 0.30ns

## WiFi

| Item | Detail |
|------|--------|
| Module | Qualcomm WCN6855 |
| USB ID | `17cb:1103` |
| Slot | M.2 Key E (PCIe Gen2 x1, via pcie0 + combphy0) |
| Driver | `ath11k_pci` |
| Firmware | `ath11k-firmware-wcn6855` |
| Capabilities | WiFi 6E (2.4 GHz + 5 GHz + 6 GHz), Bluetooth 5.2 |
| rfkill | PCIe WLAN (`pcie-wlan`) + PCIe BT (`pcie-bt`) via GPIO rfkill |

## Cellular

| Item | Detail |
|------|--------|
| Modem | Quectel RM520N-GL |
| USB ID | `2c7c:0801` |
| Slot | M.2 Key B |
| Interface | USB QMI raw-ip (wwan0) |
| Drivers | `kmod-usb-net-qmi-wwan`, `kmod-usb-serial-option` |
| Management | `uqmi`, `luci-proto-qmi` |
| rfkill | USB WWAN (`usb-wwan`) via GPIO rfkill on GPIO0_PB5 |

## USB

| Item | Detail |
|------|--------|
| Hub | GenesysLogic GL3510 USB 2.0 hub |
| PHYs | u2phy0 (host + OTG) + u2phy1 (host) |
| Hub reset | GPIO3_PB0 (active-low hog) |

## Display

| Item | Detail |
|------|--------|
| Controller | GC9307 (ST7789-compatible) |
| Resolution | 172 × 320 pixels |
| Color | RGB565 (16-bit) |
| Bus | SPI1.0 (`/dev/spidev1.0`) at 6 MHz |
| Rotation | 180° (MADCTL = MX) |
| Column offset | 34 pixels |
| Inversion | INVOFF (not INVON) |

### Display GPIO Pinout

| Signal | GPIO | Bank/Offset | Active |
|--------|------|-------------|--------|
| DC (Data/Command) | GPIO3_PD1 | bank 3 / offset 25 | High=Data, Low=Command |
| RST (Reset) | GPIO3_PD2 | bank 3 / offset 26 | Active LOW |
| BL (Backlight) | GPIO3_C5 | bank 3 / offset 21 | **Active LOW** (LOW=on) |

### Driver Features

The `pcat2-display` daemon supports a multi-page user interface mirroring the
factory firmware.  Short pressing the **power button** cycles through pages and a
long press (≥3 s) initiates shutdown.  The pages are configurable via UCI
(`display.pages`) and default to `clock`, `cellular`, `battery`, `network` and
`system`.  Power button events are read from `/dev/input/event0` with an
exclusive grab so the stock `/etc/rc.button/power` handler doesn't power off on
short presses.

(To revert to the single‑screen "dashboard" mode, set
`display.pages='dashboard'`.)

> **Note:** The backlight is active-LOW. The factory firmware uses PWM (`pwm@2ade2000`) with
> inverted polarity. Since `CONFIG_PWM` is not enabled in OpenWrt's kernel config, we use
> GPIO on/off (full brightness only).

### GPIO API

The display uses the **Linux GPIO chardev v2 API** (`GPIO_V2_GET_LINE_IOCTL`, `GPIO_V2_LINE_SET_VALUES_IOCTL`). This is because:

1. GPIO sysfs (`/sys/class/gpio/`) — `CONFIG_GPIO_SYSFS` not enabled
2. GPIO chardev v1 — `CONFIG_GPIO_CDEV_V1` explicitly disabled in OpenWrt
3. GPIO chardev v2 — **works** (`CONFIG_GPIO_CDEV=y`)

## MCU (Power Manager)

| Item | Detail |
|------|--------|
| UART | UART10 (`serial@2adc0000`) |
| Baud rate | 115200 |
| Protocol | Binary framed with CRC16 (see [mcu-protocol.md](mcu-protocol.md)) |
| DT compatible | `photonicat-pm` |
| PM version | 2 |
| Features | Battery, charger, fan, RTC, temperature, accelerometer, power control |
| Power GPIO | GPIO for MCU shutdown signaling |

## PCIe

| Slot | Controller | PHY | Speed | Usage |
|------|-----------|-----|-------|-------|
| M.2 Key E | pcie0 | combphy0 | Gen2 x1 | WiFi (WCN6855) |
| M.2 Key M | pcie1 | combphy1 | Gen1 x1 | NVMe SSD |

## Battery

| Item | Detail |
|------|--------|
| Type | Li-ion |
| Nominal voltage | 3.7V |
| Charge voltage | 4.2V |
| Cutoff voltage | 3.4V |
| Capacity | Reported by MCU via energy_full |
| OCV table | See DTS `simple-battery` node (20°C) |
| Management | Entirely MCU-controlled |

## Serial Console

| Item | Detail |
|------|--------|
| UART | UART0 (`serial@2ad40000`) |
| Baud rate | 1,500,000 |
| Pinctrl | uart0m0 |

## Boot

| Item | Detail |
|------|--------|
| Bootloader | U-Boot (generic-rk3576) |
| Boot media | SD card (primary for OpenWrt), eMMC |
| SD card quirk | RK3576 BootROM has an issue with SD card boot; U-Boot mkimage patch 116-7 works around it |
