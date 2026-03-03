// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mcu-probe.c — Photonicat 2 MCU command probe tool
 *
 * Sends commands to the MCU via /dev/pcat-pm-ctl (the kernel driver's
 * passthrough interface) and displays responses. Used for reverse-
 * engineering unknown command payloads.
 *
 * Key design note: The ctl device's receive buffer also accumulates
 * heartbeat ACK frames (cmd=0x02) from the driver's periodic heartbeats
 * (~1/sec). This tool drains stale data before each command and filters
 * responses to match the expected ACK command ID.
 *
 * The kernel driver internally handles and does NOT forward to ctl:
 *   STATUS_REPORT (0x07), DATE_TIME_SYNC_ACK (0x0A),
 *   HOST_REQUEST_SHUTDOWN_ACK (0x10), DEVICE_MOVEMENT (0x95)
 *
 * The kernel driver blocks userspace from SENDING via ctl:
 *   HEARTBEAT (0x01/0x02), STATUS_REPORT (0x07/0x08),
 *   DATE_TIME_SYNC (0x09/0x0A), HOST_SHUTDOWN (0x0F/0x10),
 *   WATCHDOG (0x13/0x14), FAN (0x93/0x94)
 *
 * Everything else passes through bidirectionally.
 *
 * Copyright (C) 2026 Brandon Cleary <cleary.brandon@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <getopt.h>

/* ── Protocol constants ──────────────────────────────────────────── */

#define FRAME_HEADER  0xA5
#define FRAME_TAIL    0x5A
#define ADDR_HOST     0x01
#define ADDR_MCU      0x81
#define MAX_PAYLOAD   512

/* Command IDs */
#define CMD_HEARTBEAT           0x01
#define CMD_HEARTBEAT_ACK       0x02
#define CMD_HW_VERSION_GET      0x03
#define CMD_FW_VERSION_GET      0x05
#define CMD_STATUS_REPORT       0x07
#define CMD_DATETIME_SYNC       0x09
#define CMD_SCHEDULE_STARTUP    0x0B
#define CMD_PMU_SHUTDOWN        0x0D
#define CMD_HOST_SHUTDOWN       0x0F
#define CMD_FACTORY_RESET       0x11
#define CMD_WATCHDOG_SET        0x13
#define CMD_CHARGER_AUTO_START  0x15
#define CMD_VOLTAGE_THRESHOLD   0x17
#define CMD_LED_SETUP           0x19
#define CMD_POWER_ON_EVENT      0x1B
#define CMD_FAN_SET             0x93
#define CMD_DEVICE_MOVEMENT     0x95

static volatile int running = 1;
static uint16_t frame_counter = 1;
static int verbose = 0;

static void sighandler(int sig) { (void)sig; running = 0; }

/* ── CRC16 Modbus ────────────────────────────────────────────────── */

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

/* ── Command name lookup ─────────────────────────────────────────── */

static const char *cmd_name(uint16_t cmd)
{
    switch (cmd) {
    case 0x01: return "HEARTBEAT";
    case 0x02: return "HEARTBEAT_ACK";
    case 0x03: return "HW_VERSION_GET";
    case 0x04: return "HW_VERSION_GET_ACK";
    case 0x05: return "FW_VERSION_GET";
    case 0x06: return "FW_VERSION_GET_ACK";
    case 0x07: return "STATUS_REPORT";
    case 0x08: return "STATUS_REPORT_ACK";
    case 0x09: return "DATETIME_SYNC";
    case 0x0A: return "DATETIME_SYNC_ACK";
    case 0x0B: return "SCHEDULE_STARTUP_SET";
    case 0x0C: return "SCHEDULE_STARTUP_SET_ACK";
    case 0x0D: return "PMU_REQUEST_SHUTDOWN";
    case 0x0E: return "PMU_REQUEST_SHUTDOWN_ACK";
    case 0x0F: return "HOST_REQUEST_SHUTDOWN";
    case 0x10: return "HOST_REQUEST_SHUTDOWN_ACK";
    case 0x11: return "PMU_FACTORY_RESET";
    case 0x12: return "PMU_FACTORY_RESET_ACK";
    case 0x13: return "WATCHDOG_SET";
    case 0x14: return "WATCHDOG_SET_ACK";
    case 0x15: return "CHARGER_AUTO_START";
    case 0x16: return "CHARGER_AUTO_START_ACK";
    case 0x17: return "VOLTAGE_THRESHOLD_SET";
    case 0x18: return "VOLTAGE_THRESHOLD_SET_ACK";
    case 0x19: return "LED_SETUP";
    case 0x1A: return "LED_SETUP_ACK";
    case 0x1B: return "POWER_ON_EVENT_GET";
    case 0x1C: return "POWER_ON_EVENT_GET_ACK";
    case 0x93: return "FAN_SET";
    case 0x94: return "FAN_SET_ACK";
    case 0x95: return "DEVICE_MOVEMENT";
    case 0x96: return "DEVICE_MOVEMENT_ACK";
    default:   return "UNKNOWN";
    }
}

