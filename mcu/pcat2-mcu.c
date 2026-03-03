// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pcat2-mcu.c — Photonicat 2 MCU communication library
 *
 * Open-source implementation of the MCU serial protocol used by the
 * Ariaboard Photonicat 2. Protocol reverse-engineered from the GPL-licensed
 * photonicat-pm kernel driver by Kyosuke Nekoyashiki.
 *
 * Copyright (C) 2026 Brandon Cleary <cleary.brandon@gmail.com>
 */

#include "pcat2-mcu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>

/* ── CRC16 (Modbus) ─────────────────────────────────────────────── */

uint16_t mcu_crc16(const uint8_t *data, size_t len)
{
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

/* ── Serial port ─────────────────────────────────────────────────── */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 921600:  return B921600;
    case 1500000: return B1500000;
    default:      return B115200;
    }
}

int mcu_open(mcu_ctx_t *ctx, const char *device, int baud)
{
    struct termios tio;

    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (tcgetattr(fd, &tio) < 0) {
        close(fd);
        return -1;
    }

    /* Raw mode — no echo, no signals, no canonical processing */
    cfmakeraw(&tio);

    /* 8N1, no flow control */
    tio.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    tio.c_cflag |= CLOCAL | CREAD;

    speed_t sp = baud_to_speed(baud);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    /* Non-blocking reads, return immediately */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        close(fd);
        return -1;
    }

    /* Flush any stale data */
    tcflush(fd, TCIOFLUSH);

    ctx->fd = fd;
    ctx->frame_counter = 0;
    ctx->rx_used = 0;

    return 0;
}

