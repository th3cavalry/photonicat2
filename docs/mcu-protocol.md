# Photonicat 2 MCU Serial Protocol Specification

> **Status:** Reverse-engineered from the GPL-licensed `photonicat-pm` kernel driver
> by Kyosuke Nekoyashiki. This document describes the binary protocol between the
> host CPU (RK3576) and the onboard power management MCU.

## Physical Layer

| Parameter | Value |
|-----------|-------|
| Interface | UART (serial) via Linux serdev |
| Default baud rate | 115200 |
| Parity | None |
| Flow control | None |
| DT node | `uart10` (`serial@2adc0000`) on RK3576 |
| DT compatible | `photonicat-pm` |

The MCU is connected to UART10 on the RK3576. The device tree also references a `power-gpios` line used to signal the MCU during shutdown.

## Frame Format

All communication uses a binary framed protocol. Every frame has the following structure:

```
Offset  Size  Field         Description
──────  ────  ──────────    ───────────────────────────────────────
0       1     Header        Always 0xA5
1       1     Source        Source address
2       1     Destination   Destination address
3       2     Frame Number  Sequence counter (LE)
5       2     Data Length   Length of data section in bytes (LE), minimum 3
7       2     Command       Command ID (LE)
9       N     Extra Data    Optional payload (Data Length - 3 bytes)
7+DL    1     Need ACK      0x00 = no ACK needed, 0x01 = ACK requested
8+DL    2     CRC16         CRC16 checksum (LE)
10+DL   1     Tail          Always 0x5A
```

**Total frame size:** `10 + Data Length` bytes (minimum 13 bytes when Data Length = 3)

### Addressing

| Address | Meaning |
|---------|---------|
| `0x01`  | Host CPU (Linux) |
| `0x81`  | MCU |
| `0x80`  | Broadcast (MCU → host) |
| `0xFF`  | Broadcast |

- **Host → MCU:** `src=0x01, dst=0x81`
- **MCU → Host:** `src=0x01, dst=0x01` or `dst=0x80` or `dst=0xFF`

### CRC16 Calculation

The CRC16 uses the **Modbus CRC-16** algorithm:

- **Initial value:** `0xFFFF`
- **Polynomial:** `0xA001` (bit-reversed `0x8005`)
- **Input:** Bytes from offset 1 (Source) through offset `6 + Data Length` (Need ACK), inclusive
- **Output:** 16-bit value, stored little-endian

```c
uint16_t crc16_modbus(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}
```

### ACK Convention

- Commands with odd IDs are **requests** (e.g., `0x01` = heartbeat)
- Commands with even IDs are **responses/ACKs** (e.g., `0x02` = heartbeat ACK)
- When `Need ACK` is set in a received frame, the receiver should reply with `command + 1`

---

## Command Reference

### Overview

| ID | Name | Direction | Has Payload |
|----|------|-----------|-------------|
| `0x01` | Heartbeat | Host → MCU | No |
| `0x02` | Heartbeat ACK | MCU → Host | No |
| `0x03` | HW Version Get | Host → MCU | No |
| `0x04` | HW Version Get ACK | MCU → Host | Yes |
| `0x05` | FW Version Get | Host → MCU | No |
| `0x06` | FW Version Get ACK | MCU → Host | Yes |
| `0x07` | Status Report | MCU → Host | Yes |
| `0x08` | Status Report ACK | Host → MCU | No |
| `0x09` | Date/Time Sync | Host → MCU | Yes (7 bytes) |
| `0x0A` | Date/Time Sync ACK | MCU → Host | Yes (1 byte) |
| `0x0B` | Schedule Startup Time Set | Host → MCU | Yes |
| `0x0C` | Schedule Startup Time Set ACK | MCU → Host | Yes |
| `0x0D` | PMU Request Shutdown | MCU → Host | No |
| `0x0E` | PMU Request Shutdown ACK | Host → MCU | No |
| `0x0F` | Host Request Shutdown | Host → MCU | No |
| `0x10` | Host Request Shutdown ACK | MCU → Host | No |
| `0x11` | PMU Request Factory Reset | MCU → Host | No |
| `0x12` | PMU Request Factory Reset ACK | Host → MCU | No |
| `0x13` | Watchdog Timeout Set | Host → MCU | Yes (3 bytes) |
| `0x14` | Watchdog Timeout Set ACK | MCU → Host | No |
| `0x15` | Charger On Auto Start | Host → MCU | Yes |
| `0x16` | Charger On Auto Start ACK | MCU → Host | Yes |
| `0x17` | Voltage Threshold Set | Host → MCU | Yes |
| `0x18` | Voltage Threshold Set ACK | MCU → Host | Yes |
| `0x19` | Net Status LED Setup | Host → MCU | Yes |
| `0x1A` | Net Status LED Setup ACK | MCU → Host | Yes |
| `0x1B` | Power On Event Get | Host → MCU | No |
| `0x1C` | Power On Event Get ACK | MCU → Host | Yes |
| `0x93` | Fan Set | Host → MCU | Yes (1 byte) |
| `0x94` | Fan Set ACK | MCU → Host | No |
| `0x95` | Device Movement | MCU → Host | No |
| `0x96` | Device Movement ACK | Host → MCU | No |

