/*
 * pcat2-display - PhotoniCAT 2 Mini Display Status Application
 *
 * Drives the GC9307 172x320 TFT LCD via SPI to show system status.
 * All configuration is read from UCI (/etc/config/photonicat).
 *
 * Hardware: GC9307 on SPI1.0 (6MHz), DC=GPIO3_PD1, RST=GPIO3_PD2, BL=GPIO3_C5
 * Display: 172x320 pixels, RGB565 color, rotation 180 deg, column offset 34
 * Backlight: active LOW (PWM polarity inverted per factory DTS)
 *
 * Configurable: pages, theme, refresh rate, backlight, custom params.
 * SIGHUP reloads config.  SIGTERM/SIGINT cleanly shutdown.
 *
 * Copyright (C) 2026 - GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>
#include <linux/input.h>

/* ================================================================== */
/*  Hardware constants                                                 */
/* ================================================================== */

#define SPI_DEVICE      "/dev/spidev1.0"
#define SPI_SPEED_HZ    6000000
#define SPI_CHUNK       4096

#define DISP_W          172
#define DISP_H          320
#define COL_OFFSET      34

#define DC_BANK     3
#define DC_OFFSET   25
#define RST_BANK    3
#define RST_OFFSET  26
#define BL_BANK     3
#define BL_OFFSET   21

/* GC9307 / ST7789 registers */
#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_SLPIN   0x10
#define CMD_NORON   0x13
#define CMD_INVOFF  0x20
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_COLMOD  0x3A
#define CMD_MADCTL  0x36
#define MADCTL_MX   0x40

/* BGR565 colour helpers - display byte order is big-endian */
#define RGB565(r,g,b) ( (uint16_t)( \
        (((uint16_t)((b)>>3))<<11) | \
        (((uint16_t)((g)>>2))<<5)  | \
         ((uint16_t)((r)>>3)) ))

/* ================================================================== */
/*  Theme colours (set at runtime from config)                         */
/* ================================================================== */

static uint16_t COL_BG;         /* background */
static uint16_t COL_FG;         /* primary text */
static uint16_t COL_ACCENT;     /* section headers, highlights */
static uint16_t COL_DIM;        /* separators, secondary text */
static uint16_t COL_TOPBAR;     /* top bar background */
static uint16_t COL_GOOD;       /* green: OK indicators */
static uint16_t COL_WARN;       /* yellow/orange: warnings */
static uint16_t COL_BAD;        /* red: errors */

/* preset themes */
struct theme {
    const char *name;
    uint16_t bg, fg, accent, dim, topbar, good, warn, bad;
};

static const struct theme themes[] = {
    {"dark",
        RGB565(0,0,0),       RGB565(255,255,255), RGB565(0,220,220),
        RGB565(40,40,40),    RGB565(0,8,30),      RGB565(40,255,40),
        RGB565(255,200,0),   RGB565(255,40,40)},
    {"light",
        RGB565(240,240,235), RGB565(20,20,20),    RGB565(0,100,180),
        RGB565(180,180,170), RGB565(200,200,195), RGB565(0,130,0),
        RGB565(200,150,0),   RGB565(200,0,0)},
    {"green",
        RGB565(0,0,0),       RGB565(0,255,0),     RGB565(0,200,0),
        RGB565(0,60,0),      RGB565(0,15,0),      RGB565(0,255,0),
        RGB565(255,255,0),   RGB565(255,40,40)},
    {"cyan",
        RGB565(0,0,0),       RGB565(0,230,230),   RGB565(0,180,255),
        RGB565(0,40,50),     RGB565(0,10,20),     RGB565(0,255,128),
        RGB565(255,200,0),   RGB565(255,40,40)},
    {"amber",
        RGB565(0,0,0),       RGB565(255,180,0),   RGB565(255,140,0),
        RGB565(50,30,0),     RGB565(20,10,0),     RGB565(0,255,0),
        RGB565(255,255,0),   RGB565(255,40,40)},
    {NULL, 0,0,0,0,0,0,0,0}
};

static void apply_theme(const char *name)
{
    for (int i = 0; themes[i].name; i++) {
        if (strcmp(themes[i].name, name) == 0) {
            COL_BG     = themes[i].bg;
            COL_FG     = themes[i].fg;
            COL_ACCENT = themes[i].accent;
            COL_DIM    = themes[i].dim;
            COL_TOPBAR = themes[i].topbar;
            COL_GOOD   = themes[i].good;
            COL_WARN   = themes[i].warn;
            COL_BAD    = themes[i].bad;
            return;
        }
    }
    /* default to dark */
    apply_theme("dark");
}

/* ================================================================== */
/*  Configuration                                                      */
/* ================================================================== */

#define MAX_PAGES    8
#define MAX_CUSTOM  10

struct custom_param {
    char label[32];
    char source[256];       /* sysfs path or 'cmd:shell command' */
    char unit[16];
    int  divide;            /* divide integer result by this (e.g. 1000 for mV->V) */
};

struct config {
    int  backlight;                 /* 0=off, 1=on */
    int  refresh;                   /* seconds */
    int  poweroff_ms;               /* length of press to trigger shutdown */
    char theme[16];
    float font_scale;               /* 1.0 - 2.0 */

    /* pages to show, in order */
    int  num_pages;
    char pages[MAX_PAGES][16];      /* "clock", "battery", "network", "wifi", "thermal", "system", "custom" */

    /* custom parameters */
    int  num_custom;
    struct custom_param custom[MAX_CUSTOM];
};

static struct config cfg;

/* Simple UCI parser - reads /etc/config/photonicat */
static void config_defaults(void)
{
    cfg.backlight  = 1;
    cfg.refresh    = 5;
    cfg.poweroff_ms = 1000;          /* default 1 second long-press to shutdown */
    strncpy(cfg.theme, "dark", sizeof(cfg.theme));
    cfg.font_scale = 1.0f;

    /* default: 5-page cycling mode (short-press power button to cycle) */
    cfg.num_pages = 5;
    strncpy(cfg.pages[0], "clock",    sizeof(cfg.pages[0]));
    strncpy(cfg.pages[1], "cellular", sizeof(cfg.pages[1]));
    strncpy(cfg.pages[2], "battery",  sizeof(cfg.pages[2]));
    strncpy(cfg.pages[3], "network",  sizeof(cfg.pages[3]));
    strncpy(cfg.pages[4], "system",   sizeof(cfg.pages[4]));

    cfg.num_custom = 0;
}

/* Read UCI option using 'uci -q get' */
static int uci_get(const char *key, char *buf, size_t len)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "uci -q get photonicat.%s 2>/dev/null", key);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    buf[0] = '\0';
    if (fgets(buf, len, f) == NULL) buf[0] = '\0';
    pclose(f);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return buf[0] ? 0 : -1;
}

/* Read UCI list option */
static int uci_get_list(const char *section, const char *option,
                        char list[][16], int max_items)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "uci -q get photonicat.%s.%s 2>/dev/null", section, option);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;

    int count = 0;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* UCI lists are space-separated when read with 'get' */
        char *tok = strtok(line, " ");
        while (tok && count < max_items) {
            strncpy(list[count], tok, 15);
            list[count][15] = '\0';
            count++;
            tok = strtok(NULL, " ");
        }
    }
    pclose(f);
    return count;
}

static void config_load(void)
{
    char buf[256];

    config_defaults();

    if (uci_get("display.backlight", buf, sizeof(buf)) == 0)
        cfg.backlight = atoi(buf);

    if (uci_get("display.refresh", buf, sizeof(buf)) == 0) {
        cfg.refresh = atoi(buf);
        if (cfg.refresh < 1) cfg.refresh = 1;
        if (cfg.refresh > 60) cfg.refresh = 60;
    }

    if (uci_get("display.poweroff_ms", buf, sizeof(buf)) == 0) {
        cfg.poweroff_ms = atoi(buf);
        if (cfg.poweroff_ms < 100) cfg.poweroff_ms = 100;
        if (cfg.poweroff_ms > 30000) cfg.poweroff_ms = 30000;
    }

    if (uci_get("display.theme", buf, sizeof(buf)) == 0)
        strncpy(cfg.theme, buf, sizeof(cfg.theme) - 1);

    if (uci_get("display.font_scale", buf, sizeof(buf)) == 0) {
        cfg.font_scale = strtof(buf, NULL);
        if (cfg.font_scale < 1.0f) cfg.font_scale = 1.0f;
        if (cfg.font_scale > 2.0f) cfg.font_scale = 2.0f;
    }

    /* pages list */
    int n = uci_get_list("display", "pages", cfg.pages, MAX_PAGES);
    if (n > 0) cfg.num_pages = n;

    /* custom parameters: display_param sections */
    cfg.num_custom = 0;
    for (int i = 0; i < MAX_CUSTOM; i++) {
        char key[64];
        snprintf(key, sizeof(key), "display_param_%d.label", i);
        if (uci_get(key, buf, sizeof(buf)) != 0) break;
        strncpy(cfg.custom[cfg.num_custom].label, buf, 31);

        snprintf(key, sizeof(key), "display_param_%d.source", i);
        if (uci_get(key, buf, sizeof(buf)) == 0)
            strncpy(cfg.custom[cfg.num_custom].source, buf, 255);

        snprintf(key, sizeof(key), "display_param_%d.unit", i);
        if (uci_get(key, buf, sizeof(buf)) == 0)
            strncpy(cfg.custom[cfg.num_custom].unit, buf, 15);

        snprintf(key, sizeof(key), "display_param_%d.divide", i);
        if (uci_get(key, buf, sizeof(buf)) == 0)
            cfg.custom[cfg.num_custom].divide = atoi(buf);
        else
            cfg.custom[cfg.num_custom].divide = 0;

        cfg.num_custom++;
    }

    apply_theme(cfg.theme);
}

/* ================================================================== */
/*  5x7 font bitmap (column-major, LSB = top row) ASCII 0x20..0x7E    */
/* ================================================================== */

static const unsigned char font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */
};

/* ================================================================== */
/*  Global state                                                       */
/* ================================================================== */

static volatile sig_atomic_t running    = 1;
static volatile sig_atomic_t reload_cfg = 0;
static volatile sig_atomic_t dump_screenshot = 0;
static int spi_fd = -1;
static int dc_line_fd  = -1;
static int rst_line_fd = -1;
static int bl_line_fd  = -1;
static int input_fd    = -1;
static int page_idx    = 0;

static uint8_t fb[DISP_W * DISP_H * 2];

/* ================================================================== */
/*  Utility                                                            */
/* ================================================================== */

