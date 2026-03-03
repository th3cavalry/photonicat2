/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * pcat2-mcu.h — Photonicat 2 MCU communication library
 *
 * Open-source implementation of the MCU serial protocol used by the
 * Ariaboard Photonicat 2. Protocol reverse-engineered from the GPL-licensed
 * photonicat-pm kernel driver by Kyosuke Nekoyashiki.
 *
 * Copyright (C) 2026 Brandon Cleary <cleary.brandon@gmail.com>
 */

#ifndef PCAT2_MCU_H
#define PCAT2_MCU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Protocol constants ──────────────────────────────────────────── */

#define MCU_FRAME_HEADER     0xA5
#define MCU_FRAME_TAIL       0x5A
#define MCU_FRAME_MIN_SIZE   13    /* header(1)+src(1)+dst(1)+fnum(2)+dlen(2)+cmd(2)+ack(1)+crc(2)+tail(1) */
#define MCU_MAX_EXTRA_DATA   512
#define MCU_MAX_FRAME_SIZE   (MCU_FRAME_MIN_SIZE + MCU_MAX_EXTRA_DATA)

/* Addresses */
#define MCU_ADDR_HOST        0x01
#define MCU_ADDR_MCU         0x81
#define MCU_ADDR_BROADCAST   0x80
#define MCU_ADDR_ALL         0xFF

/* ── Command IDs ─────────────────────────────────────────────────── */

typedef enum {
    MCU_CMD_HEARTBEAT                = 0x01,
    MCU_CMD_HEARTBEAT_ACK            = 0x02,
    MCU_CMD_HW_VERSION_GET           = 0x03,
    MCU_CMD_HW_VERSION_GET_ACK       = 0x04,
    MCU_CMD_FW_VERSION_GET           = 0x05,
    MCU_CMD_FW_VERSION_GET_ACK       = 0x06,
    MCU_CMD_STATUS_REPORT            = 0x07,
    MCU_CMD_STATUS_REPORT_ACK        = 0x08,
    MCU_CMD_DATETIME_SYNC            = 0x09,
    MCU_CMD_DATETIME_SYNC_ACK        = 0x0A,
    MCU_CMD_SCHEDULE_STARTUP_SET     = 0x0B,
    MCU_CMD_SCHEDULE_STARTUP_SET_ACK = 0x0C,
    MCU_CMD_PMU_REQUEST_SHUTDOWN     = 0x0D,
    MCU_CMD_PMU_REQUEST_SHUTDOWN_ACK = 0x0E,
    MCU_CMD_HOST_REQUEST_SHUTDOWN    = 0x0F,
    MCU_CMD_HOST_REQUEST_SHUTDOWN_ACK = 0x10,
    MCU_CMD_PMU_REQUEST_FACTORY_RESET     = 0x11,
    MCU_CMD_PMU_REQUEST_FACTORY_RESET_ACK = 0x12,
    MCU_CMD_WATCHDOG_SET             = 0x13,
    MCU_CMD_WATCHDOG_SET_ACK         = 0x14,
    MCU_CMD_CHARGER_AUTO_START       = 0x15,
    MCU_CMD_CHARGER_AUTO_START_ACK   = 0x16,
    MCU_CMD_VOLTAGE_THRESHOLD_SET    = 0x17,
    MCU_CMD_VOLTAGE_THRESHOLD_SET_ACK = 0x18,
    MCU_CMD_LED_SETUP                = 0x19,
    MCU_CMD_LED_SETUP_ACK            = 0x1A,
    MCU_CMD_POWER_ON_EVENT_GET       = 0x1B,
    MCU_CMD_POWER_ON_EVENT_GET_ACK   = 0x1C,
    MCU_CMD_FAN_SET                  = 0x93,
    MCU_CMD_FAN_SET_ACK              = 0x94,
    MCU_CMD_DEVICE_MOVEMENT          = 0x95,
    MCU_CMD_DEVICE_MOVEMENT_ACK      = 0x96,
} mcu_command_t;

/* ── Parsed status report ────────────────────────────────────────── */

typedef struct {
    /* Base fields (always present) */
    uint16_t battery_mv;          /* Battery voltage in mV */
    uint16_t charger_mv;          /* Charger voltage in mV */
    uint16_t gpio_input;          /* MCU GPIO input bitmask */
    uint16_t gpio_output;         /* MCU GPIO output bitmask */

    /* RTC */
    uint16_t rtc_year;
    uint8_t  rtc_month;           /* 1-12 */
    uint8_t  rtc_day;             /* 1-31 */
    uint8_t  rtc_hour;            /* 0-23 */
    uint8_t  rtc_min;             /* 0-59 */
    uint8_t  rtc_sec;             /* 0-59 */
    uint8_t  rtc_status;          /* 0 = valid */

    /* Extended (if available) */
    bool     has_temp;
    int      board_temp_c;        /* Board temperature in °C */
    int16_t  battery_current_ma;  /* mA, positive=discharging */

    /* Energy (if available) */
    bool     has_energy;
    uint8_t  soc;                 /* State of charge, 0-100% */
    uint32_t energy_now_uwh;      /* Current energy in µWh */
    uint32_t energy_full_uwh;     /* Full energy in µWh */

    /* Accelerometer & fan (if available) */
    bool     has_motion;
    int32_t  accel_x;
    int32_t  accel_y;
    int32_t  accel_z;
    bool     accel_ready;
    uint32_t fan_rpm;

    /* Derived */
    bool     on_battery;
    bool     on_charger;
} mcu_status_t;