void mcu_close(mcu_ctx_t *ctx)
{
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

/* ── Send ────────────────────────────────────────────────────────── */

int mcu_send(mcu_ctx_t *ctx, uint16_t command,
             const uint8_t *extra_data, uint16_t extra_len,
             bool need_ack)
{
    uint8_t frame[MCU_MAX_FRAME_SIZE];
    size_t pos = 0;
    uint16_t data_len;

    if (extra_len > MCU_MAX_EXTRA_DATA)
        extra_len = MCU_MAX_EXTRA_DATA;

    /* Header */
    frame[pos++] = MCU_FRAME_HEADER;

    /* Source & destination */
    frame[pos++] = MCU_ADDR_HOST;
    frame[pos++] = MCU_ADDR_MCU;

    /* Frame number (LE) */
    frame[pos++] = ctx->frame_counter & 0xFF;
    frame[pos++] = (ctx->frame_counter >> 8) & 0xFF;
    ctx->frame_counter++;

    /* Data length (LE): command(2) + extra_data + need_ack(1) = extra_len + 3 */
    data_len = extra_len + 3;
    frame[pos++] = data_len & 0xFF;
    frame[pos++] = (data_len >> 8) & 0xFF;

    /* Command (LE) */
    frame[pos++] = command & 0xFF;
    frame[pos++] = (command >> 8) & 0xFF;

    /* Extra data */
    if (extra_data && extra_len > 0) {
        memcpy(frame + pos, extra_data, extra_len);
        pos += extra_len;
    }

    /* Need ACK flag */
    frame[pos++] = need_ack ? 1 : 0;

    /* CRC16 over bytes [1..pos-1] (src through need_ack) */
    uint16_t crc = mcu_crc16(frame + 1, pos - 1);
    frame[pos++] = crc & 0xFF;
    frame[pos++] = (crc >> 8) & 0xFF;

    /* Tail */
    frame[pos++] = MCU_FRAME_TAIL;

    /* Write to serial */
    ssize_t written = write(ctx->fd, frame, pos);
    if (written < 0)
        return -1;

    return (int)written;
}

/* ── Receive ─────────────────────────────────────────────────────── */

/*
 * Try to parse one frame from the rx buffer.
 * Returns 1 if a frame was parsed (and consumed from buffer), 0 if not enough data.
 */
static int try_parse_frame(mcu_ctx_t *ctx, mcu_frame_t *frame)
{
    size_t i = 0;
    size_t consumed = 0;

    while (i < ctx->rx_used) {
        /* Scan for header byte */
        if (ctx->rx_buf[i] != MCU_FRAME_HEADER) {
            consumed = i + 1;
            i++;
            continue;
        }

        size_t remaining = ctx->rx_used - i;
        if (remaining < MCU_FRAME_MIN_SIZE)
            break;  /* Need more data */

        const uint8_t *p = ctx->rx_buf + i;

        /* Data length */
        uint16_t data_len = p[5] | ((uint16_t)p[6] << 8);
        if (data_len < 3 || data_len > MCU_MAX_EXTRA_DATA + 3) {
            consumed = i + 1;
            i++;
            continue;
        }

        size_t frame_size = 10 + data_len;
        if (frame_size > remaining)
            break;  /* Need more data */

        /* Check tail byte */
        if (p[frame_size - 1] != MCU_FRAME_TAIL) {
            consumed = i + 1;
            i++;
            continue;
        }

        /* Verify CRC */
        uint16_t expected_crc = p[7 + data_len] | ((uint16_t)p[8 + data_len] << 8);
        uint16_t actual_crc = mcu_crc16(p + 1, 6 + data_len);
        if (expected_crc != actual_crc) {
            fprintf(stderr, "mcu: CRC mismatch: got 0x%04X, expected 0x%04X\n",
                    expected_crc, actual_crc);
            consumed = i + frame_size;
            i += frame_size;
            continue;
        }

        /* Parse frame */
        frame->src = p[1];
        frame->dst = p[2];
        frame->frame_num = p[3] | ((uint16_t)p[4] << 8);
        frame->command = p[7] | ((uint16_t)p[8] << 8);
        frame->extra_len = data_len - 3;
        frame->need_ack = (p[6 + data_len] != 0);

        if (frame->extra_len > 0)
            memcpy(frame->extra_data, p + 9, frame->extra_len);

        consumed = i + frame_size;

        /* Remove consumed bytes from buffer */
        if (consumed > 0 && consumed <= ctx->rx_used) {
            memmove(ctx->rx_buf, ctx->rx_buf + consumed, ctx->rx_used - consumed);
            ctx->rx_used -= consumed;
        }

        return 1;
    }

    /* Remove any consumed garbage bytes */
    if (consumed > 0 && consumed <= ctx->rx_used) {
        memmove(ctx->rx_buf, ctx->rx_buf + consumed, ctx->rx_used - consumed);
        ctx->rx_used -= consumed;
    }

    return 0;
}

int mcu_recv(mcu_ctx_t *ctx, mcu_frame_t *frame)
{
    /* First try to parse from existing buffer */
    if (try_parse_frame(ctx, frame))
        return 1;

    /* Read more data from serial */
    size_t space = sizeof(ctx->rx_buf) - ctx->rx_used;
    if (space == 0) {
        /* Buffer full with no valid frame — discard */
        ctx->rx_used = 0;
        space = sizeof(ctx->rx_buf);
    }

    ssize_t n = read(ctx->fd, ctx->rx_buf + ctx->rx_used, space);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n == 0)
        return 0;

    ctx->rx_used += n;

    /* Try to parse again */
    return try_parse_frame(ctx, frame);
}

int mcu_recv_timeout(mcu_ctx_t *ctx, mcu_frame_t *frame, int timeout_ms)
{
    /* Check buffer first */
    if (try_parse_frame(ctx, frame))
        return 1;

    struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
    int64_t deadline = 0;

    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    }

    while (1) {
        int remaining = -1;
        if (timeout_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            remaining = (int)(deadline - now);
            if (remaining <= 0)
                return 0;  /* Timeout */
        }

        int ret = poll(&pfd, 1, remaining);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            return 0;  /* Timeout */

        /* Read data */
        size_t space = sizeof(ctx->rx_buf) - ctx->rx_used;
        if (space == 0) {
            ctx->rx_used = 0;
            space = sizeof(ctx->rx_buf);
        }

        ssize_t n = read(ctx->fd, ctx->rx_buf + ctx->rx_used, space);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (n == 0)
            continue;

        ctx->rx_used += n;

        if (try_parse_frame(ctx, frame))
            return 1;
    }
}

/* ── Status report parser ────────────────────────────────────────── */