static int read_sysfs(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

static int read_sysfs_int(const char *path)
{
    char buf[32];
    if (read_sysfs(path, buf, sizeof(buf)) < 0) return -1;
    return atoi(buf);
}

static int run_cmd(const char *cmd, char *buf, size_t len)
{
    buf[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t off = 0;
    while (off < len - 1) {
        size_t n = fread(buf + off, 1, len - 1 - off, f);
        if (n == 0) break;
        off += n;
    }
    buf[off] = '\0';
    /* strip trailing whitespace */
    while (off > 0 && (buf[off-1] == '\n' || buf[off-1] == '\r' ||
                        buf[off-1] == ' '  || buf[off-1] == '\t'))
        buf[--off] = '\0';
    int rc = pclose(f);
    return (rc == 0 && buf[0]) ? 0 : -1;
}

/* ================================================================== */
/*  GPIO (chardev v2 API)                                              */
/* ================================================================== */

static int gpio_request_output(int bank, int offset,
                               const char *name, int initial)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/gpiochip%d", bank);
    int chip_fd = open(path, O_RDWR);
    if (chip_fd < 0) {
        fprintf(stderr, "gpio: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    req.offsets[0] = offset;
    req.num_lines  = 1;
    strncpy(req.consumer, name, sizeof(req.consumer) - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (initial) {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id     = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = 1;
        req.config.attrs[0].mask        = 1;
    }

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "gpio: V2_GET_LINE bank%d offset%d (%s): %s\n",
                bank, offset, name, strerror(errno));
        close(chip_fd);
        return -1;
    }
    close(chip_fd);
    return req.fd;
}

static void gpio_set(int line_fd, int value)
{
    if (line_fd < 0) return;
    struct gpio_v2_line_values vals;
    memset(&vals, 0, sizeof(vals));
    vals.mask = 1;
    vals.bits = value ? 1 : 0;
    ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
}

/* ================================================================== */
/*  Power button input                                                 */
/* ================================================================== */

static void input_init(void)
{
    input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        fprintf(stderr, "pcat2-display: cannot open /dev/input/event0: %s\n",
                strerror(errno));
        return;
    }
    /* Grab exclusively so procd button handler won't trigger poweroff */
    int grab = 1;
    if (ioctl(input_fd, EVIOCGRAB, &grab) < 0)
        fprintf(stderr, "pcat2-display: EVIOCGRAB: %s\n", strerror(errno));
    fprintf(stderr, "pcat2-display: power button input ready\n");
}

/* Returns: 1 = short press, 2 = long press (>= cfg.poweroff_ms ms), 0 = nothing */
static int input_check(void)
{
    if (input_fd < 0) return 0;

    static int pressed = 0;
    static struct timespec press_ts;
    struct input_event ev;
    int result = 0;

    while (read(input_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY || ev.code != KEY_POWER) continue;
        if (ev.value == 1) {                /* key down */
            pressed = 1;
            clock_gettime(CLOCK_MONOTONIC, &press_ts);
        } else if (ev.value == 0 && pressed) {  /* key up */
            pressed = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - press_ts.tv_sec) * 1000 +
                      (now.tv_nsec - press_ts.tv_nsec) / 1000000;
            result = (ms >= cfg.poweroff_ms) ? 2 : 1;
        }
    }

    /* Detect ongoing long press (held >=3s without release) */
    if (pressed) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - press_ts.tv_sec) * 1000 +
                  (now.tv_nsec - press_ts.tv_nsec) / 1000000;
        if (ms >= cfg.poweroff_ms) { pressed = 0; result = 2; }
    }

    return result;
}

/* ================================================================== */
/*  SPI                                                                */
/* ================================================================== */

static int spi_init(void)
{
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) { perror(SPI_DEVICE); return -1; }
    uint8_t  mode = SPI_MODE_0, bits = 8;
    uint32_t speed = SPI_SPEED_HZ;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    return 0;
}

static void spi_write(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = (len > SPI_CHUNK) ? SPI_CHUNK : len;
        struct spi_ioc_transfer tr = {
            .tx_buf        = (unsigned long)data,
            .len           = chunk,
            .speed_hz      = SPI_SPEED_HZ,
            .bits_per_word = 8,
        };
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0)
            perror("SPI_IOC_MESSAGE");
        data += chunk;
        len  -= chunk;
    }
}

/* ================================================================== */
/*  GC9307 display                                                     */
/* ================================================================== */

static void disp_cmd(uint8_t cmd)
{
    gpio_set(dc_line_fd, 0);
    spi_write(&cmd, 1);
}

static void disp_data(const uint8_t *data, size_t len)
{
    gpio_set(dc_line_fd, 1);
    spi_write(data, len);
}

static void disp_data1(uint8_t val)
{
    disp_data(&val, 1);
}

static void disp_set_window(int x, int y, int w, int h)
{
    uint16_t xs = x + COL_OFFSET, xe = x + w - 1 + COL_OFFSET;
    uint16_t ys = y, ye = y + h - 1;
    uint8_t ca[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
    uint8_t ra[4] = { ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF };
    disp_cmd(CMD_CASET); disp_data(ca, 4);
    disp_cmd(CMD_RASET); disp_data(ra, 4);
}

static void disp_init(void)
{
    gpio_set(rst_line_fd, 1); usleep(10000);
    gpio_set(rst_line_fd, 0); usleep(50000);
    gpio_set(rst_line_fd, 1); usleep(10000);
    disp_cmd(CMD_SWRESET); usleep(150000);
    disp_cmd(CMD_SLPOUT);  usleep(150000);
    disp_cmd(CMD_COLMOD);  disp_data1(0x55); usleep(10000);
    disp_cmd(CMD_MADCTL);  disp_data1(MADCTL_MX);
    disp_cmd(CMD_INVOFF);  usleep(10000);
    disp_cmd(CMD_NORON);   usleep(10000);
    disp_cmd(CMD_DISPON);  usleep(10000);
    gpio_set(bl_line_fd, 0);  /* backlight on (active LOW) */
}

static void disp_sleep(void)
{
    gpio_set(bl_line_fd, 1);
    disp_cmd(0x28); usleep(10000);  /* DISPOFF */
    disp_cmd(CMD_SLPIN); usleep(10000);
}

/* ================================================================== */
/*  Framebuffer primitives                                             */
/* ================================================================== */

static void fb_clear(uint16_t col)
{
    uint8_t hi = col >> 8, lo = col & 0xFF;
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        fb[i * 2]     = hi;
        fb[i * 2 + 1] = lo;
    }
}

static inline void fb_pixel(int x, int y, uint16_t col)
{
    if ((unsigned)x >= DISP_W || (unsigned)y >= DISP_H) return;
    int off = (y * DISP_W + x) * 2;
    fb[off]     = col >> 8;
    fb[off + 1] = col & 0xFF;
}

static void fb_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            fb_pixel(x + dx, y + dy, col);
}

static void fb_hline(int x, int y, int w, uint16_t col)
{
    for (int dx = 0; dx < w; dx++) fb_pixel(x + dx, y, col);
}

static void fb_flush(void)
{
    disp_set_window(0, 0, DISP_W, DISP_H);
    disp_cmd(CMD_RAMWR);
    gpio_set(dc_line_fd, 1);
    spi_write(fb, sizeof(fb));
}

/* ================================================================== */
/*  Text rendering                                                     */
/* ================================================================== */

/* Character dimensions at a given scale */
#define CHAR_W(s) ((int)(6.0f * (s)))
#define CHAR_H(s) ((int)(8.0f * (s)))

static void draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg, float s)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const unsigned char *glyph = font5x7[ch - 0x20];
    for (int col = 0; col < 6; col++) {
        uint8_t bits = (col < 5) ? glyph[col] : 0;
        int px0 = x + (int)(col * s);
        int px1 = x + (int)((col + 1) * s);
        for (int row = 0; row < 8; row++) {
            uint16_t c = (row < 7 && (bits & (1 << row))) ? fg : bg;
            int py0 = y + (int)(row * s);
            int py1 = y + (int)((row + 1) * s);
            for (int py = py0; py < py1; py++)
                for (int px = px0; px < px1; px++)
                    fb_pixel(px, py, c);
        }
    }
}

static int draw_str(int x, int y, const char *str,
                    uint16_t fg, uint16_t bg, float s)
{
    while (*str) {
        draw_char(x, y, *str, fg, bg, s);
        x += CHAR_W(s);
        str++;
    }
    return x;
}

static void draw_str_r(int xr, int y, const char *str,
                       uint16_t fg, uint16_t bg, float s)
{
    int len = strlen(str);
    draw_str(xr - len * CHAR_W(s), y, str, fg, bg, s);
}

static void draw_str_c(int y, const char *str,
                       uint16_t fg, uint16_t bg, float s)
{
    int w = strlen(str) * CHAR_W(s);
    draw_str((DISP_W - w) / 2, y, str, fg, bg, s);
}

/* ================================================================== */
/*  Icon & widget drawing primitives                                   */
/* ================================================================== */

/* Vertical line */
static void fb_vline(int x, int y, int h, uint16_t col)
{
    for (int dy = 0; dy < h; dy++) fb_pixel(x, y + dy, col);
}

/* Filled rounded-ish status dot (5x5 diamond/circle) */
static void draw_dot(int cx, int cy, uint16_t col)
{
    fb_pixel(cx, cy-2, col);
    for (int dx = -1; dx <= 1; dx++) fb_pixel(cx+dx, cy-1, col);
    for (int dx = -2; dx <= 2; dx++) fb_pixel(cx+dx, cy,   col);
    for (int dx = -1; dx <= 1; dx++) fb_pixel(cx+dx, cy+1, col);
    fb_pixel(cx, cy+2, col);
}

/* Battery icon - 16w x 9h, returns width consumed */
static int draw_battery_icon(int x, int y, int pct, int charging)
{
    uint16_t border = COL_DIM;
    /* outer frame */
    fb_hline(x+1, y,   12, border);          /* top */
    fb_hline(x+1, y+8, 12, border);          /* bottom */
    fb_vline(x,   y+1, 7, border);           /* left */
    fb_vline(x+13,y+1, 7, border);           /* right */
    /* nub */
    fb_rect(x+14, y+2, 2, 5, border);

    /* fill - 11 pixels wide max (x+1 to x+12) */
    int fw = (11 * pct + 50) / 100;
    if (fw < 0) fw = 0;
    if (fw > 11) fw = 11;
    uint16_t fc = (pct > 25) ? COL_GOOD : (pct > 10 ? COL_WARN : COL_BAD);
    if (fw > 0) fb_rect(x+1, y+1, fw, 7, fc);

    /* charging bolt overlay (small zigzag) */
    if (charging) {
        uint16_t bc = COL_WARN;
        fb_pixel(x+7, y+1, bc); fb_pixel(x+6, y+2, bc);
        fb_pixel(x+5, y+3, bc); fb_pixel(x+6, y+3, bc);
        fb_pixel(x+7, y+3, bc); fb_pixel(x+8, y+3, bc);
        fb_pixel(x+7, y+4, bc); fb_pixel(x+8, y+4, bc);
        fb_pixel(x+7, y+5, bc); fb_pixel(x+6, y+6, bc);
        fb_pixel(x+5, y+7, bc);
    }
    return 18;
}

/* WiFi signal bars icon - 12w x 9h */
static void draw_wifi_icon(int x, int y, int clients, uint16_t col)
{
    uint16_t dim = COL_DIM;
    /* 4 bars of increasing height */
    fb_rect(x,   y+7, 2, 2, (clients>0) ? col : dim);  /* bar 1: 2px tall */
    fb_rect(x+3, y+5, 2, 4, (clients>0) ? col : dim);  /* bar 2: 4px tall */
    fb_rect(x+6, y+3, 2, 6, (clients>0) ? col : dim);  /* bar 3: 6px tall */
    fb_rect(x+9, y+1, 2, 8, (clients>0) ? col : dim);  /* bar 4: 8px tall */
}