/* ── Parsed frame ────────────────────────────────────────────────── */

typedef struct {
    uint8_t  src;
    uint8_t  dst;
    uint16_t frame_num;
    uint16_t command;
    uint8_t  extra_data[MCU_MAX_EXTRA_DATA];
    uint16_t extra_len;
    bool     need_ack;
} mcu_frame_t;

/* ── MCU connection context ──────────────────────────────────────── */

typedef struct {
    int      fd;                  /* Serial port file descriptor */
    uint16_t frame_counter;       /* Outgoing frame sequence number */
    uint8_t  rx_buf[4096];        /* Receive buffer */
    size_t   rx_used;             /* Bytes in receive buffer */
} mcu_ctx_t;

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * Compute CRC16-Modbus checksum.
 * @param data   Input bytes
 * @param len    Number of bytes
 * @return       16-bit CRC
 */
uint16_t mcu_crc16(const uint8_t *data, size_t len);

/**
 * Open a serial connection to the MCU.
 * @param ctx    Context to initialize
 * @param device Serial device path (e.g., "/dev/ttyS10")
 * @param baud   Baud rate (typically 115200)
 * @return       0 on success, -1 on error (errno set)
 */
int mcu_open(mcu_ctx_t *ctx, const char *device, int baud);

/**
 * Close the MCU connection.
 */
void mcu_close(mcu_ctx_t *ctx);

/**
 * Send a command to the MCU.
 * @param ctx         MCU context
 * @param command     Command ID
 * @param extra_data  Optional payload (NULL if none)
 * @param extra_len   Payload length
 * @param need_ack    Request ACK from MCU
 * @return            Bytes written, or -1 on error
 */
int mcu_send(mcu_ctx_t *ctx, uint16_t command,
             const uint8_t *extra_data, uint16_t extra_len,
             bool need_ack);

/**
 * Try to read and parse one frame from the serial port.
 * Non-blocking if no data available (returns 0).
 * @param ctx    MCU context
 * @param frame  Output: parsed frame
 * @return       1 if frame parsed, 0 if no complete frame, -1 on error
 */
int mcu_recv(mcu_ctx_t *ctx, mcu_frame_t *frame);

/**
 * Block until a frame is received, with timeout.
 * @param ctx        MCU context
 * @param frame      Output: parsed frame
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return           1 if frame received, 0 on timeout, -1 on error
 */
int mcu_recv_timeout(mcu_ctx_t *ctx, mcu_frame_t *frame, int timeout_ms);

/**
 * Parse a status report payload into a structured format.
 * @param data     Extra data from a STATUS_REPORT frame
 * @param data_len Length of extra data
 * @param status   Output: parsed status
 * @return         0 on success, -1 if data too short
 */
int mcu_parse_status(const uint8_t *data, uint16_t data_len, mcu_status_t *status);

/* ── Convenience functions ───────────────────────────────────────── */

/** Send a heartbeat (command 0x01). */
int mcu_send_heartbeat(mcu_ctx_t *ctx);

/** Request HW version (command 0x03). */
int mcu_get_hw_version(mcu_ctx_t *ctx);

/** Request FW version (command 0x05). */
int mcu_get_fw_version(mcu_ctx_t *ctx);

/**
 * Set the MCU RTC.
 * @param year   e.g. 2026
 * @param month  1-12
 * @param day    1-31
 * @param hour   0-23
 * @param min    0-59
 * @param sec    0-59
 */
int mcu_set_datetime(mcu_ctx_t *ctx, uint16_t year, uint8_t month,
                     uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);

/**
 * Set fan speed level.
 * @param level  0=off, 1-9=speed levels
 */
int mcu_set_fan(mcu_ctx_t *ctx, int level);

/**
 * Configure the watchdog.
 * @param shutdown_timeout    Seconds before forced shutdown (typically 60)
 * @param force_poweroff      Force poweroff timeout
 * @param heartbeat_interval  Watchdog interval (0=disable, default 10)
 */
int mcu_set_watchdog(mcu_ctx_t *ctx, uint8_t shutdown_timeout,
                     uint8_t force_poweroff, uint8_t heartbeat_interval);

/**
 * Request host shutdown (tells MCU we're powering off).
 */
int mcu_request_shutdown(mcu_ctx_t *ctx);

/**
 * Get a human-readable name for a command ID.
 */
const char *mcu_cmd_name(uint16_t command);

/**
 * Print a frame in human-readable format to stderr.
 */
void mcu_dump_frame(const mcu_frame_t *frame);

/**
 * Print a hex dump of raw bytes.
 */
void mcu_hexdump(const uint8_t *data, size_t len);

#endif /* PCAT2_MCU_H */