/* ── Parsed frame structure ──────────────────────────────────────── */

typedef struct {
    uint8_t  src;
    uint8_t  dst;
    uint16_t frame_num;
    uint16_t cmd;
    uint16_t payload_len;
    uint8_t  payload[MAX_PAYLOAD];
    int      need_ack;
    int      crc_ok;
    int      valid;  /* set to 1 when a frame was actually parsed */
} parsed_frame_t;

/* ── Frame builder ───────────────────────────────────────────────── */

static int build_frame(uint8_t *buf, uint16_t command,
                       const uint8_t *extra, uint16_t extra_len,
                       int need_ack)
{
    size_t pos = 0;
    uint16_t data_len = extra_len + 3;

    buf[pos++] = FRAME_HEADER;
    buf[pos++] = ADDR_HOST;
    buf[pos++] = ADDR_MCU;
    buf[pos++] = frame_counter & 0xFF;
    buf[pos++] = (frame_counter >> 8) & 0xFF;
    frame_counter++;
    buf[pos++] = data_len & 0xFF;
    buf[pos++] = (data_len >> 8) & 0xFF;
    buf[pos++] = command & 0xFF;
    buf[pos++] = (command >> 8) & 0xFF;

    if (extra && extra_len > 0) {
        memcpy(buf + pos, extra, extra_len);
        pos += extra_len;
    }

    buf[pos++] = need_ack ? 1 : 0;

    uint16_t c = crc16(buf + 1, pos - 1);
    buf[pos++] = c & 0xFF;
    buf[pos++] = (c >> 8) & 0xFF;
    buf[pos++] = FRAME_TAIL;

    return (int)pos;
}

/* ── Frame parser ────────────────────────────────────────────────── */

static void hexdump(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02X ", data[i]);
}