/* Up arrow (upload) - 7w x 5h */
static void draw_arrow_up(int x, int y, uint16_t col)
{
    fb_pixel(x+3, y, col);
    fb_pixel(x+2, y+1, col); fb_pixel(x+3, y+1, col); fb_pixel(x+4, y+1, col);
    fb_pixel(x+1, y+2, col); fb_pixel(x+2, y+2, col); fb_pixel(x+3, y+2, col);
    fb_pixel(x+4, y+2, col); fb_pixel(x+5, y+2, col);
    fb_pixel(x+3, y+3, col); fb_pixel(x+3, y+4, col);
}

/* Down arrow (download) - 7w x 5h */
static void draw_arrow_down(int x, int y, uint16_t col)
{
    fb_pixel(x+3, y, col); fb_pixel(x+3, y+1, col);
    fb_pixel(x+1, y+2, col); fb_pixel(x+2, y+2, col); fb_pixel(x+3, y+2, col);
    fb_pixel(x+4, y+2, col); fb_pixel(x+5, y+2, col);
    fb_pixel(x+2, y+3, col); fb_pixel(x+3, y+3, col); fb_pixel(x+4, y+3, col);
    fb_pixel(x+3, y+4, col);
}

/* Horizontal progress bar with border */
static void draw_progress_bar(int x, int y, int w, int h, int pct,
                              uint16_t fg, uint16_t border)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* outer border (1px) */
    fb_hline(x, y, w, border);
    fb_hline(x, y + h - 1, w, border);
    fb_vline(x, y + 1, h - 2, border);
    fb_vline(x + w - 1, y + 1, h - 2, border);

    /* inner background (already COL_BG from fb_clear) */
    /* fill */
    int fw = (w - 2) * pct / 100;
    if (fw > 0)
        fb_rect(x + 1, y + 1, fw, h - 2, fg);
}

/* Page indicator dots at bottom of screen */
static void draw_page_dots(int current, int total)
{
    if (total <= 1) return;
    int spacing = 14;
    int total_w = (total - 1) * spacing;
    int sx = (DISP_W - total_w) / 2;
    int dy = DISP_H - 8;

    for (int i = 0; i < total; i++) {
        int cx = sx + i * spacing;
        uint16_t col = (i == current) ? COL_ACCENT : COL_DIM;
        /* 5px filled diamond/dot */
        fb_pixel(cx, dy - 2, col);
        for (int dx = -1; dx <= 1; dx++) fb_pixel(cx + dx, dy - 1, col);
        for (int dx = -2; dx <= 2; dx++) fb_pixel(cx + dx, dy, col);
        for (int dx = -1; dx <= 1; dx++) fb_pixel(cx + dx, dy + 1, col);
        fb_pixel(cx, dy + 2, col);
    }
}

/* Small thermometer icon: 5w x 9h */
static void draw_thermo_icon(int x, int y, uint16_t col)
{
    fb_pixel(x+2, y, col);
    fb_vline(x+1, y+1, 5, col); fb_vline(x+3, y+1, 5, col);
    /* fill inside */
    fb_vline(x+2, y+3, 3, col);
    /* bulb at bottom */
    for (int dx = 0; dx <= 4; dx++) fb_pixel(x+dx, y+6, col);
    for (int dx = 0; dx <= 4; dx++) fb_pixel(x+dx, y+7, col);
    for (int dx = 1; dx <= 3; dx++) fb_pixel(x+dx, y+8, col);
}

/* Fan icon: simple 4-spoke propeller 9w x 9h */
static void draw_fan_icon(int x, int y, uint16_t col)
{
    /* center hub */
    fb_pixel(x+4, y+3, col); fb_pixel(x+3, y+4, col);
    fb_pixel(x+4, y+4, col); fb_pixel(x+5, y+4, col);
    fb_pixel(x+4, y+5, col);
    /* blades */
    fb_pixel(x+4, y,   col); fb_pixel(x+3, y+1, col); fb_pixel(x+4, y+1, col);  /* top */
    fb_pixel(x+4, y+2, col);
    fb_pixel(x+7, y+3, col); fb_pixel(x+7, y+4, col); fb_pixel(x+6, y+4, col);  /* right */
    fb_pixel(x+8, y+4, col);
    fb_pixel(x+4, y+8, col); fb_pixel(x+5, y+7, col); fb_pixel(x+4, y+7, col);  /* bottom */
    fb_pixel(x+4, y+6, col);
    fb_pixel(x+1, y+5, col); fb_pixel(x+1, y+4, col); fb_pixel(x+2, y+4, col);  /* left */
    fb_pixel(x+0, y+4, col);
}

/* ================================================================== */
/*  Data collection helpers                                            */
/* ================================================================== */

static int get_battery_pct(void)
{ return read_sysfs_int("/sys/class/power_supply/battery/capacity"); }

static void get_battery_status(char *buf, size_t len)
{
    if (read_sysfs("/sys/class/power_supply/battery/status", buf, len) < 0)
        strncpy(buf, "N/A", len);
}

static int get_battery_voltage_mv(void)
{
    int uv = read_sysfs_int("/sys/class/power_supply/battery/voltage_now");
    return (uv > 0) ? uv / 1000 : -1;
}

static int get_battery_current_ma(void)
{
    int ua = read_sysfs_int("/sys/class/power_supply/battery/current_now");
    return (ua >= 0) ? ua / 1000 : -1;
}

static int get_cpu_temp(void)
{
    static const char *paths[] = {
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/class/thermal/thermal_zone0/temp",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        int t = read_sysfs_int(paths[i]);
        if (t > 0) return t / 1000;
    }
    return -1;
}

static void get_uptime_str(char *buf, size_t len)
{
    char raw[64];
    if (read_sysfs("/proc/uptime", raw, sizeof(raw)) < 0) {
        strncpy(buf, "N/A", len); return;
    }
    int secs = atoi(raw);
    int d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    if (d > 0)      snprintf(buf, len, "%dd %dh %dm", d, h, m);
    else if (h > 0) snprintf(buf, len, "%dh %dm", h, m);
    else            snprintf(buf, len, "%dm", m);
}

static void get_memory(int *used_mb, int *total_mb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    *used_mb = *total_mb = 0;
    if (!f) return;
    long total = 0, avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, " %ld", &total);
        else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, " %ld", &avail);
    }
    fclose(f);
    *total_mb = (int)(total / 1024);
    *used_mb  = (int)((total - avail) / 1024);
}

static void get_wan_info(char *iface, size_t ilen, char *ip, size_t iplen)
{
    strncpy(iface, "", ilen);
    strncpy(ip, "N/A", iplen);
    char line[256];
    run_cmd("ip route show default 2>/dev/null | head -1", line, sizeof(line));
    char *dev = strstr(line, "dev ");
    if (dev) sscanf(dev + 4, "%s", iface);
    if (iface[0] == '\0') return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "ip -4 addr show %s 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d/ -f1 | head -1",
        iface);
    run_cmd(cmd, ip, iplen);
}

static void get_wifi_ssid(char *buf, size_t len)
{
    run_cmd("uci -q get wireless.default_radio0.ssid 2>/dev/null", buf, len);
    if (buf[0] == '\0') strncpy(buf, "N/A", len);
}

static void get_wifi_band_chan(char *buf, size_t len)
{
    char band[16] = "", chan[16] = "";
    run_cmd("uci -q get wireless.radio0.band 2>/dev/null", band, sizeof(band));
    run_cmd("uci -q get wireless.radio0.channel 2>/dev/null", chan, sizeof(chan));
    if (band[0] && chan[0]) snprintf(buf, len, "%s Ch%s", band, chan);
    else strncpy(buf, "N/A", len);
}

static int get_wifi_clients(void)
{
    char buf[16];
    run_cmd("iwinfo wlan0 assoclist 2>/dev/null | grep -c 'dBm'", buf, sizeof(buf));
    return atoi(buf);
}

static int get_fan_rpm(void)
{
    /* find hwmon with name pcat_pm_hwmon_speed_fan */
    for (int i = 0; i < 20; i++) {
        char path[64], name[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
        if (read_sysfs(path, name, sizeof(name)) < 0) continue;
        if (strcmp(name, "pcat_pm_hwmon_speed_fan") == 0) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/fan1_input", i);
            return read_sysfs_int(path);
        }
    }
    return -1;
}

static int get_fan_level(void)
{
    for (int i = 0; i < 10; i++) {
        char path[64], type[32];
        snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%d/type", i);
        if (read_sysfs(path, type, sizeof(type)) < 0) continue;
        if (strcmp(type, "pcat-pm-fan") == 0) {
            snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%d/cur_state", i);
            return read_sysfs_int(path);
        }
    }
    return -1;
}

static int get_board_temp(void)
{
    for (int i = 0; i < 20; i++) {
        char path[64], name[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
        if (read_sysfs(path, name, sizeof(name)) < 0) continue;
        if (strcmp(name, "pcat_pm_hwmon_temp_mb") == 0) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", i);
            int t = read_sysfs_int(path);
            return (t > 0) ? t / 1000 : -1;
        }
    }
    return -1;
}

/* CPU usage percentage (delta-based from /proc/stat) */
static int get_cpu_usage(void)
{
    static long prev_idle = 0, prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);

    long user, nice, sys, idle, iow, irq, sirq, steal;
    if (sscanf(buf, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &sys, &idle, &iow, &irq, &sirq, &steal) < 4)
        return -1;

    long total = user + nice + sys + idle + iow + irq + sirq + steal;
    long idle_all = idle + iow;

    int pct = 0;
    if (prev_total > 0) {
        long dt = total - prev_total;
        long di = idle_all - prev_idle;
        if (dt > 0) pct = (int)(100 * (dt - di) / dt);
    }

    prev_total = total;
    prev_idle  = idle_all;
    return pct;
}

/* Disk usage percentage for root filesystem */
static int get_disk_usage_pct(void)
{
    struct statvfs st;
    if (statvfs("/overlay", &st) != 0) {
        if (statvfs("/", &st) != 0) return -1;
    }
    if (st.f_blocks == 0) return 0;
    return (int)(100 * (st.f_blocks - st.f_bfree) / st.f_blocks);
}

/* ── Cellular modem helpers (uqmi / QMI) ────────────────────────── */

/* Parse carrier, RAT, roaming from uqmi --get-serving-system JSON */
static void get_cellular_info(char *carrier, size_t clen,
                              char *rat,     size_t rlen,
                              int *roaming)
{
    carrier[0] = '\0';
    strncpy(rat, "?", rlen); rat[rlen-1] = '\0';
    *roaming = 0;

    char buf[512];
    if (run_cmd("uqmi -d /dev/cdc-wdm0 --get-serving-system 2>/dev/null", buf, sizeof(buf)) != 0)
        return;

    /* parse plmn_description */
    char *p = strstr(buf, "plmn_description");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++; while (*p == ' ' || *p == '"') p++;
            char *e = strchr(p, '"');
            if (e) { int n = e - p; if (n >= (int)clen) n = clen-1;
                     memcpy(carrier, p, n); carrier[n] = '\0'; }
        }
    }

    /* parse radio_interface - look for 5gnr, lte, umts, etc */
    p = strstr(buf, "radio_interface");
    if (p) {
        if (strstr(p, "5gnr"))      strncpy(rat, "5G", rlen);
        else if (strstr(p, "lte"))  strncpy(rat, "4G", rlen);
        else if (strstr(p, "umts")) strncpy(rat, "3G", rlen);
        else if (strstr(p, "gsm"))  strncpy(rat, "2G", rlen);
        rat[rlen-1] = '\0';
    }

    /* parse roaming */
    p = strstr(buf, "roaming");
    if (p && strstr(p, "true")) *roaming = 1;
}

