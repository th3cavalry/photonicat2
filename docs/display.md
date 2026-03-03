# Photonicat 2 Display Technical Reference

## Hardware

| Item | Detail |
|------|--------|
| Controller | GC9307 (Galaxy Core / ST7789-compatible) |
| Resolution | 172 × 320 pixels |
| Color depth | RGB565 (16-bit, 65536 colors) |
| Interface | SPI (4-wire with DC pin) |
| SPI bus | SPI1.0 (`/dev/spidev1.0`) |
| SPI speed | 6 MHz |
| Column offset | 34 pixels (CASET starts at 34, ends at 205) |

## GPIO Pins

| Signal | GPIO Name | Bank | Offset | Function |
|--------|-----------|------|--------|----------|
| DC | GPIO3_PD1 | 3 | 25 | Data/Command select (High=data, Low=command) |
| RST | GPIO3_PD2 | 3 | 26 | Hardware reset (active LOW) |
| BL | GPIO3_C5 | 3 | 21 | Backlight enable (**active LOW**: LOW=on, HIGH=off) |

## Initialization Sequence

The GC9307 requires a specific initialization sequence. Key commands:

```
1. SWRESET (0x01)         — Software reset
   Wait 150ms

2. SLPOUT (0x11)          — Exit sleep mode
   Wait 150ms

3. COLMOD (0x3A, 0x05)    — 16-bit color (RGB565)

4. MADCTL (0x36, 0x40)    — Memory access control: MX bit set (rotation 180°)

5. INVOFF (0x20)          — Display inversion OFF
   (Factory uses INVOFF, NOT INVON)

6. NORON (0x13)           — Normal display mode

7. DISPON (0x29)          — Display ON

8. Backlight GPIO → LOW   — Turn on backlight (active LOW!)
```

### Important Notes

- **INVOFF, not INVON:** The factory Go driver uses `INVOFF` (0x20). Using `INVON` (0x21) causes incorrect colors.
- **Active-LOW backlight:** Setting the BL GPIO HIGH turns the backlight OFF. Setting it LOW turns it ON. The factory firmware uses PWM with inverted polarity.
- **Column offset 34:** When writing pixel data, column addresses must be offset by 34. For a 172-pixel-wide display, the CASET range is [34, 205].
- **MADCTL 0x40 (MX):** Sets X mirroring for 180° rotation. Without this, the display is upside-down.

## Memory Layout

The display framebuffer is 172 × 320 × 2 = 110,080 bytes (RGB565).

Column address (CASET) range: `COL_OFFSET` to `COL_OFFSET + WIDTH - 1` = 34 to 205
Row address (RASET) range: 0 to 319

## SPI Communication

### Command write (DC=LOW):

```
DC pin → LOW
SPI transfer: [command_byte]
DC pin → HIGH
```

### Data write (DC=HIGH):

```
DC pin → HIGH
SPI transfer: [data_bytes...]
```

### Pixel data transfer:

```
Set CASET (0x2A): [x_start_hi, x_start_lo, x_end_hi, x_end_lo]
Set RASET (0x2B): [y_start_hi, y_start_lo, y_end_hi, y_end_lo]
RAMWR (0x2C): [pixel_data...]   // RGB565, MSB first
```

## Current Display Application

The `pcat2-display` daemon shows:

| Line | Content | Source |
|------|---------|--------|
| Header | System hostname | `/proc/sys/kernel/hostname` |
| Line 1 | Date and time | `gettimeofday()` |
| Line 2 | Battery % and status | `/sys/class/power_supply/battery/` |
| Line 3 | WAN IP and status | `ifconfig` parsing |
| Line 4 | WiFi SSID, band, clients | `/sys/class/ieee80211/`, UCI, `iw` |
| Line 5 | Uptime, CPU load, memory | `/proc/uptime`, `/proc/loadavg`, `/proc/meminfo` |

Refresh rate: every **5 seconds**.

## Font

Built-in 5×7 bitmap font covering ASCII 0x20–0x7E, rendered at **2× scaling** (10×14 pixels per character) for readability on the small display.

## Challenges & Solutions

| Problem | Solution |
|---------|----------|
| GPIO sysfs unavailable | `CONFIG_GPIO_SYSFS` not enabled → use chardev API |
| GPIO chardev v1 unavailable | `CONFIG_GPIO_CDEV_V1` disabled → use v2 API |
| Wrong backlight pin | Was GPIO0_B4 → corrected to GPIO3_C5 via factory DTS analysis |
| Inverted backlight polarity | Factory PWM inverted → GPIO LOW = backlight ON |
| INVON incorrect | Factory uses INVOFF → colors wrong with INVON |
| No PWM driver | `CONFIG_PWM` disabled → GPIO on/off (full brightness only) |

## Future Plans

- [ ] Configurable display layouts (user-selectable screens)
- [ ] Touch/button input for screen cycling
- [ ] Brightness control (if PWM can be enabled)
- [ ] Custom graphics/icons
- [ ] Settings/configuration screen
- [ ] Integration with MCU data (direct battery/temp from MCU instead of sysfs)