static void hexdump_ascii(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02X ", data[i]);
    printf(" | ");
    for (size_t i = 0; i < len; i++)
        printf("%c", (data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.');
}

/*
 * Try to extract one frame starting at or after buf[0].
 * On success: fills *out (with valid=1), returns bytes consumed.
 * On partial: returns 0 (need more data).
 * Skips garbage bytes before the frame header.
 */
static size_t parse_one_frame(const uint8_t *buf, size_t len,
                              parsed_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t i = 0;

    while (i < len) {
        if (buf[i] != FRAME_HEADER) { i++; continue; }

        size_t rem = len - i;
        if (rem < 13) return 0;  /* need more data */

        const uint8_t *p = buf + i;
        uint16_t data_len = p[5] | ((uint16_t)p[6] << 8);
        if (data_len < 3 || data_len > 515) { i++; continue; }

        size_t fsize = 10 + data_len;
        if (fsize > rem) return 0;  /* need more data */
        if (p[fsize - 1] != FRAME_TAIL) { i++; continue; }

        /* Verify CRC */
        uint16_t ecrc = p[7 + data_len] | ((uint16_t)p[8 + data_len] << 8);
        uint16_t acrc = crc16(p + 1, 6 + data_len);

        out->src = p[1];
        out->dst = p[2];
        out->frame_num = p[3] | ((uint16_t)p[4] << 8);
        out->cmd = p[7] | ((uint16_t)p[8] << 8);
        out->payload_len = data_len - 3;
        out->need_ack = (p[6 + data_len] != 0);
        out->crc_ok = (ecrc == acrc);
        out->valid = 1;

        if (out->payload_len > 0 && out->payload_len <= MAX_PAYLOAD)
            memcpy(out->payload, p + 9, out->payload_len);

        return i + fsize;
    }

    /* All bytes consumed as garbage */
    return len;
}

static void print_frame(const parsed_frame_t *f, const char *label)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    printf("[%ld.%03ld] %s cmd=0x%04X %-28s src=0x%02X dst=0x%02X "
           "frame#%u ack=%d crc=%s",
           ts.tv_sec, ts.tv_nsec / 1000000,
           label, f->cmd, cmd_name(f->cmd),
           f->src, f->dst, f->frame_num, f->need_ack,
           f->crc_ok ? "OK" : "BAD");

    if (f->payload_len > 0) {
        printf("\n  payload (%u bytes): ", f->payload_len);
        hexdump_ascii(f->payload, f->payload_len);
    }
    printf("\n");
    fflush(stdout);
}

/* ── Buffer drain ────────────────────────────────────────────────── */

static void drain_buffer(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint8_t trash[4096];
    int drained = 0;

    while (1) {
        int ret = poll(&pfd, 1, 50);
        if (ret <= 0) break;
        ssize_t n = read(fd, trash, sizeof(trash));
        if (n <= 0) break;
        drained += (int)n;
    }

    if (verbose && drained > 0)
        printf("  (drained %d bytes of stale data)\n", drained);
}

/* ── Send and receive with response matching ─────────────────────── */

/*
 * Send a command and wait for the matching response (cmd+1).
 * Filters out heartbeat ACKs and other unrelated frames.
 * Returns 1 on matched response, 0 on timeout, -1 on error.
 */
static int send_and_recv(int fd, uint16_t command,
                         const uint8_t *extra, uint16_t extra_len,
                         int timeout_ms, parsed_frame_t *out_frame)
{
    uint16_t expected_ack = command + 1;

    drain_buffer(fd);

    uint8_t frame[1024];
    int flen = build_frame(frame, command, extra, extra_len, 1);

    printf(">>> Sending cmd=0x%04X (%s)", command, cmd_name(command));
    if (extra_len > 0) {
        printf(" payload (%u): ", extra_len);
        hexdump(extra, extra_len);
    }
    printf("\n");

    ssize_t w = write(fd, frame, flen);
    if (w < 0) {
        fprintf(stderr, "write error: %s\n", strerror(errno));
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint8_t rbuf[4096];
    size_t rused = 0;

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    int64_t deadline = (int64_t)ts_start.tv_sec * 1000 +
                       ts_start.tv_nsec / 1000000 + timeout_ms;

    while (1) {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t now = (int64_t)ts_now.tv_sec * 1000 +
                      ts_now.tv_nsec / 1000000;
        int remaining = (int)(deadline - now);
        if (remaining <= 0) {
            printf("<<< No response (timeout %dms)\n\n", timeout_ms);
            return 0;
        }

        int ret = poll(&pfd, 1, remaining);
        if (ret <= 0) continue;

        ssize_t n = read(fd, rbuf + rused, sizeof(rbuf) - rused);
        if (n <= 0) continue;
        rused += n;

        /* Parse all complete frames in the buffer */
        while (rused > 0) {
            parsed_frame_t f;
            size_t consumed = parse_one_frame(rbuf, rused, &f);

            if (consumed == 0)
                break;  /* need more data */

            /* Shift buffer regardless */
            if (consumed <= rused) {
                memmove(rbuf, rbuf + consumed, rused - consumed);
                rused -= consumed;
            } else {
                rused = 0;
            }

            if (!f.valid)
                continue;  /* consumed garbage */

            if (!f.crc_ok) {
                if (verbose)
                    printf("  (bad CRC frame, skipping)\n");
                continue;
            }

            if (f.cmd == CMD_HEARTBEAT_ACK) {
                if (verbose)
                    printf("  (skipping heartbeat ACK #%u)\n", f.frame_num);
                continue;
            }

            if (f.cmd == expected_ack) {
                print_frame(&f, "<<<");
                printf("\n");
                if (out_frame)
                    *out_frame = f;
                return 1;
            }

            /* Unexpected — print but keep waiting */
            if (verbose) {
                printf("  (unexpected: ");
                print_frame(&f, "???");
                printf(")\n");
            }
        }
    }
}

static int send_recv(int fd, uint16_t cmd,
                     const uint8_t *extra, uint16_t extra_len,
                     int timeout_ms)
{
    return send_and_recv(fd, cmd, extra, extra_len, timeout_ms, NULL);
}

/* ── Monitor mode ────────────────────────────────────────────────── */

static void do_monitor(int fd)
{
    printf("Monitoring /dev/pcat-pm-ctl for passthrough traffic...\n");
    printf("(Heartbeat ACKs filtered unless -v. Press Ctrl+C to stop.)\n\n");

    uint8_t buf[4096];
    size_t used = 0;
    unsigned long hb_count = 0;

    while (running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        ssize_t n = read(fd, buf + used, sizeof(buf) - used);
        if (n <= 0) continue;
        used += n;

        while (used > 0) {
            parsed_frame_t f;
            size_t consumed = parse_one_frame(buf, used, &f);
            if (consumed == 0) break;

            if (consumed <= used) {
                memmove(buf, buf + consumed, used - consumed);
                used -= consumed;
            } else {
                used = 0;
            }

            if (!f.valid || !f.crc_ok) continue;

            if (f.cmd == CMD_HEARTBEAT_ACK) {
                hb_count++;
                if (verbose || hb_count % 60 == 0)
                    printf("  [%lu heartbeat ACKs so far]\n", hb_count);
            } else {
                print_frame(&f, "RX");
            }
        }
    }

    printf("\nFiltered %lu heartbeat ACKs total.\n", hb_count);
}

/* ── Version query ───────────────────────────────────────────────── */

static void do_version(int fd)
{
    parsed_frame_t f;

    printf("=== HW Version ===\n");
    if (send_and_recv(fd, CMD_HW_VERSION_GET, NULL, 0, 2000, &f) > 0) {
        printf("  -> \"");
        for (int i = 0; i < f.payload_len; i++)
            putchar(f.payload[i]);
        printf("\"\n\n");
    }

    printf("=== FW Version ===\n");
    if (send_and_recv(fd, CMD_FW_VERSION_GET, NULL, 0, 2000, &f) > 0) {
        printf("  -> \"");
        for (int i = 0; i < f.payload_len; i++)
            putchar(f.payload[i]);
        printf("\"\n");
        if (f.payload_len >= 11) {
            printf("  -> MCU: %.5s, FW date: 20%.2s-%.2s-%.2s",
                   f.payload, f.payload + 5, f.payload + 7, f.payload + 9);
            if (f.payload_len > 11)
                printf(", build: %.*s", f.payload_len - 11, f.payload + 11);
            printf("\n");
        }
    }
}

/* ── LED probe ───────────────────────────────────────────────────── */

static void do_led_probe(int fd, int argc, char *argv[])
{
    printf("=== LED Control Probe (cmd 0x19) ===\n\n");

    if (argc > 0) {
        uint8_t payload[256];
        int plen = 0;
        for (int i = 0; i < argc && plen < 256; i++) {
            unsigned int val;
            if (sscanf(argv[i], "%x", &val) == 1)
                payload[plen++] = (uint8_t)val;
        }
        printf("Custom: ");
        send_recv(fd, CMD_LED_SETUP, payload, plen, 2000);
        return;
    }

    printf("--- Single-byte payloads (0x00 - 0x0F) ---\n");
    for (int i = 0; i <= 0x0F; i++) {
        uint8_t b = (uint8_t)i;
        printf("[%02X] ", b);
        send_recv(fd, CMD_LED_SETUP, &b, 1, 1000);
    }

    printf("--- Two-byte payloads: [mode, value] ---\n");
    uint8_t patterns[][2] = {
        {0x00, 0x00}, {0x01, 0x00}, {0x01, 0x01}, {0x01, 0xFF},
        {0x02, 0x00}, {0x02, 0x01}, {0x02, 0x05}, {0x02, 0x0A},
        {0x03, 0x00}, {0x03, 0x01}, {0xFF, 0x00}, {0xFF, 0xFF},
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        printf("[%02X %02X] ", patterns[i][0], patterns[i][1]);
        send_recv(fd, CMD_LED_SETUP, patterns[i], 2, 1000);
    }
}

/* ── Power-on event ──────────────────────────────────────────────── */

static void do_power_event(int fd)
{
    parsed_frame_t f;
    printf("=== Power On Event (cmd 0x1B) ===\n");
    if (send_and_recv(fd, CMD_POWER_ON_EVENT, NULL, 0, 2000, &f) > 0) {
        if (f.payload_len >= 1) {
            const char *reason = "unknown";
            switch (f.payload[0]) {
            case 0x00: reason = "charger inserted"; break;
            case 0x01: reason = "power button press"; break;
            case 0x02: reason = "scheduled startup"; break;
            case 0x03: reason = "watchdog reset"; break;
            }
            printf("  -> Power-on reason: 0x%02X (%s)\n", f.payload[0], reason);
        }
    }
}

/* ── Charger auto-start ──────────────────────────────────────────── */

static void do_charger_auto(int fd)
{
    parsed_frame_t f;
    printf("=== Charger Auto Start (cmd 0x15) ===\n\n");

    printf("Query: ");
    if (send_and_recv(fd, CMD_CHARGER_AUTO_START, NULL, 0, 2000, &f) > 0)
        printf("  -> value: 0x%02X\n\n",
               f.payload_len ? f.payload[0] : 0xFF);

    printf("Set 0x01 (enable): ");
    send_recv(fd, CMD_CHARGER_AUTO_START, (uint8_t[]){0x01}, 1, 2000);

    printf("Query after enable: ");
    if (send_and_recv(fd, CMD_CHARGER_AUTO_START, NULL, 0, 2000, &f) > 0)
        printf("  -> value: 0x%02X\n\n",
               f.payload_len ? f.payload[0] : 0xFF);

    printf("Set 0x00 (disable): ");
    send_recv(fd, CMD_CHARGER_AUTO_START, (uint8_t[]){0x00}, 1, 2000);

    printf("Query after disable: ");
    if (send_and_recv(fd, CMD_CHARGER_AUTO_START, NULL, 0, 2000, &f) > 0)
        printf("  -> value: 0x%02X\n\n",
               f.payload_len ? f.payload[0] : 0xFF);
}

/* ── Voltage threshold ───────────────────────────────────────────── */

static void do_voltage_thresh(int fd)
{
    parsed_frame_t f;
    printf("=== Voltage Threshold (cmd 0x17) ===\n\n");

    printf("Query: ");
    if (send_and_recv(fd, CMD_VOLTAGE_THRESHOLD, NULL, 0, 2000, &f) > 0) {
        if (f.payload_len >= 2) {
            uint16_t mv = f.payload[0] | ((uint16_t)f.payload[1] << 8);
            printf("  -> threshold: %u mV\n\n", mv);
        } else if (f.payload_len == 1) {
            printf("  -> byte: 0x%02X\n\n", f.payload[0]);
        }
    }

    uint16_t voltages[] = { 3200, 3400, 3600, 6800, 7000 };
    for (size_t i = 0; i < sizeof(voltages) / sizeof(voltages[0]); i++) {
        uint8_t p[2] = { voltages[i] & 0xFF, (voltages[i] >> 8) & 0xFF };
        printf("Set %u mV: ", voltages[i]);
        send_recv(fd, CMD_VOLTAGE_THRESHOLD, p, 2, 1000);
    }
}

/* ── Schedule startup ────────────────────────────────────────────── */

static void do_schedule(int fd)
{
    parsed_frame_t f;
    printf("=== Schedule Startup (cmd 0x0B) ===\n\n");

    printf("Query: ");
    send_and_recv(fd, CMD_SCHEDULE_STARTUP, NULL, 0, 2000, &f);

    printf("Set (2026-12-31 23:59:59): ");
    uint8_t dt[7] = { 0xEA, 0x07, 12, 31, 23, 59, 59 };
    send_recv(fd, CMD_SCHEDULE_STARTUP, dt, 7, 2000);

    printf("Query after set: ");
    send_and_recv(fd, CMD_SCHEDULE_STARTUP, NULL, 0, 2000, &f);

    printf("Clear (zeros): ");
    uint8_t zeros[7] = {0};
    send_recv(fd, CMD_SCHEDULE_STARTUP, zeros, 7, 2000);
}

/* ── Full scan ───────────────────────────────────────────────────── */

static void do_scan(int fd)
{
    parsed_frame_t f;

    printf("=== Full Command Scan ===\n");
    printf("Drains + filters heartbeat ACKs between each probe.\n\n");

    printf("--- Known commands ---\n");
    struct { uint16_t cmd; const char *desc; } known[] = {
        {0x03, "HW version"},
        {0x05, "FW version"},
        {0x0B, "Schedule startup"},
        {0x15, "Charger auto-start"},
        {0x17, "Voltage threshold"},
        {0x19, "LED setup"},
        {0x1B, "Power-on event"},
    };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        printf("[0x%02X] %s: ", known[i].cmd, known[i].desc);
        if (send_and_recv(fd, known[i].cmd, NULL, 0, 2000, &f) > 0) {
            if (f.payload_len > 0 && f.payload_len <= 2) {
                uint16_t val = f.payload[0];
                if (f.payload_len == 2)
                    val |= ((uint16_t)f.payload[1] << 8);
                printf("  -> 0x%04X (%u)\n", val, val);
            }
        }
    }

    printf("\n--- Unknown 0x1D-0x30 (odd cmds) ---\n");
    for (uint16_t cmd = 0x1D; cmd <= 0x30; cmd += 2) {
        printf("[0x%02X] ", cmd);
        int ret = send_and_recv(fd, cmd, NULL, 0, 500, &f);
        if (ret > 0 && f.payload_len > 0) {
            printf("  ->");
            for (int j = 0; j < f.payload_len && j < 16; j++)
                printf(" %02X", f.payload[j]);
            if (f.payload_len <= 4) {
                uint32_t val = 0;
                for (int j = 0; j < f.payload_len; j++)
                    val |= ((uint32_t)f.payload[j] << (j * 8));
                printf("  (LE=%u)", val);
            }
            printf("\n");
        }
    }

    printf("\n--- Unknown 0x81-0x92 (odd cmds) ---\n");
    for (uint16_t cmd = 0x81; cmd <= 0x92; cmd += 2) {
        printf("[0x%02X] ", cmd);
        int ret = send_and_recv(fd, cmd, NULL, 0, 500, &f);
        if (ret > 0 && f.payload_len > 0) {
            printf("  ->");
            for (int j = 0; j < f.payload_len && j < 16; j++)
                printf(" %02X", f.payload[j]);
            if (f.payload_len <= 4) {
                uint32_t val = 0;
                for (int j = 0; j < f.payload_len; j++)
                    val |= ((uint32_t)f.payload[j] << (j * 8));
                printf("  (LE=%u)", val);
            }
            printf("\n");
        }
    }

    printf("\n--- Unknown 0x97-0xC0 (odd cmds) ---\n");
    for (uint16_t cmd = 0x97; cmd <= 0xC0; cmd += 2) {
        printf("[0x%02X] ", cmd);
        int ret = send_and_recv(fd, cmd, NULL, 0, 500, &f);
        if (ret > 0 && f.payload_len > 0) {
            printf("  ->");
            for (int j = 0; j < f.payload_len && j < 16; j++)
                printf(" %02X", f.payload[j]);
            if (f.payload_len <= 4) {
                uint32_t val = 0;
                for (int j = 0; j < f.payload_len; j++)
                    val |= ((uint32_t)f.payload[j] << (j * 8));
                printf("  (LE=%u)", val);
            }
            printf("\n");
        }
    }
}

/* ── Raw command ─────────────────────────────────────────────────── */

static void do_raw(int fd, int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "Usage: mcu-probe raw <cmd_hex> [payload_hex...]\n");
        return;
    }

    unsigned int cmd;
    sscanf(argv[0], "%x", &cmd);

    uint8_t payload[256];
    int plen = 0;
    for (int i = 1; i < argc && plen < 256; i++) {
        unsigned int val;
        if (sscanf(argv[i], "%x", &val) == 1)
            payload[plen++] = (uint8_t)val;
    }

    parsed_frame_t f;
    int ret = send_and_recv(fd, (uint16_t)cmd,
                            plen > 0 ? payload : NULL, plen, 3000, &f);
    if (ret > 0 && f.payload_len > 0 && f.payload_len <= 4) {
        uint32_t val = 0;
        for (int i = 0; i < f.payload_len; i++)
            val |= ((uint32_t)f.payload[i] << (i * 8));
        printf("  -> LE value: 0x%X (%u)\n", val, val);
    }
}