/* Get signal info: rsrp (dBm), snr (dB*10), rsrq (dB) */
static void get_cellular_signal(int *rsrp, int *snr10, int *rsrq)
{
    *rsrp = 0; *snr10 = 0; *rsrq = 0;

    char buf[256];
    if (run_cmd("uqmi -d /dev/cdc-wdm0 --get-signal-info 2>/dev/null", buf, sizeof(buf)) != 0)
        return;

    char *p;
    p = strstr(buf, "rsrp");
    if (p) { p = strchr(p, ':'); if (p) *rsrp = atoi(p+1); }
    p = strstr(buf, "snr");
    if (p) { p = strchr(p, ':'); if (p) *snr10 = (int)(atof(p+1) * 10); }
    p = strstr(buf, "rsrq");
    if (p) { p = strchr(p, ':'); if (p) *rsrq = atoi(p+1); }
}

/* Signal quality 0-4 from RSRP (dBm) */
static int signal_bars(int rsrp)
{
    if (rsrp >= -80) return 4;
    if (rsrp >= -90) return 3;
    if (rsrp >= -100) return 2;
    if (rsrp >= -110) return 1;
    return 0;
}

/* Draw signal strength bars icon (cellular style) 11w x 9h */
static void draw_signal_icon(int x, int y, int bars, uint16_t col)
{
    uint16_t dim = COL_DIM;
    fb_rect(x,   y+7, 2, 2, (bars >= 1) ? col : dim);
    fb_rect(x+3, y+5, 2, 4, (bars >= 2) ? col : dim);
    fb_rect(x+6, y+3, 2, 6, (bars >= 3) ? col : dim);
    fb_rect(x+9, y+1, 2, 8, (bars >= 4) ? col : dim);
}

/* Ping latency in ms (-1 on failure).  Non-blocking: runs in background,
   we cache the last result and only re-ping every N seconds. */
static int get_ping_ms(void)
{
    static int last_ms = -1;
    static time_t last_ping = 0;
    time_t now = time(NULL);

    if (now - last_ping < 10)   /* re-ping every 10s */
        return last_ms;
    last_ping = now;

    char buf[256];
    if (run_cmd("ping -c1 -W2 1.1.1.1 2>/dev/null | tail -1", buf, sizeof(buf)) != 0) {
        last_ms = -1;
        return last_ms;
    }
    /* parse "round-trip min/avg/max = 34.696/34.696/34.696 ms" */
    char *p = strstr(buf, "= ");
    if (p) {
        p += 2;
        /* skip min, find avg after first / */
        char *sl = strchr(p, '/');
        if (sl) last_ms = (int)atof(sl + 1);
        else last_ms = (int)atof(p);
    } else {
        last_ms = -1;
    }
    return last_ms;
}

/* Get phone number (MSISDN) from modem, cached */
static void get_phone_number(char *buf, size_t len)
{
    static char cached[20] = "";
    static int tried = 0;

    if (!tried) {
        tried = 1;
        char raw[64];
        if (run_cmd("uqmi -d /dev/cdc-wdm0 --get-msisdn 2>/dev/null", raw, sizeof(raw)) == 0) {
            /* strip quotes: "18129879270" -> 18129879270 */
            char *p = raw;
            if (*p == '"') p++;
            char *e = strchr(p, '"');
            if (e) *e = '\0';
            /* format as (812) 987-9270 if 10+ digits */
            size_t sl = strlen(p);
            if (sl >= 10) {
                char *d = p + sl - 10; /* last 10 digits */
                snprintf(cached, sizeof(cached), "(%c%c%c)%c%c%c-%c%c%c%c",
                         d[0],d[1],d[2], d[3],d[4],d[5], d[6],d[7],d[8],d[9]);
            } else {
                strncpy(cached, p, sizeof(cached)-1);
            }
        }
    }
    strncpy(buf, cached[0] ? cached : "N/A", len);
    buf[len-1] = '\0';
}

/* Get SMS count from modem (uqmi --list-messages).  Cached, refreshed every 30s */
static int get_sms_count(void)
{
    static int count = 0;
    static time_t last_check = 0;
    time_t now = time(NULL);

    if (now - last_check < 30)
        return count;
    last_check = now;

    char buf[1024];
    count = 0;
    if (run_cmd("uqmi -d /dev/cdc-wdm0 --list-messages --storage me 2>/dev/null",
                buf, sizeof(buf)) == 0) {
        /* count "id" occurrences in JSON array */
        char *p = buf;
        while ((p = strstr(p, "\"id\"")) != NULL) { count++; p += 4; }
    }
    return count;
}

/* ================================================================== */
/*  Section heading helper                                             */
/* ================================================================== */

static int section_header(int y, const char *title)
{
    float s = cfg.font_scale;
    draw_str(4, y, title, COL_ACCENT, COL_BG, s);
    fb_hline(4, y + CHAR_H(s) + 2, DISP_W - 8, COL_DIM);
    return y + CHAR_H(s) + 4;
}

/* pick colour by temperature */
static uint16_t temp_color(int t)
{
    if (t < 45) return COL_GOOD;
    if (t < 65) return COL_FG;
    if (t < 80) return COL_WARN;
    return COL_BAD;
}

/* ================================================================== */
/*  Renderable pages                                                   */
/* ================================================================== */

/* Returns: next Y position after drawing */