---

### 0x01 — Heartbeat

Sent by the host every **1 second** to indicate the system is alive. The MCU uses this as a watchdog — if heartbeats stop, the MCU can force a power cycle.

- **Direction:** Host → MCU
- **Payload:** None

### 0x07 — Status Report

The MCU sends this periodically to report system status. This is the primary data source for battery, temperature, RTC, accelerometer, and fan speed.

- **Direction:** MCU → Host
- **Payload:** Variable length, up to 52 bytes

The payload is versioned by length — longer payloads contain more fields:

#### Base fields (always present, length ≥ 16):

| Offset | Size | Type | Field | Unit |
|--------|------|------|-------|------|
| 0 | 2 | u16 LE | Battery voltage | mV |
| 2 | 2 | u16 LE | Charger voltage | mV |
| 4 | 2 | u16 LE | GPIO input state | bitmask |
| 6 | 2 | u16 LE | GPIO output state | bitmask |
| 8 | 2 | u16 LE | RTC year | year (e.g., 2026) |
| 10 | 1 | u8 | RTC month | 1–12 |
| 11 | 1 | u8 | RTC day | 1–31 |
| 12 | 1 | u8 | RTC hour | 0–23 |
| 13 | 1 | u8 | RTC minute | 0–59 |
| 14 | 1 | u8 | RTC second | 0–59 |
| 15 | 1 | u8 | RTC status | 0=OK, nonzero=invalid |

#### Extended fields (length ≥ 20):

| Offset | Size | Type | Field | Unit |
|--------|------|------|-------|------|
| 16 | 1 | u8 | *(reserved)* | — |
| 17 | 1 | u8 | Board temperature | raw − 100 = °C |
| 18 | 2 | s16 LE | Battery current | mA (positive=discharging, negative=charging) |

**Charger detection (v1 fallback):** If length < 20, `on_battery = (charger_voltage < 4200)`.
**Charger detection (v2):** If length ≥ 20, `on_battery = (battery_current > 0)`.

#### Energy fields (length ≥ 31):

| Offset | Size | Type | Field | Unit |
|--------|------|------|-------|------|
| 20 | 2 | — | *(reserved/unknown)* | — |
| 22 | 1 | u8 | State of Charge (SoC) | 0–100% |
| 23 | 4 | u32 LE | Energy now | µWh |
| 27 | 4 | u32 LE | Energy full | µWh |

**SoC fallback (v1):** If length < 31, SoC is computed from `battery_voltage` using the OCV table in the device tree (`simple-battery` node).

#### Accelerometer & fan fields (length ≥ 52):

| Offset | Size | Type | Field | Unit |
|--------|------|------|-------|------|
| 31 | 4 | — | *(reserved/unknown)* | — |
| 35 | 4 | s32 LE | G-sensor X | raw accelerometer |
| 39 | 4 | s32 LE | G-sensor Y | raw accelerometer |
| 43 | 4 | s32 LE | G-sensor Z | raw accelerometer |
| 47 | 1 | u8 | G-sensor ready | 0=not ready, 1=ready |
| 48 | 4 | u32 LE | Fan speed | RPM |

### 0x09 — Date/Time Sync

Set the MCU's RTC to the specified date and time.

- **Direction:** Host → MCU
- **Payload:** 7 bytes

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 2 | u16 LE | Year (e.g., 2026) |
| 2 | 1 | u8 | Month (1–12) |
| 3 | 1 | u8 | Day (1–31) |
| 4 | 1 | u8 | Hour (0–23) |
| 5 | 1 | u8 | Minute (0–59) |
| 6 | 1 | u8 | Second (0–59) |

**ACK (0x0A):** 1 byte — `0x00` = success, nonzero = error code.

### 0x0F — Host Request Shutdown

Tell the MCU the host is shutting down. The MCU will cut power after acknowledging.

- **Direction:** Host → MCU
- **Payload:** None
- **Need ACK:** Yes

The host waits up to **5 seconds** for `0x10` (ACK) before forcing power off via the `power-gpios` line.

### 0x13 — Watchdog Timeout Set

Configure the MCU's watchdog behavior.