/* ── Repeat command (sensor monitoring) ──────────────────────────── */

static void do_repeat(int fd, int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "Usage: mcu-probe repeat <cmd_hex> [count] [delay_ms]\n");
        return;
    }

    unsigned int cmd;
    sscanf(argv[0], "%x", &cmd);
    int count = 5;
    int delay_ms = 1000;
    if (argc >= 2) sscanf(argv[1], "%d", &count);
    if (argc >= 3) sscanf(argv[2], "%d", &delay_ms);

    printf("=== Repeat cmd 0x%04X, %dx, %dms interval ===\n\n",
           cmd, count, delay_ms);

    for (int i = 0; i < count && running; i++) {
        parsed_frame_t f;
        printf("[%d/%d] ", i + 1, count);
        int ret = send_and_recv(fd, (uint16_t)cmd, NULL, 0, 2000, &f);
        if (ret > 0 && f.payload_len > 0 && f.payload_len <= 4) {
            uint32_t val = 0;
            for (int j = 0; j < f.payload_len; j++)
                val |= ((uint32_t)f.payload[j] << (j * 8));
            printf("  -> LE: 0x%X (%u)\n", val, val);
        }
        if (i < count - 1 && delay_ms > 0)
            usleep((unsigned)delay_ms * 1000);
    }
}

