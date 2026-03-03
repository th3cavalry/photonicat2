// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mcu-dump.c — Photonicat 2 MCU protocol monitor & diagnostic tool
 *
 * Connects to the MCU serial port, sends heartbeats, and prints all
 * received frames in human-readable format. Use this to inspect MCU
 * communication, test commands, and reverse-engineer unknown payloads.
 *
 * Usage:
 *   mcu-dump [options]
 *
 * Options:
 *   -d DEVICE    Serial device (default: /dev/ttyS10)
 *   -b BAUD      Baud rate (default: 115200)
 *   -r           Raw hex dump mode (show all bytes)
 *   -s           Status report mode (pretty-print status)
 *   -f LEVEL     Set fan level (0-9) and exit
 *   -t           Sync current time to MCU RTC and exit
 *   -V           Query HW/FW version and exit
 *   -h           Show help
 *
 * Copyright (C) 2026 Brandon Cleary <cleary.brandon@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

#include "pcat2-mcu.h"

static volatile int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

static void print_status(const mcu_status_t *s)
{
    printf("── Status Report ──────────────────────────────\n");
    printf("  Battery:     %u mV", s->battery_mv);
    if (s->has_energy)
        printf("  SoC: %u%%  Energy: %u/%u mWh",
               s->soc, s->energy_now_uwh / 1000, s->energy_full_uwh / 1000);
    printf("\n");

    printf("  Charger:     %u mV  [%s]\n", s->charger_mv,
           s->on_charger ? "PLUGGED IN" : (s->on_battery ? "ON BATTERY" : "unknown"));

    if (s->has_temp) {
        printf("  Current:     %d mA (%s)\n", s->battery_current_ma,
               s->battery_current_ma > 0 ? "discharging" : "charging");
        printf("  Board temp:  %d°C\n", s->board_temp_c);
    }

    printf("  RTC:         %04u-%02u-%02u %02u:%02u:%02u",
           s->rtc_year, s->rtc_month, s->rtc_day,
           s->rtc_hour, s->rtc_min, s->rtc_sec);
    if (s->rtc_status)
        printf("  [INVALID]");
    printf("\n");

    printf("  GPIO in:     0x%04X  out: 0x%04X\n",
           s->gpio_input, s->gpio_output);

    if (s->has_motion) {
        printf("  Fan:         %u RPM\n", s->fan_rpm);
        printf("  Accel:       X=%d Y=%d Z=%d %s\n",
               s->accel_x, s->accel_y, s->accel_z,
               s->accel_ready ? "[ready]" : "[not ready]");
    }

    printf("───────────────────────────────────────────────\n");
}

static void usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Photonicat 2 MCU protocol monitor & diagnostic tool.\n"
        "\n"
        "Options:\n"
        "  -d DEVICE    Serial device (default: /dev/ttyS10)\n"
        "  -b BAUD      Baud rate (default: 115200)\n"
        "  -r           Raw hex dump mode\n"
        "  -s           Status report pretty-print mode (default)\n"
        "  -f LEVEL     Set fan level (0-9) and exit\n"
        "  -t           Sync system time to MCU RTC and exit\n"
        "  -V           Query HW/FW version and exit\n"
        "  -h           Show this help\n"
        "\n"
        "Monitor mode (default): sends heartbeats and prints all received\n"
        "frames until interrupted (Ctrl+C).\n",
        progname);
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/ttyS10";
    int baud = 115200;
    int raw_mode = 0;
    int fan_level = -1;
    int do_time_sync = 0;
    int do_version = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d:b:rsf:tVh")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'b': baud = atoi(optarg); break;
        case 'r': raw_mode = 1; break;
        case 's': raw_mode = 0; break;
        case 'f': fan_level = atoi(optarg); break;
        case 't': do_time_sync = 1; break;
        case 'V': do_version = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Open MCU connection */
    mcu_ctx_t ctx;
    if (mcu_open(&ctx, device, baud) < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", device, strerror(errno));
        return 1;
    }

    fprintf(stderr, "Connected to MCU on %s at %d baud\n", device, baud);

    /* One-shot commands */
    if (fan_level >= 0) {
        fprintf(stderr, "Setting fan to level %d\n", fan_level);
        mcu_set_fan(&ctx, fan_level);
        mcu_close(&ctx);
        return 0;
    }

    if (do_time_sync) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        fprintf(stderr, "Syncing time: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        mcu_set_datetime(&ctx, tm->tm_year + 1900, tm->tm_mon + 1,
                         tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

        /* Wait for ACK */
        mcu_frame_t frame;
        if (mcu_recv_timeout(&ctx, &frame, 2000) == 1) {
            if (frame.command == MCU_CMD_DATETIME_SYNC_ACK) {
                if (frame.extra_len > 0 && frame.extra_data[0] == 0)
                    fprintf(stderr, "Time sync OK\n");
                else
                    fprintf(stderr, "Time sync failed (error %d)\n",
                            frame.extra_len > 0 ? frame.extra_data[0] : -1);
            } else {
                mcu_dump_frame(&frame);
            }
        } else {
            fprintf(stderr, "No ACK received (timeout)\n");
        }
        mcu_close(&ctx);
        return 0;
    }

    if (do_version) {
        mcu_frame_t frame;

        fprintf(stderr, "Querying HW version...\n");
        mcu_get_hw_version(&ctx);
        if (mcu_recv_timeout(&ctx, &frame, 2000) == 1)
            mcu_dump_frame(&frame);
        else
            fprintf(stderr, "No response\n");

        fprintf(stderr, "Querying FW version...\n");
        mcu_get_fw_version(&ctx);
        if (mcu_recv_timeout(&ctx, &frame, 2000) == 1)
            mcu_dump_frame(&frame);
        else
            fprintf(stderr, "No response\n");

        mcu_close(&ctx);
        return 0;
    }

    /* Monitor mode */
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    fprintf(stderr, "Monitoring MCU traffic (Ctrl+C to stop)...\n\n");

    time_t last_heartbeat = 0;
    unsigned long frame_count = 0;

    while (running) {
        /* Send heartbeat every second */
        time_t now = time(NULL);
        if (now > last_heartbeat) {
            mcu_send_heartbeat(&ctx);
            last_heartbeat = now;
        }

        /* Try to receive a frame (100ms timeout) */
        mcu_frame_t frame;
        int ret = mcu_recv_timeout(&ctx, &frame, 100);

        if (ret < 0) {
            fprintf(stderr, "Read error: %s\n", strerror(errno));
            break;
        }

        if (ret == 0)
            continue;

        frame_count++;

        /* Send ACK if requested */
        if (frame.need_ack)
            mcu_send(&ctx, frame.command + 1, NULL, 0, false);

        /* Display frame */
        if (raw_mode) {
            printf("[%lu] ", frame_count);
            mcu_dump_frame(&frame);
            printf("\n");
        } else {
            /* Pretty-print status reports, dump everything else */
            if (frame.command == MCU_CMD_STATUS_REPORT && frame.extra_len >= 16) {
                mcu_status_t status;
                if (mcu_parse_status(frame.extra_data, frame.extra_len, &status) == 0)
                    print_status(&status);
            } else {
                printf("[%lu] ", frame_count);
                mcu_dump_frame(&frame);
                printf("\n");
            }
        }

        fflush(stdout);
    }

    fprintf(stderr, "\nReceived %lu frames total\n", frame_count);
    mcu_close(&ctx);
    return 0;
}