int mcu_parse_status(const uint8_t *data, uint16_t data_len, mcu_status_t *status)
{
    if (data_len < 16)
        return -1;

    memset(status, 0, sizeof(*status));

    /* Base fields */
    status->battery_mv = data[0] | ((uint16_t)data[1] << 8);
    status->charger_mv = data[2] | ((uint16_t)data[3] << 8);
    status->gpio_input = data[4] | ((uint16_t)data[5] << 8);
    status->gpio_output = data[6] | ((uint16_t)data[7] << 8);
    status->rtc_year = data[8] | ((uint16_t)data[9] << 8);
    status->rtc_month = data[10];
    status->rtc_day = data[11];
    status->rtc_hour = data[12];
    status->rtc_min = data[13];
    status->rtc_sec = data[14];
    status->rtc_status = data[15];

    /* Default charger detection (v1) */
    status->on_battery = (status->charger_mv < 4200);
    status->on_charger = (status->charger_mv >= 4200);

    /* Extended: temperature + current */
    if (data_len >= 20) {
        status->has_temp = true;
        status->board_temp_c = (int)data[17] - 100;
        status->battery_current_ma = (int16_t)(data[18] | ((uint16_t)data[19] << 8));

        /* v2 charger detection */
        status->on_battery = (status->battery_current_ma > 0);
        status->on_charger = !status->on_battery;
    }

    /* Energy fields */
    if (data_len >= 31) {
        status->has_energy = true;
        status->soc = data[22];
        status->energy_now_uwh = data[23] | ((uint32_t)data[24] << 8) |
                                 ((uint32_t)data[25] << 16) | ((uint32_t)data[26] << 24);
        status->energy_full_uwh = data[27] | ((uint32_t)data[28] << 8) |
                                  ((uint32_t)data[29] << 16) | ((uint32_t)data[30] << 24);
    }

    /* Accelerometer & fan */
    if (data_len >= 52) {
        status->has_motion = true;
        status->accel_x = data[35] | ((uint32_t)data[36] << 8) |
                          ((uint32_t)data[37] << 16) | ((uint32_t)data[38] << 24);
        status->accel_y = data[39] | ((uint32_t)data[40] << 8) |
                          ((uint32_t)data[41] << 16) | ((uint32_t)data[42] << 24);
        status->accel_z = data[43] | ((uint32_t)data[44] << 8) |
                          ((uint32_t)data[45] << 16) | ((uint32_t)data[46] << 24);
        status->accel_ready = (data[47] != 0);
        status->fan_rpm = data[48] | ((uint32_t)data[49] << 8) |
                          ((uint32_t)data[50] << 16) | ((uint32_t)data[51] << 24);
    }

    return 0;
}

/* ── Convenience functions ───────────────────────────────────────── */

int mcu_send_heartbeat(mcu_ctx_t *ctx)
{
    return mcu_send(ctx, MCU_CMD_HEARTBEAT, NULL, 0, false);
}

int mcu_get_hw_version(mcu_ctx_t *ctx)
{
    return mcu_send(ctx, MCU_CMD_HW_VERSION_GET, NULL, 0, true);
}

int mcu_get_fw_version(mcu_ctx_t *ctx)
{
    return mcu_send(ctx, MCU_CMD_FW_VERSION_GET, NULL, 0, true);
}

int mcu_set_datetime(mcu_ctx_t *ctx, uint16_t year, uint8_t month,
                     uint8_t day, uint8_t hour, uint8_t min, uint8_t sec)
{
    uint8_t payload[7];
    payload[0] = year & 0xFF;
    payload[1] = (year >> 8) & 0xFF;
    payload[2] = month;
    payload[3] = day;
    payload[4] = hour;
    payload[5] = min;
    payload[6] = sec;
    return mcu_send(ctx, MCU_CMD_DATETIME_SYNC, payload, 7, true);
}

int mcu_set_fan(mcu_ctx_t *ctx, int level)
{
    uint8_t raw;
    if (level <= 0)
        raw = 0;
    else if (level > 9)
        raw = 100;
    else
        raw = 10 * (level - 1) + 20;

    return mcu_send(ctx, MCU_CMD_FAN_SET, &raw, 1, false);
}

int mcu_set_watchdog(mcu_ctx_t *ctx, uint8_t shutdown_timeout,
                     uint8_t force_poweroff, uint8_t heartbeat_interval)
{
    uint8_t payload[3] = { shutdown_timeout, force_poweroff, heartbeat_interval };
    return mcu_send(ctx, MCU_CMD_WATCHDOG_SET, payload, 3, false);
}

int mcu_request_shutdown(mcu_ctx_t *ctx)
{
    return mcu_send(ctx, MCU_CMD_HOST_REQUEST_SHUTDOWN, NULL, 0, true);
}

/* ── Debug / dump utilities ──────────────────────────────────────── */