static int render_clock(int y)
{
    char tmp[64];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    float s = cfg.font_scale;

    /* time in large font */
    snprintf(tmp, sizeof(tmp), "%02d:%02d", t->tm_hour, t->tm_min);
    draw_str_c(y, tmp, COL_FG, COL_BG, 2.0f * s);
    y += CHAR_H(2.0f * s) + 2;

    /* date */
    static const char *wday[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *mon[]  = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(tmp, sizeof(tmp), "%s %s %02d, %d",
             wday[t->tm_wday], mon[t->tm_mon], t->tm_mday, t->tm_year + 1900);
    draw_str_c(y, tmp, COL_DIM, COL_BG, s);
    y += CHAR_H(s) + 4;

    return y;
}

static int render_battery(int y)
{
    char tmp[64];
    float s = cfg.font_scale;

    y = section_header(y, "BATTERY");

    char status[32];
    get_battery_status(status, sizeof(status));
    uint16_t scol = COL_FG;
    if (strstr(status, "Charging") || strstr(status, "Full")) scol = COL_GOOD;
    else if (strstr(status, "Not")) scol = COL_WARN;

    int bpct = get_battery_pct();
    if (bpct >= 0) {
        snprintf(tmp, sizeof(tmp), "%s %d%%", status, bpct);
        uint16_t pcol = (bpct > 20) ? scol : ((bpct > 10) ? COL_WARN : COL_BAD);
        draw_str(8, y, tmp, pcol, COL_BG, s);
    } else {
        draw_str(8, y, status, scol, COL_BG, s);
    }
    y += CHAR_H(s) + 2;

    int mv = get_battery_voltage_mv();
    int ma = get_battery_current_ma();
    if (mv > 0 && ma >= 0)
        snprintf(tmp, sizeof(tmp), "%d.%02dV  %dmA", mv/1000, (mv%1000)/10, ma);
    else if (mv > 0)
        snprintf(tmp, sizeof(tmp), "%d.%02dV", mv/1000, (mv%1000)/10);
    else
        strncpy(tmp, "N/A", sizeof(tmp));
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 4;

    return y;
}

static int render_network(int y)
{
    char tmp[128];
    float s = cfg.font_scale;

    y = section_header(y, "NETWORK");

    char wan_if[32] = "", wan_ip[64] = "N/A";
    get_wan_info(wan_if, sizeof(wan_if), wan_ip, sizeof(wan_ip));
    if (wan_if[0]) {
        char opstate[16] = "?";
        snprintf(tmp, sizeof(tmp), "/sys/class/net/%s/operstate", wan_if);
        read_sysfs(tmp, opstate, sizeof(opstate));
        int is_up = (strcmp(opstate, "up") == 0 || strcmp(opstate, "unknown") == 0);
        snprintf(tmp, sizeof(tmp), "%s %s", wan_if, is_up ? "UP" : "DOWN");
        draw_str(8, y, tmp, is_up ? COL_GOOD : COL_BAD, COL_BG, s);
    } else {
        draw_str(8, y, "No default route", COL_BAD, COL_BG, s);
    }
    y += CHAR_H(s) + 2;

    snprintf(tmp, sizeof(tmp), "IP: %s", wan_ip);
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 4;

    return y;
}

static int render_wifi(int y)
{
    char tmp[128];
    float s = cfg.font_scale;

    y = section_header(y, "WIFI");

    char ssid[64];
    get_wifi_ssid(ssid, sizeof(ssid));
    draw_str(8, y, ssid, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 2;

    char bandchan[32];
    get_wifi_band_chan(bandchan, sizeof(bandchan));
    int clients = get_wifi_clients();
    snprintf(tmp, sizeof(tmp), "%s  %dSTA", bandchan, clients);
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 4;

    return y;
}

static int render_thermal(int y)
{
    char tmp[64];
    float s = cfg.font_scale;

    y = section_header(y, "THERMAL");

    /* Fan info */
    int rpm = get_fan_rpm();
    int level = get_fan_level();
    if (rpm >= 0 && level >= 0) {
        snprintf(tmp, sizeof(tmp), "Fan: %dRPM (L%d)", rpm, level);
    } else if (rpm >= 0) {
        snprintf(tmp, sizeof(tmp), "Fan: %d RPM", rpm);
    } else {
        snprintf(tmp, sizeof(tmp), "Fan: off");
    }
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 2;

    /* Board temp (MCU) */
    int bt = get_board_temp();
    if (bt >= 0) {
        snprintf(tmp, sizeof(tmp), "Board: %dC", bt);
        draw_str(8, y, tmp, temp_color(bt), COL_BG, s);
        y += CHAR_H(s) + 2;
    }

    /* All thermal zones in compact 2-column grid */
    static const char *zone_names[] = {
        "Pkg", "Big", "Ltl", "GPU", "NPU", "DDR"
    };
    for (int i = 0; i < 6; i += 2) {
        char path[64];
        int t1 = -1, t2 = -1;

        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        t1 = read_sysfs_int(path);
        if (t1 > 0) t1 /= 1000;

        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i+1);
        t2 = read_sysfs_int(path);
        if (t2 > 0) t2 /= 1000;

        char col1[16], col2[16];
        if (t1 >= 0)
            snprintf(col1, sizeof(col1), "%s:%dC", zone_names[i], t1);
        else
            snprintf(col1, sizeof(col1), "%s:--", zone_names[i]);

        if (i + 1 < 6 && t2 >= 0)
            snprintf(col2, sizeof(col2), "%s:%dC", zone_names[i+1], t2);
        else if (i + 1 < 6)
            snprintf(col2, sizeof(col2), "%s:--", zone_names[i+1]);
        else
            col2[0] = '\0';

        draw_str(8, y, col1, (t1 >= 0) ? temp_color(t1) : COL_DIM, COL_BG, s);
        if (col2[0])
            draw_str(8 + DISP_W / 2, y, col2, (t2 >= 0) ? temp_color(t2) : COL_DIM, COL_BG, s);
        y += CHAR_H(s) + 1;
    }
    y += 3;

    return y;
}

static int render_system(int y)
{
    char tmp[64];
    float s = cfg.font_scale;

    y = section_header(y, "SYSTEM");

    int cpu_c = get_cpu_temp();
    int mem_used, mem_total;
    get_memory(&mem_used, &mem_total);
    snprintf(tmp, sizeof(tmp), "CPU %dC  Mem %d/%dM", cpu_c, mem_used, mem_total);
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 2;

    char upstr[32];
    get_uptime_str(upstr, sizeof(upstr));
    snprintf(tmp, sizeof(tmp), "Up: %s", upstr);
    draw_str(8, y, tmp, COL_FG, COL_BG, s);
    y += CHAR_H(s) + 4;

    return y;
}

static int render_custom(int y)
{
    float s = cfg.font_scale;

    if (cfg.num_custom == 0) return y;

    y = section_header(y, "CUSTOM");

    for (int i = 0; i < cfg.num_custom && y < DISP_H - 10; i++) {
        struct custom_param *p = &cfg.custom[i];
        char val[128];

        if (strncmp(p->source, "cmd:", 4) == 0) {
            run_cmd(p->source + 4, val, sizeof(val));
        } else {
            /* sysfs read */
            if (read_sysfs(p->source, val, sizeof(val)) < 0)
                strncpy(val, "N/A", sizeof(val));
            else if (p->divide > 0) {
                int v = atoi(val);
                if (p->divide == 1000)
                    snprintf(val, sizeof(val), "%d.%03d", v / 1000, abs(v % 1000));
                else
                    snprintf(val, sizeof(val), "%d", v / p->divide);
            }
        }

        char line[128];
        if (p->unit[0])
            snprintf(line, sizeof(line), "%s: %s%s", p->label, val, p->unit);
        else
            snprintf(line, sizeof(line), "%s: %s", p->label, val);

        draw_str(8, y, line, COL_FG, COL_BG, s);
        y += CHAR_H(s) + 2;
    }
    y += 2;

    return y;
}

/* ================================================================== */
/*  Full-screen page renderers (one page per screen, button cycles)    */
/* ================================================================== */

static void render_fullpage_clock(void)
{
    char tmp[64];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int y;

    /* Big time HH:MM centred */
    y = 30;
    snprintf(tmp, sizeof(tmp), "%02d:%02d", t->tm_hour, t->tm_min);
    draw_str_c(y, tmp, COL_FG, COL_BG, 4.0f);
    y += CHAR_H(4.0f) + 8;

    /* Accent separator */
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 10;

    /* Weekday */
    static const char *wdays[] = {"Sunday","Monday","Tuesday","Wednesday",
                                   "Thursday","Friday","Saturday"};
    draw_str_c(y, wdays[t->tm_wday], COL_FG, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;

    /* Date */
    static const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(tmp, sizeof(tmp), "%s %d, %d",
             mons[t->tm_mon], t->tm_mday, t->tm_year + 1900);
    draw_str_c(y, tmp, COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 16;

    /* Dim separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 12;

    /* Uptime */
    char upstr[32];
    get_uptime_str(upstr, sizeof(upstr));
    snprintf(tmp, sizeof(tmp), "Up: %s", upstr);
    draw_str_c(y, tmp, COL_DIM, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 8;

    /* Battery summary */
    int bpct = get_battery_pct();
    if (bpct >= 0) {
        char bstat[32];
        get_battery_status(bstat, sizeof(bstat));
        int chg = (strstr(bstat, "Charging") || strstr(bstat, "Full"));
        uint16_t bc = (bpct > 25) ? COL_GOOD : (bpct > 10 ? COL_WARN : COL_BAD);
        snprintf(tmp, sizeof(tmp), "%d%% %s", bpct, chg ? "CHG" : "BAT");
        draw_str_c(y, tmp, bc, COL_BG, 2.0f);
    }
}

static void render_fullpage_cellular(void)
{
    char tmp[128];
    int y = 8;

    /* Header */
    draw_str_c(y, "CELLULAR", COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 8;

    char carrier[32], rat[8];
    int roaming = 0;
    get_cellular_info(carrier, sizeof(carrier), rat, sizeof(rat), &roaming);
    int rsrp = 0, snr10 = 0, rsrq = 0;
    get_cellular_signal(&rsrp, &snr10, &rsrq);
    int bars = signal_bars(rsrp);

    /* No modem fallback */
    if (!carrier[0] && strcmp(rat, "?") == 0) {
        y += 40;
        draw_str_c(y, "No modem", COL_DIM, COL_BG, 2.0f);
        y += CHAR_H(2.0f) + 8;
        draw_str_c(y, "detected", COL_DIM, COL_BG, 2.0f);
        return;
    }

    /* Carrier name */
    if (carrier[0]) {
        if (strlen(carrier) > 14) carrier[14] = '\0';
        draw_str_c(y, carrier, COL_FG, COL_BG, 2.0f);
    } else {
        draw_str_c(y, "Searching..", COL_DIM, COL_BG, 2.0f);
    }
    y += CHAR_H(2.0f) + 6;

    /* Roaming indicator */
    if (roaming)
        draw_str_r(DISP_W - 4, 8, "R", COL_WARN, COL_BG, 2.0f);

    /* RAT badge - large */
    {
        uint16_t rc = COL_ACCENT;
        if (strcmp(rat, "5G") == 0) rc = COL_GOOD;
        else if (strcmp(rat, "3G") == 0 || strcmp(rat, "2G") == 0) rc = COL_WARN;
        draw_str_c(y, rat, rc, COL_BG, 3.0f);
        y += CHAR_H(3.0f) + 2;
    }

    /* Signal bars - large custom drawn */
    {
        int bw = 8, gap = 4, total_w = 4 * bw + 3 * gap;
        int sx = (DISP_W - total_w) / 2;
        int bar_base = y + 26;
        int heights[] = {8, 14, 20, 26};
        for (int i = 0; i < 4; i++) {
            uint16_t col = (bars > i) ? COL_ACCENT : COL_DIM;
            fb_rect(sx + i * (bw + gap), bar_base - heights[i],
                    bw, heights[i], col);
        }
        y = bar_base + 8;
    }

    /* Separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 8;

    /* RSRP */
    {
        uint16_t rc = (rsrp >= -90) ? COL_GOOD :
                      (rsrp >= -110) ? COL_WARN : COL_BAD;
        snprintf(tmp, sizeof(tmp), "%d dBm", rsrp);
        draw_str_c(y, tmp, rc, COL_BG, 2.0f);
        y += CHAR_H(2.0f) + 4;
    }

    /* SNR */
    if (snr10 != 0) {
        snprintf(tmp, sizeof(tmp), "SNR %d.%d dB", snr10 / 10, abs(snr10 % 10));
        draw_str_c(y, tmp, COL_DIM, COL_BG, 1.5f);
        y += CHAR_H(1.5f) + 4;
    }

    /* Ping */
    {
        int ping = get_ping_ms();
        if (ping >= 0) {
            uint16_t pc = (ping < 50) ? COL_GOOD :
                          (ping < 100) ? COL_WARN : COL_BAD;
            snprintf(tmp, sizeof(tmp), "%d ms", ping);
            draw_str_c(y, tmp, pc, COL_BG, 2.0f);
        } else {
            draw_str_c(y, "--- ms", COL_BAD, COL_BG, 2.0f);
        }
        y += CHAR_H(2.0f) + 8;
    }

    /* Separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 8;

    /* Phone number */
    {
        char phone[20];
        get_phone_number(phone, sizeof(phone));
        draw_str_c(y, phone, COL_FG, COL_BG, 1.5f);
        y += CHAR_H(1.5f) + 4;
    }

    /* SMS count */
    {
        int sms = get_sms_count();
        snprintf(tmp, sizeof(tmp), "%d SMS", sms);
        draw_str_c(y, tmp, (sms > 0) ? COL_WARN : COL_DIM, COL_BG, 1.5f);
    }
}

static void render_fullpage_battery(void)
{
    char tmp[64];
    int y = 8;

    /* Header */
    draw_str_c(y, "BATTERY", COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 12;

    int bpct = get_battery_pct();
    char bstat[32];
    get_battery_status(bstat, sizeof(bstat));

    /* Big percentage */
    if (bpct >= 0) {
        uint16_t bcol = (bpct > 25) ? COL_GOOD :
                        (bpct > 10 ? COL_WARN : COL_BAD);
        snprintf(tmp, sizeof(tmp), "%d%%", bpct);
        draw_str_c(y, tmp, bcol, COL_BG, 4.0f);
        y += CHAR_H(4.0f) + 6;

        /* Wide progress bar */
        draw_progress_bar(10, y, DISP_W - 20, 14, bpct, bcol, COL_DIM);
        y += 22;
    } else {
        draw_str_c(y, "N/A", COL_DIM, COL_BG, 3.0f);
        y += CHAR_H(3.0f) + 6;
    }

    /* Status */
    {
        const char *ss = "Unknown";
        uint16_t sc = COL_DIM;
        if (strstr(bstat, "Charging"))    { ss = "Charging";     sc = COL_GOOD; }
        else if (strstr(bstat, "Full"))   { ss = "Full";         sc = COL_GOOD; }
        else if (strstr(bstat, "Dischar")){ ss = "Discharging";  sc = COL_WARN; }
        else if (strstr(bstat, "Not"))    { ss = "Not Charging"; sc = COL_DIM;  }
        draw_str_c(y, ss, sc, COL_BG, 2.0f);
        y += CHAR_H(2.0f) + 10;
    }

    /* Separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 10;

    /* Voltage */
    int mv = get_battery_voltage_mv();
    if (mv > 0) {
        snprintf(tmp, sizeof(tmp), "%d.%02d V", mv / 1000, (mv % 1000) / 10);
        draw_str_c(y, tmp, COL_FG, COL_BG, 2.0f);
        y += CHAR_H(2.0f) + 6;
    }

    /* Current */
    int ma = get_battery_current_ma();
    if (ma >= 0) {
        snprintf(tmp, sizeof(tmp), "%d mA", ma);
        draw_str_c(y, tmp, COL_FG, COL_BG, 2.0f);
    }
}

static void render_fullpage_network(void)
{
    char tmp[128];
    int y = 8;

    /* WAN header */
    draw_str_c(y, "NETWORK", COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 8;

    char wan_if[32] = "", wan_ip[64] = "N/A";
    get_wan_info(wan_if, sizeof(wan_if), wan_ip, sizeof(wan_ip));
    int is_up = 0;
    if (wan_if[0]) {
        char opstate[16] = "?";
        snprintf(tmp, sizeof(tmp), "/sys/class/net/%s/operstate", wan_if);
        read_sysfs(tmp, opstate, sizeof(opstate));
        is_up = (strcmp(opstate, "up") == 0 || strcmp(opstate, "unknown") == 0);
    }

    /* WAN status */
    draw_dot(12, y + 7, is_up ? COL_GOOD : COL_BAD);
    snprintf(tmp, sizeof(tmp), "WAN %s", is_up ? "UP" : "DOWN");
    draw_str(22, y, tmp, is_up ? COL_GOOD : COL_BAD, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;

    /* Interface name */
    if (wan_if[0]) {
        draw_str(8, y, wan_if, COL_DIM, COL_BG, 1.5f);
        y += CHAR_H(1.5f) + 4;
    }

    /* IP address */
    draw_str_c(y, wan_ip, COL_FG, COL_BG, 1.5f);
    y += CHAR_H(1.5f) + 12;

    /* WiFi separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 10;

    /* WiFi section */
    draw_str_c(y, "WIFI", COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 8;

    /* SSID */
    char ssid[64];
    get_wifi_ssid(ssid, sizeof(ssid));
    int max_ch = DISP_W / CHAR_W(2.0f);
    if ((int)strlen(ssid) > max_ch) ssid[max_ch] = '\0';
    draw_str_c(y, ssid, COL_FG, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;

    /* Band + Channel */
    char bandchan[32];
    get_wifi_band_chan(bandchan, sizeof(bandchan));
    draw_str_c(y, bandchan, COL_DIM, COL_BG, 1.5f);
    y += CHAR_H(1.5f) + 6;

    /* Clients */
    int clients = get_wifi_clients();
    snprintf(tmp, sizeof(tmp), "%d clients", clients);
    draw_str_c(y, tmp, (clients > 0) ? COL_GOOD : COL_DIM, COL_BG, 2.0f);
}

static void render_fullpage_system(void)
{
    char tmp[64];
    int y = 8;

    /* Header */
    draw_str_c(y, "SYSTEM", COL_ACCENT, COL_BG, 2.0f);
    y += CHAR_H(2.0f) + 4;
    fb_hline(10, y, DISP_W - 20, COL_ACCENT);
    y += 8;

    /* CPU bar */
    int cpu_pct = get_cpu_usage();
    if (cpu_pct < 0) cpu_pct = 0;
    draw_str(8, y, "CPU", COL_ACCENT, COL_BG, 1.5f);
    snprintf(tmp, sizeof(tmp), "%d%%", cpu_pct);
    draw_str_r(DISP_W - 8, y, tmp, COL_FG, COL_BG, 1.5f);
    y += CHAR_H(1.5f) + 2;
    {
        uint16_t bc = (cpu_pct < 60) ? COL_GOOD :
                      (cpu_pct < 85) ? COL_WARN : COL_BAD;
        draw_progress_bar(8, y, DISP_W - 16, 12, cpu_pct, bc, COL_DIM);
    }
    y += 18;

    /* MEM bar */
    int mem_used, mem_total;
    get_memory(&mem_used, &mem_total);
    int mem_pct = (mem_total > 0) ? (100 * mem_used / mem_total) : 0;
    draw_str(8, y, "MEM", COL_ACCENT, COL_BG, 1.5f);
    snprintf(tmp, sizeof(tmp), "%d%%", mem_pct);
    draw_str_r(DISP_W - 8, y, tmp, COL_FG, COL_BG, 1.5f);
    y += CHAR_H(1.5f) + 2;
    {
        uint16_t mc = (mem_pct < 70) ? COL_GOOD :
                      (mem_pct < 90) ? COL_WARN : COL_BAD;
        draw_progress_bar(8, y, DISP_W - 16, 12, mem_pct, mc, COL_DIM);
    }
    y += 18;

    /* DSK bar */
    int dsk_pct = get_disk_usage_pct();
    if (dsk_pct >= 0) {
        draw_str(8, y, "DSK", COL_ACCENT, COL_BG, 1.5f);
        snprintf(tmp, sizeof(tmp), "%d%%", dsk_pct);
        draw_str_r(DISP_W - 8, y, tmp, COL_FG, COL_BG, 1.5f);
        y += CHAR_H(1.5f) + 2;
        {
            uint16_t dc = (dsk_pct < 70) ? COL_GOOD :
                          (dsk_pct < 90) ? COL_WARN : COL_BAD;
            draw_progress_bar(8, y, DISP_W - 16, 12, dsk_pct, dc, COL_DIM);
        }
        y += 18;
    }

    /* Memory detail */
    snprintf(tmp, sizeof(tmp), "%d / %d MB", mem_used, mem_total);
    draw_str_c(y, tmp, COL_DIM, COL_BG, 1.0f);
    y += CHAR_H(1.0f) + 6;

    /* Separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 8;

    /* Temperatures */
    int cpu_c = get_cpu_temp();
    int board_c = get_board_temp();

    draw_thermo_icon(8, y, COL_ACCENT);
    if (cpu_c > 0) {
        snprintf(tmp, sizeof(tmp), "CPU %dC", cpu_c);
        draw_str(20, y, tmp, temp_color(cpu_c), COL_BG, 2.0f);
    }
    y += CHAR_H(2.0f) + 4;

    if (board_c >= 0) {
        snprintf(tmp, sizeof(tmp), "Board %dC", board_c);
        draw_str(20, y, tmp, temp_color(board_c), COL_BG, 1.5f);
        y += CHAR_H(1.5f) + 6;
    }

    /* Fan */
    int rpm = get_fan_rpm();
    int level = get_fan_level();
    draw_fan_icon(8, y, COL_ACCENT);
    if (rpm >= 0) {
        snprintf(tmp, sizeof(tmp), "%d RPM", rpm);
        draw_str(20, y, tmp, COL_FG, COL_BG, 1.5f);
        if (level >= 0) {
            snprintf(tmp, sizeof(tmp), "L%d", level);
            draw_str_r(DISP_W - 8, y, tmp, COL_ACCENT, COL_BG, 1.5f);
        }
    } else {
        draw_str(20, y, "Fan OFF", COL_DIM, COL_BG, 1.5f);
    }
    y += CHAR_H(1.5f) + 8;

    /* Separator */
    fb_hline(10, y, DISP_W - 20, COL_DIM);
    y += 8;

    /* Uptime */
    char upstr[32];
    get_uptime_str(upstr, sizeof(upstr));
    snprintf(tmp, sizeof(tmp), "Up: %s", upstr);
    draw_str_c(y, tmp, COL_DIM, COL_BG, 1.5f);
}

/* ================================================================== */
/*  Dashboard - single-screen unified status view                      */
/* ================================================================== */

static void render_dashboard(void)
{
    char tmp[128];
    int y;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* ── TIME ─────────────────────────────────────────────── */
    y = 2;
    snprintf(tmp, sizeof(tmp), "%02d:%02d", t->tm_hour, t->tm_min);
    draw_str_c(y, tmp, COL_FG, COL_BG, 3.5f);
    y += CHAR_H(3.5f) + 1;

    /* date */
    static const char *wday[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *mon[]  = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(tmp, sizeof(tmp), "%s %s %d, %d",
             wday[t->tm_wday], mon[t->tm_mon], t->tm_mday, t->tm_year + 1900);
    draw_str_c(y, tmp, COL_DIM, COL_BG, 1.2f);
    y += CHAR_H(1.2f) + 3;

    /* accent double separator */
    fb_hline(4, y,   DISP_W - 8, COL_ACCENT);
    fb_hline(4, y+2, DISP_W - 8, COL_ACCENT);
    y += 6;

    /* ── CELLULAR ─────────────────────────────────────────── */
    {
        char carrier[32], rat[8];
        int roaming = 0;
        get_cellular_info(carrier, sizeof(carrier), rat, sizeof(rat), &roaming);

        int rsrp = 0, snr10 = 0, rsrq = 0;
        get_cellular_signal(&rsrp, &snr10, &rsrq);
        int bars = signal_bars(rsrp);

        /* Row 1: signal bars + carrier + RAT badge */
        draw_signal_icon(4, y, bars, COL_ACCENT);
        int cx = 18;
        if (carrier[0]) {
            /* truncate carrier to fit */
            if (strlen(carrier) > 12) carrier[12] = '\0';
            cx = draw_str(cx, y, carrier, COL_FG, COL_BG, 1.3f);
            cx += 4;
        }
        /* RAT badge (5G / 4G / 3G) */
        {
            uint16_t rc = COL_ACCENT;
            if (strcmp(rat, "5G") == 0) rc = COL_GOOD;
            else if (strcmp(rat, "4G") == 0) rc = COL_ACCENT;
            else rc = COL_WARN;
            draw_str(cx, y, rat, rc, COL_BG, 1.3f);
        }
        if (roaming)
            draw_str_r(DISP_W - 4, y, "R", COL_WARN, COL_BG, 1.0f);
        y += CHAR_H(1.3f) + 2;

        /* Row 2: RSRP + SNR + ping */
        {
            int ping = get_ping_ms();
            snprintf(tmp, sizeof(tmp), "%ddBm", rsrp);
            draw_str(6, y, tmp, COL_DIM, COL_BG, 1.0f);

            if (ping >= 0) {
                uint16_t pc = (ping < 50) ? COL_GOOD :
                              (ping < 100) ? COL_WARN : COL_BAD;
                snprintf(tmp, sizeof(tmp), "%dms", ping);
                draw_str_r(DISP_W - 4, y, tmp, pc, COL_BG, 1.0f);
            } else {
                draw_str_r(DISP_W - 4, y, "---", COL_BAD, COL_BG, 1.0f);
            }
        }
        y += CHAR_H(1.0f) + 1;

        /* Row 3: phone number + SMS count */
        {
            char phone[20];
            get_phone_number(phone, sizeof(phone));
            draw_str(6, y, phone, COL_FG, COL_BG, 1.0f);

            int sms = get_sms_count();
            if (sms > 0) {
                snprintf(tmp, sizeof(tmp), "%dSMS", sms);
                draw_str_r(DISP_W - 4, y, tmp, COL_WARN, COL_BG, 1.0f);
            } else {
                draw_str_r(DISP_W - 4, y, "0SMS", COL_DIM, COL_BG, 1.0f);
            }
        }
        y += CHAR_H(1.0f) + 3;
    }

    /* dim separator */
    fb_hline(4, y, DISP_W - 8, COL_DIM);
    y += 5;

    /* ── BATTERY ──────────────────────────────────────────── */
    {
        int bpct = get_battery_pct();
        char bstat[32];
        get_battery_status(bstat, sizeof(bstat));
        int charging = (strstr(bstat, "Charging") || strstr(bstat, "Full"));

        int ix = 4;
        if (bpct >= 0) {
            draw_battery_icon(ix, y + 1, bpct, charging);
            ix += 20;
        }

        /* percentage - big */
        if (bpct >= 0) {
            uint16_t bcol = (bpct > 25) ? COL_GOOD : (bpct > 10 ? COL_WARN : COL_BAD);
            snprintf(tmp, sizeof(tmp), "%d%%", bpct);
            ix = draw_str(ix, y, tmp, bcol, COL_BG, 1.5f);
            ix += 4;
        }

        /* status */
        {
            const char *ss = "N/A";
            if (strstr(bstat, "Charging"))    ss = "CHG";
            else if (strstr(bstat, "Full"))   ss = "FULL";
            else if (strstr(bstat, "Dischar"))ss = "BAT";
            else if (strstr(bstat, "Not"))    ss = "IDLE";
            uint16_t sc = charging ? COL_GOOD : COL_DIM;
            draw_str(ix, y + 2, ss, sc, COL_BG, 1.0f);
        }

        /* voltage right-aligned */
        {
            int mv = get_battery_voltage_mv();
            if (mv > 0) {
                snprintf(tmp, sizeof(tmp), "%d.%02dV", mv/1000, (mv%1000)/10);
                draw_str_r(DISP_W - 4, y + 2, tmp, COL_DIM, COL_BG, 1.0f);
            }
        }
        y += CHAR_H(1.5f) + 3;
    }

    /* dim separator */
    fb_hline(4, y, DISP_W - 8, COL_DIM);
    y += 5;

    /* ── NETWORK ──────────────────────────────────────────── */
    {
        char wan_if[32] = "", wan_ip[64] = "N/A";
        get_wan_info(wan_if, sizeof(wan_if), wan_ip, sizeof(wan_ip));

        int is_up = 0;
        if (wan_if[0]) {
            char opstate[16] = "?";
            snprintf(tmp, sizeof(tmp), "/sys/class/net/%s/operstate", wan_if);
            read_sysfs(tmp, opstate, sizeof(opstate));
            is_up = (strcmp(opstate, "up") == 0 || strcmp(opstate, "unknown") == 0);
        }

        /* WAN: dot + iface + IP */
        draw_dot(8, y + 4, is_up ? COL_GOOD : COL_BAD);
        int nx = 15;
        nx = draw_str(nx, y, "WAN", COL_ACCENT, COL_BG, 1.2f);
        nx += 4;
        draw_str(nx, y + 1, wan_ip, COL_FG, COL_BG, 1.0f);
        y += CHAR_H(1.2f) + 2;

        /* WiFi: icon + SSID + clients */
        char ssid[64];
        get_wifi_ssid(ssid, sizeof(ssid));
        int clients = get_wifi_clients();

        draw_wifi_icon(4, y, clients, COL_ACCENT);
        int wx = 18;
        if (strlen(ssid) > 14) ssid[14] = '\0';
        wx = draw_str(wx, y, ssid, COL_FG, COL_BG, 1.2f);

        /* client count right-aligned */
        snprintf(tmp, sizeof(tmp), "%dSTA", clients);
        draw_str_r(DISP_W - 4, y + 1, tmp, COL_ACCENT, COL_BG, 1.0f);
        y += CHAR_H(1.2f) + 3;
    }

    /* dim separator */
    fb_hline(4, y, DISP_W - 8, COL_DIM);
    y += 5;

    /* ── PERFORMANCE ──────────────────────────────────────── */
    {
        int cpu_pct = get_cpu_usage();
        if (cpu_pct < 0) cpu_pct = 0;
        int mem_used, mem_total;
        get_memory(&mem_used, &mem_total);
        int mem_pct = (mem_total > 0) ? (100 * mem_used / mem_total) : 0;
        int dsk_pct = get_disk_usage_pct();

        /* CPU bar */
        draw_str(4, y, "CPU", COL_ACCENT, COL_BG, 1.0f);
        {
            uint16_t bc = (cpu_pct < 60) ? COL_GOOD :
                          (cpu_pct < 85) ? COL_WARN : COL_BAD;
            draw_progress_bar(30, y, 100, 8, cpu_pct, bc, COL_DIM);
        }
        snprintf(tmp, sizeof(tmp), "%d%%", cpu_pct);
        draw_str(134, y, tmp, COL_FG, COL_BG, 1.0f);
        y += 12;

        /* MEM bar */
        draw_str(4, y, "MEM", COL_ACCENT, COL_BG, 1.0f);
        {
            uint16_t mc = (mem_pct < 70) ? COL_GOOD :
                          (mem_pct < 90) ? COL_WARN : COL_BAD;
            draw_progress_bar(30, y, 100, 8, mem_pct, mc, COL_DIM);
        }
        snprintf(tmp, sizeof(tmp), "%d%%", mem_pct);
        draw_str(134, y, tmp, COL_FG, COL_BG, 1.0f);
        y += 12;

        /* DSK bar */
        if (dsk_pct >= 0) {
            draw_str(4, y, "DSK", COL_ACCENT, COL_BG, 1.0f);
            {
                uint16_t dc = (dsk_pct < 70) ? COL_GOOD :
                              (dsk_pct < 90) ? COL_WARN : COL_BAD;
                draw_progress_bar(30, y, 100, 8, dsk_pct, dc, COL_DIM);
            }
            snprintf(tmp, sizeof(tmp), "%d%%", dsk_pct);
            draw_str(134, y, tmp, COL_FG, COL_BG, 1.0f);
            y += 12;
        }
    }
    y += 2;

    /* dim separator */
    fb_hline(4, y, DISP_W - 8, COL_DIM);
    y += 5;

    /* ── TEMP + FAN ───────────────────────────────────────── */
    {
        /* CPU temp */
        int cpu_c = get_cpu_temp();
        draw_thermo_icon(4, y, COL_ACCENT);
        if (cpu_c > 0) {
            snprintf(tmp, sizeof(tmp), "%dC", cpu_c);
            draw_str(12, y, tmp, temp_color(cpu_c), COL_BG, 1.3f);
        }

        /* Fan on same line, right side */
        int rpm = get_fan_rpm();
        int level = get_fan_level();
        draw_fan_icon(DISP_W / 2 + 4, y, COL_ACCENT);
        if (rpm >= 0) {
            snprintf(tmp, sizeof(tmp), "%dRPM", rpm);
            draw_str(DISP_W / 2 + 16, y, tmp, COL_FG, COL_BG, 1.0f);
            if (level >= 0) {
                snprintf(tmp, sizeof(tmp), "L%d", level);
                draw_str_r(DISP_W - 4, y, tmp, COL_ACCENT, COL_BG, 1.0f);
            }
        } else {
            draw_str(DISP_W / 2 + 16, y, "OFF", COL_DIM, COL_BG, 1.0f);
        }
        y += CHAR_H(1.3f) + 3;
    }

    /* dim separator */
    fb_hline(4, y, DISP_W - 8, COL_DIM);
    y += 5;

    /* ── UPTIME ───────────────────────────────────────────── */
    {
        char upstr[32];
        get_uptime_str(upstr, sizeof(upstr));
        draw_arrow_up(4, y + 1, COL_DIM);
        snprintf(tmp, sizeof(tmp), "Up %s", upstr);
        draw_str(14, y, tmp, COL_DIM, COL_BG, 1.2f);
    }

    /* ── BOTTOM ACCENT LINE ───────────────────────────────── */
    fb_hline(4, DISP_H - 3, DISP_W - 8, COL_ACCENT);
    fb_hline(4, DISP_H - 1, DISP_W - 8, COL_ACCENT);
}

/* ================================================================== */
/*  Top bar (always shown when no clock page)                          */
/* ================================================================== */

static int render_topbar(void)
{
    char tmp[32];
    float s = cfg.font_scale;
    int bar_h = CHAR_H(2.0f * s) + 2;

    fb_rect(0, 0, DISP_W, bar_h, COL_TOPBAR);

    /* time */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(tmp, sizeof(tmp), "%02d:%02d", t->tm_hour, t->tm_min);
    draw_str(4, (int)(2.0f * s), tmp, COL_FG, COL_TOPBAR, 2.0f * s);

    /* battery % */
    int bpct = get_battery_pct();
    if (bpct >= 0) {
        snprintf(tmp, sizeof(tmp), "%d%%", bpct);
        uint16_t bcol = (bpct > 20) ? COL_GOOD : (bpct > 10 ? COL_WARN : COL_BAD);
        draw_str_r(DISP_W - 4, (int)(2.0f * s), tmp, bcol, COL_TOPBAR, 2.0f * s);
    }

    /* separator lines */
    fb_hline(0, bar_h,     DISP_W, COL_ACCENT);
    fb_hline(0, bar_h + 1, DISP_W, COL_ACCENT);

    return bar_h + 4;
}

/* ================================================================== */
/*  Full render                                                        */
/* ================================================================== */

static void render_screen(void)
{
    fb_clear(COL_BG);

    /* Dashboard mode: if first page is "dashboard", render single-screen HUD */
    if (cfg.num_pages > 0 && strcmp(cfg.pages[0], "dashboard") == 0) {
        render_dashboard();
        return;
    }

    /* Multi-page mode: show one page at a time */
    if (page_idx >= cfg.num_pages) page_idx = 0;

    const char *p = cfg.pages[page_idx];
    if      (strcmp(p, "clock")    == 0) render_fullpage_clock();
    else if (strcmp(p, "cellular") == 0) render_fullpage_cellular();
    else if (strcmp(p, "battery")  == 0) render_fullpage_battery();
    else if (strcmp(p, "network")  == 0) render_fullpage_network();
    else if (strcmp(p, "system")   == 0) render_fullpage_system();
    else if (strcmp(p, "wifi")     == 0) render_fullpage_network();
    else if (strcmp(p, "thermal")  == 0) render_fullpage_system();
    else if (strcmp(p, "custom")   == 0) { int y = 4; render_custom(y); }
    else {
        /* Unknown page - show name */
        draw_str_c(DISP_H / 2, p, COL_DIM, COL_BG, 2.0f);
    }

    /* Page indicator dots */
    draw_page_dots(page_idx, cfg.num_pages);
}

/* ================================================================== */
/*  Signal handlers                                                    */
/* ================================================================== */

static void sig_handler(int sig)
{
    if (sig == SIGHUP) reload_cfg = 1;
    else if (sig == SIGUSR1) dump_screenshot = 1;
    else running = 0;
}

/* Dump framebuffer as PPM (RGB888) to /tmp/pcat2-screenshot.ppm */
static void fb_dump_ppm(void)
{
    FILE *f = fopen("/tmp/pcat2-screenshot.ppm", "wb");
    if (!f) { perror("screenshot"); return; }
    fprintf(f, "P6\n%d %d\n255\n", DISP_W, DISP_H);
    for (int i = 0; i < DISP_W * DISP_H; i++) {
        uint16_t val = ((uint16_t)fb[i*2] << 8) | fb[i*2+1];
        /* BGR565: bits 15-11=B, 10-5=G, 4-0=R */
        uint8_t r = (val & 0x1F) * 255 / 31;
        uint8_t g = ((val >> 5) & 0x3F) * 255 / 63;
        uint8_t b = ((val >> 11) & 0x1F) * 255 / 31;
        uint8_t px[3] = { r, g, b };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "pcat2-display: screenshot saved to /tmp/pcat2-screenshot.ppm\n");
}

/* ================================================================== */
/*  Nyan Cat boot logo                                                 */
/* ================================================================== */

/*
 * 26x18 pixel Nyan Cat sprite with 6-frame rainbow trail animation.
 * Palette indices map to RGB565 colours below.
 * Rendered at 4x scale (104x72) centred on the 172x320 display.
 */

/* Colour palette */
#define NC_BG    0   /* background (dark blue) */
#define NC_RB_R  1   /* rainbow red */
#define NC_RB_O  2   /* rainbow orange */
#define NC_RB_Y  3   /* rainbow yellow */
#define NC_RB_G  4   /* rainbow green */
#define NC_RB_B  5   /* rainbow blue */
#define NC_RB_V  6   /* rainbow violet */
#define NC_TART  7   /* pop-tart body (tan) */
#define NC_PINK  8   /* frosting (pink) */
#define NC_SPRK  9   /* sprinkles (hot pink) */
#define NC_CAT  10   /* cat body (dark grey) */
#define NC_FACE 11   /* cat face (lighter grey) */
#define NC_EYE  12   /* eyes (black) */
#define NC_NOSE 13   /* cheeks (pink) */
#define NC_STAR 14   /* stars (white) */
#define NC_TAIL 15   /* cat tail stripe */
#define NC__    NC_BG

static const uint16_t nyan_palette[] = {
    [NC_BG]   = RGB565(15,  15,  60),   /* dark navy */
    [NC_RB_R] = RGB565(255, 0,   0),
    [NC_RB_O] = RGB565(255, 153, 0),
    [NC_RB_Y] = RGB565(255, 255, 51),
    [NC_RB_G] = RGB565(51,  255, 51),
    [NC_RB_B] = RGB565(51,  102, 255),
    [NC_RB_V] = RGB565(102, 51,  204),
    [NC_TART] = RGB565(220, 180, 120),
    [NC_PINK] = RGB565(255, 153, 204),
    [NC_SPRK] = RGB565(255, 51,  153),
    [NC_CAT]  = RGB565(102, 102, 102),
    [NC_FACE] = RGB565(153, 153, 153),
    [NC_EYE]  = RGB565(20,  20,  20),
    [NC_NOSE] = RGB565(255, 102, 153),
    [NC_STAR] = RGB565(255, 255, 255),
    [NC_TAIL] = RGB565(80,  80,  80),
};

/* 26 wide x 18 tall nyan cat sprite using palette indices */
/* P=pink, T=tart, S=sprinkle, C=cat, F=face, E=eye, N=nose/cheek */
static const uint8_t nyan_sprite[18][26] = {
    /*                    rainbow trail              pop-tart + cat                     */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 7, 7, 7,10, 0, 0, 0, 0, 0}, /* row  0 */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 8, 8, 8, 8, 8, 8, 8, 7,10, 0, 0, 0, 0}, /* row  1 */
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 7, 8, 9, 8, 8, 8, 9, 8, 8, 7, 0, 0, 0, 0}, /* row  2 */
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 0,10, 0, 7, 8, 8, 8, 9, 8, 8, 8, 8, 7, 0, 0, 0, 0}, /* row  3 */
    {3, 3, 3, 3, 3, 3, 3, 3, 3, 0,10,10, 7, 8, 8, 8, 8, 8, 8, 9, 8, 7, 0, 0, 0, 0}, /* row  4 */
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 0,10,11,11,11, 7, 7, 7, 7, 7, 7, 7, 7,11,11, 0, 0}, /* row  5 */
    {5, 5, 5, 5, 5, 5, 5, 5, 5, 0,10,11,12,11,11,11,11,11,11,11,11,11,11,12,11, 0}, /* row  6 */
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 0,10,11,11,11,11,11,11,11,11,11,11,11,11,11,11, 0}, /* row  7 */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,10,11,11,13,11,11,11,11,11,11,11,11,13,11,11, 0}, /* row  8 */
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,10,11,11,11,11,11,11,11,11,11,11,11,11,10, 0}, /* row  9 */
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0,10,10, 7, 7, 7, 7, 7, 7, 7,10,10, 0, 0, 0}, /* row 10 */
    {3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 7, 8, 8, 8, 8, 8, 9, 8, 8, 8, 8, 7, 0, 0, 0}, /* row 11 */
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 7, 8, 8, 9, 8, 8, 8, 8, 8, 8, 8, 7, 0, 0, 0}, /* row 12 */
    {5, 5, 5, 5, 5, 5, 5, 5, 5, 0, 0, 7, 8, 8, 8, 8, 8, 8, 8, 9, 8, 8, 7, 0, 0, 0}, /* row 13 */
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 7, 8, 8, 8, 9, 8, 8, 8, 8, 7, 0, 0, 0, 0}, /* row 14 */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 8, 8, 8, 8, 8, 8, 8, 8, 7, 0, 0, 0, 0}, /* row 15 */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0, 0}, /* row 16 */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,10, 0, 0, 0, 0,10, 0, 0, 0, 0, 0, 0}, /* row 17: feet */
};