/* ── Main ────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <action> [args...]\n"
        "\n"
        "Options:\n"
        "  -D DEVICE   Control device (default: /dev/pcat-pm-ctl)\n"
        "  -v          Verbose (show filtered heartbeats, noise)\n"
        "\n"
        "Actions:\n"
        "  monitor            Listen for passthrough MCU traffic\n"
        "  version            Query HW and FW version\n"
        "  led [hex...]       Probe LED control (cmd 0x19)\n"
        "  power-event        Query power-on event (cmd 0x1B)\n"
        "  charger-auto       Probe charger auto-start (cmd 0x15)\n"
        "  voltage-thresh     Probe voltage threshold (cmd 0x17)\n"
        "  schedule           Probe scheduled startup (cmd 0x0B)\n"
        "  scan               Scan all known + unknown commands\n"
        "  raw <cmd> [hex..]  Send arbitrary command\n"
        "  repeat <cmd> [n] [ms]  Repeat command N times\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/pcat-pm-ctl";
    int opt;

    while ((opt = getopt(argc, argv, "D:vh")) != -1) {
        switch (opt) {
        case 'D': device = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *action = argv[optind];

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "(Try running as root)\n");
        return 1;
    }

    printf("Opened %s (fd=%d)\n\n", device, fd);

    if (strcmp(action, "monitor") == 0)
        do_monitor(fd);
    else if (strcmp(action, "version") == 0)
        do_version(fd);
    else if (strcmp(action, "led") == 0)
        do_led_probe(fd, argc - optind - 1, argv + optind + 1);
    else if (strcmp(action, "power-event") == 0)
        do_power_event(fd);
    else if (strcmp(action, "charger-auto") == 0)
        do_charger_auto(fd);
    else if (strcmp(action, "voltage-thresh") == 0)
        do_voltage_thresh(fd);
    else if (strcmp(action, "schedule") == 0)
        do_schedule(fd);
    else if (strcmp(action, "scan") == 0)
        do_scan(fd);
    else if (strcmp(action, "raw") == 0)
        do_raw(fd, argc - optind - 1, argv + optind + 1);
    else if (strcmp(action, "repeat") == 0)
        do_repeat(fd, argc - optind - 1, argv + optind + 1);
    else {
        fprintf(stderr, "Unknown action: %s\n", action);
        usage(argv[0]);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