const char *mcu_cmd_name(uint16_t command)
{
    switch (command) {
    case MCU_CMD_HEARTBEAT:                return "HEARTBEAT";
    case MCU_CMD_HEARTBEAT_ACK:            return "HEARTBEAT_ACK";
    case MCU_CMD_HW_VERSION_GET:           return "HW_VERSION_GET";
    case MCU_CMD_HW_VERSION_GET_ACK:       return "HW_VERSION_GET_ACK";
    case MCU_CMD_FW_VERSION_GET:           return "FW_VERSION_GET";
    case MCU_CMD_FW_VERSION_GET_ACK:       return "FW_VERSION_GET_ACK";
    case MCU_CMD_STATUS_REPORT:            return "STATUS_REPORT";
    case MCU_CMD_STATUS_REPORT_ACK:        return "STATUS_REPORT_ACK";
    case MCU_CMD_DATETIME_SYNC:            return "DATETIME_SYNC";
    case MCU_CMD_DATETIME_SYNC_ACK:        return "DATETIME_SYNC_ACK";
    case MCU_CMD_SCHEDULE_STARTUP_SET:     return "SCHEDULE_STARTUP_SET";
    case MCU_CMD_SCHEDULE_STARTUP_SET_ACK: return "SCHEDULE_STARTUP_SET_ACK";
    case MCU_CMD_PMU_REQUEST_SHUTDOWN:     return "PMU_REQUEST_SHUTDOWN";
    case MCU_CMD_PMU_REQUEST_SHUTDOWN_ACK: return "PMU_REQUEST_SHUTDOWN_ACK";
    case MCU_CMD_HOST_REQUEST_SHUTDOWN:    return "HOST_REQUEST_SHUTDOWN";
    case MCU_CMD_HOST_REQUEST_SHUTDOWN_ACK:return "HOST_REQUEST_SHUTDOWN_ACK";
    case MCU_CMD_PMU_REQUEST_FACTORY_RESET:     return "PMU_FACTORY_RESET";
    case MCU_CMD_PMU_REQUEST_FACTORY_RESET_ACK: return "PMU_FACTORY_RESET_ACK";
    case MCU_CMD_WATCHDOG_SET:             return "WATCHDOG_SET";
    case MCU_CMD_WATCHDOG_SET_ACK:         return "WATCHDOG_SET_ACK";
    case MCU_CMD_CHARGER_AUTO_START:       return "CHARGER_AUTO_START";
    case MCU_CMD_CHARGER_AUTO_START_ACK:   return "CHARGER_AUTO_START_ACK";
    case MCU_CMD_VOLTAGE_THRESHOLD_SET:    return "VOLTAGE_THRESHOLD_SET";
    case MCU_CMD_VOLTAGE_THRESHOLD_SET_ACK:return "VOLTAGE_THRESHOLD_SET_ACK";
    case MCU_CMD_LED_SETUP:                return "LED_SETUP";
    case MCU_CMD_LED_SETUP_ACK:            return "LED_SETUP_ACK";
    case MCU_CMD_POWER_ON_EVENT_GET:       return "POWER_ON_EVENT_GET";
    case MCU_CMD_POWER_ON_EVENT_GET_ACK:   return "POWER_ON_EVENT_GET_ACK";
    case MCU_CMD_FAN_SET:                  return "FAN_SET";
    case MCU_CMD_FAN_SET_ACK:              return "FAN_SET_ACK";
    case MCU_CMD_DEVICE_MOVEMENT:          return "DEVICE_MOVEMENT";
    case MCU_CMD_DEVICE_MOVEMENT_ACK:      return "DEVICE_MOVEMENT_ACK";
    default:                               return "UNKNOWN";
    }
}

void mcu_hexdump(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (i % 16) == 0)
            fprintf(stderr, "\n");
        fprintf(stderr, "%02X ", data[i]);
    }
    if (len > 0)
        fprintf(stderr, "\n");
}

void mcu_dump_frame(const mcu_frame_t *frame)
{
    fprintf(stderr, "[%s] src=0x%02X dst=0x%02X frame#%u cmd=0x%04X (%s) "
            "extra=%u bytes ack=%d\n",
            (frame->src == MCU_ADDR_HOST) ? "TX" : "RX",
            frame->src, frame->dst, frame->frame_num,
            frame->command, mcu_cmd_name(frame->command),
            frame->extra_len, frame->need_ack);

    if (frame->extra_len > 0)
        mcu_hexdump(frame->extra_data, frame->extra_len);
}
