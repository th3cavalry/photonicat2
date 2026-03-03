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
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

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
    strncpy(cfg.theme, "dark", sizeof(cfg.theme));
    cfg.font_scale = 1.0f;

    /* default pages */
    cfg.num_pages = 6;
    strncpy(cfg.pages[0], "clock", sizeof(cfg.pages[0]));
    strncpy(cfg.pages[1], "battery", sizeof(cfg.pages[1]));
    strncpy(cfg.pages[2], "network", sizeof(cfg.pages[2]));
    strncpy(cfg.pages[3], "wifi", sizeof(cfg.pages[3]));
    strncpy(cfg.pages[4], "thermal", sizeof(cfg.pages[4]));
    strncpy(cfg.pages[5], "system", sizeof(cfg.pages[5]));

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
static int spi_fd = -1;
static int dc_line_fd  = -1;
static int rst_line_fd = -1;
static int bl_line_fd  = -1;

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

static void run_cmd(const char *cmd, char *buf, size_t len)
{
    buf[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return;
    if (fgets(buf, len, f) == NULL) buf[0] = '\0';
    pclose(f);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
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

    int y;
    int has_clock_page = 0;

    /* check if clock is a page (renders its own large time) */
    for (int i = 0; i < cfg.num_pages; i++)
        if (strcmp(cfg.pages[i], "clock") == 0) has_clock_page = 1;

    /* top bar only if no clock page (clock page has its own big time) */
    if (!has_clock_page)
        y = render_topbar();
    else
        y = 2;

    for (int i = 0; i < cfg.num_pages && y < DISP_H - 10; i++) {
        const char *p = cfg.pages[i];
        if      (strcmp(p, "clock")   == 0) y = render_clock(y);
        else if (strcmp(p, "battery") == 0) y = render_battery(y);
        else if (strcmp(p, "network") == 0) y = render_network(y);
        else if (strcmp(p, "wifi")    == 0) y = render_wifi(y);
        else if (strcmp(p, "thermal") == 0) y = render_thermal(y);
        else if (strcmp(p, "system")  == 0) y = render_system(y);
        else if (strcmp(p, "custom")  == 0) y = render_custom(y);
    }
}

/* ================================================================== */
/*  Signal handlers                                                    */
/* ================================================================== */

static void sig_handler(int sig)
{
    if (sig == SIGHUP) reload_cfg = 1;
    else running = 0;
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
    fprintf(stderr, "pcat2-display: display initialised (%dx%d) theme=%s refresh=%ds scale=%.1f\n",
            DISP_W, DISP_H, cfg.theme, cfg.refresh, cfg.font_scale);

    /* apply initial backlight state */
    gpio_set(bl_line_fd, cfg.backlight ? 0 : 1);

    /* Nyan Cat boot animation! */
    if (cfg.backlight)
        render_nyan_boot();

    while (running) {
        if (reload_cfg) {
            reload_cfg = 0;
            config_load();
            gpio_set(bl_line_fd, cfg.backlight ? 0 : 1);
            fprintf(stderr, "pcat2-display: config reloaded theme=%s refresh=%ds bl=%d scale=%.1f\n",
                    cfg.theme, cfg.refresh, cfg.backlight, cfg.font_scale);
        }

        if (cfg.backlight) {
            render_screen();
            fb_flush();
        }

        for (int i = 0; i < cfg.refresh * 10 && running && !reload_cfg; i++)
            usleep(100000);
    }

    disp_sleep();

    if (dc_line_fd  >= 0) close(dc_line_fd);
    if (rst_line_fd >= 0) close(rst_line_fd);
    if (bl_line_fd  >= 0) close(bl_line_fd);
    if (spi_fd      >= 0) close(spi_fd);

    fprintf(stderr, "pcat2-display: exited cleanly\n");
    return 0;
}