#define NYAN_W  26
#define NYAN_H  18
#define NYAN_SCALE 5   /* 5x scale: 130x90 pixels, fits nicely on 172x320 */

/* Animated star positions for twinkling effect */
static const struct { int x; int y; } nyan_stars[] = {
    {10, 40}, {150, 20}, {30, 120}, {155, 100}, {5, 200}, {160, 180},
    {20, 260}, {140, 240}, {45, 300}, {130, 290}, {80, 15}, {95, 305},
};
#define NUM_STARS 12

static void render_nyan_boot(void)
{
    uint16_t bg = nyan_palette[NC_BG];
    int sx = (DISP_W - NYAN_W * NYAN_SCALE) / 2;    /* centre X */
    int sy = (DISP_H - NYAN_H * NYAN_SCALE) / 2;    /* centre Y */

    /* 3-frame animation: nyan bobs up/down while stars twinkle */
    for (int frame = 0; frame < 8; frame++) {
        fb_clear(bg);

        /* Draw twinkling stars */
        for (int i = 0; i < NUM_STARS; i++) {
            /* Each star twinkles at a different phase */
            int phase = (frame + i) % 4;
            if (phase < 2) {
                uint16_t sc = (phase == 0) ? nyan_palette[NC_STAR] :
                              RGB565(180, 180, 200);
                int sz = (phase == 0) ? 3 : 2;
                /* draw small cross shape for star */
                for (int d = -sz; d <= sz; d++) {
                    fb_pixel(nyan_stars[i].x + d, nyan_stars[i].y, sc);
                    fb_pixel(nyan_stars[i].x, nyan_stars[i].y + d, sc);
                }
            }
        }

        /* Bob offsets */
        int bob_y = (frame % 2 == 0) ? 0 : -3;

        /* Rainbow trail offset cycling */
        int rb_phase = frame % 3;

        /* Draw sprite */
        for (int row = 0; row < NYAN_H; row++) {
            for (int col = 0; col < NYAN_W; col++) {
                uint8_t idx = nyan_sprite[row][col];
                if (idx == NC_BG) continue;

                /* Cycle rainbow colours for the trail columns */
                if (col < 9 && idx >= NC_RB_R && idx <= NC_RB_V) {
                    int rb = (idx - NC_RB_R + rb_phase) % 6 + NC_RB_R;
                    idx = rb;
                }

                uint16_t c = nyan_palette[idx];
                int px = sx + col * NYAN_SCALE;
                int py = sy + row * NYAN_SCALE + bob_y;
                fb_rect(px, py, NYAN_SCALE, NYAN_SCALE, c);
            }
        }

        /* "NYAN!" text below */
        float ts = 2.0f;
        const char *msg = "NYAN!";
        int tw = strlen(msg) * CHAR_W(ts);
        draw_str((DISP_W - tw) / 2, sy + NYAN_H * NYAN_SCALE + 15 + bob_y,
                 msg, nyan_palette[NC_PINK], bg, ts);

        fb_flush();
        usleep(200000);  /* 200ms per frame */
    }

    /* Hold final frame briefly */
    usleep(400000);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(void)
{
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGHUP,  sig_handler);
    signal(SIGUSR1, sig_handler);

    config_load();

    if (spi_init() < 0) { fprintf(stderr, "SPI init failed\n"); return 1; }

    dc_line_fd  = gpio_request_output(DC_BANK,  DC_OFFSET,  "pcat2-dc",  0);
    rst_line_fd = gpio_request_output(RST_BANK, RST_OFFSET, "pcat2-rst", 1);
    bl_line_fd  = gpio_request_output(BL_BANK,  BL_OFFSET,  "pcat2-bl",  0);

    if (dc_line_fd < 0 || rst_line_fd < 0) {
        fprintf(stderr, "GPIO init failed (DC=%d RST=%d BL=%d)\n",
                dc_line_fd, rst_line_fd, bl_line_fd);
        return 1;
    }

    fprintf(stderr, "pcat2-display: GPIOs OK  DC=bank%d/%d  RST=bank%d/%d  BL=bank%d/%d\n",
            DC_BANK, DC_OFFSET, RST_BANK, RST_OFFSET, BL_BANK, BL_OFFSET);

    disp_init();
    fprintf(stderr, "pcat2-display: display initialised (%dx%d) theme=%s refresh=%ds scale=%.1f pages=%d\n",
            DISP_W, DISP_H, cfg.theme, cfg.refresh, cfg.font_scale, cfg.num_pages);

    /* apply initial backlight state */
    gpio_set(bl_line_fd, cfg.backlight ? 0 : 1);

    /* Nyan Cat boot animation! */
    if (cfg.backlight)
        render_nyan_boot();

    /* Open power button input device (grabs exclusively) */
    input_init();

    while (running) {
        if (reload_cfg) {
            reload_cfg = 0;
            config_load();
            gpio_set(bl_line_fd, cfg.backlight ? 0 : 1);
            fprintf(stderr, "pcat2-display: config reloaded theme=%s refresh=%ds bl=%d scale=%.1f pages=%d\n",
                    cfg.theme, cfg.refresh, cfg.backlight, cfg.font_scale, cfg.num_pages);
        }

        if (cfg.backlight) {
            render_screen();
            fb_flush();
        }

        if (dump_screenshot) {
            dump_screenshot = 0;
            fb_dump_ppm();
        }

        for (int i = 0; i < cfg.refresh * 10 && running && !reload_cfg; i++) {
            usleep(100000);     /* 100ms tick */

            int btn = input_check();
            if (btn == 1) {
                /* Short press: cycle to next page */
                page_idx = (page_idx + 1) % cfg.num_pages;
                fprintf(stderr, "pcat2-display: page %d/%d (%s)\n",
                        page_idx + 1, cfg.num_pages, cfg.pages[page_idx]);
                if (cfg.backlight) {
                    render_screen();
                    fb_flush();
                }
            } else if (btn == 2) {
                /* Long press (>= cfg.poweroff_ms ms): power off */
                fprintf(stderr, "pcat2-display: long press -> poweroff\n");
                running = 0;
                system("/sbin/poweroff");
            }

            if (dump_screenshot) {
                dump_screenshot = 0;
                fb_dump_ppm();
            }
        }
    }

    if (input_fd >= 0) close(input_fd);
    disp_sleep();

    if (dc_line_fd  >= 0) close(dc_line_fd);
    if (rst_line_fd >= 0) close(rst_line_fd);
    if (bl_line_fd  >= 0) close(bl_line_fd);
    if (spi_fd      >= 0) close(spi_fd);

    fprintf(stderr, "pcat2-display: exited cleanly\n");
    return 0;
}