- **Direction:** Host → MCU
- **Payload:** 3 bytes

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | Shutdown timeout | Seconds before forced shutdown (typically 60) |
| 1 | 1 | Force poweroff timeout | From DT `force-poweroff-timeout` property |
| 2 | 1 | Heartbeat interval | Watchdog interval in seconds (default: 10) |

Setting the heartbeat interval to **0** disables the watchdog (used before reboot).

### 0x93 — Fan Set

Set the fan speed.

- **Direction:** Host → MCU
- **Payload:** 1 byte

| Value | Meaning |
|-------|---------|
| `0` | Fan off |
| `20` | Level 1 (minimum) |
| `30` | Level 2 |
| `40` | Level 3 |
| `50` | Level 4 |
| `60` | Level 5 |
| `70` | Level 6 |
| `80` | Level 7 |
| `90` | Level 8 |
| `100` | Level 9 (maximum) |

**Formula:** `raw = (level == 0) ? 0 : (10 * (level - 1) + 20)`

The thermal cooling device exposes 10 states (0–9), where 0 = off.

### 0x19 — Net Status LED Setup

Control the power button LED. The exact payload format is still under investigation.

- **Direction:** Host → MCU
- **Payload:** TBD

### 0x0D — PMU Request Shutdown

The MCU requests the host to shut down (e.g., battery critically low, power button held).

- **Direction:** MCU → Host
- **Payload:** None

### 0x95 — Device Movement

The MCU reports that the accelerometer detected motion.

- **Direction:** MCU → Host
- **Payload:** None

The driver tracks a 5-second "movement activated" window after receiving this event.

---

## Device Tree Properties

The `photonicat-pm` node in the device tree supports:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `compatible` | string | — | Must be `"photonicat-pm"` |
| `baudrate` | u32 | 115200 | UART baud rate |
| `pm-version` | u32 | 1 | Protocol version (1 or 2) |
| `force-poweroff-timeout` | u32 | 0 | Forced poweroff timeout in seconds |
| `power-gpios` | GPIO | — | GPIO line to signal MCU during shutdown |

### Child nodes:

- **`charger`** — contains `simple-battery` reference for power_supply registration
- **`fan`** — used for thermal_cooling_device registration

---

## Timing

| Event | Interval |
|-------|----------|
| Heartbeat (Host → MCU) | Every 1 second |
| Status Report (MCU → Host) | Periodic (MCU-initiated, ~1–2 seconds) |
| Status Report timeout warning | 15 seconds without a report |
| Watchdog default interval | 10 seconds |
| Movement detection window | 5 seconds after last movement event |
| Shutdown ACK timeout | 5 seconds |

---

## Protocol Versions

The driver supports two protocol versions, selected via the `pm-version` device tree property:

### Version 1 (pm-version = 1)
- Battery properties: status, capacity, voltage, technology, model
- SoC computed from OCV table in device tree
- Charger detection via voltage threshold (< 4200 mV = on battery)

### Version 2 (pm-version = 2)
- All v1 properties plus: energy_full, energy_now, current_now, power_now
- SoC reported directly by MCU (byte 22 of status report)
- Energy reported by MCU (bytes 23–30)
- Charger detection via current direction (positive = discharging)

The Photonicat 2 uses **version 2**.

---

## Misc Device: `/dev/pcat-pm-ctl`

The kernel driver exposes a **miscdevice** at `/dev/pcat-pm-ctl` that allows userspace to send/receive raw MCU commands. Commands that the kernel driver handles internally (heartbeat, status report, shutdown, watchdog, fan, date/time sync) are **filtered out** — they are not forwarded to userspace.

This device is used for commands not yet handled by the kernel (LED control, version queries, etc.). It supports:

- `read()` — receive raw MCU frames (blocking or O_NONBLOCK)
- `write()` — send raw MCU frames (frame number and CRC are recomputed)
- `poll()` — POLLIN when data available, POLLOUT always

---

## Sysfs Interface

The driver creates `/sys/kernel/photonicat-pm/movement_trigger`:

- **Read:** Returns `1` if motion was detected in the last 5 seconds, `0` otherwise

---

## TODO / Unknown

- [ ] Exact payload format for `0x19` (Net Status LED Setup)
- [ ] Payload details for `0x15` (Charger On Auto Start)
- [ ] Payload details for `0x17` (Voltage Threshold Set)
- [ ] Payload details for `0x0B` (Schedule Startup Time Set)
- [ ] Payload details for `0x1B`/`0x1C` (Power On Event)
- [ ] HW/FW version response formats (`0x04`, `0x06`)
- [ ] Accelerometer units and calibration
- [ ] GPIO input/output bitmask meanings (bytes 4–7 of status report)
- [ ] Factory reset behavior details
- [ ] LED color/pattern control specifics

These can be investigated by capturing traffic on the device via `/dev/pcat-pm-ctl` or by sniffing UART10 directly.
