/*
 * jtfiles — a small, zen-styled file manager for Wayland.
 *
 * Wayland + xdg-shell + cairo + Pango. No X11, no GTK, no Qt.
 * Icons are drawn in code with Cairo/Pango and follow the live accent.
 * Image/video thumbnails are rendered with ffmpeg straight into Cairo.
 *
 *   make
 *   ./jtfiles
 *   ./jtfiles '#378484'              (optional accent override)
 *   ./jtfiles rainbow                (cycle the accent colours)
 *   ./jtfilesctl accent '#378484'    (change accent while running)
 *   ./jtfilesctl rainbow [on|off|toggle]  (toggle if omitted)
 *
 * Controls:
 *   Click / double-click     select / open
 *   Mount (polkit)           the drive opens automatically once mounted
 *   Right-click              Windows 10 classic context menu
 *   Ctrl+C / X / V           copy / cut / paste (progress window)
 *   Delete / F2              move to Trash / rename
 *   Ctrl+L                   edit the location field
 *   Alt+Left / Alt+Right     back / forward
 *   Up / Down / Home / End   move the selection
 *   Ctrl+H                   show hidden files
 *   Ctrl+E                   toggle icon / list view
 *   Ctrl+Shift+S             toggle smooth scrolling (also in View menu)
 *   Esc                      close a menu or dialog
 *
 * Palette from ~/JTLab/config.h (mirrored locally like the other JT apps):
 *   bg #030303, fg #FFFFFF, borders #404040,
 *   accent #E8A03C (overridable), success #66B366, font Liberation.
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <udisks/udisks.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <math.h>
#include <sys/inotify.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

#define FONT_FAMILY "Liberation Sans"

#define DESIGN_W 1120.0
#define DESIGN_H 720.0
#define MIN_W 680.0
#define MIN_H 460.0

#define TOOLBAR_H 56.0
#define SIDEBAR_W 216.0
#define STATUS_H 30.0

/* Sidebar hover animation slots. Quick Access rows occupy 0..39, drive rows
 * start at SIDE_HOV_DRIVE (40 + drive index). */
#define SIDE_HOV_DRIVE 40
#define SIDE_HOV_COUNT (SIDE_HOV_DRIVE + 48)
#define COLHEAD_H 30.0
#define ROW_H 28.0

/* Search field: half a blink period, plus hard bounds for the query-driven
 * recursive walk so a huge tree cannot lock the UI thread. */
#define CARET_HALF_S      0.5
#define SEARCH_ROW_H      56.0          /* two normal list rows */
#define SEARCH_MAX_RESULTS 4096
#define SEARCH_MAX_VISITED 100000
#define SEARCH_MAX_DEPTH  24
#define SEARCH_KEEP_MAX   256
#define SEARCH_SPIN_TICK_MS 33          /* ~30fps spinner, gentle on one core */
#define SEARCH_SPIN_RPS    0.9          /* revolutions per second */

/* Post-transfer selection lap: same timing and geometry as jtwpi. */
#define SEL_LAP_MS       900.0
#define SEL_LAP_TICK_MS  16
#define SEL_LAP_WIDTH    3.0
#define SEL_LAP_MIN_SEG  14.0
#define SEL_LAP_MAX_SEG  48.0

/* Shared appearance toggle for the accent-border laps (see jtlabwc). */
#define APPEARANCE_CONFIG "appearance.conf"

#define ICON_CACHE_MAX 192
#define THUMB_CACHE_MAX 48
#define THUMB_QUEUE_MAX 128
#define COUNT_CACHE_MAX 64

#define C_BG        0x030303
#define C_BG_TOP    0x070708
#define C_SIDEBAR   0x060607
#define C_FG        0xFFFFFF
#define C_DIM       0x8C8C8C
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0F1011
#define C_CARD_TOP  0x131416
#define C_TRACK     0x1A1A1C
#define C_GREEN     0x66B366
#define C_DANGER    0xE0685E

static unsigned accent_rgb = 0xE8A03C;
static unsigned accent_hi_rgb = 0xF2B25C;
static unsigned accent_lo_rgb = 0xCE8A2F;
static unsigned on_accent_rgb = 0x14100A;
/* Rainbow mode cycles the accent; the base colour is restored when it is
 * switched off so an explicit accent is never lost. */
static unsigned accent_base_rgb = 0xE8A03C;
static int rainbow_mode = 0;
static double rainbow_start = 0.0;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846
#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

/* ---- jtsettings-style soft background glows ------------------------ */

#define JT_GLOW_SIZE 256

static cairo_surface_t *g_ambient_glow;
static cairo_surface_t *g_sidebar_glow;
static unsigned g_glow_accent;

static cairo_surface_t *glow_make(int size, double inner_ratio,
                                  unsigned rgb, double a0, double amid)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    size, size);
    cairo_t *cr = cairo_create(s);
    double c = size / 2.0;
    cairo_pattern_t *p = cairo_pattern_create_radial(
        c, c, c * inner_ratio, c, c, c);
    cairo_pattern_add_color_stop_rgba(p, 0.0,
        R_(rgb), G_(rgb), B_(rgb), a0);
    if (amid >= 0.0)
        cairo_pattern_add_color_stop_rgba(p, 0.55,
            R_(rgb), G_(rgb), B_(rgb), amid);
    cairo_pattern_add_color_stop_rgba(p, 1.0,
        R_(rgb), G_(rgb), B_(rgb), 0.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_destroy(cr);
    return s;
}

static void glow_ensure(void)
{
    if (g_glow_accent == accent_rgb && g_ambient_glow && g_sidebar_glow)
        return;
    if (g_ambient_glow) {
        cairo_surface_destroy(g_ambient_glow);
        g_ambient_glow = NULL;
    }
    if (g_sidebar_glow) {
        cairo_surface_destroy(g_sidebar_glow);
        g_sidebar_glow = NULL;
    }
    g_ambient_glow = glow_make(JT_GLOW_SIZE, 30.0 / 520.0,
                               accent_rgb, 0.035, -1.0);
    g_sidebar_glow = glow_make(JT_GLOW_SIZE, 8.0 / 260.0,
                               accent_rgb, 0.075, 0.018);
    g_glow_accent = accent_rgb;
}

static void glow_paint(cairo_t *cr, cairo_surface_t *s,
                       double cx, double cy, double radius)
{
    if (!s) return;
    int size = cairo_image_surface_get_width(s);
    double k = (2.0 * radius) / (double)size;
    cairo_save(cr);
    cairo_translate(cr, cx - radius, cy - radius);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, s, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void ui_card_fill(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0.0,
        R_(C_CARD_TOP), G_(C_CARD_TOP), B_(C_CARD_TOP), 0.96);
    cairo_pattern_add_color_stop_rgba(p, 1.0,
        R_(0x0A0A0C), G_(0x0A0A0C), B_(0x0A0A0C), 0.96);
    cairo_set_source(cr, p);
    rounded_rect(cr, x, y, w, h, r);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    rgba(cr, C_BORDER, 0.42);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, r - 0.5);
    cairo_stroke(cr);
}

/* Quick Access and file-list selections share this soft accent box. */
static void selected_box(cairo_t *cr, double x, double y, double w, double h)
{
    rgba(cr, C_ACCENT, 0.18);
    rounded_rect(cr, x, y, w, h, 6.0);
    cairo_fill(cr);
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- accent: identical derivation to jtspeedtest ------------------ */

static int accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int accent_parse(const char *s, unsigned *out)
{
    if (!s) return 0;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (strlen(s) != 6) return 0;
    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int n = accent_hex_nibble(s[i]);
        if (n < 0) return 0;
        value = (value << 4) | (unsigned)n;
    }
    *out = value;
    return 1;
}

static double accent_linear_channel(unsigned channel)
{
    double v = (double)channel / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static double accent_contrast(double a, double b)
{
    if (a < b) { double t = a; a = b; b = t; }
    return (a + 0.05) / (b + 0.05);
}

static unsigned accent_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A;
    const unsigned light = 0xF7F5F0;
    double bg = accent_luminance(rgb);
    return accent_contrast(bg, accent_luminance(light)) >
           accent_contrast(bg, accent_luminance(dark)) ? light : dark;
}

static unsigned accent_mix(unsigned rgb, unsigned toward, double amount)
{
    int r1 = (int)((rgb >> 16) & 0xFF), g1 = (int)((rgb >> 8) & 0xFF);
    int b1 = (int)(rgb & 0xFF);
    int r2 = (int)((toward >> 16) & 0xFF), g2 = (int)((toward >> 8) & 0xFF);
    int b2 = (int)(toward & 0xFF);
    int r = (int)lround(r1 + (r2 - r1) * amount);
    int g = (int)lround(g1 + (g2 - g1) * amount);
    int b = (int)lround(b1 + (b2 - b1) * amount);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/* One full hue wheel every eight seconds, rendered as a saturated RGB
 * triple.  t is seconds since rainbow mode was switched on. */
static unsigned rainbow_color(double t)
{
    double phase = fmod(t, 8.0);
    if (phase < 0.0)
        phase += 8.0;
    double h = phase * 0.75;
    int i = (int)h;
    double f = h - i;
    double q = 1.0 - f;
    double r = 0.0, g = 0.0, b = 0.0;
    switch (i) {
    case 0: r = 1.0; g = f;   b = 0.0; break;
    case 1: r = q;   g = 1.0; b = 0.0; break;
    case 2: r = 0.0; g = 1.0; b = f;   break;
    case 3: r = 0.0; g = q;   b = 1.0; break;
    case 4: r = f;   g = 0.0; b = 1.0; break;
    default:r = 1.0; g = 0.0; b = q;   break;
    }
    return ((unsigned)lround(r * 255.0) << 16) |
           ((unsigned)lround(g * 255.0) << 8) |
           (unsigned)lround(b * 255.0);
}

static void accent_apply(unsigned rgb)
{
    accent_rgb = rgb;
    accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
    accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
    on_accent_rgb = accent_ink(rgb);
}

static void accent_set(unsigned rgb)
{
    rainbow_mode = 0;   /* an explicit colour takes over */
    accent_base_rgb = rgb;
    accent_apply(rgb);
}

static void rainbow_set(int on, double t)
{
    int was_on = rainbow_mode;
    rainbow_mode = on ? 1 : 0;
    if (rainbow_mode) {
        if (!was_on) {
            rainbow_start = t;
            accent_apply(rainbow_color(0.0));
        }
    } else if (was_on) {
        accent_apply(accent_base_rgb);
    }
}

/* Returns 1 when the displayed colour actually changed. */
static int accent_tick(double t)
{
    if (!rainbow_mode)
        return 0;
    unsigned rgb = rainbow_color(t - rainbow_start);
    if (rgb == accent_rgb)
        return 0;
    accent_apply(rgb);
    return 1;
}

/* Set when --accent/positional color is given on the command line. In that
 * case the config file is not watched, so the override stays in effect. */
static int accent_cli_override;
static time_t accent_conf_mtime;

/* Same system accent file jtgreeter reads at startup, so the first frame
   already uses the scheme-wide color. Command-line args override it. */
#define ACCENT_CONF_PATH "/etc/jt/accent.conf"
/* Same boot flag jtgreeter reads; jtsettings writes it from Appearance. */
#define RAINBOW_CONF_PATH "/etc/jt/rainbow.conf"

static void accent_load_default(void)
{
    struct stat st;
    if (stat(ACCENT_CONF_PATH, &st) == 0) accent_conf_mtime = st.st_mtime;
    FILE *f = fopen(ACCENT_CONF_PATH, "r");
    if (!f) return;
    char buf[64];
    if (fgets(buf, sizeof buf, f)) {
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                         e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        unsigned rgb;
        if (accent_parse(s, &rgb)) accent_set(rgb);
    }
    fclose(f);
}

/* Start in the same rainbow state as jtgreeter and JTLab. The accent loaded
 * above becomes the base colour restored by an explicit "rainbow off". */
static void rainbow_load_default(void)
{
    FILE *f = fopen(RAINBOW_CONF_PATH, "r");
    if (!f) return;
    char buf[64];
    if (fgets(buf, sizeof buf, f)) {
        char *nl = strpbrk(buf, "\r\n");
        if (nl) *nl = '\0';
        static const char prefix[] = "rainbow=";
        if (!strncmp(buf, prefix, sizeof(prefix) - 1) &&
            !strcmp(buf + sizeof(prefix) - 1, "on"))
            rainbow_set(1, mono_sec());
    }
    fclose(f);
}

static int accent_cli(int *argc, char **argv)
{
    accent_load_default();
    rainbow_load_default();
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;
        if (!strcmp(argv[i], "--rainbow") || !strcmp(argv[i], "rainbow")) {
            rainbow_set(1, mono_sec());
            for (int j = i; j + 1 < *argc; j++) argv[j] = argv[j + 1];
            (*argc)--;
            i--;
            continue;
        }
        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }
        if (!value) continue;
        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr, "%s: invalid accent color '%s' (expected #RRGGBB)\n",
                    argv[0], value);
            return 0;
        }
        accent_set(rgb);
        accent_cli_override = 1;
        for (int j = i; j + consumed < *argc; j++) argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct { int key; PangoFontDescription *fd; } FontEnt;
#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *font_get(double size, int weight)
{
    /* Whole-pixel sizes only. Fractional sizes make Pango/fontconfig select
       different hinting, which changes the rendered typeface/style. */
    size = ceil(size);
    int key = (int)size * 10000 + weight;
    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key) return g_fonts[i].fd;
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_style(fd, PANGO_STYLE_NORMAL);
    pango_font_description_set_variant(fd, PANGO_VARIANT_NORMAL);
    pango_font_description_set_stretch(fd, PANGO_STRETCH_NORMAL);
    pango_font_description_set_absolute_size(fd, (int)size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);
    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    }
    return fd;
}

static PangoLayout *layout_make(cairo_t *cr, const char *text,
                                double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text ? text : "", -1);
    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double text_width(cairo_t *cr, const char *text, double size,
                         int weight, double spacing)
{
    if (!text || !*text) return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double text_draw(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        int align, double spacing)
{
    if (!text || !*text) return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    if (align == ALIGN_CENTER) x -= w / 2.0;
    else if (align == ALIGN_RIGHT) x -= w;
    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

static void ui_ellipsize(cairo_t *cr, char *buf, size_t cap, double size,
                         int weight, double max_w)
{
    (void)cap;
    if (text_width(cr, buf, size, weight, 0.0) <= max_w) return;
    size_t n = strlen(buf);
    if (n < 2) return;
    while (n > 1 && text_width(cr, buf, size, weight, 0.0) > max_w) {
        buf[n - 1] = '\0';
        n--;
    }
    if (n > 1) {
        buf[n - 1] = '.';
        if (n > 2) buf[n - 2] = '.';
        if (n > 3) buf[n - 3] = '.';
    }
}

/* ------------------------------------------------------------------ */
/*  Built-in icons: drawn with Cairo + Pango, stored in this source     */
/*  No GTK, no icon theme, no external icon assets.                    */
/* ------------------------------------------------------------------ */

enum {
    ICO_FOLDER, ICO_FOLDER_LINK, ICO_HOME, ICO_DESKTOP, ICO_DOWNLOADS,
    ICO_TRASH,
    ICO_FILE, ICO_IMAGE, ICO_VIDEO, ICO_AUDIO, ICO_ARCHIVE,
    ICO_PDF, ICO_CODE, ICO_EXEC, ICO_DISC, ICO_DRIVE,
    ICO_BACK, ICO_FORWARD, ICO_UP, ICO_REFRESH, ICO_SEARCH,
    ICO_LIST, ICO_GRID, ICO_MENU
};

static int icon_kind(const char *n)
{
    if (!strcmp(n, "user-home") || !strcmp(n, "user-home-symbolic") ||
        !strcmp(n, "drive-harddisk-root"))
        return ICO_HOME;
    if (!strcmp(n, "user-desktop"))
        return ICO_DESKTOP;
    if (!strcmp(n, "user-trash") || !strcmp(n, "user-trash-full"))
        return ICO_TRASH;
    if (!strcmp(n, "download") || !strcmp(n, "downloads") ||
        strstr(n, "download"))
        return ICO_DOWNLOADS;
    if (!strcmp(n, "folder-symlink") || strstr(n, "symlink"))
        return ICO_FOLDER_LINK;
    if (!strcmp(n, "folder") || !strncmp(n, "folder-", 7))
        return ICO_FOLDER;
    if (strstr(n, "go-previous") || strstr(n, "back"))
        return ICO_BACK;
    if (strstr(n, "go-next") || strstr(n, "forward"))
        return ICO_FORWARD;
    if (strstr(n, "go-up"))
        return ICO_UP;
    if (strstr(n, "refresh") || strstr(n, "reload"))
        return ICO_REFRESH;
    if (strstr(n, "search"))
        return ICO_SEARCH;
    if (strstr(n, "menu") || strstr(n, "settings") ||
        strstr(n, "preferences"))
        return ICO_MENU;
    if (strstr(n, "view-list") || strstr(n, "list-view"))
        return ICO_LIST;
    if (strstr(n, "view-grid") || strstr(n, "grid-view") ||
        strstr(n, "view-icons"))
        return ICO_GRID;
    if (strstr(n, "drive-") || strstr(n, "harddisk"))
        return ICO_DRIVE;
    if (strstr(n, "cd-image") || strstr(n, "optical") || strstr(n, "disc"))
        return ICO_DISC;
    if (strstr(n, "image"))
        return ICO_IMAGE;
    if (strstr(n, "video"))
        return ICO_VIDEO;
    if (strstr(n, "audio") || strstr(n, "sound"))
        return ICO_AUDIO;
    if (strstr(n, "archive") || strstr(n, "package"))
        return ICO_ARCHIVE;
    if (strstr(n, "pdf"))
        return ICO_PDF;
    if (strstr(n, "executable") || strstr(n, "application-x-desktop"))
        return ICO_EXEC;
    if (strstr(n, "script") || strstr(n, "src") || strstr(n, "chdr") ||
        strstr(n, "html"))
        return ICO_CODE;
    return ICO_FILE;
}

static void icon_accent_fill(cairo_t *cr, double x, double y,
                             double w, double h)
{
    (void)w;
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0.0, R_(C_ACCENT_HI),
                                      G_(C_ACCENT_HI), B_(C_ACCENT_HI), 1.0);
    cairo_pattern_add_color_stop_rgba(p, 0.55, R_(C_ACCENT),
                                      G_(C_ACCENT), B_(C_ACCENT), 1.0);
    cairo_pattern_add_color_stop_rgba(p, 1.0, R_(C_ACCENT_LO),
                                      G_(C_ACCENT_LO), B_(C_ACCENT_LO), 1.0);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* All file-type glyphs share the folder icon's visual height in 48px space. */
#define ICON_BODY_TOP    12.0
#define ICON_BODY_BOTTOM 42.0
#define ICON_BODY_H      (ICON_BODY_BOTTOM - ICON_BODY_TOP)

static void icon_file_path(cairo_t *cr)
{
    cairo_new_path(cr);
    cairo_move_to(cr, 13.0, ICON_BODY_TOP);
    cairo_line_to(cr, 31.0, ICON_BODY_TOP);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_line_to(cr, 39.0, ICON_BODY_BOTTOM);
    cairo_line_to(cr, 13.0, ICON_BODY_BOTTOM);
    cairo_close_path(cr);
}

static void icon_file_body(cairo_t *cr, const char *label)
{
    icon_file_path(cr);
    rgba(cr, C_CARD, 1.0);
    cairo_fill_preserve(cr);
    rgba(cr, C_BORDER, 0.85);
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, 31.0, ICON_BODY_TOP);
    cairo_line_to(cr, 31.0, 20.0);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_close_path(cr);
    rgba(cr, C_ACCENT_LO, 0.95);
    cairo_fill(cr);

    rounded_rect(cr, 16.5, ICON_BODY_TOP, 3.0, ICON_BODY_H, 1.5);
    rgba(cr, C_ACCENT, 0.90);
    cairo_fill(cr);

    if (label && *label)
        text_draw(cr, label, 9, 700, C_ACCENT, 0.95,
                  27.0, ICON_BODY_BOTTOM - 9.0, ALIGN_CENTER, 0.6);
}

static void icon_file_label(const char *name, char *out, size_t cap)
{
    *out = '\0';
    if (strstr(name, "pdf")) snprintf(out, cap, "PDF");
    else if (strstr(name, "archive") || strstr(name, "package"))
        snprintf(out, cap, "ZIP");
    else if (strstr(name, "executable") || strstr(name, "desktop"))
        snprintf(out, cap, "RUN");
    else if (strstr(name, "script") || strstr(name, "code") ||
             strstr(name, "src") || strstr(name, "chdr"))
        snprintf(out, cap, "</>");
    else if (strstr(name, "html")) snprintf(out, cap, "HTM");
    else if (strstr(name, "text")) snprintf(out, cap, "TXT");
    else snprintf(out, cap, "FILE");
}

static void icon_folder_body(cairo_t *cr)
{
    rounded_rect(cr, 5.0, 12.0, 18.0, 8.0, 2.5);
    rgba(cr, C_ACCENT_HI, 0.95);
    cairo_fill(cr);
    rounded_rect(cr, 5.0, 17.0, 38.0, 25.0, 4.0);
    icon_accent_fill(cr, 5.0, 17.0, 38.0, 25.0);
    rounded_rect(cr, 5.6, 17.6, 36.8, 23.8, 3.6);
    rgba(cr, C_ACCENT_LO, 0.9);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
}

static void icon_home_glyph(cairo_t *cr)
{
    rgba(cr, C_ON_ACCENT, 0.88);
    cairo_set_line_width(cr, 2.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, 16.0, 31.0);
    cairo_line_to(cr, 24.0, 23.0);
    cairo_line_to(cr, 32.0, 31.0);
    cairo_stroke(cr);
    rounded_rect(cr, 19.0, 30.0, 10.0, 9.0, 1.5);
    cairo_fill(cr);
}

static void icon_desktop_glyph(cairo_t *cr)
{
    rgba(cr, C_ON_ACCENT, 0.88);
    rounded_rect(cr, 17.0, 24.0, 16.0, 11.0, 2.0);
    cairo_fill(cr);
    rgba(cr, C_ACCENT_LO, 1.0);
    rounded_rect(cr, 18.5, 25.5, 13.0, 8.0, 1.0);
    cairo_fill(cr);
    rgba(cr, C_ON_ACCENT, 0.88);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, 22.0, 39.0);
    cairo_line_to(cr, 28.0, 39.0);
    cairo_move_to(cr, 25.0, 35.5);
    cairo_line_to(cr, 25.0, 39.0);
    cairo_stroke(cr);
}

static void icon_downloads_glyph(cairo_t *cr)
{
    rgba(cr, C_ON_ACCENT, 0.90);
    cairo_set_line_width(cr, 2.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, 24.0, 25.5);
    cairo_line_to(cr, 24.0, 33.0);
    cairo_move_to(cr, 20.0, 29.5);
    cairo_line_to(cr, 24.0, 33.5);
    cairo_line_to(cr, 28.0, 29.5);
    cairo_stroke(cr);
    rounded_rect(cr, 17.5, 36.0, 13.0, 3.0, 1.5);
    cairo_fill(cr);
}

/* A waste-bin glyph drawn inside a 48x48 icon canvas. Shared by the small
 * sidebar icon and the large-view plate. */
static void icon_trash_glyph(cairo_t *cr)
{
    /* Drawn to the same vertical span as the folder body (y 12..42) and
     * twice the width, so it fills the same footprint as the folder icons. */
    cairo_new_path(cr);
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 2.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    /* Handle and lid. */
    cairo_move_to(cr, 18.0, 12.0);
    cairo_line_to(cr, 18.0, 15.0);
    cairo_line_to(cr, 30.0, 15.0);
    cairo_line_to(cr, 30.0, 12.0);
    cairo_move_to(cr, 6.0, 16.5);
    cairo_line_to(cr, 42.0, 16.5);
    cairo_stroke(cr);
    /* Bin body, tapered toward the base. */
    cairo_move_to(cr, 10.0, 20.0);
    cairo_line_to(cr, 38.0, 20.0);
    cairo_line_to(cr, 33.0, 42.0);
    cairo_line_to(cr, 15.0, 42.0);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, 20.0, 24.0);
    cairo_line_to(cr, 21.0, 38.0);
    cairo_move_to(cr, 28.0, 24.0);
    cairo_line_to(cr, 27.0, 38.0);
    cairo_stroke(cr);
}

static void icon_link_badge(cairo_t *cr)
{
    cairo_save(cr);
    cairo_translate(cr, 37.0, 35.0);
    cairo_rotate(cr, -0.55);
    rounded_rect(cr, -8.0, -2.5, 16.0, 5.0, 2.5);
    rgba(cr, C_CARD, 1.0);
    cairo_fill(cr);
    rounded_rect(cr, -8.0, -2.5, 16.0, 5.0, 2.5);
    rgba(cr, C_ACCENT_HI, 0.95);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void icon_chevron(cairo_t *cr, int kind)
{
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 3.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (kind == ICO_BACK) {
        cairo_move_to(cr, 29.0, 14.0);
        cairo_line_to(cr, 19.0, 24.0);
        cairo_line_to(cr, 29.0, 34.0);
    } else if (kind == ICO_FORWARD) {
        cairo_move_to(cr, 19.0, 14.0);
        cairo_line_to(cr, 29.0, 24.0);
        cairo_line_to(cr, 19.0, 34.0);
    } else {
        cairo_move_to(cr, 14.0, 29.0);
        cairo_line_to(cr, 24.0, 19.0);
        cairo_line_to(cr, 34.0, 29.0);
    }
    cairo_stroke(cr);
}

static void icon_refresh(cairo_t *cr)
{
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, 24.0, 24.0, 11.0, 0.55, 5.35);
    cairo_stroke(cr);
    cairo_move_to(cr, 31.5, 11.5);
    cairo_line_to(cr, 36.5, 10.0);
    cairo_line_to(cr, 35.0, 15.0);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void icon_menu(cairo_t *cr)
{
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 3; i++) {
        cairo_move_to(cr, 15.0, 17.0 + (double)i * 7.0);
        cairo_line_to(cr, 33.0, 17.0 + (double)i * 7.0);
        cairo_stroke(cr);
    }
}

static void icon_search(cairo_t *cr)
{
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, 21.0, 21.0, 10.0, 0.0, 2.0 * PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 28.5, 28.5);
    cairo_line_to(cr, 38.0, 38.0);
    cairo_stroke(cr);
}

static void icon_view(cairo_t *cr, int grid)
{
    rgba(cr, C_ACCENT, 0.95);
    cairo_set_line_width(cr, 2.2);
    if (grid) {
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                rounded_rect(cr, 11.0 + col * 14.0, 12.0 + row * 13.0,
                             10.0, 9.0, 2.0);
                cairo_fill(cr);
            }
        }
    } else {
        for (int i = 0; i < 3; i++) {
            double y = 14.0 + i * 9.0;
            rounded_rect(cr, 11.0, y, 4.0, 4.0, 1.0);
            cairo_fill(cr);
            cairo_move_to(cr, 20.0, y + 2.0);
            cairo_line_to(cr, 37.0, y + 2.0);
            cairo_stroke(cr);
        }
    }
}

static void icon_drive(cairo_t *cr)
{
    cairo_save(cr);
    cairo_translate(cr, 0.0, ICON_BODY_TOP);
    cairo_scale(cr, 1.0, ICON_BODY_H / (37.0 - 12.5));
    cairo_translate(cr, 0.0, -12.5);

    rounded_rect(cr, 7.0, 16.0, 34.0, 21.0, 5.0);
    rgba(cr, C_CARD, 1.0);
    cairo_fill_preserve(cr);
    rgba(cr, C_BORDER, 0.9);
    cairo_set_line_width(cr, 1.3);
    cairo_stroke(cr);

    rounded_rect(cr, 9.5, 12.5, 29.0, 9.0, 4.5);
    icon_accent_fill(cr, 9.5, 12.5, 29.0, 9.0);

    rgba(cr, C_BORDER, 0.7);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 12.0, 31.0);
    cairo_line_to(cr, 24.0, 31.0);
    cairo_stroke(cr);
    rgba(cr, C_GREEN, 0.95);
    cairo_arc(cr, 35.0, 32.0, 1.7, 0.0, 2.0 * PI);
    cairo_fill(cr);

    cairo_restore(cr);
}

static void icon_disc(cairo_t *cr)
{
    cairo_arc(cr, 24.0, 27.0, 15.0, 0.0, 2.0 * PI);
    icon_accent_fill(cr, 9.0, 12.0, 30.0, 30.0);
    cairo_arc(cr, 24.0, 27.0, 5.0, 0.0, 2.0 * PI);
    rgba(cr, C_CARD, 1.0);
    cairo_fill(cr);
    cairo_arc(cr, 24.0, 27.0, 15.0, 0.0, 2.0 * PI);
    rgba(cr, C_ACCENT_LO, 0.9);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

/* Small and medium icons are drawn at exact pixel sizes, so disable
   antialiasing and use hard font bits to keep their edges well defined. */
static void icon_set_crisp(cairo_t *cr, int target)
{
    if (target > 48) return;
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, fo);
    cairo_font_options_destroy(fo);
}

/* The toolbar navigation icons use glyphs with slightly translucent accent
 * colors. Lift every covered pixel to full alpha (undoing cairo's premulti-
 * plied storage) so they read as solid against the window gradient. */
static void icon_force_opaque(cairo_surface_t *sf)
{
    if (!sf || cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) return;
    cairo_surface_flush(sf);
    unsigned char *data = cairo_image_surface_get_data(sf);
    int stride = cairo_image_surface_get_stride(sf);
    int w = cairo_image_surface_get_width(sf);
    int h = cairo_image_surface_get_height(sf);
    if (!data) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            uint32_t a = p >> 24;
            if (a == 0 || a == 255) continue;
            uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            r = (r * 255u + a / 2u) / a;
            g = (g * 255u + a / 2u) / a;
            b = (b * 255u + a / 2u) / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = (255u << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(sf);
}

static int icon_toolbar_name(const char *n)
{
    return !strcmp(n, "go-previous-symbolic") ||
           !strcmp(n, "go-next-symbolic") ||
           !strcmp(n, "go-up-symbolic") ||
           !strcmp(n, "user-home-symbolic") ||
           !strcmp(n, "view-refresh-symbolic") ||
           !strcmp(n, "menu-symbolic");
}

static cairo_surface_t *icon_load(const char *name, int target)
{
    if (!name || !*name || target < 1) return NULL;
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     target, target);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    cairo_t *cr = cairo_create(sf);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)target / 48.0, (double)target / 48.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    icon_set_crisp(cr, target);

    char label[16];
    switch (icon_kind(name)) {
    case ICO_FOLDER:
    case ICO_FOLDER_LINK:
    case ICO_HOME:
    case ICO_DESKTOP:
    case ICO_DOWNLOADS:
        icon_folder_body(cr);
        if (icon_kind(name) == ICO_HOME) icon_home_glyph(cr);
        else if (icon_kind(name) == ICO_DESKTOP) icon_desktop_glyph(cr);
        else if (icon_kind(name) == ICO_DOWNLOADS) icon_downloads_glyph(cr);
        else if (icon_kind(name) == ICO_FOLDER_LINK) icon_link_badge(cr);
        break;
    case ICO_TRASH:
        icon_trash_glyph(cr);
        break;
    case ICO_IMAGE:
        icon_file_body(cr, NULL);
        rounded_rect(cr, 18.0, 23.0, 17.0, 12.0, 2.0);
        rgba(cr, C_ACCENT, 0.30);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, 19.0, 33.0);
        cairo_line_to(cr, 25.0, 27.0);
        cairo_line_to(cr, 30.0, 31.0);
        cairo_line_to(cr, 34.0, 28.0);
        cairo_line_to(cr, 35.0, 34.0);
        cairo_close_path(cr);
        rgba(cr, C_ACCENT, 0.95);
        cairo_fill(cr);
        cairo_arc(cr, 31.0, 25.0, 1.8, 0.0, 2.0 * PI);
        rgba(cr, C_FG, 0.85);
        cairo_fill(cr);
        break;
    case ICO_VIDEO:
        icon_file_body(cr, NULL);
        rounded_rect(cr, 17.0, 23.0, 19.0, 12.0, 3.0);
        icon_accent_fill(cr, 17.0, 23.0, 19.0, 12.0);
        cairo_new_path(cr);
        cairo_move_to(cr, 24.0, 26.0);
        cairo_line_to(cr, 31.0, 29.0);
        cairo_line_to(cr, 24.0, 32.0);
        cairo_close_path(cr);
        rgba(cr, C_ON_ACCENT, 0.92);
        cairo_fill(cr);
        break;
    case ICO_AUDIO:
        icon_file_body(cr, NULL);
        text_draw(cr, "♪", 18, 700, C_ACCENT, 0.95,
                  26.0, 21.0, ALIGN_CENTER, 0.0);
        break;
    case ICO_DRIVE:
        icon_drive(cr);
        break;
    case ICO_DISC:
        icon_disc(cr);
        break;
    case ICO_BACK:
    case ICO_FORWARD:
    case ICO_UP:
        icon_chevron(cr, icon_kind(name));
        break;
    case ICO_REFRESH:
        icon_refresh(cr);
        break;
    case ICO_SEARCH:
        icon_search(cr);
        break;
    case ICO_MENU:
        icon_menu(cr);
        break;
    case ICO_LIST:
        icon_view(cr, 0);
        break;
    case ICO_GRID:
        icon_view(cr, 1);
        break;
    default:
        icon_file_label(name, label, sizeof label);
        icon_file_body(cr, label);
        break;
    }

    cairo_destroy(cr);
    if (icon_toolbar_name(name)) icon_force_opaque(sf);
    return sf;
}

/* ------------------------------------------------------------------ */
/*  Large-view icons                                                   */
/*  Dark plates with accent glyphs. Used only by the icon/tiles views;
   list, details, sidebar, toolbar and dialogs keep the icons above.   */
/* ------------------------------------------------------------------ */

static void big_icon_plate(cairo_t *cr, double x, double y,
                           double w, double h)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0.0, R_(C_CARD_TOP),
                                      G_(C_CARD_TOP), B_(C_CARD_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(p, 1.0, R_(C_CARD),
                                      G_(C_CARD), B_(C_CARD), 1.0);
    rounded_rect(cr, x, y, w, h, 4.0);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    rgba(cr, C_ACCENT, 0.80);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
}

static void big_icon_folder(cairo_t *cr)
{
    rounded_rect(cr, 5.0, 11.0, 19.0, 8.5, 2.5);
    rgba(cr, C_ACCENT, 0.95);
    cairo_fill(cr);
    big_icon_plate(cr, 5.0, 16.0, 38.0, 26.0);
    cairo_move_to(cr, 9.0, 19.5);
    cairo_line_to(cr, 39.0, 19.5);
    rgba(cr, C_ACCENT_HI, 0.45);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
}

static void big_icon_glyph(cairo_t *cr, int kind)
{
    rgba(cr, C_ACCENT_HI, 0.96);
    cairo_set_line_width(cr, 2.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (kind == ICO_HOME) {
        cairo_move_to(cr, 16.0, 31.0);
        cairo_line_to(cr, 24.0, 23.0);
        cairo_line_to(cr, 32.0, 31.0);
        cairo_stroke(cr);
        rounded_rect(cr, 19.0, 30.0, 10.0, 9.0, 1.5);
        cairo_fill(cr);
    } else if (kind == ICO_DESKTOP) {
        rounded_rect(cr, 16.5, 23.5, 15.0, 10.0, 2.0);
        cairo_stroke(cr);
        cairo_move_to(cr, 22.0, 38.5);
        cairo_line_to(cr, 27.0, 38.5);
        cairo_stroke(cr);
    } else if (kind == ICO_DOWNLOADS) {
        cairo_move_to(cr, 24.0, 22.5);
        cairo_line_to(cr, 24.0, 32.0);
        cairo_move_to(cr, 19.5, 27.5);
        cairo_line_to(cr, 24.0, 32.5);
        cairo_line_to(cr, 28.5, 27.5);
        cairo_stroke(cr);
        cairo_move_to(cr, 17.5, 36.5);
        cairo_line_to(cr, 30.5, 36.5);
        cairo_stroke(cr);
    } else if (kind == ICO_FOLDER_LINK) {
        icon_link_badge(cr);
    }
}

static void big_icon_file(cairo_t *cr, const char *label)
{
    icon_file_path(cr);
    cairo_pattern_t *p = cairo_pattern_create_linear(
        13.0, ICON_BODY_TOP, 13.0, ICON_BODY_BOTTOM);
    cairo_pattern_add_color_stop_rgba(p, 0.0, R_(C_CARD_TOP),
                                      G_(C_CARD_TOP), B_(C_CARD_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(p, 1.0, R_(C_CARD),
                                      G_(C_CARD), B_(C_CARD), 1.0);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    rgba(cr, C_ACCENT, 0.80);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, 31.0, ICON_BODY_TOP);
    cairo_line_to(cr, 31.0, 20.0);
    cairo_line_to(cr, 39.0, 20.0);
    cairo_close_path(cr);
    rgba(cr, C_ACCENT, 0.92);
    cairo_fill(cr);

    rounded_rect(cr, 16.5, ICON_BODY_TOP, 3.0, ICON_BODY_H, 1.5);
    rgba(cr, C_ACCENT_HI, 0.92);
    cairo_fill(cr);

    if (label && *label)
        text_draw(cr, label, 10, 700, C_ACCENT_HI, 1.0,
                  27.0, ICON_BODY_BOTTOM - 8.0, ALIGN_CENTER, 0.8);
}

static void big_icon_file_label(const char *name, char *out, size_t cap)
{
    *out = '\0';
    if (strstr(name, "pdf")) snprintf(out, cap, "PDF");
    else if (strstr(name, "image")) snprintf(out, cap, "IMG");
    else if (strstr(name, "video")) snprintf(out, cap, "VID");
    else if (strstr(name, "audio") || strstr(name, "sound"))
        snprintf(out, cap, "AUD");
    else if (strstr(name, "archive") || strstr(name, "package"))
        snprintf(out, cap, "ZIP");
    else if (strstr(name, "executable") || strstr(name, "desktop"))
        snprintf(out, cap, "RUN");
    else if (strstr(name, "script") || strstr(name, "code") ||
             strstr(name, "src") || strstr(name, "chdr"))
        snprintf(out, cap, "</>");
    else if (strstr(name, "html")) snprintf(out, cap, "HTM");
    else if (strstr(name, "text")) snprintf(out, cap, "TXT");
    else snprintf(out, cap, "FILE");
}

static cairo_surface_t *icon_load_big(const char *name, int target)
{
    if (!name || !*name || target < 1) return NULL;
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     target, target);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    cairo_t *cr = cairo_create(sf);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)target / 48.0, (double)target / 48.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    icon_set_crisp(cr, target);

    int kind = icon_kind(name);
    char label[16];
    switch (kind) {
    case ICO_FOLDER:
    case ICO_FOLDER_LINK:
    case ICO_HOME:
    case ICO_DESKTOP:
    case ICO_DOWNLOADS:
        big_icon_folder(cr);
        big_icon_glyph(cr, kind);
        break;
    case ICO_TRASH:
        icon_trash_glyph(cr);
        break;
    case ICO_DRIVE:
        icon_drive(cr);
        break;
    case ICO_DISC:
        icon_disc(cr);
        break;
    case ICO_BACK:
    case ICO_FORWARD:
    case ICO_UP:
        icon_chevron(cr, kind);
        break;
    case ICO_REFRESH:
        icon_refresh(cr);
        break;
    case ICO_SEARCH:
        icon_search(cr);
        break;
    case ICO_MENU:
        icon_menu(cr);
        break;
    case ICO_LIST:
        icon_view(cr, 0);
        break;
    case ICO_GRID:
        icon_view(cr, 1);
        break;
    default:
        big_icon_file_label(name, label, sizeof label);
        big_icon_file(cr, label);
        break;
    }

    cairo_destroy(cr);
    return sf;
}

/* ------------------------------------------------------------------ */
/*  Data model                                                         */
/* ------------------------------------------------------------------ */

typedef struct App App;
typedef struct Menu Menu;

enum { VIEW_LIST, VIEW_ICONS, VIEW_DETAILS, VIEW_TILES };
enum { SORT_NAME, SORT_DATE, SORT_TYPE, SORT_SIZE };
enum { GROUP_NONE, GROUP_TYPE, GROUP_DATE };
enum { CTX_NONE, CTX_BACKGROUND, CTX_FILE, CTX_DRIVE };

enum {
    A_NONE = 0,
    A_OPEN, A_OPEN_NEW, A_PIN, A_UNPIN,
    A_CUT, A_COPY, A_COPY_PATH, A_SHORTCUT, A_DELETE, A_DELETE_PERMANENT,
    A_RENAME, A_PROPERTIES,
    A_VIEW_LIST, A_VIEW_ICONS, A_VIEW_DETAILS, A_VIEW_TILES,
    A_SORT_NAME, A_SORT_DATE, A_SORT_TYPE, A_SORT_SIZE,
    A_SORT_ASC, A_SORT_DESC,
    A_GROUP_NONE, A_GROUP_TYPE, A_GROUP_DATE,
    A_NEW_FOLDER, A_NEW_TEXT, A_NEW_SHORTCUT, A_NEW_ZIP,
    A_REFRESH, A_PASTE, A_UNDO, A_SELECT_ALL, A_HIDDEN, A_SMOOTH_SCROLL,
    A_GO_BACK, A_GO_FORWARD, A_GO_UP, A_GO_HOME, A_MENU,
    A_DRIVE_OPEN, A_DRIVE_MOUNT, A_DRIVE_UNMOUNT, A_DRIVE_EJECT,
    A_QUIT
};
#define A_APP_BASE 1000
#define A_APP_MAX  1031

typedef struct {
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    int is_dir, is_link;
    mode_t mode;
    off_t size;
    time_t mtime;
    char icon[64];
    char type[48];
    /* Folder item counts are computed on demand, not during dir_load().
     * 0 = unknown, 1 = counted (e->size holds the count),
     * 2 = unavailable, 3 = queued on the worker. */
    unsigned char count_state;
} Entry;

typedef struct {
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
    int px;
    int state;              /* 0 empty, 1 queued/running, 2 ready */
    int failed;
    unsigned gen;
    cairo_surface_t *sf;
    unsigned long long used;
} ThumbEnt;

typedef struct {
    char dev[80];
    char label[128];
    char mount[PATH_MAX];
    char size[32];
    char fstype[32];
    char model[160];
    char avail[64];
    char object[160];           /* UDisks2 block object path */
    char drive_object[160];     /* UDisks2 drive object path */
    int removable, mounted, is_optical;
    double used;
    char icon[64];
} Drive;

typedef struct {
    char name[128];
    char exec[512];
    char id[160];
    char icon[64];
    char mime[1024];
} AppEnt;

typedef struct {
    int kind; /* 0 action, 1 separator, 2 submenu, 3 check */
    int id;
    char label[96];
    char shortcut[24];
    char icon[64];      /* optional Freedesktop icon name (Open With apps) */
    int enabled;
    int checked;
    Menu *sub;
} MenuItem;

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    int busy, retired;
} ShmBuf;

static void shm_buffer_destroy(ShmBuf *b);

struct Menu {
    App *app;
    Menu *parent;
    struct wl_surface *surface, *xdg_surface_owner;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *popup;
    struct wl_callback *frame_cb;
    ShmBuf bufs[2];
    int configured, dirty, done;
    int w, h;               /* w/h are the visible popup size */
    int content_h;          /* full height of all menu rows */
    int max_scroll;         /* content_h - h, in pixels */
    int scroll;             /* wheel-driven target, in pixels */
    double scroll_v;        /* visual offset, eased toward scroll */
    double scroll_t;        /* timestamp of the last easing step */
    int scroll_prev;        /* target seen at the previous step */
    int hover, anchor_x, anchor_y;
    /* Popup-border lap on menu open. */
    double popup_anim_start;
    int popup_anim_active;
    int px, py;
    int n;
    MenuItem items[40];
};

typedef struct {
    int type; /* 1 rename, 2 new item, 3 properties, 4 confirm delete */
    char title[128];
    char text[512];
    char path[PATH_MAX];
    char lines[12][256];
    int nlines;
    int cursor;
} Dialog;

/* One asynchronous udisks2 operation. Callbacks run in our GMainContext and
 * only flip `done`; the Wayland loop finishes the job on the next tick. */
typedef struct {
    App *app;
    char dev[80];
    char object[160];           /* stable UDisks2 block object path */
    char drive_object[160];     /* stable UDisks2 drive object path */
    char mount[PATH_MAX];
    char result[512];
    gpointer proxy;             /* UDisksFilesystem or UDisksDrive ref */
    GCancellable *cancel;
    int op;                     /* 0 mount, 1 unmount, 2 eject */
    int active, done, code;
} Job;

/* One live copy/move/trash/delete operation and its themed progress window. */
enum { XOP_COPY = 0, XOP_MOVE = 1, XOP_TRASH = 2, XOP_DELETE = 3 };

static const char *xfer_title(int op)
{
    switch (op) {
    case XOP_MOVE:   return "Moving files";
    case XOP_TRASH:  return "Moving to Trash";
    case XOP_DELETE: return "Deleting files";
    default:         return "Copying files";
    }
}

typedef struct {
    App *app;
    pid_t pid;
    int fd;
    char buf[4096];
    size_t len;
    int active, cut, op, internal;
    int show_pending;                       /* progress window is delayed */
    double show_at;                         /* monotonic time to show it */
    long long total_items, items_done;
    long long total_bytes, bytes_done;
    int done_items, failed;
    double speed, speed_t0, speed_bytes0;
    char current[NAME_MAX + 1];
    /* Only reload the view on completion if the copy actually touched it. */
    char src_dir[PATH_MAX];
    char dst_dir[PATH_MAX];

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame_cb;
    ShmBuf bufs[2];
    int configured, dirty;
    int w, h;
} Transfer;

/* Dialogs are separate xdg-toplevels with server-side decorations instead of
 * overlays painted inside the main window. */
typedef struct {
    App *app;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame_cb;
    ShmBuf bufs[2];
    int configured, dirty, w, h;
} DialogWin;

typedef struct {
    struct wl_output *output;
    uint32_t global_name;
    int32_t scale;
    int entered;
    App *app;
} Output;

/* One context per wl_data_offer.  MIME events arrive before the enter()
 * or selection() event that says whether an offer is for DnD or clipboard,
 * so the flags have to live on the offer itself, not in a single slot. */
typedef struct DataOffer {
    App *app;
    struct wl_data_offer *offer;
    struct DataOffer *next;
    unsigned uris : 1;
    unsigned gnome : 1;
    unsigned text : 1;
    char text_mime[64];
} DataOffer;

/* Frozen selection payload.  wl_data_source.send is a callback from the
 * compositor and must never read live app state that can change later. */
typedef struct {
    char paths[64][PATH_MAX];
    int n;
    int cut;
    char text[16384];
    int has_text;
} ClipSnapshot;

/* Nonblocking wl_data_offer.receive() in progress. */
typedef struct {
    int fd;
    size_t len;
    int gnome;
    int overflow;
    int attempt;
    double deadline;
    /* 64 paths at PATH_MAX plus the gnome verb and separators. */
    char buf[64 * PATH_MAX + 4096];
} ClipRead;

/* Nonblocking wl_data_source.send() writes, like GTK's GOutputStream writer. */
typedef struct ClipWrite {
    struct ClipWrite *next;
    int fd;
    char *buf;
    size_t len, off;
} ClipWrite;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wp_cursor_shape_manager_v1 *cursor_manager;
    struct wp_cursor_shape_device_v1 *cursor_device;
    uint32_t cursor_shape, cursor_serial, cursor_applied_serial;
    struct wl_seat *seat;
    uint32_t seat_global;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    uint32_t pointer_serial;
    int pointer_target; /* 0 none, 1 main, 2 menu */
    Menu *pointer_menu;
    double pointer_x, pointer_y;

    /* Wayland drag and drop */
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct wl_data_source *dnd_source;  /* outgoing drag */
    struct wl_surface *dnd_icon_surface; /* drag icon surface */
    ShmBuf dnd_icon_buf;                 /* its backing buffer */
    struct wl_data_offer *dnd_offer;    /* incoming drag */
    DataOffer *dnd_ctx;                 /* context for dnd_offer */
    DataOffer *offers;                  /* all live offer contexts */
    uint32_t dnd_enter_serial;
    int dnd_accept, dnd_accepted;
    int dnd_pending;                    /* left press on a file, awaiting threshold */
    int dnd_press_x, dnd_press_y;
    uint32_t dnd_press_serial;
    uint32_t dnd_action;                /* negotiated drag action (source side) */
    uint32_t dnd_offer_action;          /* action reported by wl_data_offer.action */
    int dnd_have_gnome;                 /* incoming drag offers cut/copy verb */
    struct wl_surface *dnd_target_surface; /* surface the drag entered */
    double dnd_drop_x, dnd_drop_y;      /* last enter/motion coordinates */
    char dnd_paths[64][PATH_MAX];       /* snapshot of our outgoing drag payload */
    int n_dnd_paths;

    /* Wayland clipboard (wl_data_device selection) */
    struct wl_data_source *clip_source;  /* we own the selection */
    struct wl_data_offer *clip_offer;    /* another client owns it */
    DataOffer *clip_ctx;                 /* context for clip_offer */
    ClipSnapshot clip_snap;              /* payload we published */
    ClipRead clip_read;                  /* async remote paste */
    ClipWrite *clip_writes;              /* pending source.send writes */
    int clip_cache_valid;                /* clipboard[] belongs to clip_ctx */
    uint32_t clip_serial;                /* last input serial for set_selection */

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    int configured, dirty, quit;
    struct wl_callback *frame_cb;
    ShmBuf bufs[4];
    int scale, fixed_scale, has_argb;
    Output outputs[8];
    double WIN_W, WIN_H;
    int state_w, state_h, state_maximized;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* jtlabwc only advertises repeat_info; it does not send synthetic repeated
     * key events, so the client generates them from these deadlines. */
    uint32_t repeat_key;
    xkb_keysym_t repeat_sym;
    int repeat_active, repeat_ctrl, repeat_alt, repeat_shift;
    double repeat_at, repeat_delay, repeat_rate;

    char cwd[PATH_MAX];
    /* Set by --select FILE (or a file passed as PATH).  The folder is opened
     * normally and this entry is highlighted once its directory is loaded. */
    char cli_start[PATH_MAX];
    char pending_select[NAME_MAX + 1];
    int pending_select_stage;      /* 0: none, 1: applied, 2: re-applied */
    /* Basenames created by the last paste/drop.  Kept until the user selects
     * something or navigates away so automatic rescans cannot wipe the
     * highlight on freshly created entries. */
    char post_dir[PATH_MAX];
    char post_names[64][NAME_MAX + 1];
    int n_post_names, post_armed;
    /* Shared appearance.conf toggle for all accent-border laps. */
    int border_anim_enabled;
    /* >0: lap running; -1: lap already played for this batch. */
    double post_sel_anim_start;
    char *hist[64];
    int hist_n, hist_cap, hist_pos;
    Entry *ents;              /* entries currently painted */
    int n_ents, cap_ents;
    Entry *dir_ents;          /* full listing of a->cwd */
    int n_dir, cap_dir;
    Entry *search_ents;       /* current query's matches under a->cwd */
    int n_search, cap_search;
    int sort_by, sort_desc, group_by, view, show_hidden;
    int scroll, sidebar_scroll, scroll_max;
    int smooth_scroll;          /* animated wheel scrolling, persisted */
    double scroll_v;            /* visual position between scroll targets */
    double scroll_t;            /* time of the last animation step */
    int scroll_prev;            /* target at the previous animation step */
    int sb_drag;                /* scrollbar thumb is being dragged */
    int sb_hover;               /* pointer is over the scrollbar */
    double sb_grab;             /* pointer offset inside the thumb */
    int sel, anchor;
    int hover_row, hover_btn;
    int hover_side;             /* sidebar row under pointer, see SIDE_HOV_* */
    int side_hov_last;          /* previous hover_side, for dt reset */
    float side_hov[SIDE_HOV_COUNT];
    double side_hov_t;
    unsigned char *selected;
    int n_selected;

    UDisksClient *udisks;
    GMainContext *dbus_ctx;
    int drives_dirty;           /* set by UDisks2 signals */

    Drive drives[48];
    int n_drives;
    int sidebar_drive;
    char sidebar_drive_dev[80]; /* stable identity for the highlighted row */
    int drives_y0;
    double last_click_t;
    int last_click_row;
    int drag_select, drag_add;
    double drag_x0, drag_y0, drag_x1, drag_y1;

    char clipboard[64][PATH_MAX];
    int n_clip, clip_cut;
    char last_cut[PATH_MAX];

    Menu *menu;
    int ctx_kind, ctx_index;
    Dialog dialog;

    char path_edit[PATH_MAX];
    int editing_path, path_cursor;

    /* Top-right search box. dir_ents stays the current folder; search_ents
     * holds only names matching the current query (or that folder when the
     * query is empty); a->ents is the filtered list on screen. The caret uses
     * absolute deadlines so blinking does not drift. */
    char search[128];
    int search_cursor;
    int searching;             /* field has keyboard focus */
    int search_active;         /* search result view is on screen */
    int search_truncated;
    unsigned search_gen;       /* only results for this generation are applied */
    int search_pending;        /* worker scan in flight */
    int saved_view;            /* view to restore when search closes */
    int caret_on;
    double caret_next;
    double search_box_x, search_box_w; /* last drawn geometry, for hit-tests */

    char status[256];
    double status_t;

    char favorites[24][PATH_MAX];
    int n_favorites;

    AppEnt apps[A_APP_MAX - A_APP_BASE];
    int n_apps;

    Job jobs[8];
    int n_jobs;
    Transfer xfer;
    DialogWin dlg;

    struct {
        char name[64];
        int px, big;
        cairo_surface_t *sf;
        unsigned long long used;    /* last-access stamp, for LRU eviction */
    } icons[ICON_CACHE_MAX];
    int n_icons;
    unsigned long long icon_clock;

    /* Folder item counts, filled lazily from the details column/properties. */
    struct {
        char path[PATH_MAX];
        time_t mtime;
        long long count;
        unsigned char state;
        unsigned long long used;
    } counts[COUNT_CACHE_MAX];
    int n_counts;
    unsigned long long count_clock;

    ThumbEnt thumbs[THUMB_CACHE_MAX];
    unsigned long long thumb_clock;

    cairo_t *measure;
    cairo_surface_t *measure_surf;

    int ctl_fd, ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;

    /* Live directory watch, like Nemo's GFileMonitor backend. */
    int ino_fd, ino_wd, ino_pending;
    double ino_at;

    volatile sig_atomic_t signal_quit;
};

static App *g_app_signal;

/* Bumped on every directory change so in-flight thumbnail work from the old
 * folder is discarded instead of painting a stale frame. */
static atomic_uint g_thumb_gen;

static int worker_submit_count(const char *path, time_t mtime);
static void watch_update(App *a, const char *path);
static void dir_load(App *a, const char *path);
static void search_rebuild(App *a, int reset);
static void search_cancel_pending(App *a);
static void search_start(App *a);
static void search_stop(App *a);
static void search_sort(App *a);
static void search_caret_reset(App *a);
static void keyboard_cancel_repeat(App *a);
static void scroll_index_into_view(App *a, int idx);
static void post_xfer_disarm(App *a);
static void apply_post_xfer_selection(App *a);
static void apply_pending_select(App *a);
static void clipboard_publish(App *a);
static void clipboard_publish_text(App *a, const char *text);
static int  clipboard_fetch(App *a, int *cut);
static int  clipboard_has(App *a);
static void clip_read_cancel(App *a);
static void clip_write_cancel_all(App *a);
static void clip_write_poll(App *a, int fd);

/* ---- sidebar hover easing (driven by wl_surface.frame / vsync) ------- */

/* Fade every sidebar row toward its target on the compositor frame clock.
 * Only the row under the pointer has a target of 1, so old highlights fade
 * out while the new one fades in. Returns 1 while anything is still moving. */
static int side_hover_tick(App *a)
{
    double now = mono_sec();
    double dt = now - a->side_hov_t;
    a->side_hov_t = now;
    if (a->hover_side != a->side_hov_last) {
        a->side_hov_last = a->hover_side;
        /* Don't charge idle time to the first frame of a new hover. */
        dt = 1.0 / 60.0;
    }
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    double k = 1.0 - exp(-dt / 0.055);
    int moving = 0;
    for (int i = 0; i < SIDE_HOV_COUNT; i++) {
        float target = (i == a->hover_side) ? 1.0f : 0.0f;
        float v = a->side_hov[i];
        if (fabsf((float)target - v) < 0.004f) {
            if (v != target) {
                a->side_hov[i] = target;
                moving = 1;
            }
            continue;
        }
        v += (float)((double)(target - v) * k);
        a->side_hov[i] = v;
        moving = 1;
    }
    if (moving) a->dirty = 1;
    return moving;
}

static int trash_files_dir(char *out, size_t cap);
static int quick_access_places(char places[8][PATH_MAX]);

/* Shared geometry for sidebar hit testing and drawing. Returns the hover
 * slot for a point, or -1 when the pointer is over a header/gap. */
static int side_hover_slot(App *a, double x, double y)
{
    if (x < 0.0 || x >= SIDEBAR_W ||
        y < TOOLBAR_H || y >= a->WIN_H - STATUS_H) return -1;

    int nf = a->n_favorites > 24 ? 24 : a->n_favorites;
    char places[8][PATH_MAX];
    int np = quick_access_places(places);
    double qy = TOOLBAR_H + 30.0 - (double)a->sidebar_scroll;
    double qend = qy + (double)(nf + np) * 28.0;
    if (y >= qy && y < qend)
        return (int)((y - qy) / 28.0);

    double dy = TOOLBAR_H + 64.0 + (double)(nf + np) * 28.0 -
                (double)a->sidebar_scroll;
    if (y >= dy - 3.0 && y < dy + (double)a->n_drives * 40.0 - 2.0) {
        int i = (int)((y - dy) / 40.0);
        if (i >= 0 && i < a->n_drives) return SIDE_HOV_DRIVE + i;
    }
    return -1;
}

/* ---- smooth scrolling ------------------------------------------------ */

/* Keep the visual position in step with the target when the change did not
 * come from the wheel (new folder, resize, keyboard navigation, toggle). */
static void scroll_snap(App *a)
{
    a->scroll_v = (double)a->scroll;
    a->scroll_prev = a->scroll;
    a->scroll_t = mono_sec();
}

/* Ease the visual position toward a->scroll. Returns 1 while there is still
 * motion left so the event loop can keep waking at frame rate. */
static int scroll_tick(App *a)
{
    double now = mono_sec();
    double dt = now - a->scroll_t;
    a->scroll_t = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;

    /* A new target arriving after an idle stretch must not charge all that
     * idle time to the first frame; cap it to roughly one vsync so the ease
     * starts gently instead of snapping most of the way there. */
    if (a->scroll != a->scroll_prev) {
        a->scroll_prev = a->scroll;
        if (dt > 0.05) dt = 1.0 / 60.0;
    }

    double target = (double)a->scroll;
    if (!a->smooth_scroll || fabs(target - a->scroll_v) < 0.02) {
        if (a->scroll_v != target) {
            a->scroll_v = target;
            a->dirty = 1;
        }
        return 0;
    }
    /* Frame-rate independent ease, ~80ms time constant (~400ms to settle).
     * The loop is woken by wl_surface.frame at presentation boundaries; the
     * 16ms poll while animating is only a fallback for missed callbacks. */
    double k = 1.0 - exp(-dt / 0.08);
    a->scroll_v += (target - a->scroll_v) * k;
    if (fabs(target - a->scroll_v) < 0.12) a->scroll_v = target;
    a->dirty = 1;
    return a->scroll_v != target;
}

/* ------------------------------------------------------------------ */
/*  Filesystem helpers                                                 */
/* ------------------------------------------------------------------ */

static void path_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    int slash = la > 0 && a[la - 1] == '/';
    snprintf(out, n, "%s%s%s", a, slash ? "" : "/", b);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Track the exact destination names chosen for a paste/drop.  do_paste() is
 * the single funnel for both clipboard paste and external DnD, and it is the
 * only place where collision-resolved names like "file (copy)" exist. */
static void post_xfer_disarm(App *a)
{
    a->post_armed = 0;
    a->n_post_names = 0;
    a->post_dir[0] = 0;
    a->post_sel_anim_start = 0;
}

static void post_xfer_arm(App *a, const char *dir,
                          char (*dsts)[PATH_MAX], int n)
{
    post_xfer_disarm(a);
    if (!dir || !*dir || n <= 0) return;
    snprintf(a->post_dir, sizeof a->post_dir, "%s", dir);
    for (int i = 0; i < n && a->n_post_names < 64; i++) {
        const char *name = base_name(dsts[i]);
        if (!name || !*name) continue;
        snprintf(a->post_names[a->n_post_names], NAME_MAX + 1, "%s", name);
        a->n_post_names++;
    }
    if (a->n_post_names) a->post_armed = 1;
    a->post_sel_anim_start = 0; /* starts when the entries actually appear */
}

/* Called at the end of every dir_load(), including the inotify-driven ones
 * that happen right after a transfer.  Matching is by visible name so only
 * entries that really exist get highlighted, and partial failures are safe. */
static void apply_post_xfer_selection(App *a)
{
    if (!a->post_armed || !a->post_dir[0]) return;
    if (strcmp(a->cwd, a->post_dir)) return;
    int first = -1, any = 0;
    for (int k = 0; k < a->n_post_names; k++) {
        for (int i = 0; i < a->n_ents; i++) {
            if (strcmp(a->ents[i].name, a->post_names[k])) continue;
            if (!a->selected[i]) { a->selected[i] = 1; a->n_selected++; }
            if (first < 0) first = i;
            any = 1;
        }
    }
    if (any) {
        if (first >= 0) {
            a->sel = a->anchor = first;
            scroll_index_into_view(a, first);
        }
        if (a->border_anim_enabled && a->post_sel_anim_start == 0.0)
            a->post_sel_anim_start = mono_sec();
        a->dirty = 1;
    }
}

static int post_sel_entry_pending(const App *a, int idx)
{
    if (!a || !a->border_anim_enabled || !a->post_armed || !a->selected ||
        idx < 0 || idx >= a->n_ents)
        return 0;
    if (a->post_sel_anim_start <= 0.0) return 0;
    for (int k = 0; k < a->n_post_names; k++)
        if (!strcmp(a->ents[idx].name, a->post_names[k])) return 1;
    return 0;
}

static double post_sel_elapsed_ms(const App *a)
{
    if (!a || a->post_sel_anim_start <= 0.0) return SEL_LAP_MS;
    return (mono_sec() - a->post_sel_anim_start) * 1000.0;
}

static int post_sel_anim_active(const App *a)
{
    return a && a->border_anim_enabled &&
           post_sel_elapsed_ms(a) < SEL_LAP_MS;
}

/* Mark a repaint each tick and retire the batch once the lap completes. */
static int post_sel_anim_tick(App *a)
{
    if (!a || !a->border_anim_enabled || a->post_sel_anim_start <= 0.0)
        return 0;
    if (post_sel_elapsed_ms(a) > SEL_LAP_MS)
        a->post_sel_anim_start = -1;
    a->dirty = 1;
    return 1;
}

/* Turn a file path into "open its parent, highlight this name".  Keep the
 * lexical basename (not realpath's) so a symlink is selected by the name it
 * actually has in the directory listing. */
static int select_set_target(App *a, const char *target)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", target ? target : "");
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    if (!n || !strcmp(buf, "/") || !*base_name(buf)) {
        fprintf(stderr, "jtfiles: invalid --select path '%s'\n", target);
        return 0;
    }

    const char *base = base_name(buf);
    const char *slash = strrchr(buf, '/');
    char parent[PATH_MAX];
    if (!slash)
        snprintf(parent, sizeof parent, ".");
    else if (slash == buf)
        snprintf(parent, sizeof parent, "/");
    else
        snprintf(parent, sizeof parent, "%.*s", (int)(slash - buf), buf);

    char real[PATH_MAX];
    if (!realpath(parent, real)) snprintf(real, sizeof real, "%s", parent);
    snprintf(a->cli_start, sizeof a->cli_start, "%s", real);
    snprintf(a->pending_select, sizeof a->pending_select, "%s", base);
    return 1;
}

/* Strip --select FILE / --select=FILE and remember what to highlight.
 * A plain file path argument is treated the same way.  If a directory is
 * given as PATH, a relative --select name is resolved inside that directory. */
static int select_cli(int *argc, char **argv, App *a)
{
    const char *target = NULL;
    int from_flag = 0;

    for (int i = 1; i < *argc;) {
        if (!strcmp(argv[i], "--select")) {
            if (i + 1 >= *argc || !*argv[i + 1]) {
                fprintf(stderr, "%s: --select requires a file path\n", argv[0]);
                return 0;
            }
            target = argv[i + 1];
            from_flag = 1;
            for (int j = i; j + 2 <= *argc; j++) argv[j] = argv[j + 2];
            *argc -= 2;
        } else if (!strncmp(argv[i], "--select=", 9)) {
            if (!argv[i][9]) {
                fprintf(stderr, "%s: --select requires a file path\n", argv[0]);
                return 0;
            }
            target = argv[i] + 9;
            from_flag = 1;
            for (int j = i; j + 1 < *argc; j++) argv[j] = argv[j + 1];
            (*argc)--;
        } else {
            i++;
        }
    }

    const char *pos = *argc > 1 ? argv[1] : NULL;
    struct stat st;
    if (!target && pos && stat(pos, &st) == 0 && !S_ISDIR(st.st_mode)) {
        target = pos;
        from_flag = 0;
    }
    if (!target) return 1;

    char full[PATH_MAX];
    if (from_flag && pos && target[0] != '/' &&
        stat(pos, &st) == 0 && S_ISDIR(st.st_mode))
        path_join(full, sizeof full, pos, target);
    else
        snprintf(full, sizeof full, "%s", target);
    return select_set_target(a, full);
}

static void fmt_size(char *out, size_t n, off_t bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%lld B", (long long)bytes);
    else if (v < 10.0) snprintf(out, n, "%.1f %s", v, units[i]);
    else snprintf(out, n, "%.0f %s", v, units[i]);
}

static void fmt_date(char *out, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y/%m/%d  %H:%M", &tm);
}

static const char *ext_icon(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return "text-x-generic";
    const char *e = dot + 1;
    struct { const char *ext, *icon; } map[] = {
        { "txt", "text-x-generic" }, { "md", "text-x-generic" },
        { "log", "text-x-generic" }, { "rst", "text-x-generic" },
        { "pdf", "application-pdf" }, { "ps", "application-pdf" },
        { "png", "image-x-generic" }, { "jpg", "image-x-generic" },
        { "jpeg", "image-x-generic" }, { "gif", "image-x-generic" },
        { "webp", "image-x-generic" }, { "bmp", "image-x-generic" },
        { "svg", "image-x-generic" }, { "ico", "image-x-generic" },
        { "mp4", "video-x-generic" }, { "mkv", "video-x-generic" },
        { "avi", "video-x-generic" }, { "webm", "video-x-generic" },
        { "mov", "video-x-generic" }, { "mp3", "audio-x-generic" },
        { "flac", "audio-x-generic" }, { "wav", "audio-x-generic" },
        { "ogg", "audio-x-generic" }, { "opus", "audio-x-generic" },
        { "zip", "application-x-archive" }, { "tar", "application-x-archive" },
        { "gz", "application-x-archive" }, { "xz", "application-x-archive" },
        { "7z", "application-x-archive" }, { "rar", "application-x-archive" },
        { "deb", "package-x-generic" }, { "rpm", "package-x-generic" },
        { "AppImage", "application-x-executable" },
        { "iso", "application-x-cd-image" }, { "desktop", "application-x-desktop" },
        { "sh", "text-x-script" }, { "py", "text-x-script" },
        { "js", "text-x-script" }, { "c", "text-x-csrc" },
        { "h", "text-x-chdr" }, { "cpp", "text-x-c++src" },
        { "json", "text-x-generic" }, { "html", "text-html" },
        { "css", "text-x-generic" }, { "conf", "text-x-generic" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(e, map[i].ext)) return map[i].icon;
    return "text-x-generic";
}

/* Build the built-in quick-access rows identically for drawing, click hit
 * testing, hover slots and scroll clamping. Trash is always the last row. */
static int quick_access_places(char places[8][PATH_MAX])
{
    int np = 0;
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(places[np++], PATH_MAX, "%s", home);
        const char *subs[] = { "Desktop", "Documents", "Downloads",
                               "Pictures", "Music", "Videos" };
        for (unsigned i = 0; i < sizeof subs / sizeof subs[0]; i++)
            path_join(places[np++], PATH_MAX, home, subs[i]);
        char trash[PATH_MAX];
        if (trash_files_dir(trash, sizeof trash) == 0)
            snprintf(places[np++], PATH_MAX, "%s", trash);
    }
    return np;
}

static int path_is_trash(const char *path)
{
    char trash[PATH_MAX];
    return trash_files_dir(trash, sizeof trash) == 0 &&
           !strcmp(path, trash);
}

static const char *place_icon(const char *path)
{
    if (path_is_trash(path)) return "user-trash";
    const char *home = getenv("HOME");
    if (home) {
        char tmp[PATH_MAX];
        if (!strcmp(path, home)) return "user-home";
        path_join(tmp, sizeof tmp, home, "Desktop");
        if (!strcmp(path, tmp)) return "user-desktop";
        path_join(tmp, sizeof tmp, home, "Documents");
        if (!strcmp(path, tmp)) return "folder-documents";
        path_join(tmp, sizeof tmp, home, "Downloads");
        if (!strcmp(path, tmp)) return "download";
        path_join(tmp, sizeof tmp, home, "Pictures");
        if (!strcmp(path, tmp)) return "folder-images";
        path_join(tmp, sizeof tmp, home, "Music");
        if (!strcmp(path, tmp)) return "folder-sound";
        path_join(tmp, sizeof tmp, home, "Videos");
        if (!strcmp(path, tmp)) return "folder-video";
    }
    if (!strcmp(path, "/")) return "drive-harddisk-root";
    return "folder";
}

static void entry_icon(Entry *e)
{
    if (e->is_dir) snprintf(e->icon, sizeof e->icon, "%s", place_icon(e->path));
    else if (e->is_link) snprintf(e->icon, sizeof e->icon, "%s", "folder-symlink");
    else if (S_ISREG(e->mode) && (e->mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        snprintf(e->icon, sizeof e->icon, "%s", "application-x-executable");
    else snprintf(e->icon, sizeof e->icon, "%s", ext_icon(e->name));
}

static void entry_type(Entry *e)
{
    if (e->is_dir) snprintf(e->type, sizeof e->type, "File folder");
    else if (e->is_link) snprintf(e->type, sizeof e->type, "Shortcut");
    else {
        const char *dot = strrchr(e->name, '.');
        if (dot && dot != e->name && strlen(dot + 1) <= 12) {
            char ext[32];
            snprintf(ext, sizeof ext, "%s", dot + 1);
            for (char *p = ext; *p; p++) *p = toupper((unsigned char)*p);
            snprintf(e->type, sizeof e->type, "%s File", ext);
        } else snprintf(e->type, sizeof e->type, "File");
    }
}

/* Sorting is pure over these fields; the search worker uses a private copy
 * so it never reads the App while the UI thread is editing it. */
typedef struct {
    int sort_by, sort_desc, group_by;
} SortCtx;

static int entry_cmp_fields(const void *va, const void *vb, const SortCtx *c)
{
    const Entry *a = va, *b = vb;
    int desc = c->sort_desc;
    int r = 0;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    if (c->group_by == GROUP_TYPE && !a->is_dir && !b->is_dir) {
        const char *ia = strstr(a->icon, "image") ? "image" :
                          strstr(a->icon, "video") ? "video" :
                          strstr(a->icon, "audio") ? "audio" : "file";
        const char *ib = strstr(b->icon, "image") ? "image" :
                          strstr(b->icon, "video") ? "video" :
                          strstr(b->icon, "audio") ? "audio" : "file";
        int g = strcmp(ia, ib);
        if (g) return g;
    }
    if (c->group_by == GROUP_DATE) {
        int da = (int)(a->mtime / 86400), db = (int)(b->mtime / 86400);
        if (da != db) return desc ? db - da : da - db;
    }
    switch (c->sort_by) {
    case SORT_DATE: r = (a->mtime > b->mtime) - (a->mtime < b->mtime); break;
    case SORT_TYPE:
        r = strcmp(a->type, b->type);
        if (!r) r = strcasecmp(a->name, b->name);
        break;
    case SORT_SIZE: r = (a->size > b->size) - (a->size < b->size); break;
    default:        r = strcasecmp(a->name, b->name); break;
    }
    return desc ? -r : r;
}

static int entry_cmp(const void *va, const void *vb)
{
    App *app = g_app_signal;
    SortCtx c = { app ? app->sort_by : SORT_NAME,
                  app ? app->sort_desc : 0,
                  app ? app->group_by : GROUP_NONE };
    return entry_cmp_fields(va, vb, &c);
}

static void status_set(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    a->status_t = mono_sec();
    a->dirty = 1;
}

/* Folder counts used to be computed for every subfolder inside dir_load(),
 * which turned a single navigation into hundreds of readdir() calls. Now a
 * count is only produced when the details column (or Properties) asks for it,
 * and is cached by path+mtime so back/forward navigation stays cheap. */
static void count_cache_lookup(App *a, Entry *e)
{
    for (int i = 0; i < a->n_counts; i++)
        if (a->counts[i].state && a->counts[i].mtime == e->mtime &&
            !strcmp(a->counts[i].path, e->path)) {
            e->size = a->counts[i].count;
            e->count_state = a->counts[i].state;
            a->counts[i].used = ++a->count_clock;
            return;
        }
}

static void count_cache_store(App *a, const Entry *e)
{
    int slot;
    if (a->n_counts < COUNT_CACHE_MAX) {
        slot = a->n_counts++;
    } else {
        slot = 0;
        for (int i = 1; i < COUNT_CACHE_MAX; i++)
            if (a->counts[i].used < a->counts[slot].used) slot = i;
    }
    snprintf(a->counts[slot].path, sizeof a->counts[slot].path, "%s", e->path);
    a->counts[slot].mtime = e->mtime;
    a->counts[slot].count = e->size;
    a->counts[slot].state = e->count_state;
    a->counts[slot].used = ++a->count_clock;
}

static void entry_count(App *a, Entry *e)
{
    if (!e->is_dir || e->count_state) return;
    count_cache_lookup(a, e);
    if (e->count_state) return;
    e->count_state = 3;
    if (!worker_submit_count(e->path, e->mtime)) e->count_state = 0;
}

static void entry_size_text(App *a, Entry *e, char *out, size_t n)
{
    if (e->is_dir) {
        entry_count(a, e);
        if (e->count_state != 1) {
            snprintf(out, n, e->count_state == 2 ? "?" : "...");
        } else {
            fmt_size(out, n, e->size);
        }
        return;
    }
    fmt_size(out, n, e->size);
}

/* Nemo refreshes a folder as soon as the kernel reports a change instead of
 * waiting for F5. Watch the current directory and coalesce bursts of events
 * into one debounced reload. */
#define WATCH_MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | \
                    IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | \
                    IN_MOVE_SELF | IN_ONLYDIR)

static void watch_update(App *a, const char *path)
{
    if (a->ino_fd < 0) return;
    if (a->ino_wd >= 0) {
        (void)inotify_rm_watch(a->ino_fd, a->ino_wd);
        a->ino_wd = -1;
    }
    if (path && *path)
        a->ino_wd = inotify_add_watch(a->ino_fd, path, WATCH_MASK);
}

static void watch_init(App *a)
{
    a->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    a->ino_wd = -1;
    a->ino_pending = 0;
    a->ino_at = 0.0;
}

static void watch_drain(App *a)
{
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(a->ino_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                a->ino_wd = -1;
            /* IN_IGNORED is emitted by watch_update() itself; don't reload
             * just because we replaced the watch after a navigation. */
            if (ev->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF) ||
                !(ev->mask & IN_IGNORED))
                a->ino_pending = 1;
            p += sizeof(struct inotify_event) + ev->len;
        }
        if (a->ino_pending) a->ino_at = mono_sec() + 0.06;
    }
}

static void watch_tick(App *a)
{
    if (!a->ino_pending || mono_sec() < a->ino_at) return;
    a->ino_pending = 0;
    if (a->ino_wd < 0) watch_update(a, a->cwd);
    dir_load(a, a->cwd);
}

static void dir_load(App *a, const char *path)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
    DIR *d = opendir(real);
    if (!d) {
        status_set(a, "Cannot open %s: %s", real, strerror(errno));
        return;
    }
    snprintf(a->cwd, sizeof a->cwd, "%s", real);
    watch_update(a, real);
    /* Any thumbnail rendering still in flight belongs to the old folder. */
    atomic_fetch_add_explicit(&g_thumb_gen, 1, memory_order_relaxed);
    for (int i = 0; i < THUMB_CACHE_MAX; i++)
        if (a->thumbs[i].state == 1) a->thumbs[i].state = 0;
    a->n_ents = 0;
    int cap = 256;
    Entry *ents = malloc((size_t)cap * sizeof *ents);
    if (!ents) { closedir(d); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!a->show_hidden && de->d_name[0] == '.') continue;
        if (a->n_ents >= cap) {
            cap *= 2;
            Entry *n = realloc(ents, (size_t)cap * sizeof *ents);
            if (!n) break;
            ents = n;
        }
        Entry *e = &ents[a->n_ents];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        path_join(e->path, sizeof e->path, real, de->d_name);
        struct stat st;
        if (lstat(e->path, &st) == 0) {
            e->mode = st.st_mode;
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            e->is_link = S_ISLNK(st.st_mode);
            e->is_dir = S_ISDIR(st.st_mode);
            if (e->is_link) {
                struct stat ts;
                if (stat(e->path, &ts) == 0 && S_ISDIR(ts.st_mode))
                    e->is_dir = 1;
            }
            if (e->is_dir) count_cache_lookup(a, e);
        }
        entry_icon(e);
        entry_type(e);
        a->n_ents++;
    }
    closedir(d);
    /* Sorting by size compares e->size, which for folders is the item count.
     * Ask the worker for those counts up front (never on the UI thread) so the
     * initial order is close to the final one. */
    if (a->sort_by == SORT_SIZE) {
        int queued = 0;
        for (int i = 0; i < a->n_ents && queued < THUMB_QUEUE_MAX / 2; i++) {
            Entry *e = &ents[i];
            if (!e->is_dir || e->count_state) continue;
            e->count_state = 3;
            if (worker_submit_count(e->path, e->mtime)) queued++;
            else e->count_state = 0;
        }
    }
    qsort(ents, (size_t)a->n_ents, sizeof *ents, entry_cmp);
    free(a->dir_ents);
    a->dir_ents = ents;
    a->n_dir = a->n_ents;
    a->cap_dir = cap;

    /* Navigation always leaves search mode. Restore the saved view only when
     * leaving search; ordinary navigation must keep the view the user chose. */
    int was_search = a->search_active || a->searching;
    search_cancel_pending(a);
    free(a->search_ents);
    a->search_ents = NULL;
    a->n_search = a->cap_search = 0;
    a->searching = 0;
    a->search_active = 0;
    a->search[0] = '\0';
    a->search_cursor = 0;
    a->search_truncated = 0;
    if (was_search) a->view = a->saved_view;

    search_rebuild(a, 1);
    apply_post_xfer_selection(a);
    a->dirty = 1;
}

static void hist_push(App *a, const char *path)
{
    if (a->hist_pos >= 0 && a->hist_pos < a->hist_n &&
        a->hist[a->hist_pos] && !strcmp(a->hist[a->hist_pos], path)) return;

    /* hist_pos starts at 0 on a zeroed App, before any entry exists. Clamp
     * the truncation point to the real history length so the first navigate
     * does not leave a NULL slot at index 0. */
    int keep = a->hist_pos + 1;
    if (keep < 0) keep = 0;
    if (keep > a->hist_n) keep = a->hist_n;
    for (int i = keep; i < a->hist_n; i++) free(a->hist[i]);
    a->hist_n = keep;
    if (a->hist_n >= 64) {
        free(a->hist[0]);
        for (int i = 1; i < a->hist_n; i++) a->hist[i - 1] = a->hist[i];
        a->hist_n--;
    }
    a->hist[a->hist_n++] = strdup(path);
    a->hist_pos = a->hist_n - 1;
}

static void navigate(App *a, const char *path, int push)
{
    post_xfer_disarm(a);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        status_set(a, "Not a folder: %s", path);
        return;
    }
    if (push) hist_push(a, path);
    dir_load(a, path);
    snprintf(a->path_edit, sizeof a->path_edit, "%s", a->cwd);
    a->editing_path = 0;
}

static void go_hist(App *a, int dir)
{
    post_xfer_disarm(a);
    int p = a->hist_pos + dir;
    if (p < 0 || p >= a->hist_n || !a->hist[p]) return;
    a->hist_pos = p;
    dir_load(a, a->hist[p]);
    snprintf(a->path_edit, sizeof a->path_edit, "%s", a->cwd);
}

/* ---- recursive search ------------------------------------------------ *
 * The recursive walk runs on its own thread. Each query gets a generation;
 * superseding it (typing, cancelling, navigating, quitting) flips an atomic
 * flag so the worker stops mid-scan, closes its open directories and drops
 * partial results. Only a completed result for the current generation is
 * installed into a->ents on the UI thread. An empty query just lists cwd.
 * ----------------------------------------------------------------------- */

static void search_caret_reset(App *a)
{
    a->caret_on = 1;
    a->caret_next = mono_sec() + CARET_HALF_S;
}

static void search_caret_tick(App *a)
{
    if (!a->searching) return;
    double now = mono_sec();
    if (now < a->caret_next) return;
    int flips = 0;
    while (now >= a->caret_next && flips < 8) {
        a->caret_on = !a->caret_on;
        a->caret_next += CARET_HALF_S;
        flips++;
    }
    if (flips == 8) a->caret_next = now + CARET_HALF_S;
    a->dirty = 1;
}

typedef struct SearchReq {
    struct SearchReq *next;
    unsigned gen;
    char cwd[PATH_MAX];
    char query[128];
    int show_hidden;
    SortCtx sort;
} SearchReq;

typedef struct SearchRes {
    struct SearchRes *next;
    unsigned gen;
    Entry *ents;
    int n, cap;
    int truncated;
} SearchRes;

typedef struct {
    unsigned gen;
    char cwd[PATH_MAX];
    char query[128];
    int show_hidden;
    Entry *ents;
    int n, cap;
    int truncated;
    int sorted;
} SearchScan;

static pthread_mutex_t search_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t search_cv = PTHREAD_COND_INITIALIZER;
static SearchReq *search_req_head, *search_req_tail;
static SearchRes *search_res_head, *search_res_tail;
static pthread_t search_tid;
static int search_thread_started, search_wake[2] = { -1, -1 };
static atomic_uint search_gen_counter = 0;
static atomic_uint search_live_gen = 0;
static atomic_uint search_done_gen = 0;
static atomic_int search_shutdown = 0;
static _Thread_local SortCtx search_sort_ctx;

/* The worker only touches its own scan, never the App, so cancellation can
 * be checked without taking a lock on every directory entry. */
static int sr_cancelled(const SearchScan *s)
{
    return atomic_load(&search_shutdown) ||
           atomic_load(&search_live_gen) != s->gen;
}

static Entry *sr_push(SearchScan *s)
{
    if (s->n >= SEARCH_MAX_RESULTS) {
        s->truncated = 1;
        return NULL;
    }
    if (s->n >= s->cap) {
        int cap = s->cap > 0 ? s->cap * 2 : 256;
        Entry *n = realloc(s->ents, (size_t)cap * sizeof *n);
        if (!n) {
            s->truncated = 1;
            return NULL;
        }
        s->ents = n;
        s->cap = cap;
    }
    Entry *e = &s->ents[s->n++];
    memset(e, 0, sizeof *e);
    return e;
}

static void sr_free(SearchScan *s)
{
    free(s->ents);
    s->ents = NULL;
    s->n = s->cap = 0;
}

static int sr_name_match(const char *name, const char *q)
{
    if (!q || !*q) return 1;
    const unsigned char *needle = (const unsigned char *)q;
    const unsigned char *hay = (const unsigned char *)name;
    for (; *hay; hay++) {
        const unsigned char *p = needle, *h = hay;
        while (*p && *h && tolower(*p) == tolower(*h)) { p++; h++; }
        if (!*p) return 1;
    }
    return 0;
}

static int search_match(const Entry *e, const char *q)
{
    return sr_name_match(e->name, q);
}

/* Kernel pseudo-filesystems under / are huge and never hold user files. */
static int sr_skip_root_dir(const char *dir, const char *name)
{
    if (strcmp(dir, "/")) return 0;
    return !strcmp(name, "proc") || !strcmp(name, "sys") ||
           !strcmp(name, "dev") || !strcmp(name, "run");
}

/* Checked before every recurse and every entry so a superseded scan stops
 * quickly and every opened DIR is still closed. */
static void sr_walk(SearchScan *s, const char *dir, int depth, int *visited)
{
    if (sr_cancelled(s) || s->truncated || depth > SEARCH_MAX_DEPTH) return;
    if (++(*visited) > SEARCH_MAX_VISITED) {
        s->truncated = 1;
        return;
    }
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (sr_cancelled(s) || s->truncated) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!s->show_hidden && de->d_name[0] == '.') continue;
        if (sr_skip_root_dir(dir, de->d_name)) continue;
        char path[PATH_MAX];
        path_join(path, sizeof path, dir, de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        int is_link = S_ISLNK(st.st_mode);
        int is_dir = S_ISDIR(st.st_mode);
        if (is_link) {
            struct stat ts;
            if (stat(path, &ts) == 0 && S_ISDIR(ts.st_mode)) is_dir = 1;
        }
        int descend = is_dir && !is_link;
        if (!sr_name_match(de->d_name, s->query)) {
            if (descend) sr_walk(s, path, depth + 1, visited);
            continue;
        }
        Entry *e = sr_push(s);
        if (!e) break;
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        snprintf(e->path, sizeof e->path, "%s", path);
        e->mode = st.st_mode;
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->is_dir = is_dir;
        e->is_link = is_link;
        entry_icon(e);
        entry_type(e);
        if (descend) sr_walk(s, path, depth + 1, visited);
    }
    closedir(d);
}

static int search_entry_cmp(const void *va, const void *vb)
{
    return entry_cmp_fields(va, vb, &search_sort_ctx);
}

static void search_wake_drain(void)
{
    unsigned char buf[128];
    while (search_wake[0] >= 0 && read(search_wake[0], buf, sizeof buf) > 0) {}
}

/* Move a finished scan into the App. Only ever called on the UI thread. */
static void search_install(App *a, SearchScan *s)
{
    int n = s->n;
    free(a->search_ents);
    a->search_ents = s->ents;
    a->n_search = s->n;
    a->cap_search = s->cap;
    a->search_truncated = s->truncated;
    a->search_pending = 0;
    s->ents = NULL;
    s->n = s->cap = 0;
    if (!s->sorted && n > 1)
        qsort(a->search_ents, (size_t)n, sizeof *a->search_ents, entry_cmp);
    search_rebuild(a, 0);
    a->dirty = 1;
}

/* Supersede any in-flight scan. The worker sees the new generation on its
 * next check and unwinds without waiting for us. */
static void search_cancel_pending(App *a)
{
    a->search_gen = atomic_fetch_add(&search_gen_counter, 1) + 1;
    atomic_store(&search_live_gen, a->search_gen);
    a->search_pending = 0;

    pthread_mutex_lock(&search_mu);
    SearchReq *r = search_req_head;
    search_req_head = search_req_tail = NULL;
    pthread_mutex_unlock(&search_mu);
    while (r) {
        SearchReq *next = r->next;
        free(r);
        r = next;
    }
}

static void search_collect(App *a)
{
    search_wake_drain();
    for (;;) {
        pthread_mutex_lock(&search_mu);
        SearchRes *res = search_res_head;
        if (res) {
            search_res_head = res->next;
            if (!search_res_head) search_res_tail = NULL;
        }
        pthread_mutex_unlock(&search_mu);
        if (!res) break;

        if (res->gen == a->search_gen && a->search_active) {
            SearchScan s;
            memset(&s, 0, sizeof s);
            s.gen = res->gen;
            s.ents = res->ents;
            s.n = res->n;
            s.cap = res->cap;
            s.truncated = res->truncated;
            s.sorted = 1;
            res->ents = NULL;
            search_install(a, &s);
        }
        free(res->ents);
        free(res);
    }

    /* A worker that aborted or hit an allocation failure still wakes us. */
    if (a->search_pending && atomic_load(&search_done_gen) == a->search_gen) {
        free(a->search_ents);
        a->search_ents = NULL;
        a->n_search = a->cap_search = 0;
        a->search_truncated = 0;
        a->search_pending = 0;
        search_rebuild(a, 0);
        a->dirty = 1;
    }
}

static void *search_thread_main(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&search_mu);
        while (!search_req_head && !atomic_load(&search_shutdown))
            pthread_cond_wait(&search_cv, &search_mu);
        SearchReq *r = search_req_head;
        if (!r) {
            pthread_mutex_unlock(&search_mu);
            break;
        }
        search_req_head = r->next;
        if (!search_req_head) search_req_tail = NULL;
        pthread_mutex_unlock(&search_mu);

        SearchScan s;
        memset(&s, 0, sizeof s);
        s.gen = r->gen;
        snprintf(s.cwd, sizeof s.cwd, "%s", r->cwd);
        snprintf(s.query, sizeof s.query, "%s", r->query);
        s.show_hidden = r->show_hidden;
        search_sort_ctx = r->sort;
        free(r);

        if (!sr_cancelled(&s)) {
            int visited = 0;
            sr_walk(&s, s.cwd, 0, &visited);
        }
        if (!sr_cancelled(&s)) {
            if (s.n > 1) {
                qsort(s.ents, (size_t)s.n, sizeof *s.ents, search_entry_cmp);
                s.sorted = 1;
            }
            SearchRes *res = calloc(1, sizeof *res);
            if (res) {
                res->gen = s.gen;
                res->ents = s.ents;
                res->n = s.n;
                res->cap = s.cap;
                res->truncated = s.truncated;
                pthread_mutex_lock(&search_mu);
                res->next = NULL;
                if (search_res_tail) search_res_tail->next = res;
                else search_res_head = res;
                search_res_tail = res;
                pthread_mutex_unlock(&search_mu);
                s.ents = NULL;
                s.n = s.cap = 0;
            }
        }
        atomic_store(&search_done_gen, s.gen);
        unsigned char byte = 1;
        if (search_wake[1] >= 0) (void)!write(search_wake[1], &byte, 1);
        sr_free(&s);
    }
    return NULL;
}

static void search_thread_start(void)
{
    atomic_store(&search_shutdown, 0);
    if (pipe2(search_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        search_wake[0] = search_wake[1] = -1;
        return;
    }
    if (pthread_create(&search_tid, NULL, search_thread_main, NULL) == 0) {
        search_thread_started = 1;
        return;
    }
    close(search_wake[0]);
    close(search_wake[1]);
    search_wake[0] = search_wake[1] = -1;
}

static void search_thread_stop(void)
{
    if (!search_thread_started) return;
    atomic_store(&search_shutdown, 1);
    pthread_cond_broadcast(&search_cv);
    pthread_join(search_tid, NULL);
    search_thread_started = 0;

    pthread_mutex_lock(&search_mu);
    while (search_req_head) {
        SearchReq *r = search_req_head;
        search_req_head = r->next;
        free(r);
    }
    search_req_tail = NULL;
    while (search_res_head) {
        SearchRes *r = search_res_head;
        search_res_head = r->next;
        free(r->ents);
        free(r);
    }
    search_res_tail = NULL;
    pthread_mutex_unlock(&search_mu);

    if (search_wake[0] >= 0) close(search_wake[0]);
    if (search_wake[1] >= 0) close(search_wake[1]);
    search_wake[0] = search_wake[1] = -1;
}

/* With an empty query there is nothing to hunt for, so stay in the current
 * folder instead of walking the whole tree just to match everything. */
static void search_build(App *a, const char *q)
{
    search_cancel_pending(a);
    free(a->search_ents);
    a->search_ents = NULL;
    a->n_search = a->cap_search = 0;
    a->search_truncated = 0;

    if (!q || !*q) {
        SearchScan s;
        memset(&s, 0, sizeof s);
        s.gen = a->search_gen;
        for (int i = 0; i < a->n_dir; i++) {
            Entry *e = sr_push(&s);
            if (!e) break;
            *e = a->dir_ents[i];
        }
        search_install(a, &s);
        return;
    }

    a->search_pending = 1;

    if (!search_thread_started) {
        SearchScan s;
        memset(&s, 0, sizeof s);
        s.gen = a->search_gen;
        snprintf(s.cwd, sizeof s.cwd, "%s", a->cwd);
        snprintf(s.query, sizeof s.query, "%s", q);
        s.show_hidden = a->show_hidden;
        int visited = 0;
        sr_walk(&s, s.cwd, 0, &visited);
        search_install(a, &s);
        return;
    }

    SearchReq *r = calloc(1, sizeof *r);
    if (!r) {
        a->search_pending = 0;
        search_rebuild(a, 0);
        return;
    }
    r->gen = a->search_gen;
    snprintf(r->cwd, sizeof r->cwd, "%s", a->cwd);
    snprintf(r->query, sizeof r->query, "%s", q);
    r->show_hidden = a->show_hidden;
    r->sort.sort_by = a->sort_by;
    r->sort.sort_desc = a->sort_desc;
    r->sort.group_by = a->group_by;

    pthread_mutex_lock(&search_mu);
    r->next = NULL;
    if (search_req_tail) search_req_tail->next = r;
    else search_req_head = r;
    search_req_tail = r;
    pthread_cond_signal(&search_cv);
    pthread_mutex_unlock(&search_mu);

    search_rebuild(a, 0);
    a->dirty = 1;
}

/* Directory shown under each result, relative to the folder being searched. */
static void search_rel_dir(const App *a, const Entry *e,
                           char *out, size_t n)
{
    const char *p = e->path;
    size_t cwd_len = strlen(a->cwd);
    if (!strncmp(p, a->cwd, cwd_len) && p[cwd_len] == '/')
        p += cwd_len + 1;
    const char *slash = strrchr(p, '/');
    if (slash && slash > p)
        snprintf(out, n, "%.*s", (int)(slash - p), p);
    else
        snprintf(out, n, ".");
}

static void search_keep_add(char **keep, int *n, const char *path)
{
    if (!path || !*path || *n >= SEARCH_KEEP_MAX) return;
    for (int i = 0; i < *n; i++)
        if (keep[i] && !strcmp(keep[i], path)) return;
    keep[*n] = strdup(path);
    if (keep[*n]) (*n)++;
}

static void search_rebuild(App *a, int reset)
{
    Entry *src = a->search_active ? a->search_ents : a->dir_ents;
    int nsrc = a->search_active ? a->n_search : a->n_dir;
    if (a->search_active) a->view = VIEW_LIST;

    /* Selection is tracked by path: filtering must not leave stale indices. */
    char **keep = calloc(SEARCH_KEEP_MAX, sizeof *keep);
    int nkeep = 0;
    char sel_path[PATH_MAX] = { 0 }, anchor_path[PATH_MAX] = { 0 };
    char ctx_path[PATH_MAX] = { 0 };
    if (!reset && keep) {
        if (a->sel >= 0 && a->sel < a->n_ents)
            snprintf(sel_path, sizeof sel_path, "%s", a->ents[a->sel].path);
        if (a->anchor >= 0 && a->anchor < a->n_ents)
            snprintf(anchor_path, sizeof anchor_path, "%s",
                     a->ents[a->anchor].path);
        if (a->ctx_kind == CTX_FILE && a->ctx_index >= 0 &&
            a->ctx_index < a->n_ents)
            snprintf(ctx_path, sizeof ctx_path, "%s",
                     a->ents[a->ctx_index].path);
        search_keep_add(keep, &nkeep, sel_path);
        if (a->selected)
            for (int i = 0; i < a->n_ents && nkeep < SEARCH_KEEP_MAX; i++)
                if (a->selected[i])
                    search_keep_add(keep, &nkeep, a->ents[i].path);
    }

    if (a->cap_ents < nsrc) {
        int cap = a->cap_ents > 0 ? a->cap_ents : 256;
        while (cap < nsrc) cap *= 2;
        Entry *n = realloc(a->ents, (size_t)cap * sizeof *n);
        if (!n) {
            for (int i = 0; i < nkeep; i++) free(keep[i]);
            free(keep);
            return;
        }
        a->ents = n;
        a->cap_ents = cap;
    }

    a->n_ents = 0;
    for (int i = 0; i < nsrc; i++) {
        Entry *s = &src[i];
        if (!search_match(s, a->search)) continue;
        a->ents[a->n_ents++] = *s;
    }

    free(a->selected);
    a->selected = calloc((size_t)(a->n_ents > 0 ? a->n_ents : 1), 1);
    a->n_selected = 0;
    int sel = -1, anchor = -1, ctx = -1;
    for (int i = 0; i < a->n_ents; i++) {
        const char *path = a->ents[i].path;
        for (int k = 0; k < nkeep; k++) {
            if (!keep[k] || strcmp(path, keep[k])) continue;
            a->selected[i] = 1;
            a->n_selected++;
            break;
        }
        if (sel_path[0] && !strcmp(path, sel_path)) sel = i;
        if (!anchor_path[0]) {
            if (sel >= 0) anchor = sel;
        } else if (!strcmp(path, anchor_path)) anchor = i;
        if (ctx_path[0] && !strcmp(path, ctx_path)) ctx = i;
    }
    a->sel = sel;
    a->anchor = anchor >= 0 ? anchor : sel;
    if (a->ctx_kind == CTX_FILE) a->ctx_index = ctx;

    a->scroll = 0;
    scroll_snap(a);
    a->drag_select = 0;
    a->sb_drag = 0;
    a->dirty = 1;

    for (int i = 0; i < nkeep; i++) free(keep[i]);
    free(keep);
}

static void search_start(App *a)
{
    if (!a->search_active) {
        a->saved_view = a->view;
        a->search_active = 1;
    }
    a->editing_path = 0;
    a->searching = 1;
    a->search[0] = '\0';
    a->search_cursor = 0;
    search_build(a, a->search);
    search_caret_reset(a);
}

static void search_stop(App *a)
{
    if (!a->search_active && !a->searching) return;
    search_cancel_pending(a);
    a->searching = 0;
    a->search_active = 0;
    a->search[0] = '\0';
    a->search_cursor = 0;
    a->search_truncated = 0;
    a->view = a->saved_view;
    free(a->search_ents);
    a->search_ents = NULL;
    a->n_search = a->cap_search = 0;
    search_rebuild(a, 0);
}

static void search_sort(App *a)
{
    /* Resubmit so the worker's sort snapshot matches the new setting. */
    if (a->search_active) {
        search_build(a, a->search);
        return;
    }
    if (a->n_dir > 1)
        qsort(a->dir_ents, (size_t)a->n_dir, sizeof *a->dir_ents, entry_cmp);
    search_rebuild(a, 0);
}

/* Enter/double-click on a result: open its containing folder and highlight
 * the result there, even when the result itself is a folder. */
static void search_accept_path(App *a, const char *path)
{
    if (!a->search_active || !path || !*path) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    else snprintf(dir, sizeof dir, "/");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(a->pending_select, sizeof a->pending_select, "%s", base);
    a->pending_select_stage = 0;
    navigate(a, dir, 1);
    apply_pending_select(a);
    a->pending_select[0] = '\0';
    a->dirty = 1;
}

/* ---- drives: libudisks2 for enumeration and mount operations ---------
 *
 * No lsblk parsing and no process spawn. UDisksClient gives us stable object
 * paths, D-Bus change signals and async mount/unmount/eject calls; the
 * privileged work still happens in udisksd behind polkit. */
static void drives_changed_cb(GDBusObjectManager *manager,
                              GDBusObject *object, gpointer user_data)
{
    (void)manager; (void)object;
    App *a = user_data;
    if (!a) return;
    a->drives_dirty = 1;
    a->dirty = 1;
}

static void drives_iface_cb(GDBusObjectManager *manager, GDBusObject *object,
                            GDBusInterface *interface, gpointer user_data)
{
    (void)manager; (void)object; (void)interface;
    App *a = user_data;
    if (!a) return;
    a->drives_dirty = 1;
    a->dirty = 1;
}

static void drives_props_changed_cb(GDBusObjectManagerClient *manager,
                                    GDBusObjectProxy *object_proxy,
                                    GDBusProxy *interface_proxy,
                                    GVariant *changed_properties,
                                    const gchar *const *invalidated_properties,
                                    gpointer user_data)
{
    (void)manager; (void)object_proxy; (void)interface_proxy;
    (void)changed_properties; (void)invalidated_properties;
    App *a = user_data;
    if (!a) return;
    a->drives_dirty = 1;
    a->dirty = 1;
}

static void udisks_init(App *a)
{
    a->dbus_ctx = g_main_context_new();
    /* UDisksClient and its proxies attach to the thread-default context. */
    g_main_context_push_thread_default(a->dbus_ctx);

    GError *err = NULL;
    a->udisks = udisks_client_new_sync(NULL, &err);
    if (!a->udisks) {
        fprintf(stderr, "jtfiles: udisks2 unavailable: %s\n",
                err ? err->message : "unknown error");
        if (err) g_error_free(err);
        return;
    }

    GDBusObjectManager *om = udisks_client_get_object_manager(a->udisks);
    if (om) {
        g_signal_connect(om, "object-added",
                         G_CALLBACK(drives_changed_cb), a);
        g_signal_connect(om, "object-removed",
                         G_CALLBACK(drives_changed_cb), a);
        g_signal_connect(om, "interface-added",
                         G_CALLBACK(drives_iface_cb), a);
        g_signal_connect(om, "interface-removed",
                         G_CALLBACK(drives_iface_cb), a);
        g_signal_connect(om, "interface-proxy-properties-changed",
                         G_CALLBACK(drives_props_changed_cb), a);
    }
    /* UDisksClient may already have queued changes before we connected. */
    a->drives_dirty = 1;
}

static void drive_refresh(App *a)
{
    a->n_drives = 0;
    GDBusObjectManager *om = a->udisks ?
                             udisks_client_get_object_manager(a->udisks) : NULL;
    GList *objects = om ? g_dbus_object_manager_get_objects(om) : NULL;

    for (GList *l = objects; l && a->n_drives < 48; l = l->next) {
        UDisksObject *obj = UDISKS_OBJECT(l->data);
        UDisksBlock *block = udisks_object_peek_block(obj);
        if (!block || udisks_block_get_hint_ignore(block)) continue;
        if (udisks_object_peek_loop(obj) || udisks_object_peek_swapspace(obj))
            continue;

        const char *dev = udisks_block_get_device(block);
        if (!dev || !*dev || dev[0] != '/') continue;

        const char *fstype = udisks_block_get_id_type(block);
        if (fstype && !strcmp(fstype, "swap")) continue;

        UDisksFilesystem *fs = udisks_object_peek_filesystem(obj);
        int is_part = udisks_object_peek_partition(obj) != NULL;
        int is_volume = udisks_object_peek_encrypted(obj) != NULL ||
                        udisks_object_peek_mdraid(obj) != NULL;

        UDisksDrive *drive = a->udisks ?
                             udisks_client_get_drive_for_block(a->udisks, block) : NULL;
        int removable = drive && (udisks_drive_get_removable(drive) ||
                                  udisks_drive_get_media_removable(drive));
        int is_optical = drive && udisks_drive_get_optical(drive);
        if (strstr(dev, "/sr") || strstr(dev, "/cdrom")) is_optical = 1;

        /* Keep every volume (mounted or not).  A whole disk is only useful
         * here if it is removable/optical or holds a filesystem directly. */
        if (!is_part && !is_volume && !fs && !(removable || is_optical)) {
            if (drive) g_object_unref(drive);
            continue;
        }

        const char *const *mps = fs ? udisks_filesystem_get_mount_points(fs) : NULL;
        const char *mount = mps && mps[0] ? mps[0] : "";
        int mounted = mount[0] != '\0';

        Drive *d = &a->drives[a->n_drives++];
        memset(d, 0, sizeof *d);
        snprintf(d->dev, sizeof d->dev, "%s", dev);
        snprintf(d->object, sizeof d->object, "%s",
                 g_dbus_proxy_get_object_path(G_DBUS_PROXY(block)));
        if (drive)
            snprintf(d->drive_object, sizeof d->drive_object, "%s",
                     g_dbus_proxy_get_object_path(G_DBUS_PROXY(drive)));

        const char *label = udisks_block_get_id_label(block);
        snprintf(d->label, sizeof d->label, "%s", label ? label : "");
        if (!d->label[0] && mounted && strcmp(mount, "/"))
            snprintf(d->label, sizeof d->label, "%s", base_name(mount));
        if (!d->label[0])
            snprintf(d->label, sizeof d->label, "%s",
                     removable ? "Removable Disk" : "Local Disk");

        snprintf(d->mount, sizeof d->mount, "%s", mount);
        snprintf(d->fstype, sizeof d->fstype, "%s", fstype ? fstype : "");
        if (drive) {
            const char *model = udisks_drive_get_model(drive);
            const char *vendor = udisks_drive_get_vendor(drive);
            snprintf(d->model, sizeof d->model, "%s",
                     model && *model ? model : (vendor ? vendor : ""));
            g_object_unref(drive);
        }
        char size_buf[64];
        fmt_size(size_buf, sizeof size_buf,
                 (off_t)udisks_block_get_size(block));
        snprintf(d->size, sizeof d->size, "%s", size_buf);
        d->removable = removable;
        d->is_optical = is_optical;
        d->mounted = mounted;
        if (d->is_optical)
            snprintf(d->icon, sizeof d->icon, "%s", "drive-optical");
        else if (d->removable)
            snprintf(d->icon, sizeof d->icon, "%s", "drive-removable-media-usb");
        else
            snprintf(d->icon, sizeof d->icon, "%s", "drive-harddisk");

        if (mounted) {
            struct statvfs sv;
            if (statvfs(mount, &sv) == 0 && sv.f_blocks > 0) {
                d->used = 1.0 - (double)sv.f_bavail / (double)sv.f_blocks;
                char buf[64];
                fmt_size(buf, sizeof buf,
                         (off_t)sv.f_bavail * (off_t)sv.f_frsize);
                snprintf(d->avail, sizeof d->avail, "%s free", buf);
            }
        }
    }
    if (objects) g_list_free_full(objects, g_object_unref);

    /* Sidebar highlight is stored by device, not by row number. */
    a->sidebar_drive = -1;
    if (a->sidebar_drive_dev[0]) {
        for (int i = 0; i < a->n_drives; i++) {
            if (!strcmp(a->drives[i].dev, a->sidebar_drive_dev)) {
                a->sidebar_drive = i;
                break;
            }
        }
        if (a->ctx_kind == CTX_DRIVE) a->ctx_index = a->sidebar_drive;
    }
    a->dirty = 1;
}

static void job_release(Job *j)
{
    if (j->proxy) { g_object_unref(j->proxy); j->proxy = NULL; }
    if (j->cancel) { g_object_unref(j->cancel); j->cancel = NULL; }
    j->active = 0;
    j->done = 0;
}

static void drive_op_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Job *j = data;
    GError *err = NULL;
    j->code = 1;

    if (j->op == 0) {
        gchar *path = NULL;
        if (udisks_filesystem_call_mount_finish(UDISKS_FILESYSTEM(source),
                                                &path, result, &err)) {
            j->code = 0;
            if (path) {
                snprintf(j->mount, sizeof j->mount, "%s", path);
                snprintf(j->result, sizeof j->result, "Mounted %s at %s",
                         j->dev, path);
            } else {
                snprintf(j->result, sizeof j->result, "Mounted %s", j->dev);
            }
        }
        if (path) g_free(path);
    } else if (j->op == 1) {
        if (udisks_filesystem_call_unmount_finish(UDISKS_FILESYSTEM(source),
                                                  result, &err)) {
            j->code = 0;
            snprintf(j->result, sizeof j->result, "Unmounted %s", j->dev);
        }
    } else {
        if (udisks_drive_call_eject_finish(UDISKS_DRIVE(source),
                                           result, &err)) {
            j->code = 0;
            snprintf(j->result, sizeof j->result, "Ejected %s", j->dev);
        }
    }

    if (err) {
        snprintf(j->result, sizeof j->result, "%s", err->message);
        g_error_free(err);
    }
    j->active = 0;
    j->done = 1;
    if (j->app) {
        j->app->drives_dirty = 1;
        j->app->dirty = 1;
    }
}

static void job_finish(App *a, Job *j)
{
    char msg[512];
    snprintf(msg, sizeof msg, "%s", j->result);
    if (!msg[0]) snprintf(msg, sizeof msg, "%s", j->code ? "Failed" : "Done");
    status_set(a, "%s: %s", j->dev, msg);
    drive_refresh(a);

    /* Polkit may hold the mount call for a while. Once udisksd confirms it,
     * open the drive without making the user click it a second time. */
    if (j->op == 0 && j->code == 0) {
        char path[PATH_MAX] = "";
        for (int i = 0; i < a->n_drives; i++) {
            Drive *d = &a->drives[i];
            if (d->object[0] && j->object[0] && !strcmp(d->object, j->object) &&
                d->mounted && d->mount[0]) {
                snprintf(path, sizeof path, "%s", d->mount);
                break;
            }
        }
        if (!path[0] && j->mount[0])
            snprintf(path, sizeof path, "%s", j->mount);
        struct stat st;
        if (path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            navigate(a, path, 1);
    }
    job_release(j);
}

static void drive_job(App *a, Drive *d, int op)
{
    if (!a->udisks) { status_set(a, "udisks2 is unavailable"); return; }

    for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++) {
        Job *j = &a->jobs[i];
        if (j->active && d->object[0] && !strcmp(j->object, d->object)) {
            status_set(a, "Already working on %s", d->dev);
            return;
        }
    }
    int idx = -1;
    for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++) {
        if (!a->jobs[i].active && !a->jobs[i].proxy) { idx = (int)i; break; }
    }
    if (idx < 0) { status_set(a, "Too many pending drive operations"); return; }

    Job *j = &a->jobs[idx];
    memset(j, 0, sizeof *j);
    j->app = a;
    j->op = op;
    snprintf(j->dev, sizeof j->dev, "%s", d->dev);
    snprintf(j->object, sizeof j->object, "%s", d->object);
    snprintf(j->drive_object, sizeof j->drive_object, "%s", d->drive_object);

    /* The proxy refs are passed to the callbacks via j->proxy and held until
     * job_finish(), so the object stays alive while polkit is prompting. */
    if (op == 0 || op == 1) {
        UDisksObject *obj = udisks_client_get_object(a->udisks, d->object);
        if (obj) {
            j->proxy = udisks_object_get_filesystem(obj);
            g_object_unref(obj);
        }
        if (!j->proxy) {
            status_set(a, "%s has no filesystem to %s", d->dev,
                       op == 0 ? "mount" : "unmount");
            return;
        }
    } else {
        UDisksObject *obj = udisks_client_get_object(a->udisks,
                                                      d->drive_object);
        if (obj) {
            j->proxy = udisks_object_get_drive(obj);
            g_object_unref(obj);
        }
        if (!j->proxy) {
            status_set(a, "%s cannot be ejected", d->dev);
            return;
        }
    }

    j->cancel = g_cancellable_new();
    j->active = 1;
    GVariant *opts = g_variant_new("a{sv}", NULL); /* floating, consumed by call */
    status_set(a, "%s %s…", op == 0 ? "Mounting" :
               (op == 1 ? "Unmounting" : "Ejecting"), d->dev);

    if (op == 0)
        udisks_filesystem_call_mount(UDISKS_FILESYSTEM(j->proxy), opts,
                                     j->cancel, drive_op_done, j);
    else if (op == 1)
        udisks_filesystem_call_unmount(UDISKS_FILESYSTEM(j->proxy), opts,
                                       j->cancel, drive_op_done, j);
    else
        udisks_drive_call_eject(UDISKS_DRIVE(j->proxy), opts,
                                j->cancel, drive_op_done, j);
}

static void jobs_dispatch(App *a)
{
    for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++) {
        Job *j = &a->jobs[i];
        if (j->active && j->done) job_finish(a, j);
    }
}

/* ---- generic background launch ------------------------------------ */

static void spawn_detached(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
}

static void open_path(const char *path)
{
    const char *av[] = { "xdg-open", path, NULL };
    spawn_detached(av);
}

/* ------------------------------------------------------------------ */
/*  File operations                                                    */
/* ------------------------------------------------------------------ */

static int path_unique(const char *cwd, const char *src,
                       char *out, size_t n, int copy)
{
    const char *base = base_name(src);
    snprintf(out, n, "%s/%s", cwd, base);
    struct stat st;
    if (stat(out, &st) != 0) return 0;
    const char *dot = strrchr(base, '.');
    size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    const char *ext = dot && dot != base ? dot : "";
    for (int i = 2; i < 1000; i++) {
        if (copy && i == 2) snprintf(out, n, "%s/%.*s (copy)%s",
                                     cwd, (int)stem, base, ext);
        else snprintf(out, n, "%s/%.*s (%d)%s",
                      cwd, (int)stem, base, i, ext);
        if (stat(out, &st) != 0) return 0;
    }
    return -1;
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[PATH_MAX];
            path_join(p, sizeof p, path, de->d_name);
            remove_tree(p);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static int selected_indices(App *a, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < a->n_ents && n < max; i++)
        if (a->selected[i]) out[n++] = i;
    if (n == 0 && a->sel >= 0 && a->sel < a->n_ents) { out[0] = a->sel; n = 1; }
    return n;
}

static int mod_active(App *a, const char *name)
{
    return a->xkb_state &&
           xkb_state_mod_name_is_active(a->xkb_state, name,
                                        XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void select_none(App *a)
{
    post_xfer_disarm(a);
    if (a->selected && a->n_ents > 0)
        memset(a->selected, 0, (size_t)a->n_ents);
    a->n_selected = 0;
}

static void select_one(App *a, int idx)
{
    if (!a->selected || idx < 0 || idx >= a->n_ents) return;
    select_none(a);
    a->selected[idx] = 1;
    a->n_selected = 1;
    a->sel = idx;
    a->anchor = idx;
}

static void select_toggle(App *a, int idx)
{
    post_xfer_disarm(a);
    if (!a->selected || idx < 0 || idx >= a->n_ents) return;
    if (a->selected[idx]) {
        a->selected[idx] = 0;
        if (a->n_selected > 0) a->n_selected--;
    } else {
        a->selected[idx] = 1;
        a->n_selected++;
    }
    a->sel = idx;
    a->anchor = idx;
}

static void select_range(App *a, int idx)
{
    if (!a->selected || idx < 0 || idx >= a->n_ents) return;
    int from = a->anchor >= 0 && a->anchor < a->n_ents ? a->anchor : idx;
    int lo = from < idx ? from : idx;
    int hi = from > idx ? from : idx;
    select_none(a);
    for (int i = lo; i <= hi; i++) {
        a->selected[i] = 1;
        a->n_selected++;
    }
    a->sel = idx;
}

static void xfer_window_open(App *a);
static void xfer_window_schedule(App *a);
static void xfer_window_maybe_open(App *a);
static void xfer_window_close(App *a);
static void xfer_draw(App *a);
static void xfer_poll(App *a);
static void dialog_window_open(App *a);
static void dialog_window_close(App *a);
static void dialog_window_draw(App *a);
static void dialog_clear(App *a);
static void dialog_open(App *a, int type, const char *title,
                        const char *text, const char *path);

typedef struct {
    int fd;
    long long items, bytes;
} XferCtx;

static void xfer_sendf(int fd, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    line[n] = '\n';
    ssize_t ignored = write(fd, line, (size_t)n + 1);
    (void)ignored;
}

static void xfer_scan(const char *path, long long *items, long long *bytes)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    (*items)++;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[PATH_MAX];
            path_join(p, sizeof p, path, de->d_name);
            xfer_scan(p, items, bytes);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        *bytes += (long long)st.st_size;
    }
}

static void xfer_name(char *out, size_t n, const char *name)
{
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < n; p++)
        out[k++] = (unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E ?
                   '_' : *p;
    out[k] = '\0';
}

static int xfer_copy_file(const char *src, const char *dst, XferCtx *c)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   st.st_mode & 0777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    long long since_report = 0, wrote = 0;
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) { close(in); close(out); return -1; }
            off += w;
            wrote += (long long)w;
            c->bytes += (long long)w;
            since_report += (long long)w;
        }
        if (since_report >= 1048576) {
            xfer_sendf(c->fd, "B %lld", c->bytes);
            since_report = 0;
        }
    }
    int bad = r < 0;
    close(in);
    close(out);
    if (bad) return -1;
    (void)wrote;
    c->items++;
    xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
    return 0;
}

static int xfer_copy_tree(const char *src, const char *dst, XferCtx *c)
{
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = '\0';
        if (symlink(target, dst) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *de;
        int bad = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char s[PATH_MAX], t[PATH_MAX];
            path_join(s, sizeof s, src, de->d_name);
            path_join(t, sizeof t, dst, de->d_name);
            if (xfer_copy_tree(s, t, c) != 0) bad = 1;
        }
        closedir(d);
        return bad ? -1 : 0;
    }
    return xfer_copy_file(src, dst, c);
}

/* The copy child re-execs this binary with --xfer-child, so it never inherits
 * locked allocator state from the thumbnail worker threads. The request (cut
 * flag and path pairs) is length-framed over stdin; progress goes to stdout. */
static int xfer_write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (w == 0) return 0;
        p += w;
        n -= (size_t)w;
    }
    return 1;
}

static int xfer_write_u32(int fd, uint32_t v)
{
    return xfer_write_all(fd, &v, sizeof v);
}

static int xfer_read_path(char *out, size_t cap)
{
    uint32_t len;
    if (fread(&len, sizeof len, 1, stdin) != 1 || len == 0 || len > cap)
        return 0;
    return fread(out, 1, len, stdin) == len;
}

/* Runs in the forked/exec'd child. Only talks to the parent through pipes. */
static void xfer_child(int fd, int cut,
                       char (*srcs)[PATH_MAX], char (*dsts)[PATH_MAX], int n)
{
    long long item_bytes[64] = { 0 };
    long long total_items = 0, total_bytes = 0;
    for (int i = 0; i < n; i++) {
        xfer_scan(srcs[i], &total_items, &item_bytes[i]);
        total_bytes += item_bytes[i];
    }
    xfer_sendf(fd, "T %lld %lld", total_items, total_bytes);

    XferCtx ctx = { .fd = fd };
    int done = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        char name[NAME_MAX + 1];
        xfer_name(name, sizeof name, base_name(dsts[i]));
        xfer_sendf(fd, "N %s", name);

        if (cut && rename(srcs[i], dsts[i]) == 0) {
            ctx.items++;
            ctx.bytes += item_bytes[i];
            xfer_sendf(fd, "P %lld %lld", ctx.items, ctx.bytes);
            done++;
            continue;
        }
        if (xfer_copy_tree(srcs[i], dsts[i], &ctx) == 0 &&
            (!cut || remove_tree(srcs[i]) == 0)) {
            done++;
        } else {
            failed++;
        }
    }
    xfer_sendf(fd, "D %d %d", done, failed);
    _exit(failed ? 1 : 0);
}

/* Progress-aware removal for the permanent-delete child. Directories are
 * counted after rmdir() succeeds, so P never reports work that failed. */
static int xfer_delete_tree(const char *path, XferCtx *c)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *de;
        int bad = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[PATH_MAX];
            path_join(child, sizeof child, path, de->d_name);
            if (xfer_delete_tree(child, c) != 0) bad = 1;
        }
        closedir(d);
        if (rmdir(path) != 0) return -1;
        c->items++;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
        return bad ? -1 : 0;
    }
    if (unlink(path) != 0) return -1;
    c->items++;
    if (S_ISREG(st.st_mode)) c->bytes += (long long)st.st_size;
    xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
    return 0;
}

static char *url_encode(const char *s)
{
    size_t cap = strlen(s) * 3 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    for (const char *p = s; *p; p++) {
        if (isalnum((unsigned char)*p) || strchr("/-_.~", *p)) out[k++] = *p;
        else {
            snprintf(out + k, cap - k, "%%%02X", (unsigned char)*p);
            k += 3;
        }
    }
    out[k] = '\0';
    return out;
}

/* Resolve the XDG Trash directories. Honors $XDG_DATA_HOME, falling back to
 * ~/.local/share. Both the sidebar Trash entry and delete use this so the
 * navigation target and the delete target can never diverge. */
static int trash_dirs(char *files, size_t files_cap,
                      char *info, size_t info_cap)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    const char *xdg = getenv("XDG_DATA_HOME");
    char base[PATH_MAX];
    if (xdg && *xdg == '/') snprintf(base, sizeof base, "%s", xdg);
    else snprintf(base, sizeof base, "%s/.local/share", home);
    if (files) snprintf(files, files_cap, "%s/Trash/files", base);
    if (info) snprintf(info, info_cap, "%s/Trash/info", base);
    return 0;
}

static int trash_files_dir(char *out, size_t cap)
{
    return trash_dirs(out, cap, NULL, 0);
}

static int trash_mkdir_p(const char *path);

static int trash_ensure(void)
{
    char files[PATH_MAX], info[PATH_MAX];
    if (trash_dirs(files, sizeof files, info, sizeof info) != 0) return -1;
    if (trash_mkdir_p(files) != 0 || trash_mkdir_p(info) != 0) return -1;
    return 0;
}

/* Minimal mkdir -p (leaf included) with private 0700 permissions. */
static int trash_mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (!len || len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) { *p = '/'; return -1; }
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int trash_remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    int bad = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[PATH_MAX];
        path_join(child, sizeof child, path, de->d_name);
        if (trash_remove_tree(child) != 0) bad = 1;
    }
    closedir(d);
    if (rmdir(path) != 0) bad = 1;
    return bad ? -1 : 0;
}

/* Runs in the exec'd filesystem child, never on the UI thread. When Trash is
 * on another filesystem, xfer_copy_tree() reports progress through the same
 * pipe the copy/move worker uses, so a 100 MB delete no longer freezes the
 * window. */
static int trash_path_ex(const char *path, XferCtx *c,
                         long long item_count, long long item_bytes)
{
    char files[PATH_MAX], info[PATH_MAX];
    if (trash_dirs(files, sizeof files, info, sizeof info) != 0) return -1;
    if (trash_mkdir_p(files) != 0 || trash_mkdir_p(info) != 0) return -1;

    /* Keep the lexical path (important for symlinks) rather than realpath's
     * target, so a restored item lands where it originally lived. */
    char abs[PATH_MAX];
    if (path[0] == '/') {
        snprintf(abs, sizeof abs, "%s", path);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) path_join(abs, sizeof abs, cwd, path);
        else snprintf(abs, sizeof abs, "%s", path);
    }
    const char *base = base_name(abs);
    char dest[PATH_MAX], meta[PATH_MAX];
    for (int i = 0; i < 1000; i++) {
        if (i == 0) snprintf(dest, sizeof dest, "%s/%s", files, base);
        else snprintf(dest, sizeof dest, "%s/%s.%d", files, base, i);
        if (access(dest, F_OK) != 0) break;
    }
    if (rename(path, dest) != 0) {
        if (errno != EXDEV) return -1;
        if (xfer_copy_tree(path, dest, c) != 0) {
            trash_remove_tree(dest);
            return -1;
        }
        if (trash_remove_tree(path) != 0) return -1;
    } else {
        c->items += item_count;
        c->bytes += item_bytes;
        xfer_sendf(c->fd, "P %lld %lld", c->items, c->bytes);
    }

    snprintf(meta, sizeof meta, "%s/%s.trashinfo", info, base_name(dest));
    FILE *f = fopen(meta, "w");
    if (!f) {
        trash_remove_tree(dest);   /* stranded file without metadata */
        return -1;
    }
    char *enc = url_encode(abs);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%04d-%02d-%02dT%02d:%02d:%02d\n",
            enc ? enc : abs, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    free(enc);
    if (fclose(f) != 0) {
        unlink(meta);
        trash_remove_tree(dest);
        return -1;
    }
    return 0;
}

/* --fs-child request: u32 op (XOP_TRASH/XOP_DELETE), u32 count, then
 * length-framed source paths. Progress/result lines match --xfer-child. */
static void fs_child(int fd, int op, char (*paths)[PATH_MAX], int n)
{
    long long counts[64] = { 0 }, sizes[64] = { 0 };
    long long total_items = 0, total_bytes = 0;
    for (int i = 0; i < n; i++) {
        xfer_scan(paths[i], &counts[i], &sizes[i]);
        total_items += counts[i];
        total_bytes += sizes[i];
    }
    xfer_sendf(fd, "T %lld %lld", total_items, total_bytes);

    XferCtx ctx = { .fd = fd };
    int done = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        char name[NAME_MAX + 1];
        xfer_name(name, sizeof name, base_name(paths[i]));
        xfer_sendf(fd, "N %s", name);
        int ok = op == XOP_TRASH
            ? trash_path_ex(paths[i], &ctx, counts[i], sizes[i]) == 0
            : xfer_delete_tree(paths[i], &ctx) == 0;
        if (ok) done++;
        else failed++;
    }
    xfer_sendf(fd, "D %d %d", done, failed);
    _exit(failed ? 1 : 0);
}

static void path_dirname(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, n, ".");
    } else if (slash == path) {
        snprintf(out, n, "/");
    } else {
        size_t len = (size_t)(slash - path);
        if (len >= n) len = n - 1;
        memcpy(out, path, len);
        out[len] = '\0';
    }
}

static void do_paste(App *a, const char *dest_dir)
{
    if (a->n_clip <= 0) { status_set(a, "Clipboard is empty"); return; }
    if (a->xfer.active) {
        status_set(a, "Another transfer is already running");
        return;
    }
    /* An internal drop can target a different folder without changing cwd. */
    const char *dir = dest_dir && *dest_dir ? dest_dir : a->cwd;

    int n = a->n_clip;
    if (n > 64) n = 64;
    char (*srcs)[PATH_MAX] = calloc((size_t)n, sizeof *srcs);
    char (*dsts)[PATH_MAX] = calloc((size_t)n, sizeof *dsts);
    if (!srcs || !dsts) {
        free(srcs); free(dsts);
        status_set(a, "Not enough memory to start transfer");
        return;
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        snprintf(srcs[i], PATH_MAX, "%s", a->clipboard[i]);
        if (path_unique(dir, srcs[i], dsts[i], PATH_MAX,
                        !a->clip_cut) != 0)
            bad++;
    }
    if (bad) {
        free(srcs); free(dsts);
        status_set(a, "Cannot choose a destination name");
        return;
    }

    int p[2], q[2];
    if (pipe2(p, O_CLOEXEC) < 0) {
        free(srcs); free(dsts);
        status_set(a, "pipe failed: %s", strerror(errno));
        return;
    }
    if (pipe2(q, O_CLOEXEC) < 0) {
        close(p[0]); close(p[1]);
        free(srcs); free(dsts);
        status_set(a, "pipe failed: %s", strerror(errno));
        return;
    }
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    int cut = a->clip_cut;
    char src_dir[PATH_MAX], dst_dir[PATH_MAX];
    path_dirname(srcs[0], src_dir, sizeof src_dir);
    path_dirname(dsts[0], dst_dir, sizeof dst_dir);
    post_xfer_arm(a, dst_dir, dsts, n);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        close(q[1]);
        if (dup2(q[0], STDIN_FILENO) < 0) _exit(127);
        if (q[0] != STDIN_FILENO) close(q[0]);
        /* Progress must go to the pipe the parent polls, not to the
         * inherited stdout. Without this the window never sees T/N/B/P. */
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127);
        if (p[1] != STDOUT_FILENO) close(p[1]);
        char *av[] = { (char *)"/proc/self/exe", (char *)"--xfer-child", NULL };
        execv(av[0], av);
        _exit(127);
    }
    close(p[1]);
    close(q[0]);
    if (pid < 0) {
        post_xfer_disarm(a);
        close(q[1]);
        close(p[0]);
        free(srcs);
        free(dsts);
        status_set(a, "fork failed: %s", strerror(errno));
        return;
    }
    int sent = xfer_write_u32(q[1], (uint32_t)cut) &&
               xfer_write_u32(q[1], (uint32_t)n);
    for (int i = 0; sent && i < n; i++) {
        uint32_t sl = (uint32_t)strlen(srcs[i]) + 1;
        uint32_t dl = (uint32_t)strlen(dsts[i]) + 1;
        sent = xfer_write_u32(q[1], sl) &&
               xfer_write_all(q[1], srcs[i], sl) &&
               xfer_write_u32(q[1], dl) &&
               xfer_write_all(q[1], dsts[i], dl);
    }
    close(q[1]);
    free(srcs);
    free(dsts);
    if (!sent) {
        post_xfer_disarm(a);
        close(p[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        status_set(a, "Cannot start transfer");
        return;
    }

    Transfer *x = &a->xfer;
    memset(x, 0, sizeof *x);
    x->pid = pid;
    x->fd = p[0];
    x->app = a;
    x->active = 1;
    x->cut = cut;
    x->op = cut ? XOP_MOVE : XOP_COPY;
    snprintf(x->src_dir, sizeof x->src_dir, "%s", src_dir);
    snprintf(x->dst_dir, sizeof x->dst_dir, "%s", dst_dir);
    x->w = 520;
    x->h = 220;
    snprintf(x->current, sizeof x->current, "Scanning...");
    xfer_window_schedule(a);
    status_set(a, "%s %d item%s", cut ? "Moving" : "Copying", n,
               n == 1 ? "" : "s");
    a->dirty = 1;
}

/* Start a trash or permanent-delete operation in a re-exec'd child. It uses
 * the exact same pipe/progress window as copy/paste, so the UI stays live. */
static void fs_op_start(App *a, int op, char (*paths)[PATH_MAX], int n)
{
    if (n <= 0) return;
    if (a->xfer.active) {
        status_set(a, "Another operation is already running");
        return;
    }

    int p[2] = { -1, -1 }, q[2] = { -1, -1 };
    if (pipe2(p, O_CLOEXEC) < 0 || pipe2(q, O_CLOEXEC) < 0) {
        for (int i = 0; i < 2; i++) {
            if (p[i] >= 0) close(p[i]);
            if (q[i] >= 0) close(q[i]);
        }
        status_set(a, "pipe failed: %s", strerror(errno));
        return;
    }
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        close(q[1]);
        if (dup2(q[0], STDIN_FILENO) < 0) _exit(127);
        if (q[0] != STDIN_FILENO) close(q[0]);
        /* Progress must go to the pipe the parent polls, not to the
         * inherited stdout. Without this the window never sees T/N/B/P. */
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127);
        if (p[1] != STDOUT_FILENO) close(p[1]);
        char *av[] = { (char *)"/proc/self/exe", (char *)"--fs-child", NULL };
        execv(av[0], av);
        _exit(127);
    }
    close(p[1]);
    close(q[0]);
    if (pid < 0) {
        close(q[1]);
        close(p[0]);
        status_set(a, "fork failed: %s", strerror(errno));
        return;
    }

    int sent = xfer_write_u32(q[1], (uint32_t)op) &&
               xfer_write_u32(q[1], (uint32_t)n);
    for (int i = 0; sent && i < n; i++) {
        uint32_t len = (uint32_t)strlen(paths[i]) + 1;
        sent = xfer_write_u32(q[1], len) && xfer_write_all(q[1], paths[i], len);
    }
    close(q[1]);
    if (!sent) {
        close(p[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        status_set(a, "Cannot start operation");
        return;
    }

    Transfer *x = &a->xfer;
    memset(x, 0, sizeof *x);
    x->pid = pid;
    x->fd = p[0];
    x->app = a;
    x->active = 1;
    x->op = op;
    path_dirname(paths[0], x->src_dir, sizeof x->src_dir);
    x->w = 520;
    x->h = 220;
    snprintf(x->current, sizeof x->current, "Scanning...");
    xfer_window_schedule(a);
    status_set(a, "%s %d item%s", op == XOP_TRASH ?
               "Moving to Trash" : "Permanently deleting", n,
               n == 1 ? "" : "s");
    a->dirty = 1;
}

static char g_delete_paths[64][PATH_MAX];
static int g_delete_n;

static void dialog_clear(App *a)
{
    if (!a->dialog.type && !a->dlg.surface) return;
    dialog_window_close(a);
    memset(&a->dialog, 0, sizeof a->dialog);
    g_delete_n = 0;
    a->dirty = 1;
}

static void do_delete(App *a)
{
    int idx[512];
    int n = selected_indices(a, idx, 512);
    if (n == 0) return;
    if (n > 64) n = 64;
    char (*paths)[PATH_MAX] = calloc((size_t)n, sizeof *paths);
    if (!paths) { status_set(a, "Not enough memory"); return; }
    for (int i = 0; i < n; i++)
        snprintf(paths[i], PATH_MAX, "%s", a->ents[idx[i]].path);
    fs_op_start(a, XOP_TRASH, paths, n);
    free(paths);
}

static void do_delete_permanent(App *a)
{
    int idx[512];
    int n = selected_indices(a, idx, 512);
    if (n == 0) return;
    if (a->xfer.active) {
        status_set(a, "Another operation is already running");
        return;
    }
    g_delete_n = 0;
    for (int i = 0; i < n && g_delete_n < 64; i++)
        snprintf(g_delete_paths[g_delete_n++], PATH_MAX, "%s",
                 a->ents[idx[i]].path);

    char msg[512];
    if (g_delete_n == 1)
        snprintf(msg, sizeof msg,
                 "Permanently delete \u201c%s\u201d? This cannot be undone.",
                 base_name(g_delete_paths[0]));
    else
        snprintf(msg, sizeof msg,
                 "Permanently delete %d items? This cannot be undone.",
                 g_delete_n);
    dialog_open(a, 4, "Delete Permanently", msg, NULL);
}

static void dialog_open(App *a, int type, const char *title,
                        const char *text, const char *path)
{
    memset(&a->dialog, 0, sizeof a->dialog);
    if (type != 4) g_delete_n = 0;
    a->dialog.type = type;
    snprintf(a->dialog.title, sizeof a->dialog.title, "%s", title);
    snprintf(a->dialog.text, sizeof a->dialog.text, "%s", text ? text : "");
    if (path) snprintf(a->dialog.path, sizeof a->dialog.path, "%s", path);
    a->dialog.cursor = (int)strlen(a->dialog.text);
    a->dirty = 1;
    dialog_window_open(a);
}

static void do_rename(App *a, const char *path)
{
    char title[128];
    snprintf(title, sizeof title, "Rename %s", base_name(path));
    dialog_open(a, 1, title, base_name(path), path);
}

static void dialog_commit(App *a)
{
    Dialog *d = &a->dialog;
    int reload = d->type == 1 || d->type == 2;
    if (d->type == 4) {
        if (g_delete_n > 0)
            fs_op_start(a, XOP_DELETE, g_delete_paths, g_delete_n);
        g_delete_n = 0;
    } else if (d->type == 1 && d->path[0] && d->text[0]) {
        char parent[PATH_MAX], dst[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", d->path);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0'; else snprintf(parent, sizeof parent, ".");
        path_join(dst, sizeof dst, parent, d->text);
        if (rename(d->path, dst) != 0)
            status_set(a, "Cannot rename: %s", strerror(errno));
        else status_set(a, "Renamed to %s", d->text);
    } else if (d->type == 2 && d->text[0]) {
        char p[PATH_MAX];
        path_join(p, sizeof p, a->cwd, d->text);
        FILE *f = fopen(p, "w");
        if (f) { fclose(f); status_set(a, "Created %s", d->text); }
        else status_set(a, "Cannot create %s: %s", d->text, strerror(errno));
    }
    dialog_clear(a);
    if (reload) dir_load(a, a->cwd);
    a->dirty = 1;
}

static void do_new_item(App *a, int kind)
{
    const char *name = kind == A_NEW_FOLDER ? "New folder" :
                       kind == A_NEW_ZIP ? "New zip folder.zip" :
                       "New Text Document.txt";
    char p[PATH_MAX];
    path_join(p, sizeof p, a->cwd, name);
    if (kind == A_NEW_FOLDER || kind == A_NEW_ZIP) {
        if (mkdir(p, 0755) != 0) { status_set(a, "Cannot create folder: %s", strerror(errno)); return; }
        dir_load(a, a->cwd);
        do_rename(a, p);
    } else {
        FILE *f = fopen(p, "w");
        if (!f) { status_set(a, "Cannot create file: %s", strerror(errno)); return; }
        fclose(f);
        dir_load(a, a->cwd);
        do_rename(a, p);
    }
}

static void do_shortcut(App *a, const char *path)
{
    char name[PATH_MAX], desktop[PATH_MAX];
    snprintf(name, sizeof name, "Shortcut to %s.desktop", base_name(path));
    path_join(desktop, sizeof desktop, a->cwd, name);
    const char *icon = "text-x-generic";
    for (int i = 0; i < a->n_ents; i++)
        if (!strcmp(a->ents[i].path, path)) { icon = a->ents[i].icon; break; }
    FILE *f = fopen(desktop, "w");
    if (!f) { status_set(a, "Cannot create shortcut: %s", strerror(errno)); return; }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nIcon=%s\n"
               "Exec=gio open \"%s\"\nTerminal=false\n",
            base_name(path), icon, path);
    fclose(f);
    status_set(a, "Created %s", name);
    dir_load(a, a->cwd);
}

static void toggle_favorite(App *a, const char *path, int add)
{
    if (add) {
        for (int i = 0; i < a->n_favorites; i++)
            if (!strcmp(a->favorites[i], path)) return;
        if (a->n_favorites < 24) {
            snprintf(a->favorites[a->n_favorites++], PATH_MAX, "%s", path);
            status_set(a, "Pinned to Quick access");
        }
    } else {
        for (int i = 0; i < a->n_favorites; i++) {
            if (!strcmp(a->favorites[i], path)) {
                for (int k = i + 1; k < a->n_favorites; k++)
                    snprintf(a->favorites[k - 1], PATH_MAX, "%s", a->favorites[k]);
                a->n_favorites--;
                status_set(a, "Removed from Quick access");
                return;
            }
        }
    }
}

static int is_favorite(App *a, const char *path)
{
    for (int i = 0; i < a->n_favorites; i++)
        if (!strcmp(a->favorites[i], path)) return 1;
    return 0;
}

/* ---- Open with: applications from installed .desktop files --------- */

static const char *mime_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "text/plain";
    const char *e = dot + 1;
    struct { const char *ext, *mime; } m[] = {
        { "png", "image/png" }, { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "svg", "image/svg+xml" },
        { "mp4", "video/mp4" }, { "mkv", "video/x-matroska" },
        { "webm", "video/webm" }, { "mp3", "audio/mpeg" },
        { "flac", "audio/flac" }, { "wav", "audio/x-wav" },
        { "pdf", "application/pdf" }, { "txt", "text/plain" },
        { "md", "text/plain" }, { "html", "text/html" },
        { "zip", "application/zip" },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!strcasecmp(e, m[i].ext)) return m[i].mime;
    return "text/plain";
}

static void apps_load(App *a)
{
    a->n_apps = 0;
    const char *home = getenv("HOME");
    char dirs[3][PATH_MAX];
    int nd = 0;
    if (home) snprintf(dirs[nd++], sizeof dirs[0], "%s/.local/share/applications", home);
    snprintf(dirs[nd++], sizeof dirs[0], "/usr/local/share/applications");
    snprintf(dirs[nd++], sizeof dirs[0], "/usr/share/applications");
    int max = A_APP_MAX - A_APP_BASE;
    for (int k = 0; k < nd && a->n_apps < max; k++) {
        DIR *d = opendir(dirs[k]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && a->n_apps < max) {
            size_t dl = strlen(de->d_name);
            if (dl < 9 || strcmp(de->d_name + dl - 8, ".desktop")) continue;
            char path[PATH_MAX];
            path_join(path, sizeof path, dirs[k], de->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            char line[2048];
            char name[128] = "", exec[512] = "", icon[64] = "", mime[1024] = "";
            int skip = 0, seen_name = 0;
            while (fgets(line, sizeof line, f)) {
                if (line[0] == '[' && strncmp(line, "[Desktop Entry", 14)) break;
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                char *v = eq + 1;
                size_t vl = strlen(v);
                while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r')) v[--vl] = '\0';
                if (!strcmp(line, "Name") && !seen_name) {
                    snprintf(name, sizeof name, "%s", v); seen_name = 1;
                } else if (!strcmp(line, "Exec")) snprintf(exec, sizeof exec, "%s", v);
                else if (!strcmp(line, "Icon")) snprintf(icon, sizeof icon, "%s", v);
                else if (!strcmp(line, "MimeType")) snprintf(mime, sizeof mime, "%s", v);
                else if (!strcmp(line, "NoDisplay") || !strcmp(line, "Hidden"))
                    if (!strcmp(v, "true") || !strcmp(v, "1")) skip = 1;
            }
            fclose(f);
            if (skip || !name[0] || !exec[0]) continue;
            AppEnt *ap = &a->apps[a->n_apps++];
            memset(ap, 0, sizeof *ap);
            snprintf(ap->name, sizeof ap->name, "%s", name);
            snprintf(ap->exec, sizeof ap->exec, "%s", exec);
            snprintf(ap->icon, sizeof ap->icon, "%s", icon);
            snprintf(ap->id, sizeof ap->id, "%s", de->d_name);
            snprintf(ap->mime, sizeof ap->mime, "%s", mime);
        }
        closedir(d);
    }
}

static void launch_app(App *a, int index, const char *path)
{
    if (index < 0 || index >= a->n_apps) return;
    const char *src = a->apps[index].exec;
    char *argv[32];
    int n = 0;
    char word[512];
    int wi = 0, quote = 0;
    int substituted = 0;
    for (const char *p = src; ; p++) {
        if (*p == '"' || *p == '\'') { quote = quote ? 0 : *p; continue; }
        if (*p && (!isspace((unsigned char)*p) || quote)) {
            if (*p == '%' && p[1] && strchr("fuFU", p[1])) {
                if (wi == 0) {
                    snprintf(word, sizeof word, "%s", path);
                    wi = (int)strlen(word);
                    substituted = 1;
                }
                p++;
                continue;
            }
            if (wi < (int)sizeof word - 1) word[wi++] = *p;
        } else if (wi > 0) {
            word[wi] = '\0';
            if (n < 30) argv[n++] = strdup(word);
            wi = 0;
        }
        if (!*p) break;
    }
    if (wi > 0) {
        word[wi] = '\0';
        if (n < 30) argv[n++] = strdup(word);
    }
    if (!substituted && n < 30 && path) argv[n++] = strdup(path);
    argv[n] = NULL;
    spawn_detached((const char *const *)argv);
    for (int i = 0; i < n; i++) free(argv[i]);
    status_set(a, "Opening with %s", a->apps[index].name);
}

/* ------------------------------------------------------------------ */
/*  Menus                                                              */
/* ------------------------------------------------------------------ */

#define SUB_VIEW   (-1)
#define SUB_SORT   (-2)
#define SUB_GROUP  (-3)
#define SUB_NEW    (-4)
#define SUB_OPENWITH (-5)

static MenuItem *mitem(Menu *m, int kind, int id, const char *label,
                       const char *shortcut, int enabled)
{
    MenuItem *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->id = id;
    snprintf(it->label, sizeof it->label, "%s", label);
    snprintf(it->shortcut, sizeof it->shortcut, "%s", shortcut ? shortcut : "");
    it->enabled = enabled;
    return it;
}

static void msep(Menu *m) { mitem(m, 1, 0, "", "", 1); }

static int menu_item_y(const Menu *m, int idx)
{
    int y = 5;
    for (int i = 0; i < idx && i < m->n; i++)
        y += m->items[i].kind == 1 ? 9 : 26;
    /* Submenus anchor to the row as currently drawn, not its scrolled-away
     * position in content space. */
    y -= (int)m->scroll_v;
    if (y < 0) y = 0;
    if (y > m->h - 20) y = m->h - 20;
    return y;
}

static void menu_measure(App *a, Menu *m)
{
    double w = 150.0;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->items[i];
        if (it->kind == 1) continue;
        double x = 14.0 + text_width(a->measure, it->label, 13, 400, 0.0);
        if (it->icon[0]) x += 22.0;
        if (it->kind == 2) x += 22.0;
        else if (it->shortcut[0])
            x += 14.0 + text_width(a->measure, it->shortcut, 11, 400, 0.0);
        if (x > w) w = x;
    }

    int content = 10;
    for (int i = 0; i < m->n; i++) content += m->items[i].kind == 1 ? 9 : 26;
    m->content_h = content;

    /* Keep long menus (notably Open With with many apps) on screen. This is
     * the same idea as JTLab's app-list menu: show as many rows as fit, then
     * ease the rest into view as the wheel turns. */
    int max_h = 640;
    if (a->WIN_H > 0) {
        int room = (int)a->WIN_H - 48;
        if (room < max_h) max_h = room;
    }
    if (max_h < 120) max_h = 120;
    m->h = content < max_h ? content : max_h;
    m->max_scroll = content - m->h;
    if (m->max_scroll < 0) m->max_scroll = 0;
    if (m->max_scroll > 0) w += 12.0;   /* room for the scrollbar */
    m->w = (int)ceil(w + 14.0);

    m->scroll = 0;
    m->scroll_v = 0.0;
    m->scroll_t = mono_sec();
    m->scroll_prev = 0;
}

static void menu_scroll_by(Menu *m, int delta)
{
    if (!m || m->max_scroll <= 0) return;
    int next = m->scroll + delta;
    if (next < 0) next = 0;
    if (next > m->max_scroll) next = m->max_scroll;
    if (next == m->scroll) return;
    m->scroll = next;
    m->dirty = 1;   /* popup only; no need to repaint the whole main window */
}

static void menu_scroll_into_view(Menu *m, int idx)
{
    if (!m || m->max_scroll <= 0 || idx < 0 || idx >= m->n) return;
    int y0 = 5;
    for (int i = 0; i < idx; i++) y0 += m->items[i].kind == 1 ? 9 : 26;
    int y1 = y0 + (m->items[idx].kind == 1 ? 9 : 26);
    int top = m->scroll, bottom = top + m->h - 6;
    if (y0 < top + 6) menu_scroll_by(m, y0 - top - 6);
    else if (y1 > bottom) menu_scroll_by(m, y1 - bottom + 6);
}

/* Exponential approach toward the wheel target, ticked from the Wayland
 * event loop (no GLib timer here). Mirrors JTLab's scroll_anim behaviour. */
static int menu_scroll_tick(Menu *m)
{
    if (!m) return 0;
    int animating = 0;
    double now = mono_sec();
    double dt = now - m->scroll_t;
    m->scroll_t = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    if (m->scroll != m->scroll_prev) {
        int settled = fabs((double)m->scroll_prev - m->scroll_v) < 0.12;
        m->scroll_prev = m->scroll;
        if (settled) dt = 0.0;
    }
    double target = (double)m->scroll;
    if (!m->app->smooth_scroll || fabs(target - m->scroll_v) < 0.02) {
        if (m->scroll_v != target) {
            m->scroll_v = target;
            m->dirty = 1;
        }
    } else {
        /* ~45ms time constant, same feel as the file-list scroll. */
        double k = 1.0 - exp(-dt / 0.045);
        m->scroll_v += (target - m->scroll_v) * k;
        if (fabs(target - m->scroll_v) < 0.12) m->scroll_v = target;
        m->dirty = 1;
        animating = m->scroll_v != target;
    }
    for (int i = 0; i < m->n; i++)
        if (m->items[i].sub) animating |= menu_scroll_tick(m->items[i].sub);
    return animating;
}

static void popup_configure(void *data, struct xdg_popup *popup,
                            int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)popup; (void)x; (void)y; (void)w; (void)h;
    Menu *m = data;
    if (!m->configured && m->app && m->app->border_anim_enabled) {
        m->popup_anim_start = mono_sec();
        m->popup_anim_active = 1;
    }
    m->configured = 1;
    m->dirty = 1;
}

static void popup_done(void *data, struct xdg_popup *popup)
{
    (void)popup;
    Menu *m = data;
    m->done = 1;
    m->dirty = 1;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
};

static void popup_surface_configure(void *data, struct xdg_surface *xs,
                                    uint32_t serial)
{
    Menu *m = data;
    xdg_surface_ack_configure(xs, serial);
    if (m) {
        m->dirty = 1;
        if (m->app) m->app->dirty = 1;
    }
}

static const struct xdg_surface_listener popup_surface_listener = {
    .configure = popup_surface_configure,
};

static Menu *menu_create(App *a, Menu *parent, uint32_t serial,
                         int x, int y, MenuItem *items, int n)
{
    Menu *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->app = a;
    m->parent = parent;
    m->n = n > 40 ? 40 : n;
    if (items && n > 0) memcpy(m->items, items, (size_t)m->n * sizeof items[0]);
    m->anchor_x = x;
    m->anchor_y = y;
    menu_measure(a, m);

    m->surface = wl_compositor_create_surface(a->compositor);
    if (!m->surface) { free(m); return NULL; }
    m->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, m->surface);
    if (!m->xdg_surface) { wl_surface_destroy(m->surface); free(m); return NULL; }
    xdg_surface_add_listener(m->xdg_surface, &popup_surface_listener, m);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(a->wm_base);
    if (!pos) {
        xdg_surface_destroy(m->xdg_surface);
        wl_surface_destroy(m->surface);
        free(m);
        return NULL;
    }
    xdg_positioner_set_size(pos, m->w, m->h);
    if (parent) {
        xdg_positioner_set_anchor_rect(pos, 0, y, parent->w, 20);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_RIGHT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_RIGHT);
    } else {
        /* Windows 10 context menu: top-left corner sits at the cursor and
         * the menu grows down/right. TOP_LEFT gravity would grow it UP/left
         * (bottom-right at the cursor), which is why menus kept appearing
         * above the pointer. */
        xdg_positioner_set_anchor_rect(pos, x, y, 1, 1);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    }
    /* Let the compositor flip/slide at screen edges, like Windows does. */
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    /* A true xdg_popup (never a second toplevel), with a pointer grab. */
    m->popup = xdg_surface_get_popup(m->xdg_surface,
                                     parent ? parent->xdg_surface : a->xdg_surface,
                                     pos);
    xdg_positioner_destroy(pos);
    if (!m->popup) {
        xdg_surface_destroy(m->xdg_surface);
        wl_surface_destroy(m->surface);
        free(m);
        return NULL;
    }
    xdg_popup_add_listener(m->popup, &popup_listener, m);
    /* Set only after the popup role exists: setting window geometry before
       get_popup() makes the compositor raise "xdg_surface must have a role". */
    xdg_surface_set_window_geometry(m->xdg_surface, 0, 0, m->w, m->h);
    if (a->seat) xdg_popup_grab(m->popup, a->seat, serial);
    if (wl_surface_get_version(m->surface) >= 3 && a->scale > 0)
        wl_surface_set_buffer_scale(m->surface, a->scale);
    wl_surface_commit(m->surface);
    return m;
}

static void menu_close(Menu *m)
{
    if (!m) return;
    App *a = m->app;
    if (m == a->menu) a->menu = NULL;
    if (a->pointer_menu == m) a->pointer_menu = NULL;

    if (m->frame_cb) { wl_callback_destroy(m->frame_cb); m->frame_cb = NULL; }
    if (m->popup) { xdg_popup_destroy(m->popup); m->popup = NULL; }
    if (m->xdg_surface) { xdg_surface_destroy(m->xdg_surface); m->xdg_surface = NULL; }
    if (m->surface) {
        wl_surface_destroy(m->surface);
        m->surface = NULL;
        for (int i = 0; i < 2; i++) {
            ShmBuf *b = &m->bufs[i];
            if (!b->buffer) continue;
            if (b->busy) b->retired = 1;
            else shm_buffer_destroy(b);
        }
    }
    free(m);
}

static int menu_hit(const Menu *m, double x, double y)
{
    if (x < 0 || y < 0 || x > m->w || y > m->h) return -1;
    /* Hit testing follows what is drawn: convert the viewport y into
     * content-space y using the same animated offset as menu_paint(). */
    double vy = y + m->scroll_v;
    int cy = 5;
    for (int i = 0; i < m->n; i++) {
        int hh = m->items[i].kind == 1 ? 9 : 26;
        if (vy >= cy && vy < cy + hh)
            return m->items[i].kind == 1 ? -1 : i;
        cy += hh;
    }
    return -1;
}

static void menu_open_root(App *a, uint32_t serial, int x, int y);

static void menu_build_sub(App *a, Menu *parent, int idx, uint32_t serial)
{
    MenuItem items[40];
    int n = 0;
    int id = parent->items[idx].id;
    if (id == SUB_VIEW) {
        /* Build into a temporary menu just to reuse mitem(), then copy. */
        Menu tmp; memset(&tmp, 0, sizeof tmp);
        mitem(&tmp, 3, A_VIEW_TILES, "Large icons", "", 1)->checked = a->view == VIEW_TILES;
        mitem(&tmp, 3, A_VIEW_ICONS, "Medium icons", "", 1)->checked = a->view == VIEW_ICONS;
        mitem(&tmp, 3, A_VIEW_LIST, "Small icons", "", 1)->checked = a->view == VIEW_LIST;
        mitem(&tmp, 3, A_VIEW_DETAILS, "Details", "", 1)->checked = a->view == VIEW_DETAILS;
        msep(&tmp);
        mitem(&tmp, 3, A_SMOOTH_SCROLL, "Smooth scrolling", "", 1)->checked =
            a->smooth_scroll;
        n = tmp.n;
        memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    } else if (id == SUB_SORT) {
        Menu tmp; memset(&tmp, 0, sizeof tmp);
        mitem(&tmp, 3, A_SORT_NAME, "Name", "", 1)->checked = a->sort_by == SORT_NAME;
        mitem(&tmp, 3, A_SORT_DATE, "Date modified", "", 1)->checked = a->sort_by == SORT_DATE;
        mitem(&tmp, 3, A_SORT_TYPE, "Type", "", 1)->checked = a->sort_by == SORT_TYPE;
        mitem(&tmp, 3, A_SORT_SIZE, "Size", "", 1)->checked = a->sort_by == SORT_SIZE;
        msep(&tmp);
        mitem(&tmp, 3, A_SORT_ASC, "Ascending", "", 1)->checked = !a->sort_desc;
        mitem(&tmp, 3, A_SORT_DESC, "Descending", "", 1)->checked = a->sort_desc;
        n = tmp.n;
        memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    } else if (id == SUB_GROUP) {
        Menu tmp; memset(&tmp, 0, sizeof tmp);
        mitem(&tmp, 3, A_GROUP_NONE, "(None)", "", 1)->checked = a->group_by == GROUP_NONE;
        mitem(&tmp, 3, A_GROUP_TYPE, "Type", "", 1)->checked = a->group_by == GROUP_TYPE;
        mitem(&tmp, 3, A_GROUP_DATE, "Date modified", "", 1)->checked = a->group_by == GROUP_DATE;
        n = tmp.n;
        memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    } else if (id == SUB_NEW) {
        Menu tmp; memset(&tmp, 0, sizeof tmp);
        mitem(&tmp, 0, A_NEW_FOLDER, "Folder", "", 1);
        mitem(&tmp, 0, A_NEW_SHORTCUT, "Shortcut", "", 1);
        mitem(&tmp, 0, A_NEW_TEXT, "Text Document", "", 1);
        mitem(&tmp, 0, A_NEW_ZIP, "Compressed (zipped) Folder", "", 0);
        n = tmp.n;
        memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    } else if (id == SUB_OPENWITH) {
        const char *path = NULL;
        if (a->ctx_kind == CTX_FILE && a->ctx_index >= 0 && a->ctx_index < a->n_ents)
            path = a->ents[a->ctx_index].path;
        if (path && a->n_apps == 0) apps_load(a);
        const char *mime = path ? mime_for(path) : "";
        Menu tmp; memset(&tmp, 0, sizeof tmp);
        for (int i = 0; i < a->n_apps && tmp.n < 16; i++) {
            if (mime[0] && a->apps[i].mime[0] && !strstr(a->apps[i].mime, mime) &&
                !strstr(a->apps[i].mime, "*/*")) continue;
            MenuItem *ap = mitem(&tmp, 0, A_APP_BASE + i,
                                 a->apps[i].name, "", 1);
            snprintf(ap->icon, sizeof ap->icon, "%s", a->apps[i].icon);
        }
        if (!tmp.n) mitem(&tmp, 0, A_OPEN, "Default application", "", 1);
        n = tmp.n;
        memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    }
    if (!n) return;
    int y = menu_item_y(parent, idx);
    Menu *child = menu_create(a, parent, serial, parent->w, y, items, n);
    if (child) parent->items[idx].sub = child;
}

static void menu_close_rec(Menu *m)
{
    if (!m) return;
    for (int i = 0; i < m->n; i++) {
        if (m->items[i].sub) {
            menu_close_rec(m->items[i].sub);
            m->items[i].sub = NULL;
        }
    }
    menu_close(m);
}

static void menu_close_tree(App *a)
{
    if (!a->menu) return;
    menu_close_rec(a->menu);
    a->menu = NULL;
    a->pointer_menu = NULL;
}

/* ------------------------------------------------------------------ */
/*  Actions and context menus                                          */
/* ------------------------------------------------------------------ */

static void properties_entry(App *a, Entry *e)
{
    Dialog *d = &a->dialog;
    memset(d, 0, sizeof *d);
    d->type = 3;
    snprintf(d->title, sizeof d->title, "%s Properties", e->name);
    char size[64], date[64], perm[16];
    entry_size_text(a, e, size, sizeof size);
    fmt_date(date, sizeof date, e->mtime);
    mode_t m = e->mode;
    snprintf(perm, sizeof perm, "%c%c%c%c%c%c%c%c%c",
             S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-'),
             m & S_IRUSR ? 'r' : '-', m & S_IWUSR ? 'w' : '-',
             m & S_IXUSR ? 'x' : '-', m & S_IRGRP ? 'r' : '-',
             m & S_IWGRP ? 'w' : '-', m & S_IXGRP ? 'x' : '-',
             m & S_IROTH ? 'r' : '-', m & S_IWOTH ? 'w' : '-');
    snprintf(d->lines[d->nlines++], 256, "Type: %s", e->type);
    snprintf(d->lines[d->nlines++], 256, "Location: %s", a->cwd);
    snprintf(d->lines[d->nlines++], 256, "Size: %s", size);
    snprintf(d->lines[d->nlines++], 256, "Modified: %s", date);
    snprintf(d->lines[d->nlines++], 256, "Permissions: %s", perm);
    a->dirty = 1;
    dialog_window_open(a);
}

static void properties_drive(App *a, Drive *d)
{
    Entry e;
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", d->label[0] ? d->label : d->dev);
    snprintf(e.type, sizeof e.type, "%s%s", "Local Disk",
             d->removable ? " (Removable)" : "");
    snprintf(e.icon, sizeof e.icon, "%s", d->icon);
    e.size = 0;
    e.mode = S_IFDIR | 0755;
    properties_entry(a, &e);
    Dialog *dlg = &a->dialog;
    dlg->nlines = 0;
    snprintf(dlg->lines[dlg->nlines++], 256, "Device: %s", d->dev);
    snprintf(dlg->lines[dlg->nlines++], 256, "File system: %s",
             d->fstype[0] ? d->fstype : "unmounted");
    snprintf(dlg->lines[dlg->nlines++], 256, "Capacity: %s", d->size);
    snprintf(dlg->lines[dlg->nlines++], 256, "Mounted at: %s",
             d->mounted ? d->mount : "not mounted");
    if (d->mounted) {
        snprintf(dlg->lines[dlg->nlines++], 256, "Space: %.0f%% used, %s",
                 d->used * 100.0, d->avail);
    }
    dialog_window_open(a);
}

static void action_activate(App *a, int id)
{
    Entry *e = NULL;
    if (a->ctx_kind == CTX_FILE && a->ctx_index >= 0 &&
        a->ctx_index < a->n_ents) e = &a->ents[a->ctx_index];
    Drive *d = NULL;
    if (a->ctx_kind == CTX_DRIVE && a->ctx_index >= 0 &&
        a->ctx_index < a->n_drives) d = &a->drives[a->ctx_index];

    if (id >= A_APP_BASE && id < A_APP_MAX) {
        launch_app(a, id - A_APP_BASE, e ? e->path : NULL);
        return;
    }
    switch (id) {
    case A_OPEN:
        if (d) {
            if (d->mounted) navigate(a, d->mount, 1);
            else drive_job(a, d, 0);
        } else if (a->search_active) {
            /* Results are paths anywhere under cwd: reveal them in their own
             * folder instead of opening a file or descending into a folder. */
            if (e) search_accept_path(a, e->path);
            else if (a->sel >= 0 && a->sel < a->n_ents)
                search_accept_path(a, a->ents[a->sel].path);
        } else if (e) {
            if (e->is_dir) navigate(a, e->path, 1);
            else open_path(e->path);
        }
        break;
    case A_OPEN_NEW:
        if (e && e->is_dir) {
            const char *av[] = { "/proc/self/exe", e->path, NULL };
            spawn_detached(av);
        }
        break;
    case A_PIN:
        if (e && e->is_dir) toggle_favorite(a, e->path, 1);
        break;
    case A_UNPIN:
        if (e && e->is_dir) toggle_favorite(a, e->path, 0);
        break;
    case A_CUT:
    case A_COPY: {
        int idx[512];
        int n = selected_indices(a, idx, 512);
        a->n_clip = 0;
        for (int i = 0; i < n && i < 64; i++)
            snprintf(a->clipboard[a->n_clip++], PATH_MAX, "%s",
                     a->ents[idx[i]].path);
        a->clip_cut = id == A_CUT;
        clipboard_publish(a);
        status_set(a, "%s %d item%s", id == A_CUT ? "Cut" : "Copied",
                   a->n_clip, a->n_clip == 1 ? "" : "s");
        /* Confirm the staged entries with the same full-lap accent used after
         * a paste/drop.  They are already selected, so start here rather than
         * waiting for a rescan to re-apply the selection. */
        char paths[64][PATH_MAX];
        int arm_n = a->n_clip < 64 ? a->n_clip : 64;
        for (int i = 0; i < arm_n; i++)
            snprintf(paths[i], PATH_MAX, "%s", a->clipboard[i]);
        post_xfer_arm(a, a->cwd, paths, arm_n);
        if (a->border_anim_enabled && a->post_armed)
            a->post_sel_anim_start = mono_sec();
        break;
    }
    case A_COPY_PATH:
        if (e) {
            clipboard_publish_text(a, e->path);
            status_set(a, "Copied path: %s", e->path);
        }
        break;
    case A_SHORTCUT:
        if (e) do_shortcut(a, e->path);
        break;
    case A_DELETE:
        do_delete(a);
        break;
    case A_DELETE_PERMANENT:
        do_delete_permanent(a);
        break;
    case A_RENAME:
        if (e) do_rename(a, e->path);
        break;
    case A_PROPERTIES:
        if (d) properties_drive(a, d);
        else if (e) properties_entry(a, e);
        else {
            struct statvfs sv;
            char line[256];
            memset(&a->dialog, 0, sizeof a->dialog);
            a->dialog.type = 3;
            snprintf(a->dialog.title, sizeof a->dialog.title, "Folder Properties");
            snprintf(a->dialog.lines[a->dialog.nlines++], 256, "Location: %s", a->cwd);
            if (statvfs(a->cwd, &sv) == 0) {
                double used = 1.0 - (double)sv.f_bavail / (double)sv.f_blocks;
                char avail[64];
                fmt_size(avail, sizeof avail,
                         (off_t)sv.f_bavail * (off_t)sv.f_frsize);
                snprintf(line, sizeof line, "Disk: %.0f%% used, %s", used * 100.0, avail);
                snprintf(a->dialog.lines[a->dialog.nlines++], 256, "%s", line);
            }
            snprintf(a->dialog.lines[a->dialog.nlines++], 256, "Items: %d", a->n_ents);
            a->dirty = 1;
            dialog_window_open(a);
        }
        break;
    /* Results stay in list mode so the containing directory has a line under
     * each name; the choice is remembered for when search closes. */
    case A_VIEW_LIST:
        if (a->search_active) { a->saved_view = VIEW_LIST; break; }
        a->view = VIEW_LIST; scroll_snap(a); a->dirty = 1; break;
    case A_VIEW_ICONS:
        if (a->search_active) { a->saved_view = VIEW_ICONS; break; }
        a->view = VIEW_ICONS; scroll_snap(a); a->dirty = 1; break;
    case A_VIEW_DETAILS:
        if (a->search_active) { a->saved_view = VIEW_DETAILS; break; }
        a->view = VIEW_DETAILS; scroll_snap(a); a->dirty = 1; break;
    case A_VIEW_TILES:
        if (a->search_active) { a->saved_view = VIEW_TILES; break; }
        a->view = VIEW_TILES; scroll_snap(a); a->dirty = 1; break;
    case A_SORT_NAME: a->sort_by = SORT_NAME; search_sort(a); break;
    case A_SORT_DATE: a->sort_by = SORT_DATE; search_sort(a); break;
    case A_SORT_TYPE: a->sort_by = SORT_TYPE; search_sort(a); break;
    case A_SORT_SIZE: a->sort_by = SORT_SIZE; search_sort(a); break;
    case A_SORT_ASC: a->sort_desc = 0; search_sort(a); break;
    case A_SORT_DESC: a->sort_desc = 1; search_sort(a); break;
    case A_GROUP_NONE: a->group_by = GROUP_NONE; search_sort(a); break;
    case A_GROUP_TYPE: a->group_by = GROUP_TYPE; search_sort(a); break;
    case A_GROUP_DATE: a->group_by = GROUP_DATE; search_sort(a); break;
    case A_NEW_FOLDER: case A_NEW_TEXT: case A_NEW_ZIP: do_new_item(a, id); break;
    case A_NEW_SHORTCUT: {
        char p[PATH_MAX];
        path_join(p, sizeof p, a->cwd, "Shortcut.desktop");
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "[Desktop Entry]\nType=Application\nName=Shortcut\n"
                    "Exec=xdg-open .\nIcon=folder\n");
            fclose(f);
        }
        dir_load(a, a->cwd);
        do_rename(a, p);
        break;
    }
    case A_REFRESH: drive_refresh(a); dir_load(a, a->cwd); break;
    case A_PASTE: {
        int cut = 0;
        if (clipboard_fetch(a, &cut)) do_paste(a, NULL);
        break;
    }
    case A_UNDO:
        if (a->last_cut[0]) { snprintf(a->clipboard[0], PATH_MAX, "%s", a->last_cut);
                              a->n_clip = 1; a->clip_cut = 1; }
        status_set(a, "Nothing to undo");
        break;
    case A_SELECT_ALL:
        post_xfer_disarm(a);
        for (int i = 0; i < a->n_ents; i++) a->selected[i] = 1;
        a->n_selected = a->n_ents;
        a->dirty = 1;
        break;
    case A_HIDDEN:
        a->show_hidden = !a->show_hidden;
        dir_load(a, a->cwd);
        status_set(a, a->show_hidden ? "Showing hidden files" : "Hidden files hidden");
        break;
    case A_SMOOTH_SCROLL:
        a->smooth_scroll = !a->smooth_scroll;
        scroll_snap(a);
        status_set(a, a->smooth_scroll ? "Smooth scrolling on" :
                              "Smooth scrolling off");
        a->dirty = 1;
        break;
    case A_GO_BACK: go_hist(a, -1); break;
    case A_GO_FORWARD: go_hist(a, 1); break;
    case A_GO_UP: {
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", a->cwd);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = '\0';
        else snprintf(parent, sizeof parent, "/");
        navigate(a, parent, 1);
        break;
    }
    case A_GO_HOME: {
        const char *home = getenv("HOME");
        if (home) navigate(a, home, 1);
        break;
    }
    case A_MENU:
        if (a->menu) menu_close_tree(a);
        else {
            a->ctx_kind = CTX_BACKGROUND;
            a->ctx_index = -1;
            menu_open_root(a, a->pointer_serial, 88, 44);
        }
        break;
    case A_DRIVE_OPEN:
        if (d) {
            if (d->mounted) navigate(a, d->mount, 1);
            else status_set(a, "%s is not mounted", d->dev);
        }
        break;
    case A_DRIVE_MOUNT: if (d) drive_job(a, d, 0); break;
    case A_DRIVE_UNMOUNT: if (d) drive_job(a, d, 1); break;
    case A_DRIVE_EJECT: if (d) drive_job(a, d, 2); break;
    case A_QUIT: a->quit = 1; break;
    default: break;
    }
}

static void menu_open_root(App *a, uint32_t serial, int x, int y)
{
    MenuItem items[40];
    int n = 0;
    Menu tmp;
    memset(&tmp, 0, sizeof tmp);
    Menu *m = &tmp;

    if (a->ctx_kind == CTX_FILE && a->ctx_index >= 0 &&
        a->ctx_index < a->n_ents) {
        Entry *e = &a->ents[a->ctx_index];
        mitem(m, 0, A_OPEN, "Open", "Enter", 1);
        if (e->is_dir) mitem(m, 0, A_OPEN_NEW, "Open in new window", "", 1);
        else mitem(m, 2, SUB_OPENWITH, "Open with", "", 1);
        mitem(m, 0, is_favorite(a, e->path) ? A_UNPIN : A_PIN,
              is_favorite(a, e->path) ? "Unpin from Quick access" :
              "Pin to Quick access", "", e->is_dir);
        msep(m);
        mitem(m, 0, A_CUT, "Cut", "Ctrl+X", 1);
        mitem(m, 0, A_COPY, "Copy", "Ctrl+C", 1);
        mitem(m, 0, A_COPY_PATH, "Copy as path", "", 1);
        mitem(m, 0, A_SHORTCUT, "Create shortcut", "", 1);
        msep(m);
        mitem(m, 0, A_DELETE, "Move to Trash", "Del", 1);
        mitem(m, 0, A_DELETE_PERMANENT, "Delete permanently", "Shift+Del", 1);
        mitem(m, 0, A_RENAME, "Rename", "F2", 1);
        msep(m);
        mitem(m, 0, A_PROPERTIES, "Properties", "Alt+Enter", 1);
    } else if (a->ctx_kind == CTX_DRIVE && a->ctx_index >= 0 &&
               a->ctx_index < a->n_drives) {
        Drive *d = &a->drives[a->ctx_index];
        mitem(m, 0, A_DRIVE_OPEN, "Open", "", d->mounted);
        mitem(m, 0, A_DRIVE_MOUNT, "Mount", "", !d->mounted);
        mitem(m, 0, A_DRIVE_UNMOUNT, "Unmount", "", d->mounted);
        if (d->removable) mitem(m, 0, A_DRIVE_EJECT, "Eject", "", 1);
        msep(m);
        mitem(m, 0, A_PROPERTIES, "Properties", "", 1);
    } else {
        /* Windows 10 File Explorer background menu, in its native order. */
        mitem(m, 2, SUB_VIEW, "View", "", 1);
        mitem(m, 2, SUB_SORT, "Sort by", "", 1);
        mitem(m, 2, SUB_GROUP, "Group by", "", 1);
        msep(m);
        mitem(m, 2, SUB_NEW, "New", "", 1);
        msep(m);
        mitem(m, 0, A_REFRESH, "Refresh", "F5", 1);
        mitem(m, 0, A_PASTE, "Paste", "Ctrl+V", clipboard_has(a));
        msep(m);
        mitem(m, 3, A_HIDDEN, "Hidden files", "Ctrl+H", 1)->checked =
            a->show_hidden;
        msep(m);
        mitem(m, 0, A_PROPERTIES, "Properties", "", 1);
    }
    n = m->n;
    memcpy(items, tmp.items, (size_t)n * sizeof items[0]);
    menu_close_tree(a);
    a->menu = menu_create(a, NULL, serial, x, y, items, n);
    a->dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

static cairo_surface_t *icon_get(App *a, const char *name, double logical,
                                 int big)
{
    int px = (int)ceil(logical * (double)a->scale);
    if (px < 8) px = 8;
    if (px > 512) px = 512;
    for (int i = 0; i < a->n_icons; i++)
        if (a->icons[i].px == px && a->icons[i].big == big &&
            !strcmp(a->icons[i].name, name)) {
            a->icons[i].used = ++a->icon_clock;
            return a->icons[i].sf;
        }
    cairo_surface_t *sf = big ? icon_load_big(name, px) : icon_load(name, px);
    int slot;
    if (a->n_icons < ICON_CACHE_MAX) {
        slot = a->n_icons++;
    } else {
        /* Evict the least recently used entry. The old time-in-milliseconds
         * modulo landed every miss in the same frame on one slot, so icons
         * were rasterized again on every frame once the cache filled up. */
        slot = 0;
        for (int i = 1; i < ICON_CACHE_MAX; i++)
            if (a->icons[i].used < a->icons[slot].used)
                slot = i;
    }
    if (a->icons[slot].sf) cairo_surface_destroy(a->icons[slot].sf);
    snprintf(a->icons[slot].name, sizeof a->icons[slot].name, "%s", name);
    a->icons[slot].px = px;
    a->icons[slot].big = big;
    a->icons[slot].used = ++a->icon_clock;
    a->icons[slot].sf = sf;
    return sf;
}

struct png_mem {
    const unsigned char *data;
    size_t len;
    size_t pos;
};

static cairo_status_t png_mem_read(void *closure, unsigned char *data,
                                   unsigned int length)
{
    struct png_mem *m = closure;
    if (m->pos + length > m->len) return CAIRO_STATUS_READ_ERROR;
    memcpy(data, m->data + m->pos, length);
    m->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *png_surface_from_mem(const unsigned char *buf,
                                              size_t len)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n',
                                          0x1a, '\n' };
    if (len < sizeof sig || memcmp(buf, sig, sizeof sig)) return NULL;
    struct png_mem m = { buf, len, 0 };
    cairo_surface_t *sf = cairo_image_surface_create_from_png_stream(
        png_mem_read, &m);
    if (sf && cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    return sf;
}

/* Render a still frame of an image or video with ffmpeg, straight into a
 * Cairo surface. No GStreamer, no GTK, no temporary thumbnail files. */
static cairo_surface_t *thumb_render(const char *path, int px)
{
    int dim = px & ~1;
    if (dim < 16) dim = 16;
    if (dim > 256) dim = 256;
    char filter[96];
    snprintf(filter, sizeof filter,
             "scale=%d:%d:force_original_aspect_ratio=decrease", dim, dim);

    char *av[] = {
        (char *)"ffmpeg", (char *)"-v", (char *)"error",
        (char *)"-ss", (char *)"0", (char *)"-i", (char *)path,
        (char *)"-an", (char *)"-frames:v", (char *)"1",
        (char *)"-vf", filter, (char *)"-f", (char *)"image2pipe",
        (char *)"-vcodec", (char *)"png", (char *)"-", NULL
    };

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return NULL; }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        /* ffmpeg writes decoding failures to stderr; keep them off the
           terminal since an unreadable file is an expected case here. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull != STDERR_FILENO) close(devnull);
        }
        execvp("ffmpeg", av);
        _exit(127);
    }
    close(p[1]);

    size_t cap = 8192, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { close(p[0]); waitpid(pid, NULL, 0); return NULL; }
    for (;;) {
        if (len == cap) {
            if (cap >= 12u * 1024u * 1024u) break;
            size_t ncap = cap * 2;
            unsigned char *nb = realloc(buf, ncap);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        ssize_t n = read(p[0], buf + len, cap - len);
        if (n > 0) { len += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(p[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    cairo_surface_t *sf = NULL;
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 0)
        sf = png_surface_from_mem(buf, len);
    free(buf);
    return sf;
}

/* Thumbnails are rendered by one worker thread instead of forking ffmpeg from
 * the draw callback. The worker keeps a small on-disk cache (keyed by path,
 * mtime, size and pixel size), results are handed back through a pipe so the
 * main poll loop can install them without racing the worker. */
typedef struct ThumbReq {
    struct ThumbReq *prev, *next;
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
    int px;
    unsigned gen;
    int kind;               /* 0 thumbnail, 1 folder item count */
} ThumbReq;

typedef struct ThumbRes {
    struct ThumbRes *next;
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
    int px;
    unsigned gen;
    int kind;
    int count;
    int state;
    cairo_surface_t *sf;
} ThumbRes;

static pthread_mutex_t thumb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thumb_cv = PTHREAD_COND_INITIALIZER;
static ThumbReq *thumb_q_head, *thumb_q_tail;
static ThumbRes *thumb_res_head, *thumb_res_tail;
static int thumb_q_len, thumb_stop, thumb_started;
static pthread_t thumb_tid[2];
static int thumb_wake[2] = { -1, -1 };
static char thumb_dir[PATH_MAX];

static uint64_t thumb_hash(const char *path, time_t mtime, off_t size, int px)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)(unsigned long long)mtime; h *= 1099511628211ULL;
    h ^= (uint64_t)(unsigned long long)size;  h *= 1099511628211ULL;
    h ^= (uint64_t)px;                        h *= 1099511628211ULL;
    return h;
}

static void thumb_cache_file(char *out, size_t n, const char *path,
                             time_t mtime, off_t size, int px)
{
    snprintf(out, n, "%s/%016llx.png", thumb_dir,
             (unsigned long long)thumb_hash(path, mtime, size, px));
}

static void thumb_cache_init(void)
{
    const char *base = getenv("XDG_CACHE_HOME");
    char parent[PATH_MAX];
    if (base && *base)
        snprintf(parent, sizeof parent, "%s", base);
    else {
        const char *home = getenv("HOME");
        snprintf(parent, sizeof parent, "%s/.cache",
                 home && *home ? home : "/tmp");
    }
    snprintf(thumb_dir, sizeof thumb_dir, "%s/jtfiles/thumbnails", parent);
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) return;
    char jt[PATH_MAX];
    snprintf(jt, sizeof jt, "%s/jtfiles", parent);
    if (mkdir(jt, 0700) != 0 && errno != EEXIST) return;
    if (mkdir(thumb_dir, 0700) != 0 && errno != EEXIST) thumb_dir[0] = '\0';
}

static cairo_surface_t *thumb_cache_load(const char *path, time_t mtime,
                                          off_t size, int px)
{
    if (!thumb_dir[0]) return NULL;
    char file[PATH_MAX];
    thumb_cache_file(file, sizeof file, path, mtime, size, px);
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 8 || sz > 12 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    cairo_surface_t *sf = got == (size_t)sz ? png_surface_from_mem(buf, got) : NULL;
    free(buf);
    return sf;
}

static void thumb_cache_save(const char *path, time_t mtime, off_t size, int px,
                             cairo_surface_t *sf)
{
    if (!thumb_dir[0] || !sf) return;
    char file[PATH_MAX], tmp[PATH_MAX];
    thumb_cache_file(file, sizeof file, path, mtime, size, px);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", file, (int)getpid());
    if (cairo_surface_write_to_png(sf, tmp) == CAIRO_STATUS_SUCCESS)
        (void)!rename(tmp, file);
    else
        (void)!unlink(tmp);
}

static int worker_submit(ThumbReq *r)
{
    if (!thumb_started) { free(r); return 0; }
    pthread_mutex_lock(&thumb_mu);
    if (thumb_stop || thumb_q_len >= THUMB_QUEUE_MAX || thumb_wake[1] < 0) {
        pthread_mutex_unlock(&thumb_mu);
        free(r);
        return 0;
    }
    /* Newest first: the items just scrolled into view get rendered first. */
    r->next = thumb_q_head;
    if (thumb_q_head) thumb_q_head->prev = r;
    thumb_q_head = r;
    if (!thumb_q_tail) thumb_q_tail = r;
    thumb_q_len++;
    pthread_cond_signal(&thumb_cv);
    pthread_mutex_unlock(&thumb_mu);
    return 1;
}

static int thumb_submit(const char *path, time_t mtime, off_t size,
                        int px, unsigned gen)
{
    ThumbReq *r = calloc(1, sizeof *r);
    if (!r) return 0;
    snprintf(r->path, sizeof r->path, "%s", path);
    r->mtime = mtime;
    r->size = size;
    r->px = px;
    r->gen = gen;
    r->kind = 0;
    return worker_submit(r);
}

static int worker_submit_count(const char *path, time_t mtime)
{
    ThumbReq *r = calloc(1, sizeof *r);
    if (!r) return 0;
    snprintf(r->path, sizeof r->path, "%s", path);
    r->mtime = mtime;
    r->kind = 1;
    return worker_submit(r);
}

static void *thumb_worker(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&thumb_mu);
        while (!thumb_q_head && !thumb_stop)
            pthread_cond_wait(&thumb_cv, &thumb_mu);
        ThumbReq *r = thumb_q_head;
        if (!r) { pthread_mutex_unlock(&thumb_mu); break; }
        thumb_q_head = r->next;
        if (thumb_q_head) thumb_q_head->prev = NULL;
        else thumb_q_tail = NULL;
        thumb_q_len--;
        pthread_mutex_unlock(&thumb_mu);

        cairo_surface_t *sf = NULL;
        int count = 0, state = 0;
        if (r->kind == 1) {
            DIR *sd = opendir(r->path);
            if (sd) {
                while (count < 9999 && readdir(sd)) count++;
                closedir(sd);
                state = 1;
            } else {
                state = 2;
            }
        } else if (atomic_load_explicit(&g_thumb_gen,
                                        memory_order_relaxed) == r->gen) {
            sf = thumb_cache_load(r->path, r->mtime, r->size, r->px);
            if (!sf) {
                sf = thumb_render(r->path, r->px);
                if (sf) thumb_cache_save(r->path, r->mtime, r->size, r->px, sf);
            }
        }

        ThumbRes *res = calloc(1, sizeof *res);
        if (res) {
            snprintf(res->path, sizeof res->path, "%s", r->path);
            res->mtime = r->mtime;
            res->size = r->size;
            res->px = r->px;
            res->gen = r->gen;
            res->kind = r->kind;
            res->count = count;
            res->state = state;
            res->sf = sf;
            pthread_mutex_lock(&thumb_mu);
            res->next = NULL;
            if (thumb_res_tail) thumb_res_tail->next = res;
            else thumb_res_head = res;
            thumb_res_tail = res;
            unsigned char byte = 1;
            if (thumb_wake[1] >= 0) (void)!write(thumb_wake[1], &byte, 1);
            pthread_mutex_unlock(&thumb_mu);
        } else if (sf) {
            cairo_surface_destroy(sf);
        }
        free(r);
    }
    return NULL;
}

static void thumb_start(void)
{
    thumb_cache_init();
    if (pipe2(thumb_wake, O_CLOEXEC | O_NONBLOCK) != 0) {
        thumb_wake[0] = thumb_wake[1] = -1;
        return;
    }
    for (unsigned i = 0; i < sizeof thumb_tid / sizeof thumb_tid[0]; i++)
        if (pthread_create(&thumb_tid[i], NULL, thumb_worker, NULL) == 0)
            thumb_started++;
    if (!thumb_started) {
        close(thumb_wake[0]);
        close(thumb_wake[1]);
        thumb_wake[0] = thumb_wake[1] = -1;
    }
}

static void thumb_stop_worker(void)
{
    if (!thumb_started) return;
    pthread_mutex_lock(&thumb_mu);
    thumb_stop = 1;
    pthread_cond_broadcast(&thumb_cv);
    pthread_mutex_unlock(&thumb_mu);
    for (int i = 0; i < thumb_started; i++)
        pthread_join(thumb_tid[i], NULL);
    thumb_started = 0;

    pthread_mutex_lock(&thumb_mu);
    while (thumb_q_head) {
        ThumbReq *r = thumb_q_head;
        thumb_q_head = r->next;
        free(r);
    }
    thumb_q_tail = NULL;
    thumb_q_len = 0;
    while (thumb_res_head) {
        ThumbRes *r = thumb_res_head;
        thumb_res_head = r->next;
        if (r->sf) cairo_surface_destroy(r->sf);
        free(r);
    }
    thumb_res_tail = NULL;
    pthread_mutex_unlock(&thumb_mu);

    if (thumb_wake[0] >= 0) close(thumb_wake[0]);
    if (thumb_wake[1] >= 0) close(thumb_wake[1]);
    thumb_wake[0] = thumb_wake[1] = -1;
}

static void thumb_wake_drain(void)
{
    unsigned char buf[128];
    while (read(thumb_wake[0], buf, sizeof buf) > 0) {}
}

static int thumb_collect(App *a)
{
    int applied = 0, counts_changed = 0;
    unsigned gen = atomic_load_explicit(&g_thumb_gen, memory_order_relaxed);
    for (;;) {
        pthread_mutex_lock(&thumb_mu);
        ThumbRes *r = thumb_res_head;
        if (r) {
            thumb_res_head = r->next;
            if (!thumb_res_head) thumb_res_tail = NULL;
        }
        pthread_mutex_unlock(&thumb_mu);
        if (!r) break;

        if (r->kind == 1) {
            Entry fake;
            memset(&fake, 0, sizeof fake);
            snprintf(fake.path, sizeof fake.path, "%s", r->path);
            fake.mtime = r->mtime;
            fake.size = r->count;
            fake.count_state = (unsigned char)r->state;
            count_cache_store(a, &fake);
            Entry *src = a->search_active ? a->search_ents : a->dir_ents;
            int nsrc = a->search_active ? a->n_search : a->n_dir;
            for (int i = 0; i < a->n_ents; i++) {
                Entry *e = &a->ents[i];
                if (e->count_state != 3 || e->mtime != r->mtime ||
                    strcmp(e->path, r->path))
                    continue;
                e->size = r->count;
                e->count_state = (unsigned char)r->state;
                applied = 1;
                counts_changed = 1;
            }
            /* Keep the snapshot in step so the next search rebuild does not
             * throw the count away and queue the same worker job again. */
            for (int i = 0; i < nsrc; i++) {
                Entry *e = &src[i];
                if (e->mtime != r->mtime || strcmp(e->path, r->path)) continue;
                e->size = r->count;
                e->count_state = (unsigned char)r->state;
            }
        } else if (r->gen == gen) {
            for (int i = 0; i < THUMB_CACHE_MAX; i++) {
                ThumbEnt *t = &a->thumbs[i];
                if (t->state != 1 || t->gen != r->gen || t->px != r->px ||
                    t->mtime != r->mtime || t->size != r->size ||
                    strcmp(t->path, r->path))
                    continue;
                t->sf = r->sf;
                r->sf = NULL;
                t->failed = !t->sf;
                t->state = 2;
                t->used = ++a->thumb_clock;
                applied = 1;
                break;
            }
        }
        if (r->sf) cairo_surface_destroy(r->sf);
        free(r);
    }
    if (counts_changed && a->sort_by == SORT_SIZE && a->n_ents > 1) {
        char sel_path[PATH_MAX] = { 0 };
        if (a->sel >= 0 && a->sel < a->n_ents)
            snprintf(sel_path, sizeof sel_path, "%s", a->ents[a->sel].path);
        Entry *src = a->search_active ? a->search_ents : a->dir_ents;
        int nsrc = a->search_active ? a->n_search : a->n_dir;
        if (nsrc > 1) qsort(src, (size_t)nsrc, sizeof *src, entry_cmp);
        qsort(a->ents, (size_t)a->n_ents, sizeof *a->ents, entry_cmp);
        if (sel_path[0])
            for (int i = 0; i < a->n_ents; i++)
                if (!strcmp(a->ents[i].path, sel_path)) {
                    a->sel = a->anchor = i;
                    break;
                }
    }
    if (applied) a->dirty = 1;
    return applied;
}

static cairo_surface_t *thumbnail_get(App *a, Entry *e, double logical)
{
    if (!e || !thumb_started) return NULL;
    /* "application-x-cd-image" is a disc image, not a picture: don't feed
       .iso files to ffmpeg. */
    if (strstr(e->icon, "cd-image") || strstr(e->name, ".iso"))
        return NULL;
    if (!(strstr(e->icon, "image") || strstr(e->icon, "video")))
        return NULL;

    int px = (int)ceil(logical * (double)a->scale);
    if (px < 16) px = 16;
    if (px > 256) px = 256;
    unsigned gen = atomic_load_explicit(&g_thumb_gen, memory_order_relaxed);

    for (int i = 0; i < THUMB_CACHE_MAX; i++) {
        ThumbEnt *t = &a->thumbs[i];
        if (t->state == 2 && t->px == px && t->mtime == e->mtime &&
            t->size == e->size && !strcmp(t->path, e->path)) {
            t->used = ++a->thumb_clock;
            return t->sf;
        }
    }
    for (int i = 0; i < THUMB_CACHE_MAX; i++) {
        ThumbEnt *t = &a->thumbs[i];
        if (t->state == 1 && t->gen == gen && t->px == px &&
            t->mtime == e->mtime && t->size == e->size &&
            !strcmp(t->path, e->path))
            return NULL; /* already queued; draw the fallback icon for now */
    }

    int slot = -1;
    for (int i = 0; i < THUMB_CACHE_MAX; i++)
        if (a->thumbs[i].state == 0) { slot = i; break; }
    if (slot < 0) {
        int found = 0;
        slot = 0;
        for (int i = 0; i < THUMB_CACHE_MAX; i++) {
            ThumbEnt *t = &a->thumbs[i];
            if (t->state != 2) continue;
            if (!found || t->used < a->thumbs[slot].used) {
                slot = i;
                found = 1;
            }
        }
        if (!found) return NULL;
        if (a->thumbs[slot].sf) cairo_surface_destroy(a->thumbs[slot].sf);
    }

    ThumbEnt *t = &a->thumbs[slot];
    snprintf(t->path, sizeof t->path, "%s", e->path);
    t->mtime = e->mtime;
    t->size = e->size;
    t->px = px;
    t->gen = gen;
    t->state = 1;
    t->failed = 0;
    t->sf = NULL;
    t->used = ++a->thumb_clock;
    if (!thumb_submit(t->path, t->mtime, t->size, px, gen)) t->state = 0;
    return NULL;
}

static void icon_draw_surface(cairo_t *cr, cairo_surface_t *sf,
                              double size, double x, double y)
{
    if (!sf) return;
    int iw = cairo_image_surface_get_width(sf);
    int ih = cairo_image_surface_get_height(sf);
    if (iw <= 0 || ih <= 0) return;
    double side = (double)(iw > ih ? iw : ih);
    double w = (double)iw / side;
    double h = (double)ih / side;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / side, size / side);
    cairo_translate(cr, -(1.0 - w) * side / 2.0, -(1.0 - h) * side / 2.0);
    cairo_set_source_surface(cr, sf, 0, 0);
    /* Icons are cached at their exact device size; nearest keeps the hard
       edges from being blurred again when they are blitted. */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void icon_draw(App *a, cairo_t *cr, const char *name,
                       double size, double x, double y)
{
    icon_draw_surface(cr, icon_get(a, name, size, 0), size, x, y);
}

static void icon_draw_big(App *a, cairo_t *cr, const char *name,
                          double size, double x, double y)
{
    icon_draw_surface(cr, icon_get(a, name, size, 1), size, x, y);
}

/* Thumbnails use the same visual band as the folder/file glyphs. */
static void thumbnail_draw(cairo_t *cr, cairo_surface_t *sf,
                           double size, double x, double y)
{
    if (!sf) return;
    int iw = cairo_image_surface_get_width(sf);
    int ih = cairo_image_surface_get_height(sf);
    if (iw <= 0 || ih <= 0) return;

    double band_h = size * ICON_BODY_H / 48.0;
    double scale = band_h / (double)ih;
    double dw = (double)iw * scale;
    if (dw > size) { scale = size / (double)iw; dw = size; }
    double dh = (double)ih * scale;
    double top = y + size * ICON_BODY_TOP / 48.0;

    cairo_save(cr);
    cairo_translate(cr, x + (size - dw) / 2.0, top + (band_h - dh) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, sf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void icon_draw_entry(App *a, cairo_t *cr, Entry *e,
                            double size, double x, double y, int big)
{
    cairo_surface_t *thumb = thumbnail_get(a, e, size);
    if (thumb) {
        thumbnail_draw(cr, thumb, size, x, y);
        return;
    }
    if (big) icon_draw_big(a, cr, e->icon, size, x, y);
    else icon_draw(a, cr, e->icon, size, x, y);
}

static void draw_toolbar(App *a, cairo_t *cr)
{
    /* No opaque panel fill: the window-wide background gradient continues
     * through the toolbar. Only the bottom hairline separates the two. */

    struct { int x; const char *icon; int id; double size; } btns[] = {
        { 12, "go-previous-symbolic", A_GO_BACK, 17.0 },
        { 50, "go-next-symbolic", A_GO_FORWARD, 17.0 },
        { 88, "menu-symbolic", A_MENU, 20.0 },
    };
    for (unsigned i = 0; i < sizeof btns / sizeof btns[0]; i++) {
        double sz = btns[i].size;
        rounded_rect(cr, btns[i].x, 12, 30, 30, 7);
        rgba(cr, C_FG, a->hover_btn == (int)i ? 0.10 : 0.04);
        cairo_fill(cr);
        icon_draw(a, cr, btns[i].icon, sz,
                  btns[i].x + (30.0 - sz) / 2.0, 12.0 + (30.0 - sz) / 2.0);
        (void)btns[i].id;
    }

    double px = 132.0, pw = a->WIN_W - px - 248.0;
    if (pw < 120.0) pw = 120.0;
    ui_card_fill(cr, px, 12, pw, 31, 8);
    icon_draw(a, cr, "folder", 15.0, px + 9.0, 20.0);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", a->editing_path ? a->path_edit : a->cwd);
    ui_ellipsize(cr, path, sizeof path, 12, 400, pw - 58.0);
    /* Folder icon ends ~33px down; shift text so its ink bottom matches. */
    text_draw(cr, path, 12, 400, C_FG, 0.92, px + 32.0, 22.0,
              ALIGN_LEFT, 0.0);
    if (a->editing_path) {
        double cw = text_width(cr, path, 12, 400, 0.0);
        rgba(cr, C_ACCENT, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, px + 32.0 + cw + 1.0, 18.0);
        cairo_line_to(cr, px + 32.0 + cw + 1.0, 38.0);
        cairo_stroke(cr);
    }

    double vx = a->WIN_W - 156.0;       /* fixed anchor for view buttons */
    int list_on = a->view == VIEW_LIST || a->view == VIEW_DETAILS;
    double tx = vx - 76.0, gx = vx - 40.0;
    if (a->search_active || a->searching) {
        /* The old list-button slot becomes a spinner while the worker scans. */
        if (a->search_pending) {
            rounded_rect(cr, tx, 12, 30, 30, 7);
            rgba(cr, C_ACCENT, 0.07);
            cairo_fill(cr);

            double cx = tx + 15.0, cy = 27.0, r = 7.5;
            double phase = fmod(mono_sec() * SEARCH_SPIN_RPS, 1.0) * 2.0 * M_PI;
            cairo_save(cr);
            cairo_set_line_width(cr, 2.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            rgba(cr, C_ACCENT, 0.14);
            cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
            cairo_stroke(cr);
            rgba(cr, C_ACCENT, 0.95);
            cairo_arc(cr, cx, cy, r, phase, phase + 1.7);
            cairo_stroke(cr);
            rgba(cr, C_ACCENT, 0.35);
            cairo_arc(cr, cx, cy, r, phase + 2.2, phase + 2.9);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        /* The remaining slot is the X cancel button. */
        rounded_rect(cr, gx, 12, 30, 30, 7);
        rgba(cr, C_ACCENT, a->hover_btn == 5 ? 0.22 : 0.08);
        cairo_fill(cr);
        rgba(cr, C_FG, 0.88);
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, gx + 11.0, 23.0);
        cairo_line_to(cr, gx + 19.0, 31.0);
        cairo_move_to(cr, gx + 19.0, 23.0);
        cairo_line_to(cr, gx + 11.0, 31.0);
        cairo_stroke(cr);
    } else {
        rounded_rect(cr, tx, 12, 30, 30, 7);
        rgba(cr, C_ACCENT, list_on ? 0.20 : 0.05);
        cairo_fill(cr);
        icon_draw(a, cr, "view-list", 16.0, tx + 7.0, 19.0);
        rounded_rect(cr, gx, 12, 30, 30, 7);
        rgba(cr, C_ACCENT, !list_on ? 0.20 : 0.05);
        cairo_fill(cr);
        icon_draw(a, cr, "view-grid", 16.0, gx + 7.0, 19.0);
    }

    /* The box keeps its fixed width; only the search indicator is dropped
     * while typing, giving the text a little more room. */
    int search_text = a->searching || a->search[0];
    double bw = 144.0;
    double bx = vx;
    double text_x = search_text ? bx + 10.0 : bx + 30.0;
    double text_w = search_text ? bw - 20.0 : bw - 40.0;
    a->search_box_x = bx;
    a->search_box_w = bw;

    ui_card_fill(cr, bx, 12, bw, 31, 8);
    if (a->searching) {
        rgba(cr, C_ACCENT, 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, bx + 0.5, 12.5, bw - 1.0, 30.0, 7.5);
        cairo_stroke(cr);
    }
    if (search_text) {
        char query[128];
        snprintf(query, sizeof query, "%s", a->search);
        ui_ellipsize(cr, query, sizeof query, 12, 400, text_w);
        text_draw(cr, query, 12, 400, C_FG, 0.95,
                  text_x, 22.0, ALIGN_LEFT, 0.0);
        if (a->searching && a->caret_on) {
            char prefix[128];
            int cur = a->search_cursor;
            size_t len = strlen(a->search);
            if (cur < 0) cur = 0;
            if (cur > (int)len) cur = (int)len;
            snprintf(prefix, sizeof prefix, "%.*s", cur, a->search);
            double cw = text_width(cr, prefix, 12, 400, 0.0);
            if (cw > text_w) cw = text_w;
            rgba(cr, C_ACCENT, 0.95);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, text_x + cw, 18.5);
            cairo_line_to(cr, text_x + cw, 35.5);
            cairo_stroke(cr);
        }
    } else {
        icon_draw(a, cr, "system-search-symbolic", 14.0, bx + 9.0, 20.5);
        text_draw(cr, "Search", 12, 400, C_FAINT, 0.9,
                  text_x, 22.0, ALIGN_LEFT, 0.0);
    }

    rgba(cr, C_FG, 0.06);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, TOOLBAR_H - 0.5);
    cairo_line_to(cr, a->WIN_W, TOOLBAR_H - 0.5);
    cairo_stroke(cr);
}

/* Hover overlay for one sidebar row. Kept deliberately subtle so selected
 * rows still read as selected; drive rows also get a small accent rail. */
static void side_hover_paint(cairo_t *cr, double y, double h, float hv,
                             int drive)
{
    if (hv <= 0.004f) return;
    double t = hv;
    rgba(cr, C_FG, 0.035 * t);
    rounded_rect(cr, 7.0, y - 3.0, SIDEBAR_W - 14.0, h, 6.0);
    cairo_fill(cr);
    rgba(cr, C_ACCENT, 0.11 * t);
    rounded_rect(cr, 7.0, y - 3.0, SIDEBAR_W - 14.0, h, 6.0);
    cairo_fill(cr);
    if (drive) {
        rgba(cr, C_ACCENT, 0.80 * t);
        rounded_rect(cr, 7.0, y + 1.0, 2.5, h - 8.0, 1.25);
        cairo_fill(cr);
    }
}

static void draw_sidebar(App *a, cairo_t *cr)
{
    double h = a->WIN_H - TOOLBAR_H - STATUS_H;
    cairo_rectangle(cr, 0, TOOLBAR_H, SIDEBAR_W, h);
    rgba(cr, C_SIDEBAR, 1.0);
    cairo_fill(cr);
    rgba(cr, C_FG, 0.06);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, SIDEBAR_W - 0.5, TOOLBAR_H);
    cairo_line_to(cr, SIDEBAR_W - 0.5, a->WIN_H - STATUS_H);
    cairo_stroke(cr);

    glow_ensure();
    cairo_save(cr);
    cairo_rectangle(cr, 0, TOOLBAR_H, SIDEBAR_W - 1, h);
    cairo_clip(cr);
    glow_paint(cr, g_sidebar_glow, 36.0, TOOLBAR_H + 30.0, 260.0);
    cairo_restore(cr);

    cairo_save(cr);
    cairo_rectangle(cr, 0, TOOLBAR_H, SIDEBAR_W - 1, h);
    cairo_clip(cr);

    int y = TOOLBAR_H + 12 - a->sidebar_scroll;
    text_draw(cr, "QUICK ACCESS", 10, 700, C_FAINT, 0.85,
              18.0, y, ALIGN_LEFT, 1.4);
    y += 22;
    char favorites[24][PATH_MAX];
    int nf = 0;
    for (int i = 0; i < a->n_favorites && nf < 24; i++)
        snprintf(favorites[nf++], PATH_MAX, "%s", a->favorites[i]);
    const char *home = getenv("HOME");
    char places[8][PATH_MAX];
    int np = quick_access_places(places);
    for (int i = 0; i < nf; i++, y += 28) {
        int sel = !strcmp(a->cwd, favorites[i]);
        float hv = a->side_hov[i];
        if (sel) selected_box(cr, 7.0, y - 3.0, SIDEBAR_W - 14.0, 26.0);
        side_hover_paint(cr, y, 26.0, hv, 0);
        icon_draw(a, cr, place_icon(favorites[i]), 17.0, 14.0, y);
        char label[200];
        snprintf(label, sizeof label, "%s", base_name(favorites[i]));
        ui_ellipsize(cr, label, sizeof label, 12, 400, SIDEBAR_W - 56.0);
        /* 12px text ink ends 11px below its origin; 17px icons end ~15px
         * below the row origin. Offset by 4 to align their bottoms. */
        text_draw(cr, label, 12, 400, sel ? C_ACCENT : C_FG, 0.94 + 0.06 * hv,
                  38.0, y + 4.0, ALIGN_LEFT, 0.0);
    }
    for (int i = 0; i < np; i++, y += 28) {
        int sel = !strcmp(a->cwd, places[i]);
        float hv = a->side_hov[nf + i];
        if (sel) selected_box(cr, 7.0, y - 3.0, SIDEBAR_W - 14.0, 26.0);
        side_hover_paint(cr, y, 26.0, hv, 0);
        icon_draw(a, cr, place_icon(places[i]), 17.0, 14.0, y);
        char label[128];
        snprintf(label, sizeof label, "%s", base_name(places[i]));
        if (home && !strcmp(places[i], home))
            snprintf(label, sizeof label, "Home");
        if (path_is_trash(places[i]))
            snprintf(label, sizeof label, "Trash");
        ui_ellipsize(cr, label, sizeof label, 12, 400, SIDEBAR_W - 56.0);
        text_draw(cr, label, 12, 400, sel ? C_ACCENT : C_FG, 0.94 + 0.06 * hv,
                  38.0, y + 4.0, ALIGN_LEFT, 0.0);
    }
    y += 12;
    text_draw(cr, "DRIVES", 10, 700, C_FAINT, 0.85,
              18.0, y, ALIGN_LEFT, 1.4);
    y += 18;
    a->drives_y0 = y;
    for (int i = 0; i < a->n_drives; i++, y += 40) {
        Drive *d = &a->drives[i];
        int sel = a->sidebar_drive == i;
        float hv = a->side_hov[SIDE_HOV_DRIVE + i];
        float av = sel ? 1.0f : 0.0f;
        /* Swapped treatments: the solid accent fill is now the hover state,
         * while the layered fill, accent rail and lighter text mark the
         * active/selected drive. */
        if (hv > 0.004f) {
            rgba(cr, C_ACCENT, 0.16 * hv);
            rounded_rect(cr, 7.0, y - 3.0, SIDEBAR_W - 14.0, 38.0, 6.0);
            cairo_fill(cr);
        }
        side_hover_paint(cr, y, 38.0, av, 1);
        icon_draw(a, cr, d->icon, 19.0, 14.0, y + 1.0);
        char label[160];
        snprintf(label, sizeof label, "%s", d->label[0] ? d->label : d->dev);
        ui_ellipsize(cr, label, sizeof label, 11, 400, SIDEBAR_W - 52.0);
        text_draw(cr, label, 11, 400,
                  d->mounted || av > 0.35f ? C_FG : C_DIM,
                  0.95 + 0.05 * av, 40.0, y, ALIGN_LEFT, 0.0);
        char sub[200];
        if (d->mounted) snprintf(sub, sizeof sub, "%s", d->mount);
        else snprintf(sub, sizeof sub, "%s · not mounted", d->size);
        ui_ellipsize(cr, sub, sizeof sub, 9, 400, SIDEBAR_W - 52.0);
        text_draw(cr, sub, 9, 400,
                  d->mounted ? C_GREEN : (av > 0.35f ? C_DIM : C_FAINT),
                  0.85 + 0.10 * av, 40.0, y + 13.0, ALIGN_LEFT, 0.0);
        rgba(cr, C_FG, 0.07);
        rounded_rect(cr, 40.0, y + 27.0, SIDEBAR_W - 58.0, 3.0, 1.5);
        cairo_fill(cr);
        if (d->mounted && d->used > 0.0) {
            rgba(cr, d->used > 0.9 ? C_ACCENT : C_GREEN, 0.85);
            rounded_rect(cr, 40.0, y + 27.0,
                         (SIDEBAR_W - 58.0) * d->used, 3.0, 1.5);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

typedef struct {
    double x0, y0, tw, th;
    int cols, rows;
} IconGrid;

static double file_top_y(App *a)
{
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES)
        return TOOLBAR_H;
    return TOOLBAR_H + COLHEAD_H;
}

/* Search results get a taller two-line row; all list geometry goes through
 * this so hover, hit-testing, scrolling and the scrollbar stay in step. */
static double list_row_h(App *a)
{
    return a->search_active ? SEARCH_ROW_H : ROW_H;
}

static void icon_grid(App *a, IconGrid *g)
{
    double margin = a->view == VIEW_TILES ? 8.0 : 10.0;
    double pref_w = a->view == VIEW_TILES ? 136.0 : 104.0;
    double pref_h = a->view == VIEW_TILES ? 112.0 : 104.0;
    double top = file_top_y(a);
    double avail_w = a->WIN_W - SIDEBAR_W - margin * 2.0;
    double avail_h = a->WIN_H - STATUS_H - top - margin * 2.0;

    /* Nemo-style snap: whole cells that exactly fill the current view. */
    if (avail_w < pref_w) avail_w = pref_w;
    if (avail_h < pref_h) avail_h = pref_h;
    g->cols = (int)(avail_w / pref_w);
    g->rows = (int)(avail_h / pref_h);
    if (g->cols < 1) g->cols = 1;
    if (g->rows < 1) g->rows = 1;
    g->tw = avail_w / (double)g->cols;
    g->th = avail_h / (double)g->rows;
    g->x0 = SIDEBAR_W + margin;
    g->y0 = top + margin;
}

/* Scrolling is row-aligned so a resized grid never shows a partial row. */
static int icon_scroll_max(App *a, const IconGrid *g)
{
    int last_row = a->n_ents > 0 ? (a->n_ents - 1) / g->cols : 0;
    int max_row = last_row - g->rows + 1;
    if (max_row < 0) max_row = 0;
    return max_row * g->cols;
}

static int list_scroll_max(App *a)
{
    /* Only count rows that fit completely. Adding one here let the last row
     * land past the clip bottom at maximum scroll, so it was cut off with no
     * way to scroll it fully into view. */
    int visible = (int)((a->WIN_H - file_top_y(a) - STATUS_H) /
                        list_row_h(a));
    if (visible < 1) visible = 1;
    int max = a->n_ents - visible;
    return max > 0 ? max : 0;
}

static int view_scroll_max(App *a)
{
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        IconGrid g;
        icon_grid(a, &g);
        return icon_scroll_max(a, &g);
    }
    return list_scroll_max(a);
}

/* Scrollbar geometry, shared by drawing and hit-testing so the thumb always
 * matches what the user sees. Returns 0 when the view fits without one. */
static int scrollbar_geom(App *a, double *rx, double *ry, double *rw,
                          double *rh, double *ty, double *th, double *pos)
{
    double top = file_top_y(a);
    double bottom = a->WIN_H - STATUS_H;
    double track_h = bottom - top - 12.0;
    if (track_h < 40.0) return 0;

    int icon_view = a->view == VIEW_ICONS || a->view == VIEW_TILES;
    int max, visible, total;
    if (icon_view) {
        IconGrid g;
        icon_grid(a, &g);
        max = icon_scroll_max(a, &g);
        visible = g.rows;
        total = (a->n_ents + g.cols - 1) / g.cols;
    } else {
        max = list_scroll_max(a);
        visible = (int)((bottom - top) / list_row_h(a));
        if (visible < 1) visible = 1;
        total = a->n_ents;
    }
    if (max <= 0 || visible <= 0 || total <= visible) return 0;

    double v = a->smooth_scroll ? a->scroll_v : (double)a->scroll;
    if (v < 0.0) v = 0.0;
    if (v > (double)max) v = (double)max;
    v /= (double)max;

    double w = 8.0;
    double x = a->WIN_W - w - 4.0;
    double y = top + 6.0;
    double thumb_h = track_h * (double)visible / (double)total;
    if (thumb_h < 28.0) thumb_h = 28.0;
    if (thumb_h > track_h) thumb_h = track_h;
    double thumb_y = y + v * (track_h - thumb_h);

    if (rx) *rx = x;
    if (ry) *ry = y;
    if (rw) *rw = w;
    if (rh) *rh = track_h;
    if (ty) *ty = thumb_y;
    if (th) *th = thumb_h;
    if (pos) *pos = v;
    return 1;
}

/* Move the scroll target to a fraction of the scrollbar track. Dragging snaps
 * so the thumb stays under the pointer; other jumps can animate. */
static void scrollbar_set(App *a, double frac, int snap)
{
    double x, y, w, h, ty, th, cur;
    if (!scrollbar_geom(a, &x, &y, &w, &h, &ty, &th, &cur)) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    int max;
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        IconGrid g;
        icon_grid(a, &g);
        max = icon_scroll_max(a, &g);
        int idx = (int)lround(frac * (double)max);
        if (idx > max) idx = max;
        if (idx < 0) idx = 0;
        a->scroll = (idx / g.cols) * g.cols;
    } else {
        max = list_scroll_max(a);
        a->scroll = (int)lround(frac * (double)max);
    }
    if (snap) scroll_snap(a);
    else a->scroll_t = mono_sec();
    a->dirty = 1;
}

/* Hit tests follow what is drawn, not the pending wheel target. */
static int scroll_hit(App *a)
{
    if (!a->smooth_scroll) return a->scroll;
    int max = view_scroll_max(a);
    double v = a->scroll_v;
    if (v < 0.0) v = 0.0;
    if (v > (double)max) v = (double)max;
    int idx = (int)floor(v + 0.5);
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        IconGrid g;
        icon_grid(a, &g);
        idx = (idx / g.cols) * g.cols;
    }
    return idx;
}

static int icon_grid_hit(App *a, double x, double y, int *idx)
{
    IconGrid g;
    icon_grid(a, &g);
    if (x < g.x0 || y < g.y0 || y >= a->WIN_H - STATUS_H) return 0;
    int col = (int)((x - g.x0) / g.tw);
    int row = (int)((y - g.y0) / g.th);
    if (col < 0 || col >= g.cols || row < 0) return 0;
    int i = scroll_hit(a) + row * g.cols + col;
    if (i < 0 || i >= a->n_ents) return 0;
    if (idx) *idx = i;
    return 1;
}

static void icon_label_lines(cairo_t *cr, const char *name, double size,
                             int weight, double max_w,
                             char out[2][NAME_MAX + 1])
{
    out[0][0] = out[1][0] = '\0';
    if (!name || !*name) return;
    char text[NAME_MAX + 1];
    snprintf(text, sizeof text, "%s", name);

    PangoLayout *l = layout_make(cr, text, size, weight, 0.0);
    pango_layout_set_width(l, (int)lround(max_w * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    int n = pango_layout_get_line_count(l);
    if (n > 2) {
        PangoLayoutLine *ln = pango_layout_get_line(l, 1);
        int end = ln->start_index + ln->length;
        size_t total = strlen(text);
        if (end > (int)total) end = (int)total;
        char clipped[NAME_MAX + 1];
        snprintf(clipped, sizeof clipped, "%.*s", end, text);
        ui_ellipsize(cr, clipped, sizeof clipped, size, weight, max_w);
        g_object_unref(l);
        icon_label_lines(cr, clipped, size, weight, max_w, out);
        return;
    }
    for (int i = 0; i < n && i < 2; i++) {
        PangoLayoutLine *ln = pango_layout_get_line(l, i);
        snprintf(out[i], NAME_MAX + 1, "%.*s", ln->length,
                 text + ln->start_index);
    }
    g_object_unref(l);
}

static void select_in_rect(App *a, double x0, double y0, double x1, double y1,
                           int additive)
{
    post_xfer_disarm(a);
    if (!a->selected) return;
    double lx = x0 < x1 ? x0 : x1, rx = x0 > x1 ? x0 : x1;
    double ty = y0 < y1 ? y0 : y1, by = y0 > y1 ? y0 : y1;
    if (!additive) select_none(a);
    /* Compute the grid once and only walk visible cells. Rebuilding the grid
     * per entry made rubber-band selection an O(n_ents) hotspot on every
     * pointer-motion event. */
    IconGrid g;
    icon_grid(a, &g);
    int last = a->scroll + g.rows * g.cols;
    if (last > a->n_ents) last = a->n_ents;
    for (int i = a->scroll; i < last; i++) {
        int rel = i - a->scroll;
        int row = rel / g.cols, col = rel % g.cols;
        double cx = g.x0 + col * g.tw;
        double cy = g.y0 + row * g.th;
        if (cx + 4.0 > rx || cx + g.tw - 4.0 < lx ||
            cy + 2.0 > by || cy + g.th - 4.0 < ty)
            continue;
        if (!a->selected[i]) {
            a->selected[i] = 1;
            a->n_selected++;
        }
        a->sel = i;
    }
}

/* Same path start as jtwpi's jti_rounded_rect(): just past the top-left
 * radius, clockwise.  That keeps the bottom-center dash offset identical. */
static void lap_rounded_rect(cairo_t *cr, double x, double y,
                             double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;
    cairo_new_path(cr);
    cairo_move_to(cr, x + r, y);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

/* Menu popups use the same timing for a lap around their own border. */
static double menu_popup_elapsed_ms(const Menu *m)
{
    if (!m || m->popup_anim_start <= 0.0) return SEL_LAP_MS;
    return (mono_sec() - m->popup_anim_start) * 1000.0;
}

static int menu_popup_anim_active(const Menu *m)
{
    return m && m->app && m->app->border_anim_enabled &&
           m->popup_anim_active && menu_popup_elapsed_ms(m) < SEL_LAP_MS;
}

static void menu_tree_tick(Menu *m, double now)
{
    if (!m) return;
    if (m->popup_anim_active && m->popup_anim_start > 0.0) {
        if ((now - m->popup_anim_start) * 1000.0 >= SEL_LAP_MS) {
            m->popup_anim_active = 0;
            m->popup_anim_start = 0.0;
        }
        /* Keep repainting while the segment moves, and once more for the
         * clean static border after it finishes. */
        m->dirty = 1;
    }
    for (int i = 0; i < m->n; i++) menu_tree_tick(m->items[i].sub, now);
}

static int menu_tree_anim_active(const Menu *m)
{
    if (!m) return 0;
    if (menu_popup_anim_active(m)) return 1;
    for (int i = 0; i < m->n; i++)
        if (menu_tree_anim_active(m->items[i].sub)) return 1;
    return 0;
}

/* One accent segment runs clockwise from bottom center all the way around
 * the selection border, with the same fade, width, halo and timing as jtwpi. */
static void draw_sel_lap(cairo_t *cr, double x, double y,
                         double w, double h, double r, double progress)
{
    double top = w - 2.0 * r, side = h - 2.0 * r;
    double perimeter = 2.0 * (top + side) + 2.0 * PI * r;
    if (perimeter <= 2.0) return;

    double start = 1.5 * top + side + PI * r;
    double travelled = progress * perimeter;
    double seg = fmin(SEL_LAP_MAX_SEG,
                      fmax(SEL_LAP_MIN_SEG, perimeter / 10.0));
    seg = fmin(seg, perimeter / 2.0);

    double alpha = 1.0;
    if (progress < 0.08) alpha = progress / 0.08;
    else if (progress > 0.92) alpha = (1.0 - progress) / 0.08;
    if (alpha <= 0.0) return;

    cairo_save(cr);
    double dash[2] = { seg, perimeter - seg };
    cairo_set_dash(cr, dash, 2, -fmod(start + travelled, perimeter));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    /* cairo_stroke() clears the path, so rebuild it for the crisp core. */
    lap_rounded_rect(cr, x, y, w, h, r);
    rgba(cr, C_ACCENT_HI, alpha * 0.18);
    cairo_set_line_width(cr, SEL_LAP_WIDTH + 5.0);
    cairo_stroke(cr);

    lap_rounded_rect(cr, x, y, w, h, r);
    rgba(cr, C_ACCENT_HI, alpha);
    cairo_set_line_width(cr, SEL_LAP_WIDTH);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_files(App *a, cairo_t *cr)
{
    double x0 = SIDEBAR_W;
    double vw = a->WIN_W - SIDEBAR_W;
    /* Search results always use list rows so the containing directory fits
     * underneath the name. The normal view is restored when search closes. */
    int icon_view = !a->search_active &&
                    (a->view == VIEW_ICONS || a->view == VIEW_TILES);
    double list_y = file_top_y(a);
    double list_h = a->WIN_H - list_y - STATUS_H;

    /* Windows Explorer only shows column headers in Details view. Like the
     * toolbar, the header has no opaque fill so the window gradient shows
     * through; the hairline below is enough to separate it from the list. */
    if (!icon_view) {
        rgba(cr, C_FG, 0.06);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, x0, TOOLBAR_H + COLHEAD_H - 0.5);
        cairo_line_to(cr, a->WIN_W, TOOLBAR_H + COLHEAD_H - 0.5);
        cairo_stroke(cr);

        text_draw(cr, a->search_active ? "Name / Location" : "Name",
                  11, 600, C_DIM, 0.95, x0 + 46.0,
                  TOOLBAR_H + 7.0, ALIGN_LEFT, 0.5);
        if (a->view == VIEW_DETAILS && !a->search_active) {
            text_draw(cr, "Date modified", 11, 600, C_DIM, 0.95,
                      x0 + 350.0, TOOLBAR_H + 7.0, ALIGN_LEFT, 0.5);
            text_draw(cr, "Type", 11, 600, C_DIM, 0.95,
                      x0 + 570.0, TOOLBAR_H + 7.0, ALIGN_LEFT, 0.5);
            text_draw(cr, "Size", 11, 600, C_DIM, 0.95,
                      a->WIN_W - 18.0, TOOLBAR_H + 7.0, ALIGN_RIGHT, 0.5);
        }
    }

    cairo_save(cr);
    cairo_rectangle(cr, x0, list_y, vw, list_h);
    cairo_clip(cr);

    double lap_progress = post_sel_anim_active(a) ?
                          post_sel_elapsed_ms(a) / SEL_LAP_MS : 0.0;

    if (icon_view) {
        IconGrid g;
        icon_grid(a, &g);
        a->scroll_max = icon_scroll_max(a, &g);

        int first = a->scroll;
        if (first < 0) first = 0;
        first = (first / g.cols) * g.cols;
        if (first > a->scroll_max) first = a->scroll_max;
        a->scroll = first;

        /* Draw from the animated position so rows slide between snapped
           positions instead of jumping a whole row per wheel step. */
        double sv = a->smooth_scroll ? a->scroll_v : (double)first;
        if (sv < 0.0) sv = 0.0;
        if (sv > (double)a->scroll_max) sv = (double)a->scroll_max;
        double srow = sv / (double)g.cols;
        int first_row = (int)floor(srow + 1e-9);
        int count = (g.rows + 2) * g.cols;

        double icon_sz = a->view == VIEW_TILES ? 64.0 : 48.0;
        for (int k = 0; k < count; k++) {
            int i = first_row * g.cols + k;
            if (i < 0 || i >= a->n_ents) continue;
            Entry *e = &a->ents[i];
            int rel = k;
            int col = rel % g.cols, row = rel / g.cols;
            double cell_x = g.x0 + col * g.tw;
            double cell_y = g.y0 + (first_row + row - srow) * g.th;
            if (cell_y > a->WIN_H - STATUS_H || cell_y + g.th < list_y) continue;
            double cx = cell_x + g.tw / 2.0;

            if (a->selected[i] || i == a->sel || i == a->hover_row) {
                rgba(cr, C_ACCENT,
                     a->selected[i] ? 0.20 : (i == a->sel ? 0.10 : 0.05));
                rounded_rect(cr, cell_x + 4.0, cell_y + 2.0,
                             g.tw - 8.0, g.th - 6.0, 8.0);
                cairo_fill(cr);
                /* Keep the fill, but let the travelling segment stand in for
                 * the static border until the lap finishes. */
                if (a->selected[i] && !post_sel_entry_pending(a, i)) {
                    rgba(cr, C_ACCENT_HI, 0.95);
                    cairo_set_line_width(cr, 1.2);
                    rounded_rect(cr, cell_x + 4.0, cell_y + 2.0,
                                 g.tw - 8.0, g.th - 6.0, 8.0);
                    cairo_stroke(cr);
                }
            }

            double icon_top = a->view == VIEW_TILES ? 6.0 : 8.0;
            icon_draw_entry(a, cr, e, icon_sz,
                            cx - icon_sz / 2.0, cell_y + icon_top, 1);

            char lines[2][NAME_MAX + 1];
            /* Same font and color as the list-layout item names. */
            icon_label_lines(cr, e->name, 12, 400, g.tw - 18.0, lines);
            double label_gap = a->view == VIEW_TILES ? 6.0 : 8.0;
            double label_y = cell_y + icon_top + icon_sz + label_gap;
            text_draw(cr, lines[0], 12, 400, C_FG, 1.0,
                      cx, label_y, ALIGN_CENTER, 0.0);
            if (lines[1][0])
                text_draw(cr, lines[1], 12, 400, C_FG, 1.0,
                          cx, label_y + 14.0, ALIGN_CENTER, 0.0);
            if (a->selected[i] && post_sel_entry_pending(a, i))
                draw_sel_lap(cr, cell_x + 4.0, cell_y + 2.0,
                             g.tw - 8.0, g.th - 6.0, 8.0, lap_progress);
        }

        if (a->drag_select) {
            double mx = a->drag_x0 < a->drag_x1 ? a->drag_x0 : a->drag_x1;
            double my = a->drag_y0 < a->drag_y1 ? a->drag_y0 : a->drag_y1;
            double mw = fabs(a->drag_x1 - a->drag_x0);
            double mh = fabs(a->drag_y1 - a->drag_y0);
            rgba(cr, C_ACCENT, 0.12);
            rounded_rect(cr, mx, my, mw, mh, 3.0);
            cairo_fill(cr);
            rgba(cr, C_ACCENT, 0.75);
            cairo_set_line_width(cr, 1.0);
            rounded_rect(cr, mx + 0.5, my + 0.5,
                         mw - 1.0, mh - 1.0, 2.5);
            cairo_stroke(cr);
        }
    } else {
        double rh = list_row_h(a);
        int visible = (int)(list_h / rh) + 1;
        a->scroll_max = list_scroll_max(a);
        int first = a->scroll;
        if (first < 0) first = 0;
        if (first > a->scroll_max) first = a->scroll_max;
        a->scroll = first;

        double sv = a->smooth_scroll ? a->scroll_v : (double)first;
        if (sv < 0.0) sv = 0.0;
        if (sv > (double)a->scroll_max) sv = (double)a->scroll_max;
        int base = (int)floor(sv);
        double frac = sv - (double)base;
        for (int k = -1; k <= visible; k++) {
            int i = base + k;
            if (i < 0 || i >= a->n_ents) continue;
            Entry *e = &a->ents[i];
            double y = list_y + (k - frac) * rh;

            double sel_x = x0 + 7.0, sel_y = y + 1.0;
            double sel_w = vw - 14.0, sel_h = 26.0, sel_r = 6.0;
            if (a->search_active) {
                /* Result rows are rounded cards with room for two lines, so
                 * hover/selection targets are much larger than a normal row. */
                double hx = x0 + 8.0, hy = y + 3.0;
                double hw = vw - 20.0, hh = rh - 6.0;
                sel_x = hx; sel_y = hy; sel_w = hw; sel_h = hh;
                if (a->selected[i])
                    selected_box(cr, hx, hy, hw, hh);
                else if (i == a->hover_row) {
                    rounded_rect(cr, hx, hy, hw, hh, 6.0);
                    rgba(cr, C_FG, 0.065);
                    cairo_fill(cr);
                }
                if (i > 0) {
                    rgba(cr, C_FG, 0.035);
                    cairo_set_line_width(cr, 1.0);
                    cairo_move_to(cr, hx, y + rh - 0.5);
                    cairo_line_to(cr, hx + hw, y + rh - 0.5);
                    cairo_stroke(cr);
                }
                rounded_rect(cr, x0 + 16.0, y + 12.0, 32.0, 32.0, 8.0);
                rgba(cr, C_FG, 0.045);
                cairo_fill(cr);
                icon_draw_entry(a, cr, e, 24.0, x0 + 20.0, y + 16.0, 0);

                char label[NAME_MAX + 1], dir[PATH_MAX];
                snprintf(label, sizeof label, "%s", e->name);
                double label_w = vw - 92.0;
                ui_ellipsize(cr, label, sizeof label, 13, 600, label_w);
                /* Two text lines are vertically centered in the 56px card,
                 * so the location line keeps real breathing room at the
                 * bottom instead of sitting on the border. */
                text_draw(cr, label, 13, 600,
                          a->selected[i] ? C_ACCENT : C_FG, 1.0,
                          x0 + 60.0, y + 12.0, ALIGN_LEFT, 0.0);
                search_rel_dir(a, e, dir, sizeof dir);
                ui_ellipsize(cr, dir, sizeof dir, 10.5, 400, label_w);
                text_draw(cr, dir, 10.5, 400, C_FAINT, 0.85,
                          x0 + 60.0, y + 30.0, ALIGN_LEFT, 0.0);
            } else {
                if (a->selected[i])
                    selected_box(cr, sel_x, sel_y, sel_w, sel_h);
                else if (i == a->hover_row) {
                    rounded_rect(cr, sel_x, sel_y, sel_w, sel_h, sel_r);
                    rgba(cr, C_FG, 0.045);
                    cairo_fill(cr);
                }
                icon_draw_entry(a, cr, e, 19.0, x0 + 16.0, y + 4.0, 0);
                char label[NAME_MAX + 1];
                snprintf(label, sizeof label, "%s", e->name);
                ui_ellipsize(cr, label, sizeof label, 12, 400, 280.0);
                /* 19px icons end ~21px below the row origin; align text ink
                 * bottoms with that instead of sitting them near the top. */
                text_draw(cr, label, 12, 400,
                          a->selected[i] ? C_ACCENT : C_FG, 1.0,
                          x0 + 46.0, y + 7.0, ALIGN_LEFT, 0.0);
                if (a->view == VIEW_DETAILS) {
                    char date[64];
                    fmt_date(date, sizeof date, e->mtime);
                    text_draw(cr, date, 11, 400, C_DIM, 0.85,
                              x0 + 350.0, y + 8.0, ALIGN_LEFT, 0.0);
                    char type[48];
                    snprintf(type, sizeof type, "%s", e->type);
                    ui_ellipsize(cr, type, sizeof type, 11, 400, 170.0);
                    text_draw(cr, type, 11, 400, C_DIM, 0.85,
                              x0 + 570.0, y + 8.0, ALIGN_LEFT, 0.0);
                    char sz[32];
                    entry_size_text(a, e, sz, sizeof sz);
                    text_draw(cr, sz, 11, 400, C_DIM, 0.85,
                              a->WIN_W - 18.0, y + 8.0, ALIGN_RIGHT, 0.0);
                }
            }
            if (a->selected[i] && post_sel_entry_pending(a, i))
                draw_sel_lap(cr, sel_x, sel_y, sel_w, sel_h, sel_r,
                             lap_progress);
        }
    }

    if (a->n_ents == 0) {
        /* Center the placeholder in the file area, not the whole window, and
         * derive its box and type sizes from the available content rect so it
         * stays proportionate when the window is resized. */
        double rw = DESIGN_W - SIDEBAR_W;
        double rh = DESIGN_H - TOOLBAR_H - COLHEAD_H - STATUS_H;
        double s = fmin(vw / rw, list_h / rh);
        if (s < 0.62) s = 0.62;
        if (s > 1.65) s = 1.65;

        double bw = fmin(vw * 0.56, 460.0 * s);
        double bh = fmin(list_h * 0.56, 250.0 * s);
        if (bw < 220.0) bw = fmin(220.0, vw - 24.0);
        if (bh < 150.0) bh = fmin(150.0, list_h - 24.0);
        if (bw < 160.0) bw = vw - 16.0;
        if (bh < 110.0) bh = list_h - 16.0;
        if (bw < 40.0) bw = vw;
        if (bh < 40.0) bh = list_h;

        double bx = x0 + (vw - bw) / 2.0;
        double by = list_y + (list_h - bh) / 2.0;
        double cx = bx + bw / 2.0;
        double cy = by + bh / 2.0;

        double rad = fmin(16.0 * s, bh / 2.0);
        rounded_rect(cr, bx, by, bw, bh, rad);
        rgba(cr, C_FG, 0.035);
        cairo_fill(cr);
        rgba(cr, C_BORDER, 0.28);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, bx + 0.5, by + 0.5, bw - 1.0, bh - 1.0,
                     rad > 0.5 ? rad - 0.5 : 0.0);
        cairo_stroke(cr);

        if (a->search_active) {
            double is = fmax(56.0 * s, 40.0);
            double ts = fmax(13.0 * s, 11.0);
            double ss = fmax(10.0 * s, 9.0);
            double g1 = 16.0 * s, g2 = 8.0 * s;
            double group_h = is + g1 + ts * 1.25 + g2 + ss * 1.25 + 12.0 * s;
            double top = cy - group_h / 2.0;

            icon_draw(a, cr, "system-search-symbolic", is,
                      cx - is / 2.0, top);
            text_draw(cr, "No matches", ts, 400, C_DIM, 0.85,
                      cx, top + is + g1, ALIGN_CENTER, 0.2 * s);
            char where[PATH_MAX], label[PATH_MAX + 8];
            snprintf(where, sizeof where, "%s", a->cwd);
            ui_ellipsize(cr, where, sizeof where, ss, 400, bw - 40.0 * s);
            snprintf(label, sizeof label, "in %s", where);
            text_draw(cr, label, ss, 400, C_FAINT, 0.7,
                      cx, top + is + g1 + ts * 1.25 + g2,
                      ALIGN_CENTER, 0.2 * s);
        } else {
            double is = fmax(64.0 * s, 40.0);
            double ts = fmax(13.0 * s, 11.0);
            double g1 = 16.0 * s;
            double group_h = is + g1 + ts * 1.25 + 12.0 * s;
            double top = cy - group_h / 2.0;

            icon_draw(a, cr, "folder", is, cx - is / 2.0, top);
            text_draw(cr, "This folder is empty", ts, 400, C_DIM, 0.8,
                      cx, top + is + g1, ALIGN_CENTER, 0.2 * s);
        }
    }
    cairo_restore(cr);
}

/* Accent-colored scrollbar in the file view's right gutter. */
static void draw_scrollbar(App *a, cairo_t *cr)
{
    double x, y, w, h, ty, th, frac;
    if (!scrollbar_geom(a, &x, &y, &w, &h, &ty, &th, &frac)) return;

    rgba(cr, C_FG, 0.045);
    rounded_rect(cr, x, y, w, h, w / 2.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.30);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, (w - 1.0) / 2.0);
    cairo_stroke(cr);

    /* C_ACCENT is updated live by jtfilesctl, so the thumb follows the scheme. */
    double alpha = a->sb_drag ? 1.0 : (a->sb_hover ? 0.95 : 0.82);
    rgba(cr, C_ACCENT, alpha);
    rounded_rect(cr, x + 1.0, ty, w - 2.0, th, (w - 2.0) / 2.0);
    cairo_fill(cr);
    rgba(cr, C_ACCENT_HI, 0.45 * alpha);
    rounded_rect(cr, x + 2.0, ty + 1.0, w - 4.0, th - 2.0, (w - 4.0) / 2.0);
    cairo_fill(cr);
}

static void draw_status(App *a, cairo_t *cr)
{
    double y = a->WIN_H - STATUS_H;
    cairo_rectangle(cr, 0, y, a->WIN_W, STATUS_H);
    rgba(cr, 0x0A0A0B, 1.0);
    cairo_fill(cr);
    rgba(cr, C_FG, 0.07);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, y + 0.5);
    cairo_line_to(cr, a->WIN_W, y + 0.5);
    cairo_stroke(cr);

    char msg[400];
    if (a->status[0] && mono_sec() - a->status_t < 8.0)
        snprintf(msg, sizeof msg, "%s", a->status);
    else if (a->search_active && a->search_pending)
        snprintf(msg, sizeof msg, "Searching for “%s”…", a->search);
    else if (a->search_active)
        snprintf(msg, sizeof msg, "%d result%s%s%s",
                 a->n_ents, a->n_ents == 1 ? "" : "s",
                 a->search_truncated ? "+" : "",
                 a->n_selected ? "  •  selected" : "");
    else snprintf(msg, sizeof msg, "%d item%s%s", a->n_ents,
                  a->n_ents == 1 ? "" : "s",
                  a->n_selected ? "  •  selected" : "");
    text_draw(cr, msg, 11, 400, C_DIM, 0.9, 18.0, y + 8.0,
              ALIGN_LEFT, 0.1);
    text_draw(cr, "JT FILES", 10, 600, C_FAINT, 0.75,
              a->WIN_W - 18.0, y + 8.0, ALIGN_RIGHT, 1.2);
}

static void draw_dialog_content(App *a, cairo_t *cr, double x, double y)
{
    Dialog *d = &a->dialog;
    double w = 460.0, h = d->type == 3 ? 290.0 : 176.0;

    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_rectangle(cr, x + 3.0, y + 5.0, w, h);
    cairo_fill(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(g, 0, R_(0x161719), G_(0x161719),
                                      B_(0x161719), 1);
    cairo_pattern_add_color_stop_rgba(g, 1, R_(0x0E0E10), G_(0x0E0E10),
                                      B_(0x0E0E10), 1);
    cairo_set_source(cr, g);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    text_draw(cr, d->title, 14, 600, C_FG, 0.96, x + 20.0, y + 18.0,
              ALIGN_LEFT, 0.0);
    rgba(cr, C_FG, 0.06);
    cairo_move_to(cr, x + 20.0, y + 46.5);
    cairo_line_to(cr, x + w - 20.0, y + 46.5);
    cairo_stroke(cr);

    if (d->type == 3) {
        for (int i = 0; i < d->nlines; i++)
            text_draw(cr, d->lines[i], 12, 400, i == 0 ? C_FG : C_DIM,
                      0.92, x + 22.0, y + 66.0 + i * 26.0,
                      ALIGN_LEFT, 0.0);
    } else if (d->type == 4) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", d->text);
        ui_ellipsize(cr, msg, sizeof msg, 12, 400, w - 44.0);
        text_draw(cr, msg, 12, 400, C_DIM, 0.92,
                  x + 22.0, y + 76.0, ALIGN_LEFT, 0.0);
    } else {
        text_draw(cr, d->type == 2 ? "Name:" : "New name:", 11, 400,
                  C_DIM, 0.9, x + 22.0, y + 68.0, ALIGN_LEFT, 0.0);
        rounded_rect(cr, x + 22.0, y + 84.0, w - 44.0, 34.0, 7.0);
        rgba(cr, 0x070708, 1.0);
        cairo_fill(cr);
        rgba(cr, C_ACCENT, 0.75);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 22.5, y + 84.5, w - 45.0, 33.0, 6.5);
        cairo_stroke(cr);
        char txt[512];
        snprintf(txt, sizeof txt, "%s", d->text);
        ui_ellipsize(cr, txt, sizeof txt, 12, 400, w - 90.0);
        text_draw(cr, txt, 12, 400, C_FG, 0.96,
                  x + 32.0, y + 92.0, ALIGN_LEFT, 0.0);
        double cw = text_width(cr, txt, 12, 400, 0.0);
        rgba(cr, C_ACCENT, 0.9);
        cairo_move_to(cr, x + 32.0 + cw + 1.0, y + 90.0);
        cairo_line_to(cr, x + 32.0 + cw + 1.0, y + 111.0);
        cairo_stroke(cr);
    }

    const char *ok = d->type == 3 ? "Close" :
                     (d->type == 4 ? "Delete" : "OK");
    double bw = 84.0, by = y + h - 48.0;
    rounded_rect(cr, x + w - 22.0 - bw * 2.0 - 8.0, by, bw, 32.0, 7.0);
    rgba(cr, C_FG, 0.06);
    cairo_fill(cr);
    text_draw(cr, "Cancel", 12, 500, C_DIM, 0.95,
              x + w - 22.0 - bw * 2.0 - 8.0 + bw / 2.0, by + 8.0,
              ALIGN_CENTER, 0.0);
    rounded_rect(cr, x + w - 22.0 - bw, by, bw, 32.0, 7.0);
    rgba(cr, d->type == 4 ? C_DANGER : C_ACCENT, 1.0);
    cairo_fill(cr);
    text_draw(cr, ok, 12, 600,
              d->type == 4 ? C_FG : C_ON_ACCENT, 1.0,
              x + w - 22.0 - bw / 2.0, by + 8.0, ALIGN_CENTER, 0.0);
}

static void menu_paint(App *a, Menu *m, ShmBuf *b)
{
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, b->width, b->height, b->width * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    double popup_ms = menu_popup_elapsed_ms(m);
    int popup_lap = menu_popup_anim_active(m);
    double popup_progress = popup_lap ? popup_ms / SEL_LAP_MS : 1.0;

    /* Clip the offset shadow to the panel path: otherwise it fills the
     * transparent wedges outside the bottom rounded corners, which show up
     * as black artifacts under the popup. */
    cairo_save(cr);
    rounded_rect(cr, 0.0, 0.0, m->w, m->h, 8.0);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    rounded_rect(cr, 2.0, 4.0, m->w, m->h, 8.0);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, m->h);
    cairo_pattern_add_color_stop_rgba(g, 0, R_(0x1A1B1D), G_(0x1A1B1D),
                                      B_(0x1A1B1D), 0.98);
    cairo_pattern_add_color_stop_rgba(g, 1, R_(0x111113), G_(0x111113),
                                      B_(0x111113), 0.98);
    cairo_set_source(cr, g);
    rounded_rect(cr, 0, 0, m->w, m->h, 8.0);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    rgba(cr, C_BORDER, 0.85);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, 0.5, 0.5, m->w - 1.0, m->h - 1.0, 7.5);
    cairo_stroke(cr);
    if (popup_lap) {
        /* Inset enough for the 3px lap stroke to stay inside the buffer. */
        draw_sel_lap(cr, 2.5, 2.5, m->w - 5.0, m->h - 5.0, 6.0,
                     popup_progress);
    }

    /* Only the rows scroll; the popup frame and later scrollbar stay put. */
    cairo_save(cr);
    if (m->max_scroll > 0)
        cairo_rectangle(cr, 1.0, 1.0, m->w - 11.0, m->h - 2.0);
    else
        cairo_rectangle(cr, 1.0, 1.0, m->w - 2.0, m->h - 2.0);
    cairo_clip(cr);
    cairo_translate(cr, 0.0, -m->scroll_v);

    int cy = 5;
    for (int i = 0; i < m->n; i++) {
        MenuItem *it = &m->items[i];
        int hh = it->kind == 1 ? 9 : 26;
        if (it->kind == 1) {
            rgba(cr, C_FG, 0.09);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 8.0, cy + 4.0);
            cairo_line_to(cr, m->w - 8.0, cy + 4.0);
            cairo_stroke(cr);
        } else {
            int hot = i == m->hover && it->enabled;
            if (hot) {
                rgba(cr, C_ACCENT, 0.28);
                rounded_rect(cr, 5.0, cy, m->w - 10.0, hh, 4.0);
                cairo_fill(cr);
            }
            unsigned col = it->enabled ? (hot ? C_FG : 0xE8E8E8) : C_FAINT;
            if (it->kind == 3 && it->checked) {
                rgba(cr, C_ACCENT, 0.95);
                cairo_move_to(cr, 11.0, cy + 12.0);
                cairo_line_to(cr, 14.5, cy + 16.0);
                cairo_line_to(cr, 21.0, cy + 7.0);
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            }
            double lx = it->kind == 3 ? 29.0 : 14.0;
            if (it->icon[0]) {
                icon_draw(a, cr, it->icon, 16.0, lx, cy + 5.0);
                lx += 22.0;
            }
            text_draw(cr, it->label, 13, 400, col, 0.97,
                      lx, cy + 5.0, ALIGN_LEFT, 0.0);
            if (it->shortcut[0])
                text_draw(cr, it->shortcut, 11, 400, C_FAINT, 0.9,
                          m->w - 14.0, cy + 6.0, ALIGN_RIGHT, 0.0);
            if (it->kind == 2) {
                rgba(cr, col, 0.9);
                cairo_move_to(cr, m->w - 15.0, cy + 9.0);
                cairo_line_to(cr, m->w - 10.0, cy + 13.0);
                cairo_line_to(cr, m->w - 15.0, cy + 17.0);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
        }
        cy += hh;
    }
    cairo_restore(cr);

    if (m->max_scroll > 0) {
        int tx = m->w - 8, ty = 8, tw = 4, th = m->h - 16;
        if (th < 8) { ty = 2; th = m->h - 4; }
        rgba(cr, C_FG, 0.10);
        rounded_rect(cr, tx, ty, tw, th, 2.0);
        cairo_fill(cr);
        int thumb = (int)(th * (double)m->h / (double)m->content_h);
        if (thumb < 24) thumb = 24;
        if (thumb > th) thumb = th;
        double frac = m->max_scroll > 0
            ? m->scroll_v / (double)m->max_scroll : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int y = ty + (int)((double)(th - thumb) * frac);
        rgba(cr, C_ACCENT, 0.75);
        rounded_rect(cr, tx, y, tw, thumb, 2.0);
        cairo_fill(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
}

/* ------------------------------------------------------------------ */
/*  Wayland backend: buffers and painting                              */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111

static int create_anon_file(size_t size)
{
    int fd = memfd_create("jtfiles", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0) return fd;
        close(fd);
    }
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtfiles-XXXXXX", (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static void shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) { wl_buffer_destroy(b->buffer); b->buffer = NULL; }
    if (b->data) { munmap(b->data, b->size); b->data = NULL; }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired) shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    if (w <= 0 || h <= 0 || w > INT32_MAX / 4) return -1;
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    if (size > INT32_MAX) return -1;
    int fd = create_anon_file(size);
    if (fd < 0) return -1;
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) { munmap(data, size); close(fd); return -1; }
    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) { munmap(data, size); return -1; }
    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *next_buffer(App *a, ShmBuf *bufs, int count, int w, int h)
{
    for (int i = 0; i < count; i++) {
        ShmBuf *b = &bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h) return b;
    }
    for (int i = 0; i < count; i++) {
        ShmBuf *b = &bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0) return b;
    }
    for (int i = 0; i < count; i++) {
        ShmBuf *b = &bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0) return b;
        }
    }
    return NULL;
}

static void frame_done_app(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    App *a = data;
    if (a->frame_cb == cb) a->frame_cb = NULL;
    wl_callback_destroy(cb);
    /* The callback's only job is to wake the event loop at presentation time.
     * Animation stepping happens in the main loop before the next paint;
     * ticking here made every input change paint one stale frame first. */
}

static const struct wl_callback_listener frame_listener_app = {
    .done = frame_done_app,
};

static void frame_done_menu(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    Menu *m = data;
    if (m->frame_cb == cb) m->frame_cb = NULL;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener_menu = {
    .done = frame_done_menu,
};

static void request_frame_app(App *a)
{
    if (!a->surface || a->frame_cb) return;
    a->frame_cb = wl_surface_frame(a->surface);
    if (!a->frame_cb) return;
    wl_callback_add_listener(a->frame_cb, &frame_listener_app, a);
}

static void request_frame_menu(Menu *m)
{
    if (!m->surface || m->frame_cb) return;
    m->frame_cb = wl_surface_frame(m->surface);
    if (!m->frame_cb) return;
    wl_callback_add_listener(m->frame_cb, &frame_listener_menu, m);
}

static int app_draw(App *a)
{
    if (!a->configured || !a->surface || !a->shm) return 0;
    double wd = a->WIN_W * a->scale, hd = a->WIN_H * a->scale;
    if (!(wd >= 1.0 && hd >= 1.0 && wd <= 16384.0 && hd <= 16384.0)) return 0;
    int w = (int)lround(wd), h = (int)lround(hd);
    ShmBuf *b = next_buffer(a, a->bufs, NBUFS, w, h);
    if (!b) return 0;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    /* The gradient below is fully opaque, so a separate SOURCE clear only
     * doubled the full-buffer fill on every animation frame. */
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, a->WIN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0, R_(C_BG_TOP), G_(C_BG_TOP),
                                      B_(C_BG_TOP), 1);
    cairo_pattern_add_color_stop_rgba(bg, 1, R_(C_BG), G_(C_BG), B_(C_BG), 1);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    glow_ensure();
    glow_paint(cr, g_ambient_glow,
               SIDEBAR_W + (a->WIN_W - SIDEBAR_W) * 0.62, 120.0, 520.0);

    draw_toolbar(a, cr);
    draw_sidebar(a, cr);
    draw_files(a, cr);
    draw_scrollbar(a, cr);
    draw_status(a, cr);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    /* Damage is in buffer (device-pixel) coordinates, not logical ones. */
    wl_surface_damage(a->surface, 0, 0, w, h);
    request_frame_app(a);
    wl_surface_commit(a->surface);
    b->busy = 1;
    return 1;
}

static int menu_draw(App *a, Menu *m)
{
    if (!m->surface || !m->configured || m->done) return 0;
    int w = (int)lround(m->w * a->scale);
    int h = (int)lround(m->h * a->scale);
    if (w <= 0 || h <= 0) return 0;
    ShmBuf *b = next_buffer(a, m->bufs, 2, w, h);
    if (!b) return 0;
    menu_paint(a, m, b);
    if (wl_surface_get_version(m->surface) >= 3)
        wl_surface_set_buffer_scale(m->surface, a->scale);
    wl_surface_attach(m->surface, b->buffer, 0, 0);
    wl_surface_damage(m->surface, 0, 0, m->w, m->h);
    request_frame_menu(m);
    wl_surface_commit(m->surface);
    b->busy = 1;
    return 1;
}

static void icon_cache_clear(App *a)
{
    for (int i = 0; i < a->n_icons; i++) {
        if (a->icons[i].sf) cairo_surface_destroy(a->icons[i].sf);
        a->icons[i].sf = NULL;
        a->icons[i].name[0] = '\0';
        a->icons[i].px = 0;
        a->icons[i].used = 0;
    }
    a->n_icons = 0;
    a->icon_clock = 0;
}

/* Mark an open menu tree for repaint so its accent-colored rows refresh. */
static void menu_mark_dirty(Menu *m)
{
    if (!m) return;
    m->dirty = 1;
    for (int i = 0; i < m->n; i++)
        menu_mark_dirty(m->items[i].sub);
}

/* Invalidate everything that caches the accent colour. Used by explicit
 * accents and by each rainbow hue step. */
static void accent_visual_refresh(App *a)
{
    icon_cache_clear(a);
    a->dirty = 1;
    menu_mark_dirty(a->menu);
    a->xfer.dirty = 1;
}

/* Apply an accent change everywhere it is cached. Shared by the ctl socket
 * and by the config-file watcher so both paths behave identically. */
static void accent_propagate(App *a, unsigned rgb)
{
    /* An explicit colour must disable rainbow even when it happens to match
     * the hue currently being displayed. */
    if (!rainbow_mode && accent_rgb == rgb) return;
    accent_set(rgb);
    accent_visual_refresh(a);
}

/* Follow /etc/jt/accent.conf even when JTLab did not broadcast to us. The
 * mtime check keeps this cheap and avoids reloading on every loop pass. */
static void accent_conf_poll(App *a)
{
    if (accent_cli_override) return;
    struct stat st;
    if (stat(ACCENT_CONF_PATH, &st) != 0) return;
    if (accent_conf_mtime != 0 && st.st_mtime == accent_conf_mtime) return;
    accent_conf_mtime = st.st_mtime;

    /* Rainbow owns the accent. Consume the mtime change without adopting the
     * new colour, so "rainbow off" still restores the original base. */
    if (rainbow_mode) return;

    FILE *f = fopen(ACCENT_CONF_PATH, "r");
    if (!f) return;
    char buf[64];
    if (fgets(buf, sizeof buf, f)) {
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                         e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
        unsigned rgb;
        if (accent_parse(s, &rgb)) accent_propagate(a, rgb);
    }
    fclose(f);
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale) return;
    int s = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (a->outputs[i].output && a->outputs[i].entered &&
            a->outputs[i].scale > s) s = a->outputs[i].scale;
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    if (s == a->scale) return;
    a->scale = s;
    icon_cache_clear(a);
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer) continue;
        if (b->busy) b->retired = 1;
        else shm_buffer_destroy(b);
    }
    a->dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Pointer and keyboard                                               */
/* ------------------------------------------------------------------ */

static Menu *menu_find_surface_r(Menu *m, struct wl_surface *s)
{
    if (!m) return NULL;
    if (m->surface == s) return m;
    for (int i = 0; i < m->n; i++) {
        Menu *r = menu_find_surface_r(m->items[i].sub, s);
        if (r) return r;
    }
    return NULL;
}

static void menu_clear_children(Menu *m)
{
    for (int i = 0; i < m->n; i++) {
        if (m->items[i].sub) {
            menu_close_rec(m->items[i].sub);
            m->items[i].sub = NULL;
        }
    }
}

static void update_cursor(App *a)
{
    if (!a->cursor_manager || !a->pointer) return;
    if (!a->cursor_device) {
        a->cursor_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_manager, a->pointer);
        if (!a->cursor_device) return;
        a->cursor_shape = 0;
        a->cursor_serial = 0;
        a->cursor_applied_serial = 0;
    }
    /* Keep the normal arrow everywhere in jtfiles. The compositor's own
     * pointer theme already provides the cursor; changing shapes via
     * wp_cursor_shape only made it flicker between arrow/hand/text/grab. */
    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    /* Cursor-shape needs the latest enter serial, not the button serial used
       by the popup grab. Re-send when either the shape or serial changes. */
    if (shape != a->cursor_shape ||
        a->cursor_applied_serial != a->cursor_serial) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_device,
                                            a->cursor_serial, shape);
        a->cursor_shape = shape;
        a->cursor_applied_serial = a->cursor_serial;
    }
}

static void main_hover(App *a, double x, double y)
{
    int old_row = a->hover_row, old_btn = a->hover_btn, old_sb = a->sb_hover;
    int old_side = a->hover_side;
    a->hover_side = side_hover_slot(a, x, y);
    double sx, sy, sw, sh, sty, sth, sf;
    a->sb_hover = scrollbar_geom(a, &sx, &sy, &sw, &sh, &sty, &sth, &sf) &&
                  x >= sx - 5.0 && x <= sx + sw + 5.0 &&
                  y >= sy && y <= sy + sh;
    a->hover_btn = -1;
    if (y >= 0 && y < TOOLBAR_H && x >= 0 && x < 132) {
        if (x >= 12 && x < 42) a->hover_btn = 0;
        else if (x >= 50 && x < 80) a->hover_btn = 1;
        else if (x >= 88 && x < 118) a->hover_btn = 2;
    }
    if (y >= 12.0 && y < 43.0 &&
        (a->search_active || a->searching)) {
        double gx = a->WIN_W - 196.0;
        if (x >= gx && x < gx + 30.0) a->hover_btn = 5;
    }
    a->hover_row = -1;
    if (!a->sb_hover && x >= SIDEBAR_W && y >= file_top_y(a) &&
        y < a->WIN_H - STATUS_H) {
        if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
            int idx;
            if (icon_grid_hit(a, x, y, &idx)) a->hover_row = idx;
        } else {
            int r = (int)((y - file_top_y(a)) / list_row_h(a)) +
                    scroll_hit(a);
            if (r >= 0 && r < a->n_ents) a->hover_row = r;
        }
    }
    if (old_row != a->hover_row || old_btn != a->hover_btn ||
        old_sb != a->sb_hover || old_side != a->hover_side)
        a->dirty = 1;
}

static void dnd_begin(App *a);

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer;
    App *a = data;
    a->pointer_serial = serial;
    a->cursor_serial = serial;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->pointer_menu = menu_find_surface_r(a->menu, surface);
    a->pointer_target = surface == a->surface ? 1 :
                        (a->pointer_menu ? 2 :
                         (a->dlg.surface && surface == a->dlg.surface ? 3 : 0));
    if (a->pointer_menu) {
        a->pointer_menu->px = a->pointer_x;
        a->pointer_menu->py = a->pointer_y;
    }
    main_hover(a, a->pointer_x, a->pointer_y);
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    if (a->drag_select) {
        select_in_rect(a, a->drag_x0, a->drag_y0,
                       a->drag_x1, a->drag_y1, a->drag_add);
        a->drag_select = 0;
    }
    if (a->sb_drag) { a->sb_drag = 0; a->dirty = 1; }
    if (a->sb_hover) { a->sb_hover = 0; a->dirty = 1; }
    a->pointer_target = 0;
    a->pointer_menu = NULL;
    a->hover_row = -1;
    a->hover_btn = -1;
    a->hover_side = -1;
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    if (a->pointer_target == 3) { update_cursor(a); return; }
    if (a->dnd_pending && a->pointer_target == 1) {
        int dx = (int)a->pointer_x - a->dnd_press_x;
        int dy = (int)a->pointer_y - a->dnd_press_y;
        if (dx * dx + dy * dy >= 16) dnd_begin(a);
    }
    if (a->pointer_target == 2 && a->pointer_menu) {
        Menu *m = a->pointer_menu;
        m->px = a->pointer_x;
        m->py = a->pointer_y;
        int hit = menu_hit(m, a->pointer_x, a->pointer_y);
        if (hit != m->hover) {
            menu_clear_children(m);
            m->hover = hit;
            m->dirty = 1;
            /* Submenus are click-to-open; hover only changes selection and
             * menu_clear_children() closes any submenu we just left. */
        }
    } else if (a->pointer_target == 1) {
        if (a->sb_drag) {
            double sx, sy, sw, sh, sty, sth, sf;
            if (scrollbar_geom(a, &sx, &sy, &sw, &sh, &sty, &sth, &sf))
                scrollbar_set(a, (a->pointer_y - a->sb_grab - sy) /
                              (sh - sth), 1);
            a->dirty = 1;
            update_cursor(a);
            return;
        }
        if (a->drag_select) {
            a->drag_x1 = a->pointer_x;
            a->drag_y1 = a->pointer_y;
            select_in_rect(a, a->drag_x0, a->drag_y0,
                           a->drag_x1, a->drag_y1, a->drag_add);
            a->dirty = 1;
        }
        main_hover(a, a->pointer_x, a->pointer_y);
    }
    update_cursor(a);
}

static void menu_activate_at(App *a, Menu *m, int idx)
{
    if (idx < 0 || idx >= m->n) return;
    MenuItem *it = &m->items[idx];
    if (!it->enabled) return;
    if (it->kind == 2) {
        if (it->sub) menu_close_rec(it->sub);
        it->sub = NULL;
        menu_build_sub(a, m, idx, a->pointer_serial);
        return;
    }
    int id = it->id;
    menu_close_tree(a);
    action_activate(a, id);
}

static void dialog_click(App *a, double x, double y)
{
    Dialog *d = &a->dialog;
    double w = 460.0, h = d->type == 3 ? 290.0 : 176.0;
    /* The dialog is its own toplevel, so x/y are already local to it. */
    double by = h - 48.0;
    if (y >= by && y < by + 32.0) {
        if (x >= w - 22.0 - 84.0 && x < w - 22.0) {
            dialog_commit(a);
            return;
        }
        if (x >= w - 22.0 - 84.0 * 2.0 - 8.0 &&
            x < w - 22.0 - 84.0 - 8.0) {
            dialog_clear(a);
            return;
        }
    }
    if (d->type != 3 && x >= 22.0 && x <= w - 22.0 &&
        y >= 84.0 && y <= 118.0) {
        d->cursor = (int)strlen(d->text);
        a->dirty = 1;
    }
}

static void main_click(App *a, uint32_t serial, double x, double y, int button)
{
    if (button != BTN_LEFT) return;

    if (y >= 12.0 && y < 43.0) {
        static const int ids[] = { A_GO_BACK, A_GO_FORWARD, A_MENU };
        for (int i = 0; i < 3; i++) {
            if (x >= 12.0 + i * 38.0 && x < 42.0 + i * 38.0) {
                action_activate(a, ids[i]);
                return;
            }
        }
        double sx = a->WIN_W - 156.0;
        if (a->search_active || a->searching) {
            /* The button beside the search box cancels the current search. */
            if (x >= sx - 40.0 && x < sx - 10.0) {
                search_stop(a);
                return;
            }
        } else {
            if (x >= sx - 76.0 && x < sx - 46.0) {
                action_activate(a, A_VIEW_DETAILS);
                return;
            }
            if (x >= sx - 40.0 && x < sx - 10.0) {
                action_activate(a, A_VIEW_ICONS);
                return;
            }
        }
        /* Clicking the search box clears it and leaves a blinking caret. */
        double bx = a->search_box_w > 0.0 ? a->search_box_x :
                    a->WIN_W - 156.0;
        double bw = a->search_box_w > 0.0 ? a->search_box_w : 144.0;
        if (x >= bx && x < bx + bw) {
            search_start(a);
            return;
        }
        double px = 132.0, pw = a->WIN_W - px - 248.0;
        if (pw < 120.0) pw = 120.0;
        if (x >= px && x < px + pw) {
            if (a->search_active) search_stop(a);
            a->editing_path = 1;
            a->path_cursor = (int)strlen(a->path_edit);
            a->dirty = 1;
            return;
        }
    }
    if (y < TOOLBAR_H) {
        if (a->toplevel && a->seat)
            xdg_toplevel_move(a->toplevel, a->seat, serial);
        return;
    }
    if (a->view != VIEW_ICONS && a->view != VIEW_TILES &&
        y >= TOOLBAR_H && y < TOOLBAR_H + COLHEAD_H) {
        if (a->view == VIEW_DETAILS || a->view == VIEW_LIST) {
            int id = -1;
            if (x >= SIDEBAR_W + 46.0 && x < SIDEBAR_W + 340.0) id = A_SORT_NAME;
            else if (x >= SIDEBAR_W + 340.0 && x < SIDEBAR_W + 560.0) id = A_SORT_DATE;
            else if (x >= SIDEBAR_W + 560.0 && x < a->WIN_W - 100.0) id = A_SORT_TYPE;
            else if (x >= a->WIN_W - 100.0) id = A_SORT_SIZE;
            if (id >= 0) {
                if (a->sort_by == id) a->sort_desc = !a->sort_desc;
                else a->sort_by = id;
                search_sort(a);
            }
        }
        return;
    }

    double sx, sy, sw, sh, sty, sth, sf;
    if (scrollbar_geom(a, &sx, &sy, &sw, &sh, &sty, &sth, &sf) &&
        x >= sx - 5.0 && x <= sx + sw + 5.0 && y >= sy && y <= sy + sh) {
        a->sb_drag = 1;
        a->sb_grab = (y >= sty && y < sty + sth) ? y - sty : sth / 2.0;
        scrollbar_set(a, (y - a->sb_grab - sy) / (sh - sth), 1);
        return;
    }
    if (y >= a->WIN_H - STATUS_H) return;

    if (x < SIDEBAR_W) {
        a->sidebar_drive = -1;
        a->sidebar_drive_dev[0] = '\0';
        if (y >= a->drives_y0 && y < a->drives_y0 + a->n_drives * 40) {
            int idx = (y - a->drives_y0) / 40;
            if (idx >= 0 && idx < a->n_drives) {
                a->sidebar_drive = idx;
                Drive *d = &a->drives[idx];
                snprintf(a->sidebar_drive_dev, sizeof a->sidebar_drive_dev,
                         "%s", d->dev);
                if (d->mounted) navigate(a, d->mount, 1);
                else { a->ctx_kind = CTX_DRIVE; a->ctx_index = idx;
                       drive_job(a, d, 0); }
                a->dirty = 1;
            }
            return;
        }
        char places[8][PATH_MAX];
        int np = quick_access_places(places);
        int y0 = TOOLBAR_H + 30 - a->sidebar_scroll;
        int pos = (y - y0) / 28;
        int npl = a->n_favorites + np;
        if (pos >= 0 && pos < npl) {
            if (pos < a->n_favorites) navigate(a, a->favorites[pos], 1);
            else if (pos - a->n_favorites < np) {
                const char *target = places[pos - a->n_favorites];
                if (path_is_trash(target)) trash_ensure();
                navigate(a, target, 1);
            }
        }
        return;
    }

    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        int idx;
        if (icon_grid_hit(a, x, y, &idx)) {
            int ctrl = mod_active(a, XKB_MOD_NAME_CTRL);
            int shift = mod_active(a, XKB_MOD_NAME_SHIFT);
            double now = mono_sec();
            int dbl = idx == a->last_click_row &&
                      now - a->last_click_t < 0.45 && !ctrl && !shift;

            if (shift && a->anchor >= 0) select_range(a, idx);
            else if (ctrl) select_toggle(a, idx);
            else select_one(a, idx);

            a->ctx_kind = CTX_FILE;
            a->ctx_index = idx;
            if (dbl) action_activate(a, A_OPEN);
            a->last_click_row = idx;
            a->last_click_t = now;
            a->dirty = 1;
            return;
        }

        int ctrl = mod_active(a, XKB_MOD_NAME_CTRL);
        a->drag_select = 1;
        a->drag_add = ctrl;
        a->drag_x0 = a->drag_x1 = x;
        a->drag_y0 = a->drag_y1 = y;
        if (!ctrl) select_none(a);
        a->ctx_kind = CTX_BACKGROUND;
        a->ctx_index = -1;
        a->last_click_row = -1;
        a->dirty = 1;
        return;
    }

    int row = (int)((y - file_top_y(a)) / list_row_h(a)) + scroll_hit(a);
    if (row < 0 || row >= a->n_ents) return;
    int ctrl = mod_active(a, XKB_MOD_NAME_CTRL);
    int shift = mod_active(a, XKB_MOD_NAME_SHIFT);
    a->ctx_kind = CTX_FILE;
    a->ctx_index = row;
    double now = mono_sec();
    int dbl = row == a->last_click_row &&
              now - a->last_click_t < 0.45 && !ctrl && !shift;

    if (dbl) {
        select_one(a, row);
        action_activate(a, A_OPEN);
    } else if (shift && a->anchor >= 0) {
        select_range(a, row);
    } else if (ctrl) {
        select_toggle(a, row);
    } else if (!a->n_selected || !a->selected[row]) {
        select_one(a, row);
    } else {
        a->sel = row;
        a->anchor = row;
    }
    a->last_click_row = row;
    a->last_click_t = now;
    a->dirty = 1;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)pointer; (void)time;
    App *a = data;
    a->pointer_serial = serial;
    a->clip_serial = serial;

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        a->dnd_pending = 0;
        if (a->sb_drag) { a->sb_drag = 0; a->dirty = 1; return; }
        if (button == BTN_LEFT && a->drag_select) {
            select_in_rect(a, a->drag_x0, a->drag_y0,
                           a->drag_x1, a->drag_y1, a->drag_add);
            a->drag_select = 0;
            a->dirty = 1;
        }
        return;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    if (a->pointer_target == 2 && a->pointer_menu) {
        Menu *m = a->pointer_menu;
        /* Right/middle clicking the popup does nothing and leaves it open. */
        if (button != BTN_LEFT) return;
        int idx = menu_hit(m, m->px, m->py);
        if (button != BTN_LEFT || idx < 0) {
            /* The grab delivers outside clicks to the popup surface. */
            menu_close_tree(a);
        } else {
            menu_activate_at(a, m, idx);
        }
        return;
    }
    if (a->pointer_target == 3) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED && button == BTN_LEFT &&
            a->dialog.type)
            dialog_click(a, a->pointer_x, a->pointer_y);
        return;
    }
    if (a->menu && a->pointer_target == 1) {
        menu_close_tree(a);
        return;
    }
    if (a->pointer_target != 1) return;
    /* The dialog is a separate toplevel; ignore clicks that land on the
     * main window while it is open. */
    if (a->dialog.type) return;
    if (button == BTN_RIGHT) {
        a->ctx_kind = CTX_BACKGROUND;
        a->ctx_index = -1;
        if (a->pointer_x < SIDEBAR_W) {
            if (a->pointer_y >= a->drives_y0 &&
                a->pointer_y < a->drives_y0 + a->n_drives * 40) {
                int d = (int)((a->pointer_y - a->drives_y0) / 40);
                if (d >= 0 && d < a->n_drives) {
                    a->ctx_kind = CTX_DRIVE;
                    a->ctx_index = d;
                    a->sidebar_drive = d;
                    snprintf(a->sidebar_drive_dev, sizeof a->sidebar_drive_dev,
                             "%s", a->drives[d].dev);
                }
            }
        } else if (a->pointer_y >= file_top_y(a) &&
                   a->pointer_y < a->WIN_H - STATUS_H) {
            if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
                int idx;
                if (icon_grid_hit(a, a->pointer_x, a->pointer_y, &idx)) {
                    a->ctx_kind = CTX_FILE;
                    a->ctx_index = idx;
                    if (!a->selected[idx]) select_one(a, idx);
                    else a->sel = idx;
                }
            } else {
                int r = (int)((a->pointer_y - file_top_y(a)) /
                              list_row_h(a)) + scroll_hit(a);
                if (r >= 0 && r < a->n_ents) {
                    a->ctx_kind = CTX_FILE;
                    a->ctx_index = r;
                    if (!a->selected[r]) select_one(a, r);
                    else a->sel = r;
                }
            }
        }
        menu_open_root(a, serial, (int)a->pointer_x, (int)a->pointer_y);
        return;
    }
    main_click(a, serial, a->pointer_x, a->pointer_y, button);
    if (button == BTN_LEFT && a->ctx_kind == CTX_FILE && a->ctx_index >= 0 &&
        a->pointer_x >= SIDEBAR_W && a->pointer_y >= file_top_y(a) &&
        a->pointer_y < a->WIN_H - STATUS_H) {
        a->dnd_pending = 1;
        a->dnd_press_x = (int)a->pointer_x;
        a->dnd_press_y = (int)a->pointer_y;
        a->dnd_press_serial = serial;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    App *a = data;
    double v = wl_fixed_to_double(value);
    int steps = (int)(v / 16.0);
    if (steps == 0) steps = v > 0 ? 1 : -1;
    /* A popup with the pointer grab owns the wheel while it is open. */
    if (a->pointer_target == 2 && a->pointer_menu) {
        menu_scroll_by(a->pointer_menu, -steps * 26);
        return;
    }
    if (a->pointer_x < SIDEBAR_W) {
        a->sidebar_scroll += steps * 12;
        if (a->sidebar_scroll < 0) a->sidebar_scroll = 0;
        /* Match draw_sidebar(): rows are 28px, drives are 40px, and there
         * are two section headers. Otherwise extra drives cannot be
         * reached by wheeling the sidebar. */
        int nf = a->n_favorites > 24 ? 24 : a->n_favorites;
        char places[8][PATH_MAX];
        int np = quick_access_places(places);
        int content = (int)TOOLBAR_H + 12 + 22 + (nf + np) * 28 +
                      12 + 18 + a->n_drives * 40 + 8;
        int max = content - (int)(a->WIN_H - STATUS_H);
        if (max < 0) max = 0;
        if (a->sidebar_scroll > max) a->sidebar_scroll = max;
    } else if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        IconGrid g;
        icon_grid(a, &g);
        a->scroll_max = icon_scroll_max(a, &g);

        int row = a->scroll / g.cols;
        if (row < 0) row = 0;
        row += steps;
        int max_row = a->scroll_max / g.cols;
        if (row < 0) row = 0;
        if (row > max_row) row = max_row;
        a->scroll = row * g.cols;
    } else {
        a->scroll_max = list_scroll_max(a);
        a->scroll += steps * 3;
        if (a->scroll < 0) a->scroll = 0;
        if (a->scroll > a->scroll_max) a->scroll = a->scroll_max;
    }
    /* Mark when the wheel changed the target so the first eased frame uses
     * the real elapsed time rather than a stale idle timestamp. */
    a->scroll_t = mono_sec();
    a->dirty = 1;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void line_edit(App *a, char *buf, size_t cap, int *cur,
                      xkb_keysym_t sym, uint32_t keycode)
{
    size_t len = strlen(buf);
    if (*cur < 0) *cur = 0;
    if (*cur > (int)len) *cur = (int)len;
    if (sym == XKB_KEY_BackSpace) {
        if (*cur > 0) {
            memmove(buf + *cur - 1, buf + *cur, len - (size_t)*cur + 1);
            (*cur)--;
        }
    } else if (sym == XKB_KEY_Delete) {
        if (*cur < (int)len)
            memmove(buf + *cur, buf + *cur + 1, len - (size_t)*cur);
    } else if (sym == XKB_KEY_Left) {
        if (*cur > 0) (*cur)--;
    } else if (sym == XKB_KEY_Right) {
        if (*cur < (int)len) (*cur)++;
    } else if (sym == XKB_KEY_Home) {
        *cur = 0;
    } else if (sym == XKB_KEY_End) {
        *cur = (int)len;
    } else {
        char txt[8];
        int n = xkb_state_key_get_utf8(a->xkb_state, keycode + 8,
                                       txt, sizeof txt - 1);
        if (n > 0 && (unsigned char)txt[0] >= 0x20 &&
            (size_t)*cur + (size_t)n < cap) {
            txt[n] = '\0';
            memmove(buf + *cur + n, buf + *cur, len - (size_t)*cur + 1);
            memcpy(buf + *cur, txt, (size_t)n);
            *cur += n;
        }
    }
    a->dirty = 1;
}

static void menu_key(App *a, xkb_keysym_t sym)
{
    if (!a->menu) return;
    Menu *m = a->menu;
    /* descend into the open submenu when arrow keys are used there */
    for (int depth = 0; depth < 4; depth++) {
        Menu *child = NULL;
        for (int i = 0; i < m->n; i++) if (m->items[i].sub) child = m->items[i].sub;
        if (!child) break;
        m = child;
    }
    if (sym == XKB_KEY_Escape) { menu_close_tree(a); return; }
    if (sym == XKB_KEY_Left && m != a->menu) {
        menu_close_rec(m);
        for (int i = 0; i < a->menu->n; i++) a->menu->items[i].sub = NULL;
        return;
    }
    if (sym == XKB_KEY_Down || sym == XKB_KEY_Up) {
        int dir = sym == XKB_KEY_Down ? 1 : -1;
        int i = m->hover;
        for (int k = 0; k < m->n + 1; k++) {
            i += dir;
            if (i < 0) i = m->n - 1;
            if (i >= m->n) i = 0;
            if (m->items[i].kind != 1 && m->items[i].enabled) break;
        }
        if (m->hover != i) {
            menu_clear_children(m);
            m->hover = i;
            m->dirty = 1;
            menu_scroll_into_view(m, i);
        }
        return;
    }
    if (sym == XKB_KEY_Right) {
        if (m->hover >= 0 && m->items[m->hover].kind == 2 &&
            !m->items[m->hover].sub)
            menu_build_sub(a, m, m->hover, a->pointer_serial);
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (m->hover >= 0) menu_activate_at(a, m, m->hover);
    }
}

static void scroll_index_into_view(App *a, int idx);

static void select_move(App *a, int delta, int extend)
{
    if (a->n_ents <= 0) return;
    int cur = a->sel < 0 ? 0 : a->sel;
    int next = cur + delta;
    if (next < 0) next = 0;
    if (next >= a->n_ents) next = a->n_ents - 1;

    if (extend && a->anchor >= 0) select_range(a, next);
    else select_one(a, next);

    scroll_index_into_view(a, next);
    a->ctx_kind = CTX_FILE;
    a->ctx_index = next;
    a->dirty = 1;
}

static void scroll_index_into_view(App *a, int idx)
{
    if (idx < 0 || idx >= a->n_ents) return;
    a->scroll_max = view_scroll_max(a);
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        IconGrid g;
        icon_grid(a, &g);

        int first_row = a->scroll / g.cols;
        int next_row = idx / g.cols;
        if (next_row < first_row) {
            first_row = next_row;
        } else if (next_row >= first_row + g.rows) {
            first_row = next_row - g.rows + 1;
        }
        int max_row = a->scroll_max / g.cols;
        if (first_row < 0) first_row = 0;
        if (first_row > max_row) first_row = max_row;
        a->scroll = first_row * g.cols;
    } else {
        int visible = (int)((a->WIN_H - file_top_y(a) - STATUS_H) /
                            list_row_h(a));
        if (visible < 1) visible = 1;
        if (idx < a->scroll) a->scroll = idx;
        if (idx >= a->scroll + visible) a->scroll = idx - visible + 1;
    }
    if (a->scroll < 0) a->scroll = 0;
    if (a->scroll > a->scroll_max) a->scroll = a->scroll_max;
    scroll_snap(a);
}

/* Apply a --select request after navigate().  The first configure can change
 * the window size (and therefore the icon grid), so main() calls this again
 * once the final geometry is known. */
static void apply_pending_select(App *a)
{
    if (!a->pending_select[0]) return;
    for (int i = 0; i < a->n_ents; i++) {
        if (strcmp(a->ents[i].name, a->pending_select)) continue;
        select_one(a, i);
        scroll_index_into_view(a, i);
        a->dirty = 1;
        return;
    }
}

/* One key action, shared by real key presses and client-side repeats. */
static void keyboard_key_apply(App *a, xkb_keysym_t sym, uint32_t key,
                               int ctrl, int alt, int shift)
{
    if (a->menu) { menu_key(a, sym); return; }

    if (a->dialog.type) {
        if (sym == XKB_KEY_Escape) {
            dialog_clear(a);
        }
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            if (a->dialog.type == 3) dialog_clear(a);
            else dialog_commit(a);
            a->dirty = 1;
        } else if (a->dialog.type != 3)
            line_edit(a, a->dialog.text, sizeof a->dialog.text,
                      &a->dialog.cursor, sym, key);
        return;
    }

    /* Handle focus-changing shortcuts before the search field gets the key,
     * otherwise Ctrl+L/Ctrl+F would be inserted as text. */
    if (ctrl && sym == XKB_KEY_f) {
        if (a->searching) { a->searching = 0; a->dirty = 1; }
        else search_start(a);
        return;
    }
    if (ctrl && sym == XKB_KEY_l) {
        if (a->search_active) search_stop(a);
        a->editing_path = 1;
        a->path_cursor = (int)strlen(a->path_edit);
        a->dirty = 1;
        return;
    }

    if (a->searching) {
        if (ctrl || alt) return;
        if (sym == XKB_KEY_Escape) { search_stop(a); return; }
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            int idx = a->sel >= 0 && a->sel < a->n_ents ? a->sel :
                      (a->n_ents > 0 ? 0 : -1);
            if (idx >= 0) {
                select_one(a, idx);
                a->ctx_kind = CTX_FILE;
                a->ctx_index = idx;
                action_activate(a, A_OPEN);
            }
            return;
        }
        /* Up/Down move through results; line_edit keeps Left/Right/Home/End. */
        if (sym == XKB_KEY_Down) { select_move(a, 1, shift); return; }
        if (sym == XKB_KEY_Up) { select_move(a, -1, shift); return; }
        char prev_query[sizeof a->search];
        snprintf(prev_query, sizeof prev_query, "%s", a->search);
        line_edit(a, a->search, sizeof a->search, &a->search_cursor,
                  sym, key);
        if (strcmp(prev_query, a->search))
            search_build(a, a->search);
        search_caret_reset(a);
        return;
    }
    if (a->editing_path) {
        if (sym == XKB_KEY_Escape) { a->editing_path = 0; a->dirty = 1; }
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            a->editing_path = 0;
            navigate(a, a->path_edit, 1);
            snprintf(a->path_edit, sizeof a->path_edit, "%s", a->cwd);
        } else line_edit(a, a->path_edit, sizeof a->path_edit,
                         &a->path_cursor, sym, key);
        return;
    }

    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q) { a->quit = 1; return; }
    if (ctrl && sym == XKB_KEY_c) { action_activate(a, A_COPY); return; }
    if (ctrl && sym == XKB_KEY_x) { action_activate(a, A_CUT); return; }
    if (ctrl && sym == XKB_KEY_v) { action_activate(a, A_PASTE); return; }
    if (ctrl && sym == XKB_KEY_a) { action_activate(a, A_SELECT_ALL); return; }
    if (ctrl && sym == XKB_KEY_h) { action_activate(a, A_HIDDEN); return; }
    if (ctrl && shift && (sym == XKB_KEY_s || sym == XKB_KEY_S)) {
        action_activate(a, A_SMOOTH_SCROLL); return;
    }
    if (ctrl && sym == XKB_KEY_e) {
        int list_on = a->view == VIEW_LIST || a->view == VIEW_DETAILS;
        action_activate(a, list_on ? A_VIEW_ICONS : A_VIEW_DETAILS);
        return;
    }
    if (alt && sym == XKB_KEY_Left) { go_hist(a, -1); return; }
    if (alt && sym == XKB_KEY_Right) { go_hist(a, 1); return; }
    if (sym == XKB_KEY_BackSpace) { action_activate(a, A_GO_UP); return; }
    if (sym == XKB_KEY_Delete || sym == XKB_KEY_KP_Delete) {
        action_activate(a, shift ? A_DELETE_PERMANENT : A_DELETE);
        return;
    }
    if (sym == XKB_KEY_F2) { action_activate(a, A_RENAME); return; }
    if (sym == XKB_KEY_F5) { action_activate(a, A_REFRESH); return; }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (a->n_ents && a->sel >= 0) {
            a->ctx_kind = CTX_FILE;
            a->ctx_index = a->sel;
            action_activate(a, A_OPEN);
        }
        return;
    }
    if (sym == XKB_KEY_Down) { select_move(a, 1, shift); return; }
    if (sym == XKB_KEY_Up) { select_move(a, -1, shift); return; }
    if (sym == XKB_KEY_Home) {
        if (a->n_ents) {
            if (shift && a->anchor >= 0) select_range(a, 0);
            else select_one(a, 0);
            a->scroll = 0;
            scroll_snap(a);
            a->ctx_kind = CTX_FILE;
            a->ctx_index = 0;
            a->dirty = 1;
        }
        return;
    }
    if (sym == XKB_KEY_End) {
        if (a->n_ents) {
            int last = a->n_ents - 1;
            if (shift && a->anchor >= 0) select_range(a, last);
            else select_one(a, last);
            a->scroll_max = view_scroll_max(a);
            a->scroll = a->scroll_max;
            scroll_snap(a);
            a->ctx_kind = CTX_FILE;
            a->ctx_index = last;
            a->dirty = 1;
        }
        return;
    }
}

static void keyboard_cancel_repeat(App *a)
{
    a->repeat_active = 0;
    a->repeat_key = 0;
}

static int key_has_text(App *a, uint32_t key)
{
    char txt[8];
    int n = a->xkb_state ? xkb_state_key_get_utf8(
        a->xkb_state, key + 8, txt, sizeof txt - 1) : 0;
    return n > 0 && (unsigned char)txt[0] >= 0x20;
}

/* Only repeat keys that are safe to repeat. Text fields get characters and
 * editing keys; the file list only repeats Up/Down (never Delete/Return). */
static int keyboard_repeatable(App *a, xkb_keysym_t sym, uint32_t key,
                               int ctrl, int alt)
{
    if (ctrl || alt || !a->xkb_state) return 0;
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return ||
        sym == XKB_KEY_KP_Enter || sym == XKB_KEY_q || sym == XKB_KEY_Q ||
        sym == XKB_KEY_F2 || sym == XKB_KEY_F5) return 0;
    int editing_key = sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete ||
                      sym == XKB_KEY_KP_Delete || sym == XKB_KEY_Left ||
                      sym == XKB_KEY_Right || sym == XKB_KEY_Up ||
                      sym == XKB_KEY_Down || sym == XKB_KEY_Home ||
                      sym == XKB_KEY_End;
    if (a->dialog.type || a->searching || a->editing_path)
        return editing_key || key_has_text(a, key);
    return sym == XKB_KEY_Up || sym == XKB_KEY_Down;
}

static void keyboard_repeat_tick(App *a)
{
    if (!a->repeat_active || !a->xkb_state) return;
    double now = mono_sec();
    if (now < a->repeat_at) return;
    if (!keyboard_repeatable(a, a->repeat_sym, a->repeat_key,
                             a->repeat_ctrl, a->repeat_alt)) {
        keyboard_cancel_repeat(a);
        return;
    }
    keyboard_key_apply(a, a->repeat_sym, a->repeat_key,
                       a->repeat_ctrl, a->repeat_alt, a->repeat_shift);
    double period = a->repeat_rate > 0.0 ? 1.0 / a->repeat_rate : 0.02;
    a->repeat_at += period;
    if (a->repeat_at < now) a->repeat_at = now + period; /* long stall */
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)keyboard; (void)time;
    App *a = data;
    a->clip_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (a->repeat_active && a->repeat_key == key)
            keyboard_cancel_repeat(a);
        return;
    }
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state) return;
    /* A transfer runs in a child process; keep navigation and shortcuts live
     * while it copies. Starting a second transfer is still blocked separately
     * in do_paste(). */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    int ctrl = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_CTRL,
                                            XKB_STATE_MODS_DEPRESSED) > 0;
    int alt = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_ALT,
                                           XKB_STATE_MODS_DEPRESSED) > 0;
    int shift = xkb_state_mod_name_is_active(a->xkb_state, XKB_MOD_NAME_SHIFT,
                                             XKB_STATE_MODS_DEPRESSED) > 0;

    keyboard_key_apply(a, sym, key, ctrl, alt, shift);

    if (keyboard_repeatable(a, sym, key, ctrl, alt)) {
        a->repeat_key = key;
        a->repeat_sym = sym;
        a->repeat_ctrl = ctrl;
        a->repeat_alt = alt;
        a->repeat_shift = shift;
        a->repeat_active = 1;
        a->repeat_at = mono_sec() +
                       (a->repeat_delay > 0.0 ? a->repeat_delay : 0.25);
    } else {
        keyboard_cancel_repeat(a);
    }
}

/* ------------------------------------------------------------------ */
/*  Protocol listeners                                                 */
/* ------------------------------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888) a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xdg_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    App *a = data;
    a->configured = 1;
    a->dirty = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)t;
    App *a = data;
    int max = 0, full = 0;
    uint32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) max = 1;
        else if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) full = 1;
    }
    int was_max = a->state_maximized;
    a->state_maximized = max;

    if (!max && !full && was_max && a->state_w > 0 && a->state_h > 0) {
        /* Leaving maximized: reapply the size we had before maximizing.
           The compositor may report the maximized frame size here (or 0x0),
           which would otherwise leave the window at full-screen dimensions
           after restoring a session that was saved while maximized. */
        a->WIN_W = (double)a->state_w;
        a->WIN_H = (double)a->state_h;
    } else if (width > 0 && height > 0) {
        a->WIN_W = (double)width;
        a->WIN_H = (double)height;
        if (a->WIN_W < MIN_W) a->WIN_W = MIN_W;
        if (a->WIN_H < MIN_H) a->WIN_H = MIN_H;
        if (!max && !full) {
            a->state_w = (int)a->WIN_W;
            a->state_h = (int)a->WIN_H;
        }
    }
    /* Keep window geometry in sync on every configure, including restores
       and client-sized (0x0) configures, so the SSD frame matches. */
    if (a->xdg_surface)
        xdg_surface_set_window_geometry(a->xdg_surface, 0, 0,
                                        (int)a->WIN_W, (int)a->WIN_H);
    if (a->pending_select_stage == 1 && width > 0 && height > 0) {
        /* Initial selection used the restored/design size; re-evaluate the
         * scroll position now that the compositor's real size is known. */
        apply_pending_select(a);
        a->pending_select_stage = 2;
    }
    scroll_snap(a);
    a->dirty = 1;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void deco_configure(void *data,
                           struct zxdg_toplevel_decoration_v1 *dec,
                           uint32_t mode)
{
    (void)data; (void)dec; (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};

/* ------------------------------------------------------------------ */
/*  Transfer progress window (a real xdg toplevel, themed like the app) */
/* ------------------------------------------------------------------ */

static const struct wl_surface_listener surface_listener;

/* Secondary windows keep their opening size.  Min/max already pins the edges;
 * also undo any maximize/fullscreen state the compositor tries to apply. */
static void fixed_toplevel_reject_states(struct xdg_toplevel *t,
                                         struct wl_array *states)
{
    if (!t || !states) return;
    int32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            xdg_toplevel_unset_maximized(t);
        else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            xdg_toplevel_unset_fullscreen(t);
    }
}

static void frame_done_xfer(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    Transfer *x = data;
    if (x->frame_cb == cb) x->frame_cb = NULL;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener_xfer = {
    .done = frame_done_xfer,
};

static void request_frame_xfer(Transfer *x)
{
    if (!x->surface || x->frame_cb) return;
    x->frame_cb = wl_surface_frame(x->surface);
    if (!x->frame_cb) return;
    wl_callback_add_listener(x->frame_cb, &frame_listener_xfer, x);
}

static void xfer_surface_configure(void *data, struct xdg_surface *xs,
                                   uint32_t serial)
{
    Transfer *x = data;
    xdg_surface_ack_configure(xs, serial);
    x->configured = 1;
    x->dirty = 1;
}

static const struct xdg_surface_listener xfer_surface_listener = {
    .configure = xfer_surface_configure,
};

static void xfer_toplevel_configure(void *data, struct xdg_toplevel *t,
                                    int32_t width, int32_t height,
                                    struct wl_array *states)
{
    /* Transient windows are fixed-size: ignore any size proposed by a resize
     * configure and keep the dimensions we opened with. */
    (void)width; (void)height;
    fixed_toplevel_reject_states(t, states);
    Transfer *x = data;
    /* Keep the window geometry equal to the buffer so the SSD frame and the
     * themed content line up on every configure. */
    if (x->xdg_surface)
        xdg_surface_set_window_geometry(x->xdg_surface, 0, 0, x->w, x->h);
    x->dirty = 1;
}

static void xfer_toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    Transfer *x = data;
    if (x->app) status_set(x->app, "Transfer is still running");
    x->dirty = 1;
}

static const struct xdg_toplevel_listener xfer_toplevel_listener = {
    .configure = xfer_toplevel_configure,
    .close = xfer_toplevel_close,
};

static void xfer_deco_configure(void *data,
                                struct zxdg_toplevel_decoration_v1 *dec,
                                uint32_t mode)
{
    (void)data; (void)dec; (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener xfer_deco_listener = {
    .configure = xfer_deco_configure,
};

/* ------------------------------------------------------------------ */
/*  Dialog window (separate xdg toplevel, server-side decorations)      */
/* ------------------------------------------------------------------ */

static void frame_done_dialog(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    DialogWin *d = data;
    if (d->frame_cb == cb) d->frame_cb = NULL;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener_dialog = {
    .done = frame_done_dialog,
};

static void request_frame_dialog(DialogWin *d)
{
    if (!d->surface || d->frame_cb) return;
    d->frame_cb = wl_surface_frame(d->surface);
    if (d->frame_cb)
        wl_callback_add_listener(d->frame_cb, &frame_listener_dialog, d);
}

static void dialog_surface_configure(void *data, struct xdg_surface *xs,
                                     uint32_t serial)
{
    DialogWin *d = data;
    xdg_surface_ack_configure(xs, serial);
    d->configured = 1;
    d->dirty = 1;
}

static const struct xdg_surface_listener dialog_surface_listener = {
    .configure = dialog_surface_configure,
};

static void dialog_toplevel_configure(void *data, struct xdg_toplevel *t,
                                      int32_t width, int32_t height,
                                      struct wl_array *states)
{
    (void)width; (void)height;
    fixed_toplevel_reject_states(t, states);
    DialogWin *d = data;
    App *a = d->app;
    if (a->dialog.type)
        d->h = a->dialog.type == 3 ? 290 : 176;
    if (d->xdg_surface)
        xdg_surface_set_window_geometry(d->xdg_surface, 0, 0, d->w, d->h);
    d->dirty = 1;
    (void)t;
}

static void dialog_toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    DialogWin *d = data;
    dialog_clear(d->app);
}

static const struct xdg_toplevel_listener dialog_toplevel_listener = {
    .configure = dialog_toplevel_configure,
    .close = dialog_toplevel_close,
};

static void dialog_window_open(App *a)
{
    DialogWin *d = &a->dlg;
    if (!a->dialog.type || !a->compositor || !a->wm_base) return;
    d->app = a;
    d->w = 460;
    d->h = a->dialog.type == 3 ? 290 : 176;

    if (d->surface) {
        xdg_toplevel_set_title(d->toplevel, a->dialog.title);
        /* A reused dialog can switch between the short and tall layouts, so
         * re-assert the lock for its new fixed size. */
        xdg_toplevel_set_min_size(d->toplevel, d->w, d->h);
        xdg_toplevel_set_max_size(d->toplevel, d->w, d->h);
        xdg_surface_set_window_geometry(d->xdg_surface, 0, 0, d->w, d->h);
        d->dirty = 1;
        return;
    }

    d->surface = wl_compositor_create_surface(a->compositor);
    if (!d->surface) return;
    wl_surface_add_listener(d->surface, &surface_listener, a);

    d->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, d->surface);
    if (!d->xdg_surface) { dialog_window_close(a); return; }
    xdg_surface_add_listener(d->xdg_surface, &dialog_surface_listener, d);

    d->toplevel = xdg_surface_get_toplevel(d->xdg_surface);
    if (!d->toplevel) { dialog_window_close(a); return; }
    xdg_toplevel_add_listener(d->toplevel, &dialog_toplevel_listener, d);
    xdg_toplevel_set_title(d->toplevel, a->dialog.title);
    xdg_toplevel_set_app_id(d->toplevel, "jtfiles-dialog");
    /* Transient parent makes the compositor center the dialog over the file
     * manager and keep it above that window instead of behind it. */
    if (a->toplevel) xdg_toplevel_set_parent(d->toplevel, a->toplevel);
    xdg_toplevel_set_min_size(d->toplevel, d->w, d->h);
    xdg_toplevel_set_max_size(d->toplevel, d->w, d->h);
    xdg_surface_set_window_geometry(d->xdg_surface, 0, 0, d->w, d->h);

    if (a->deco_manager) {
        d->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, d->toplevel);
        if (d->decoration)
            zxdg_toplevel_decoration_v1_set_mode(d->decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (wl_surface_get_version(d->surface) >= 3 && a->scale > 0)
        wl_surface_set_buffer_scale(d->surface, a->scale);
    wl_surface_commit(d->surface);
    d->dirty = 1;
}

static void dialog_window_close(App *a)
{
    DialogWin *d = &a->dlg;
    if (d->frame_cb) { wl_callback_destroy(d->frame_cb); d->frame_cb = NULL; }
    for (int i = 0; i < 2; i++) shm_buffer_destroy(&d->bufs[i]);
    if (d->decoration) {
        zxdg_toplevel_decoration_v1_destroy(d->decoration);
        d->decoration = NULL;
    }
    if (d->toplevel) { xdg_toplevel_destroy(d->toplevel); d->toplevel = NULL; }
    if (d->xdg_surface) {
        xdg_surface_destroy(d->xdg_surface);
        d->xdg_surface = NULL;
    }
    if (d->surface) { wl_surface_destroy(d->surface); d->surface = NULL; }
    d->configured = 0;
    d->dirty = 0;
}

static void dialog_window_draw(App *a)
{
    DialogWin *d = &a->dlg;
    if (!a->dialog.type || !d->configured || !d->surface || !a->shm) return;
    if (!d->dirty) return;
    int w = (int)lround((double)d->w * (double)a->scale);
    int h = (int)lround((double)d->h * (double)a->scale);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return;
    ShmBuf *b = next_buffer(a, d->bufs, 2, w, h);
    if (!b) return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgba(cr, C_BG, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)a->scale, (double)a->scale);
    draw_dialog_content(a, cr, 0, 0);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
    d->dirty = 0;
    if (wl_surface_get_version(d->surface) >= 3)
        wl_surface_set_buffer_scale(d->surface, a->scale);
    wl_surface_attach(d->surface, b->buffer, 0, 0);
    wl_surface_damage(d->surface, 0, 0, w, h);
    request_frame_dialog(d);
    wl_surface_commit(d->surface);
    b->busy = 1;
}

static void xfer_window_open(App *a)
{
    Transfer *x = &a->xfer;
    if (x->surface || !a->compositor || !a->wm_base) return;

    x->surface = wl_compositor_create_surface(a->compositor);
    if (!x->surface) return;
    wl_surface_add_listener(x->surface, &surface_listener, a);

    x->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, x->surface);
    if (!x->xdg_surface) { xfer_window_close(a); return; }
    xdg_surface_add_listener(x->xdg_surface, &xfer_surface_listener, x);

    x->toplevel = xdg_surface_get_toplevel(x->xdg_surface);
    if (!x->toplevel) { xfer_window_close(a); return; }
    xdg_toplevel_add_listener(x->toplevel, &xfer_toplevel_listener, x);
    xdg_toplevel_set_title(x->toplevel, xfer_title(x->op));
    xdg_toplevel_set_app_id(x->toplevel, "jtfiles-progress");
    /* Transient parent makes the compositor center the progress window over
     * the file manager and raise it above the app (and other windows). */
    if (a->toplevel) xdg_toplevel_set_parent(x->toplevel, a->toplevel);
    xdg_toplevel_set_min_size(x->toplevel, 520, 220);
    xdg_toplevel_set_max_size(x->toplevel, 520, 220);
    xdg_surface_set_window_geometry(x->xdg_surface, 0, 0, x->w, x->h);

    if (a->deco_manager) {
        x->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, x->toplevel);
        if (x->decoration) {
            zxdg_toplevel_decoration_v1_add_listener(x->decoration,
                                                     &xfer_deco_listener, x);
            zxdg_toplevel_decoration_v1_set_mode(x->decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }
    if (wl_surface_get_version(x->surface) >= 3 && a->scale > 0)
        wl_surface_set_buffer_scale(x->surface, a->scale);
    wl_surface_commit(x->surface);
}

/* Fast copies finish before a progress window could be read.  Delay mapping
 * it briefly and skip it entirely if the worker is already done. */
#define XFER_WINDOW_DELAY 0.25

static void xfer_window_schedule(App *a)
{
    Transfer *x = &a->xfer;
    x->show_pending = 1;
    x->show_at = mono_sec() + XFER_WINDOW_DELAY;
}

static void xfer_window_maybe_open(App *a)
{
    Transfer *x = &a->xfer;
    if (!x->active || !x->show_pending) return;
    if (mono_sec() < x->show_at) return;
    x->show_pending = 0;
    xfer_window_open(a);
}

static void xfer_window_close(App *a)
{
    Transfer *x = &a->xfer;
    x->show_pending = 0;
    if (x->frame_cb) { wl_callback_destroy(x->frame_cb); x->frame_cb = NULL; }
    for (int i = 0; i < 2; i++) shm_buffer_destroy(&x->bufs[i]);
    if (x->decoration) {
        zxdg_toplevel_decoration_v1_destroy(x->decoration);
        x->decoration = NULL;
    }
    if (x->toplevel) { xdg_toplevel_destroy(x->toplevel); x->toplevel = NULL; }
    if (x->xdg_surface) {
        xdg_surface_destroy(x->xdg_surface);
        x->xdg_surface = NULL;
    }
    if (x->surface) { wl_surface_destroy(x->surface); x->surface = NULL; }
    x->configured = 0;
}

static void xfer_draw(App *a)
{
    Transfer *x = &a->xfer;
    if (!x->active || !x->configured || !x->surface || !a->shm) return;
    if (!x->dirty) return;
    double wd = (double)x->w * (double)a->scale;
    double hd = (double)x->h * (double)a->scale;
    if (!(wd >= 1.0 && hd >= 1.0 && wd <= 16384.0 && hd <= 16384.0)) return;
    int w = (int)lround(wd), h = (int)lround(hd);
    ShmBuf *b = next_buffer(a, x->bufs, 2, w, h);
    if (!b) return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgba(cr, C_BG, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, (double)a->scale, (double)a->scale);

    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, (double)x->h);
    cairo_pattern_add_color_stop_rgba(bg, 0, R_(C_BG_TOP), G_(C_BG_TOP),
                                      B_(C_BG_TOP), 1);
    cairo_pattern_add_color_stop_rgba(bg, 1, R_(C_BG), G_(C_BG), B_(C_BG), 1);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    glow_ensure();
    glow_paint(cr, g_ambient_glow, (double)x->w * 0.5, 60.0, 260.0);

    double pad = 18.0;
    ui_card_fill(cr, pad, pad, (double)x->w - pad * 2.0,
                 (double)x->h - pad * 2.0, 12.0);

    double left = pad + 20.0;
    double right = (double)x->w - left;
    double title_y = pad + 20.0;
    text_draw(cr, xfer_title(x->op),
              15, 600, C_FG, 0.98, left, title_y, ALIGN_LEFT, 0.2);

    char detail[NAME_MAX + 64];
    snprintf(detail, sizeof detail, "%s", x->current);
    ui_ellipsize(cr, detail, sizeof detail, 12, 400, right - left);
    text_draw(cr, detail, 12, 400, C_DIM, 0.9,
              left, title_y + 24.0, ALIGN_LEFT, 0.0);

    double bar_y = title_y + 52.0;
    double bar_w = right - left;
    rgba(cr, C_TRACK, 1.0);
    rounded_rect(cr, left, bar_y, bar_w, 8.0, 4.0);
    cairo_fill(cr);

    double frac = 0.0;
    if (x->total_bytes > 0 && x->bytes_done > 0)
        frac = (double)x->bytes_done / (double)x->total_bytes;
    else if (x->total_items > 0 && x->items_done > 0)
        frac = (double)x->items_done / (double)x->total_items;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    if (frac > 0.0) {
        cairo_pattern_t *fill = cairo_pattern_create_linear(left, bar_y,
                                                            left, bar_y + 8.0);
        cairo_pattern_add_color_stop_rgba(fill, 0, R_(C_ACCENT_HI),
            G_(C_ACCENT_HI), B_(C_ACCENT_HI), 1);
        cairo_pattern_add_color_stop_rgba(fill, 1, R_(C_ACCENT_LO),
            G_(C_ACCENT_LO), B_(C_ACCENT_LO), 1);
        cairo_set_source(cr, fill);
        rounded_rect(cr, left, bar_y, bar_w * frac, 8.0, 4.0);
        cairo_fill(cr);
        cairo_pattern_destroy(fill);
    }

    char counts[160];
    if (x->total_items > 0)
        snprintf(counts, sizeof counts, "%lld of %lld items",
                 x->items_done, x->total_items);
    else
        snprintf(counts, sizeof counts, "Preparing...");
    text_draw(cr, counts, 11, 400, C_DIM, 0.85,
              left, bar_y + 18.0, ALIGN_LEFT, 0.0);
    char pct[32];
    snprintf(pct, sizeof pct, "%.0f%%", frac * 100.0);
    text_draw(cr, pct, 11, 600, C_ACCENT, 0.95,
              right, bar_y + 18.0, ALIGN_RIGHT, 0.0);

    /* Live transfer speed and estimated time remaining (byte-based ops). */
    if ((x->op == XOP_COPY || x->op == XOP_MOVE) && x->speed > 0.0 &&
        x->total_bytes > 0 && x->bytes_done < x->total_bytes) {
        char spd[64], line[96];
        fmt_size(spd, sizeof spd, (off_t)x->speed);
        snprintf(line, sizeof line, "%s/s", spd);
        text_draw(cr, line, 11, 400, C_DIM, 0.85,
                  left, bar_y + 36.0, ALIGN_LEFT, 0.0);
        long long s = (long long)((double)(x->total_bytes - x->bytes_done) /
                                  x->speed + 0.5);
        char eta[64];
        if (s < 60)
            snprintf(eta, sizeof eta, "%llds left", s);
        else if (s < 3600)
            snprintf(eta, sizeof eta, "%lldm %llds left", s / 60, s % 60);
        else
            snprintf(eta, sizeof eta, "%lldh %lldm left",
                     s / 3600, (s % 3600) / 60);
        text_draw(cr, eta, 11, 400, C_DIM, 0.85,
                  right, bar_y + 36.0, ALIGN_RIGHT, 0.0);
    }

    text_draw(cr, "jtfiles", 10, 600, C_FAINT, 0.7,
              right, (double)x->h - pad - 15.0, ALIGN_RIGHT, 1.0);

    x->dirty = 0;
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(x->surface) >= 3)
        wl_surface_set_buffer_scale(x->surface, a->scale);
    wl_surface_attach(x->surface, b->buffer, 0, 0);
    wl_surface_damage(x->surface, 0, 0, x->w, x->h);
    request_frame_xfer(x);
    wl_surface_commit(x->surface);
    b->busy = 1;
}

static void xfer_speed_update(Transfer *x)
{
    double now = mono_sec();
    if (x->speed_t0 <= 0.0) {
        x->speed_t0 = now;
        x->speed_bytes0 = (double)x->bytes_done;
        return;
    }
    double dt = now - x->speed_t0;
    if (dt < 0.25) return;
    double delta = (double)x->bytes_done - x->speed_bytes0;
    if (delta < 0.0) delta = 0.0;
    double inst = delta / dt;
    x->speed = x->speed > 0.0 ? x->speed * 0.65 + inst * 0.35 : inst;
    x->speed_t0 = now;
    x->speed_bytes0 = (double)x->bytes_done;
}

static void xfer_parse_line(App *a, char *line)
{
    Transfer *x = &a->xfer;
    int changed = 0;
    if (!strncmp(line, "T ", 2)) {
        long long items = 0, bytes = 0;
        if (sscanf(line + 2, "%lld %lld", &items, &bytes) == 2) {
            changed = x->total_items != items || x->total_bytes != bytes;
            x->total_items = items;
            x->total_bytes = bytes;
        }
    } else if (!strncmp(line, "N ", 2)) {
        changed = strcmp(x->current, line + 2) != 0;
        snprintf(x->current, sizeof x->current, "%s", line + 2);
    } else if (!strncmp(line, "B ", 2)) {
        long long bytes = 0;
        if (sscanf(line + 2, "%lld", &bytes) == 1) {
            changed = x->bytes_done != bytes;
            x->bytes_done = bytes;
        }
    } else if (!strncmp(line, "P ", 2)) {
        long long items = 0, bytes = 0;
        if (sscanf(line + 2, "%lld %lld", &items, &bytes) == 2) {
            changed = x->items_done != items || x->bytes_done != bytes;
            x->items_done = items;
            x->bytes_done = bytes;
        }
    } else if (!strncmp(line, "D ", 2)) {
        int done = 0, failed = 0;
        if (sscanf(line + 2, "%d %d", &done, &failed) == 2) {
            changed = x->done_items != done || x->failed != failed;
            x->done_items = done;
            x->failed = failed;
        }
    } else {
        return;
    }
    if (line[0] == 'B' || line[0] == 'P') xfer_speed_update(x);
    /* Only the progress window depends on these fields; repainting the main
     * window for every byte counter was pure overhead. */
    if (changed) x->dirty = 1;
}

static void xfer_consume(App *a, int final)
{
    Transfer *x = &a->xfer;
    char *start = x->buf;
    size_t remaining = x->len;
    for (;;) {
        char *nl = memchr(start, '\n', remaining);
        if (!nl) break;
        *nl = '\0';
        xfer_parse_line(a, start);
        size_t used = (size_t)(nl - start) + 1;
        start += used;
        remaining -= used;
    }
    if (start != x->buf) {
        memmove(x->buf, start, remaining);
        x->len = remaining;
        x->buf[x->len] = '\0';
    }
    if (final && x->len > 0) {
        x->buf[x->len] = '\0';
        xfer_parse_line(a, x->buf);
        x->len = 0;
        x->buf[0] = '\0';
    }
}

static void xfer_finish(App *a, int code)
{
    Transfer *x = &a->xfer;
    int done = x->done_items > 0 ? x->done_items : (int)x->items_done;
    int failed = x->failed;
    if (code != 0 && failed == 0) failed = 1;
    if (code != 0) post_xfer_disarm(a);   /* cancelled: don't highlight partial output */
    int cut = x->cut;
    int op = x->op;
    int internal = x->internal;
    int removes = op == XOP_TRASH || op == XOP_DELETE;
    /* Reload only if this transfer changed the folder we are looking at.
     * Navigate elsewhere during a copy and the old code still did a full
     * rescan here, after the user had already moved on. */
    int refresh = ((cut || removes) && x->src_dir[0] &&
                   !strcmp(a->cwd, x->src_dir)) ||
                  (x->dst_dir[0] && !strcmp(a->cwd, x->dst_dir));

    if (x->fd >= 0) { close(x->fd); x->fd = -1; }
    if (x->pid > 0) {
        int status;
        waitpid(x->pid, &status, WNOHANG);
        x->pid = 0;
    }
    xfer_window_close(a);
    memset(x, 0, sizeof *x);
    x->fd = -1;

    const char *verb = op == XOP_MOVE ? "Moved" :
                       (op == XOP_TRASH ? "Trashed" :
                        (op == XOP_DELETE ? "Deleted" : "Copied"));
    if (failed)
        status_set(a, "%s %d item%s, %d failed", verb, done,
                   done == 1 ? "" : "s", failed);
    else if (op == XOP_TRASH)
        status_set(a, "Moved %d item%s to Trash", done,
                   done == 1 ? "" : "s");
    else if (op == XOP_DELETE)
        status_set(a, "Permanently deleted %d item%s", done,
                   done == 1 ? "" : "s");
    else
        status_set(a, "%s %d item%s", verb, done, done == 1 ? "" : "s");
    if (cut && !removes && !internal) {
        a->n_clip = 0;
        a->clip_cut = 0;
        a->clip_cache_valid = 0;
        /* The selection is now stale; give it up so other apps stop
         * offering a cut whose source files no longer exist. */
        clip_write_cancel_all(a);
        if (a->clip_source) {
            wl_data_source_destroy(a->clip_source);
            a->clip_source = NULL;
            if (a->data_device)
                wl_data_device_set_selection(a->data_device, NULL,
                                             a->clip_serial);
            wl_display_flush(a->display);
        }
    }
    if (refresh) dir_load(a, a->cwd);
    a->dirty = 1;
}

static void xfer_poll(App *a)
{
    Transfer *x = &a->xfer;
    if (!x->active || x->pid <= 0) return;

    if (x->fd >= 0) {
        char buf[256];
        for (int budget = 32; budget > 0; budget--) {
            ssize_t n = read(x->fd, buf, sizeof buf);
            if (n > 0) {
                size_t room = sizeof x->buf - 1 - x->len;
                if ((size_t)n > room) n = (ssize_t)room;
                if (n > 0) {
                    memcpy(x->buf + x->len, buf, (size_t)n);
                    x->len += (size_t)n;
                    x->buf[x->len] = '\0';
                }
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
    }

    xfer_consume(a, 0);

    int status = 0;
    pid_t ret = waitpid(x->pid, &status, WNOHANG);
    if (ret == 0) return;
    if (ret == x->pid && x->fd >= 0) {
        /* Drain anything still buffered after the child exited. */
        char tail[256];
        ssize_t n;
        while ((n = read(x->fd, tail, sizeof tail)) > 0) {
            size_t room = sizeof x->buf - 1 - x->len;
            if ((size_t)n > room) n = (ssize_t)room;
            if (n <= 0) break;
            memcpy(x->buf + x->len, tail, (size_t)n);
            x->len += (size_t)n;
            x->buf[x->len] = '\0';
        }
    }
    xfer_consume(a, 1);

    int code = 1;
    if (ret == x->pid) {
        if (WIFEXITED(status)) code = WEXITSTATUS(status);
    } else if (ret == (pid_t)-1 && errno == ECHILD) {
        code = x->failed ? 1 : 0;
    }
    xfer_finish(a, code);
}

static void output_geometry(void *data, struct wl_output *o, int32_t x,
                            int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform)
{
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh)
{
    (void)data; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}

static void output_done(void *data, struct wl_output *o)
{
    (void)data; (void)o;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
    (void)o;
    Output *out = data;
    if (factor < 1) factor = 1;
    out->scale = factor;
    if (out->entered) recalc_scale(out->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode,
    .done = output_done, .scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (a->outputs[i].output == output) a->outputs[i].entered = 1;
    recalc_scale(a);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (a->outputs[i].output == output) a->outputs[i].entered = 0;
    recalc_scale(a);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter, .leave = surface_leave,
};

static void keyboard_keymap(void *data, struct wl_keyboard *kb,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)kb;
    App *a = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) { xkb_keymap_unref(km); return; }
    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
    keyboard_cancel_repeat(a);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)kb; (void)serial; (void)surface; (void)keys;
    keyboard_cancel_repeat(data);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)kb; (void)serial; (void)surface;
    keyboard_cancel_repeat(data);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                               uint32_t serial, uint32_t dep, uint32_t lat,
                               uint32_t lock, uint32_t group)
{
    (void)kb; (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, lat, lock, 0, 0, group);
}

/* jtlabwc sends repeat_info but leaves key event generation to clients. */
static void keyboard_repeat(void *data, struct wl_keyboard *kb,
                            int32_t rate, int32_t delay)
{
    (void)kb;
    App *a = data;
    a->repeat_rate = rate > 0 ? (double)rate : 50.0;
    a->repeat_delay = delay > 0 ? (double)delay / 1000.0 : 0.25;
    if (a->repeat_active)
        a->repeat_at = mono_sec() + a->repeat_delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter,
    .leave = keyboard_leave, .key = keyboard_key,
    .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

/* ------------------------------------------------------------------ */
/*  Drag and drop (wl_data_device)                                     */
/* ------------------------------------------------------------------ */

static int dnd_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void path_to_uri(const char *path, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    if (n < 9) { if (n) out[0] = 0; return; }
    memcpy(out, "file://", 7); j = 7;
    for (const unsigned char *p = (const unsigned char *)path;
         *p && j + 3 < n; p++) {
        if (isalnum(*p) || *p == '/' || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~' || *p == ':') {
            out[j++] = (char)*p;
        } else {
            out[j++] = '%';
            out[j++] = hex[*p >> 4];
            out[j++] = hex[*p & 15];
        }
    }
    out[j] = 0;
}

static void uri_to_path(const char *uri, char *out, size_t n)
{
    if (!strncmp(uri, "file://", 7)) uri += 7;
    size_t j = 0;
    for (; *uri && *uri != '\r' && *uri != '\n' && j + 1 < n; uri++) {
        if (*uri == '%' && uri[1] && uri[2]) {
            int hi = dnd_hexval(uri[1]), lo = dnd_hexval(uri[2]);
            if (hi >= 0 && lo >= 0) {
                out[j++] = (char)(hi * 16 + lo);
                uri += 2;
                continue;
            }
        }
        out[j++] = *uri;
    }
    out[j] = 0;
}

static void dnd_write_all(int fd, const char *s, size_t len)
{
    while (len) {
        ssize_t n = write(fd, s, len);
        if (n > 0) { s += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Clipboard (wl_data_device selection)                               */
/* ------------------------------------------------------------------ */

static int clip_is_text_mime(const char *mime)
{
    return !strcmp(mime, "text/plain;charset=utf-8") ||
           !strcmp(mime, "text/plain") ||
           !strcmp(mime, "UTF8_STRING") ||
           !strcmp(mime, "STRING") ||
           !strcmp(mime, "TEXT");
}

static void clip_snapshot_files(App *a, char (*paths)[PATH_MAX], int n, int cut)
{
    memset(&a->clip_snap, 0, sizeof a->clip_snap);
    if (n > 64) n = 64;
    a->clip_snap.n = n > 0 ? n : 0;
    a->clip_snap.cut = cut;
    for (int i = 0; i < a->clip_snap.n; i++)
        snprintf(a->clip_snap.paths[i], PATH_MAX, "%s", paths[i]);
}

static void clip_snapshot_text(App *a, const char *text)
{
    memset(&a->clip_snap, 0, sizeof a->clip_snap);
    snprintf(a->clip_snap.text, sizeof a->clip_snap.text, "%s",
             text ? text : "");
    a->clip_snap.has_text = 1;
    a->n_clip = 0;
    a->clip_cut = 0;
}

/* Queue a nonblocking write for a wl_data_source.send() pipe.  GTK hands
 * these fds to a GOutputStream instead of blocking the Wayland event loop. */
static void clip_write_cancel_all(App *a)
{
    ClipWrite *w = a->clip_writes;
    a->clip_writes = NULL;
    while (w) {
        ClipWrite *next = w->next;
        close(w->fd);
        free(w->buf);
        free(w);
        w = next;
    }
}

static void clip_write_poll(App *a, int fd)
{
    ClipWrite **link = &a->clip_writes;
    while (*link && (*link)->fd != fd) link = &(*link)->next;
    ClipWrite *w = *link;
    if (!w) return;

    while (w->off < w->len) {
        ssize_t n = write(w->fd, w->buf + w->off, w->len - w->off);
        if (n > 0) { w->off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;
    }
    *link = w->next;
    close(w->fd);
    free(w->buf);
    free(w);
}

static void clip_write_enqueue(App *a, int fd, char *buf, size_t len)
{
    if (fd < 0 || !buf) {
        if (fd >= 0) close(fd);
        free(buf);
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ClipWrite *w = calloc(1, sizeof *w);
    if (!w) { close(fd); free(buf); return; }
    w->fd = fd;
    w->buf = buf;
    w->len = len;
    w->next = a->clip_writes;
    a->clip_writes = w;
    clip_write_poll(a, fd);
}

/* Freeze the current payload for one mime type.  The gnome target carries the
 * cut verb with LF endings; text/uri-list keeps its required CRLF endings. */
static char *clip_build_payload(App *a, const char *mime, size_t *out_len)
{
    ClipSnapshot *s = &a->clip_snap;
    int gnome = !strcmp(mime, "x-special/gnome-copied-files");
    int uris = !strcmp(mime, "text/uri-list");
    size_t cap = 64 * (PATH_MAX * 3 + 32) + 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define CLIP_ADD(str_) do {                                     \
        size_t n_ = strlen(str_);                               \
        if (len + n_ + 1 > cap) { free(buf); return NULL; }     \
        memcpy(buf + len, (str_), n_);                          \
        len += n_;                                              \
        buf[len] = 0;                                           \
    } while (0)

    if (s->has_text && !s->n) {
        if (clip_is_text_mime(mime)) {
            CLIP_ADD(s->text);
        } else if (uris || gnome) {
            char uri[PATH_MAX + 32];
            path_to_uri(s->text, uri, sizeof uri);
            if (gnome) CLIP_ADD("copy\n");
            CLIP_ADD(uri);
            if (!gnome) CLIP_ADD("\r\n");
        }
        *out_len = len;
        return buf;
    }

    if (!s->n) { *out_len = 0; return buf; }

    if (gnome) {
        CLIP_ADD(s->cut ? "cut\n" : "copy\n");
        /* Nemo splits this target on '\n' without dropping empty lines, so
         * there must be no trailing newline after the final URI. */
        for (int i = 0; i < s->n; i++) {
            char uri[PATH_MAX + 32];
            path_to_uri(s->paths[i], uri, sizeof uri);
            if (i) CLIP_ADD("\n");
            CLIP_ADD(uri);
        }
    } else {
        for (int i = 0; i < s->n; i++) {
            char line[PATH_MAX * 3 + 32];
            if (clip_is_text_mime(mime)) {
                snprintf(line, sizeof line, "%s\n", s->paths[i]);
            } else {
                char uri[PATH_MAX + 32];
                path_to_uri(s->paths[i], uri, sizeof uri);
                snprintf(line, sizeof line, "%s\r\n", uri);
            }
            CLIP_ADD(line);
        }
    }
#undef CLIP_ADD

    *out_len = len;
    return buf;
}

static void clip_source_send(void *data, struct wl_data_source *source,
                             const char *mime, int fd)
{
    (void)source;
    App *a = data;
    size_t len = 0;
    char *buf = clip_build_payload(a, mime ? mime : "", &len);
    clip_write_enqueue(a, fd, buf, len);
}

static void clip_source_target(void *data, struct wl_data_source *source,
                               const char *mime)
{ (void)data; (void)source; (void)mime; }

static void clip_source_cancelled(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->clip_source != source) return;
    clip_write_cancel_all(a);
    wl_data_source_destroy(source);
    a->clip_source = NULL;
    a->n_clip = 0;
    a->clip_cut = 0;
    a->clip_cache_valid = 0;
    memset(&a->clip_snap, 0, sizeof a->clip_snap);
}

static void clip_source_drop(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }
static void clip_source_finished(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }
static void clip_source_action(void *data, struct wl_data_source *source,
                               uint32_t action)
{ (void)data; (void)source; (void)action; }

static const struct wl_data_source_listener clip_source_listener = {
    .target = clip_source_target, .send = clip_source_send,
    .cancelled = clip_source_cancelled,
    .dnd_drop_performed = clip_source_drop,
    .dnd_finished = clip_source_finished,
    .action = clip_source_action,
};

static void clipboard_publish_snapshot(App *a)
{
    if (!a->data_device || !a->data_device_manager) return;
    uint32_t serial = a->clip_serial ? a->clip_serial : a->pointer_serial;

    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    struct wl_data_source *old = a->clip_source;
    a->clip_source = NULL;

    /* GTK sets the new selection before destroying the old source.  If we
     * destroy first, a clipboard manager can observe an empty selection and
     * restore its own stale payload in the gap. */
    if (!a->clip_snap.n && !a->clip_snap.has_text) {
        clip_write_cancel_all(a);
        wl_data_device_set_selection(a->data_device, NULL, serial);
        if (old) wl_data_source_destroy(old);
        wl_display_flush(a->display);
        return;
    }

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->data_device_manager);
    if (!src) {
        a->clip_source = old;
        return;
    }
    clip_write_cancel_all(a);
    wl_data_source_add_listener(src, &clip_source_listener, a);
    static const char *const mimes[] = {
        "x-special/gnome-copied-files",
        "text/uri-list",
        "text/plain;charset=utf-8",
        "text/plain",
        "UTF8_STRING",
        "STRING",
        "TEXT",
    };
    for (unsigned i = 0; i < sizeof mimes / sizeof mimes[0]; i++)
        wl_data_source_offer(src, mimes[i]);
    a->clip_source = src;
    wl_data_device_set_selection(a->data_device, src, serial);
    if (old) wl_data_source_destroy(old);
    wl_display_flush(a->display);
}

/* Cut/Copy freezes the selected paths here.  Later selection changes cannot
 * make a pending paste send the wrong payload. */
static void clipboard_publish(App *a)
{
    clip_snapshot_files(a, a->clipboard, a->n_clip, a->clip_cut);
    clipboard_publish_snapshot(a);
}

static void clipboard_publish_text(App *a, const char *text)
{
    clip_snapshot_text(a, text);
    clipboard_publish_snapshot(a);
}

static int clipboard_has(App *a)
{
    return a->n_clip > 0 || a->clip_source || a->clip_snap.has_text ||
           (a->clip_ctx && (a->clip_ctx->uris || a->clip_ctx->gnome ||
                            a->clip_ctx->text));
}

static void clip_read_cancel(App *a)
{
    if (a->clip_read.fd >= 0) {
        close(a->clip_read.fd);
        a->clip_read.fd = -1;
    }
    a->clip_read.len = 0;
    a->clip_read.overflow = 0;
}

static int clipboard_store_payload(App *a, const char *buf, int gnome,
                                   int *cut)
{
    *cut = 0;
    if (!buf || !*buf) return 0;

    const char *lines = buf;
    if (gnome) {
        const char *nl = strchr(buf, '\n');
        if (!nl) return 0;
        size_t vlen = (size_t)(nl - buf);
        if (vlen == 3 && !strncmp(buf, "cut", 3)) *cut = 1;
        lines = nl + 1;
    }

    int count = 0;
    char *save = NULL;
    char *copy = strdup(lines);
    if (!copy) return 0;
    for (char *line = strtok_r(copy, "\r\n", &save);
         line && count < 64; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char path[PATH_MAX];
        if (!strncmp(line, "file://", 7))
            uri_to_path(line, path, sizeof path);
        else
            snprintf(path, sizeof path, "%s", line);
        if (!*path || access(path, F_OK) != 0) continue;
        snprintf(a->clipboard[count], PATH_MAX, "%s", path);
        count++;
    }
    free(copy);
    return count;
}

/* Nonblocking replacement for the old blocking clipboard_read_offer(). */
static int clipboard_start_async(App *a)
{
    DataOffer *ctx = a->clip_ctx;
    if (!ctx || !(ctx->uris || ctx->gnome || ctx->text)) return 0;

    a->clip_cache_valid = 0;
    const char *mime;
    if (ctx->gnome) mime = "x-special/gnome-copied-files";
    else if (ctx->uris) mime = "text/uri-list";
    else if (ctx->text_mime[0]) mime = ctx->text_mime;
    else mime = "text/plain;charset=utf-8";

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return 0;
    wl_data_offer_receive(ctx->offer, mime, p[1]);
    close(p[1]);
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) fcntl(p[0], F_SETFL, flags | O_NONBLOCK);
    wl_display_flush(a->display);

    clip_read_cancel(a);
    ClipRead *r = &a->clip_read;
    r->fd = p[0];
    r->len = 0;
    r->overflow = 0;
    r->attempt++;
    r->gnome = ctx->gnome ? 1 : 0;
    r->deadline = mono_sec() + 5.0;
    return 1;
}

/* Called from the main poll loop whenever the transfer fd is readable or
 * its deadline has passed.  The compositor keeps the UI loop alive while
 * the source process writes, so we never block inside a key handler. */
static void clip_read_drain(App *a)
{
    ClipRead *r = &a->clip_read;
    if (r->fd < 0) return;

    for (;;) {
        if (r->len + 1 >= sizeof r->buf) {
            r->overflow = 1;
            break;
        }
        ssize_t n = read(r->fd, r->buf + r->len,
                         sizeof r->buf - 1 - r->len);
        if (n > 0) { r->len += (size_t)n; continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (mono_sec() >= r->deadline) {
                status_set(a, "Clipboard source timed out");
                clip_read_cancel(a);
            }
            return;
        }
        break;
    }

    r->buf[r->len] = 0;
    int gnome = r->gnome;
    int empty = (r->len == 0);
    int overflow = r->overflow;
    close(r->fd);
    r->fd = -1;
    r->len = 0;

    if (overflow) {
        status_set(a, "Clipboard payload is too large");
        return;
    }

    /* Guard against a focus change replacing the offer just before receive().
     * An inert offer closes the pipe immediately, so retry once against the
     * live offer if the compositor published one. */
    if (empty && r->attempt < 3) {
        DataOffer *old = a->clip_ctx;
        if (wl_display_roundtrip(a->display) == 0 &&
            a->clip_ctx && a->clip_ctx != old &&
            (a->clip_ctx->uris || a->clip_ctx->gnome || a->clip_ctx->text) &&
            clipboard_start_async(a))
            return;
    }

    int cut = 0;
    int count = clipboard_store_payload(a, r->buf, gnome, &cut);
    if (!count) {
        status_set(a, "Clipboard has no file paths");
        return;
    }
    a->n_clip = count;
    a->clip_cut = cut;
    a->clip_cache_valid = 1;   /* GTK-style per-offer buffer cache */
    do_paste(a, NULL);
}

/* Pull a remote selection into a pending async read, or use local state.
 * A zero return with an active offer means paste will finish later. */
static int clipboard_fetch(App *a, int *cut)
{
    *cut = 0;
    /* If we own the selection, our staged payload wins even before the
     * compositor echoes the selection event back to us. */
    if (a->clip_source) {
        if (a->n_clip > 0) {
            *cut = a->clip_cut;
            return a->n_clip;
        }
        if (a->clip_snap.has_text && !a->clip_snap.n) {
            int c = 0;
            int n = clipboard_store_payload(a, a->clip_snap.text, 0, &c);
            if (n) return n;
        }
    }
    /* A completed read is cached until the selection changes, just as GTK
     * caches each SelectionBuffer on the wl_data_offer. */
    if (a->clip_ctx && a->clip_cache_valid && a->n_clip > 0) {
        *cut = a->clip_cut;
        return a->n_clip;
    }
    if (a->clip_ctx && (a->clip_ctx->uris || a->clip_ctx->gnome ||
                        a->clip_ctx->text)) {
        a->clip_read.attempt = 0;   /* new paste, not an empty-offer retry */
        if (clipboard_start_async(a)) return 0;
    }
    if (a->n_clip > 0) {
        *cut = a->clip_cut;
        return a->n_clip;
    }
    if (a->clip_snap.has_text && !a->clip_snap.n) {
        int c = 0;
        int n = clipboard_store_payload(a, a->clip_snap.text, 0, &c);
        if (n) return n;
    }
    return 0;
}

static void dnd_icon_destroy(App *a);

static void dnd_source_send(void *data, struct wl_data_source *source,
                            const char *mime, int fd)
{
    (void)source;
    App *a = data;
    int gnome = !strcmp(mime, "x-special/gnome-copied-files");
    const char *eol = gnome ? "\n" : "\r\n";
    const char *prefix = "";
    if (gnome)
        prefix = a->dnd_action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                     ? "cut\n" : "copy\n";
    else if (strcmp(mime, "text/uri-list") && strcmp(mime, "text/plain") &&
             strcmp(mime, "text/plain;charset=utf-8") &&
             strcmp(mime, "UTF8_STRING") && strcmp(mime, "STRING")) {
        close(fd);
        return;
    }

    char buf[16384];
    size_t len = 0;
    size_t plen = strlen(prefix);
    if (plen < sizeof buf) { memcpy(buf, prefix, plen); len = plen; }
    int nsrc = a->n_dnd_paths ? a->n_dnd_paths : a->n_ents;
    for (int i = 0; i < nsrc && len + 300 < sizeof buf; i++) {
        if (!a->n_dnd_paths && !a->selected[i]) continue;
        const char *path = a->n_dnd_paths ? a->dnd_paths[i] : a->ents[i].path;
        char uri[PATH_MAX + 32];
        path_to_uri(path, uri, sizeof uri);
        int n = snprintf(buf + len, sizeof buf - len, "%s%s", uri, eol);
        if (n <= 0 || (size_t)n >= sizeof buf - len) break;
        len += (size_t)n;
    }
    dnd_write_all(fd, buf, len);
    close(fd);
}

static void dnd_source_target(void *data, struct wl_data_source *source,
                              const char *mime)
{ (void)data; (void)source; (void)mime; }

static void dnd_source_cancelled(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->dnd_source == source) {
        wl_data_source_destroy(source);
        a->dnd_source = NULL;
    }
    dnd_icon_destroy(a);
    a->dnd_pending = 0;
}

static void dnd_source_drop_performed(void *data, struct wl_data_source *source)
{ (void)data; (void)source; }

static void dnd_source_finished(void *data, struct wl_data_source *source)
{
    App *a = data;
    if (a->dnd_source == source) {
        wl_data_source_destroy(source);
        a->dnd_source = NULL;
    }
    dnd_icon_destroy(a);
    a->dnd_pending = 0;
}

static void dnd_source_action(void *data, struct wl_data_source *source,
                              uint32_t action)
{
    (void)source;
    App *a = data;
    a->dnd_action = action;
}

static const struct wl_data_source_listener dnd_source_listener = {
    .target = dnd_source_target, .send = dnd_source_send,
    .cancelled = dnd_source_cancelled,
    .dnd_drop_performed = dnd_source_drop_performed,
    .dnd_finished = dnd_source_finished, .action = dnd_source_action,
};

/* Build a small surface showing the dragged item's icon.  The compositor
 * places it under the pointer for the duration of the drag. */
static struct wl_surface *dnd_icon_create(App *a)
{
    int sel = -1;
    for (int i = 0; i < a->n_ents; i++)
        if (a->selected[i]) { sel = i; break; }
    if (sel < 0) return NULL;

    /* Render the drag icon exactly like the item appears in the current
     * view: the large plate icons in icon/tiles, the small glyph in list/
     * details, at the same logical size and output DPI. */
    int big = a->view == VIEW_ICONS || a->view == VIEW_TILES;
    double icon_sz = a->view == VIEW_TILES ? 64.0
                     : (big ? 48.0 : 19.0);
    const double pad = 4.0;
    int scale = a->scale > 0 ? a->scale : 1;
    int SZ = (int)ceil((icon_sz + 2.0 * pad) * (double)scale);
    if (SZ < 8) SZ = 8;

    ShmBuf *b = next_buffer(a, &a->dnd_icon_buf, 1, SZ, SZ);
    if (!b) return NULL;

    cairo_surface_t *cs = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, SZ, SZ, SZ * 4);
    if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(cs);
        return NULL;
    }
    cairo_t *cr = cairo_create(cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, scale, scale);
    icon_draw_entry(a, cr, &a->ents[sel], icon_sz, pad, pad, big);
    cairo_destroy(cr);
    cairo_surface_flush(cs);
    cairo_surface_destroy(cs);

    if (!a->dnd_icon_surface)
        a->dnd_icon_surface = wl_compositor_create_surface(a->compositor);
    if (!a->dnd_icon_surface) return NULL;
    wl_surface_attach(a->dnd_icon_surface, b->buffer, 0, 0);
    wl_surface_damage(a->dnd_icon_surface, 0, 0, SZ, SZ);
    if (wl_surface_get_version(a->dnd_icon_surface) >= 3)
        wl_surface_set_buffer_scale(a->dnd_icon_surface, scale);
    wl_surface_commit(a->dnd_icon_surface);
    b->busy = 1;
    return a->dnd_icon_surface;
}

static void dnd_icon_destroy(App *a)
{
    if (a->dnd_icon_surface) {
        wl_surface_destroy(a->dnd_icon_surface);
        a->dnd_icon_surface = NULL;
    }
    if (a->dnd_icon_buf.buffer) {
        shm_buffer_destroy(&a->dnd_icon_buf);
        memset(&a->dnd_icon_buf, 0, sizeof a->dnd_icon_buf);
    }
}

static void dnd_begin(App *a)
{
    if (!a->dnd_pending || a->dnd_source || !a->data_device_manager ||
        !a->data_device) return;
    int nsel = 0;
    for (int i = 0; i < a->n_ents; i++) if (a->selected[i]) nsel++;
    if (!nsel) { a->dnd_pending = 0; return; }

    /* Freeze the payload now: the user can change the selection while the
     * compositor owns the pointer, and receiving our own offer later must not
     * depend on the source running a nested Wayland dispatch. */
    a->n_dnd_paths = 0;
    for (int i = 0; i < a->n_ents && a->n_dnd_paths < 64; i++)
        if (a->selected[i])
            snprintf(a->dnd_paths[a->n_dnd_paths++], PATH_MAX, "%s",
                     a->ents[i].path);

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(a->data_device_manager);
    if (!src) return;
    wl_data_source_add_listener(src, &dnd_source_listener, a);
    wl_data_source_offer(src, "text/uri-list");
    wl_data_source_offer(src, "x-special/gnome-copied-files");
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    if (wl_data_source_get_version(src) >= WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)
        wl_data_source_set_actions(src, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    a->dnd_source = src;
    a->dnd_pending = 0;
    a->dnd_action = 0;
    a->drag_select = 0;
    struct wl_surface *icon = dnd_icon_create(a);
    wl_data_device_start_drag(a->data_device, src, a->surface, icon,
                              a->dnd_press_serial);
    wl_display_flush(a->display);
}

static int dnd_receive_uris(App *a, struct wl_data_offer *offer)
{
    const char *mime = a->dnd_have_gnome ? "x-special/gnome-copied-files"
                                         : "text/uri-list";
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return 0;
    wl_data_offer_receive(offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(a->display);

    char buf[16384];
    size_t len = 0;
    struct pollfd pf = { .fd = p[0], .events = POLLIN };
    while (len < sizeof buf - 1) {
        if (poll(&pf, 1, 2000) <= 0) break;
        ssize_t n = read(p[0], buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(p[0]);
    buf[len] = 0;

    /* The gnome target starts with "cut" or "copy"; following lines are the
     * same URIs text/uri-list would carry.  A cut is performed as a move by
     * do_paste(), so the source app never has to delete anything itself. */
    int cut = 0;
    char *lines = buf;
    if (a->dnd_have_gnome) {
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = 0;
            cut = !strcmp(buf, "cut") || !strncmp(buf, "cut\r", 4);
            lines = nl + 1;
        }
    }
    if (a->dnd_source) cut = 0;   /* our own drag: keep the old copy behavior */

    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(lines, "\r\n", &save);
         line && count < 64; line = strtok_r(NULL, "\r\n", &save)) {
        if (!*line || *line == '#') continue;
        char path[PATH_MAX];
        uri_to_path(line, path, sizeof path);
        if (!*path || access(path, F_OK) != 0) continue;
        snprintf(a->clipboard[count], PATH_MAX, "%s", path);
        count++;
    }
    if (count) {
        a->n_clip = count;
        a->clip_cut = cut;
        do_paste(a, NULL);
        return 1;
    }
    return 0;
}

/* MIME events arrive before enter()/selection(), so record them on the
 * offer context rather than in a shared "new offer" slot. */
static void dnd_offer_offer(void *data, struct wl_data_offer *offer,
                            const char *mime)
{
    (void)offer;
    DataOffer *ctx = data;
    if (!strcmp(mime, "text/uri-list")) ctx->uris = 1;
    if (!strcmp(mime, "x-special/gnome-copied-files")) ctx->gnome = 1;
    if (clip_is_text_mime(mime)) {
        ctx->text = 1;
        if (!ctx->text_mime[0])
            snprintf(ctx->text_mime, sizeof ctx->text_mime, "%s", mime);
    }
}

static void dnd_offer_source_actions(void *data, struct wl_data_offer *offer,
                                     uint32_t actions)
{ (void)data; (void)offer; (void)actions; }

static void dnd_offer_action(void *data, struct wl_data_offer *offer,
                             uint32_t action)
{
    (void)offer;
    DataOffer *ctx = data;
    if (ctx->app->dnd_ctx == ctx)
        ctx->app->dnd_offer_action = action;
}

static const struct wl_data_offer_listener dnd_offer_listener = {
    .offer = dnd_offer_offer, .source_actions = dnd_offer_source_actions,
    .action = dnd_offer_action,
};

static DataOffer *data_offer_create(App *a, struct wl_data_offer *offer)
{
    DataOffer *ctx = calloc(1, sizeof *ctx);
    if (!ctx) { wl_data_offer_destroy(offer); return NULL; }
    ctx->app = a;
    ctx->offer = offer;
    ctx->next = a->offers;
    a->offers = ctx;
    wl_data_offer_add_listener(offer, &dnd_offer_listener, ctx);
    return ctx;
}

static DataOffer *data_offer_find(App *a, struct wl_data_offer *offer)
{
    for (DataOffer *c = a->offers; c; c = c->next)
        if (c->offer == offer) return c;
    return NULL;
}

static void data_offer_destroy(App *a, DataOffer *ctx)
{
    if (!ctx) return;
    DataOffer **link = &a->offers;
    while (*link && *link != ctx) link = &(*link)->next;
    if (*link) *link = ctx->next;
    if (a->clip_ctx == ctx) {
        a->clip_ctx = NULL;
        a->clip_offer = NULL;
        a->clip_cache_valid = 0;
        clip_read_cancel(a);
    }
    if (a->dnd_ctx == ctx) {
        a->dnd_ctx = NULL;
        a->dnd_offer = NULL;
    }
    wl_data_offer_destroy(ctx->offer);
    free(ctx);
}

/* Offers that never became active (focus changes, cancelled drags) would
 * otherwise live until exit.  Keep the active contexts and the new one. */
static void data_offer_prune(App *a, DataOffer *keep)
{
    DataOffer *c = a->offers;
    while (c) {
        DataOffer *next = c->next;
        if (c != keep && c != a->clip_ctx && c != a->dnd_ctx)
            data_offer_destroy(a, c);
        c = next;
    }
}

static void data_offer_destroy_all(App *a)
{
    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    while (a->offers) data_offer_destroy(a, a->offers);
    a->clip_ctx = a->dnd_ctx = NULL;
    a->clip_offer = a->dnd_offer = NULL;
}

/* Folder entry under the drop point, ignoring one of the dragged items. */
static Entry *dnd_drop_entry(App *a, double x, double y)
{
    if (x < SIDEBAR_W || y < file_top_y(a) || y >= a->WIN_H - STATUS_H)
        return NULL;
    int idx = -1;
    if (a->view == VIEW_ICONS || a->view == VIEW_TILES) {
        if (!icon_grid_hit(a, x, y, &idx)) return NULL;
    } else {
        idx = (int)((y - file_top_y(a)) / list_row_h(a)) + scroll_hit(a);
    }
    if (idx < 0 || idx >= a->n_ents) return NULL;
    Entry *e = &a->ents[idx];
    if (!e->is_dir) return NULL;
    for (int i = 0; i < a->n_dnd_paths; i++)
        if (!strcmp(e->path, a->dnd_paths[i])) return NULL;
    return e;
}

/* Our own drag coming back to us: move the snapshotted files without going
 * through wl_data_offer.receive().  A client cannot dispatch the compositor's
 * wl_data_source.send event while it is blocked reading the transfer pipe. */
static void dnd_internal_drop(App *a)
{
    if (!a->dnd_source || !a->n_dnd_paths) return;

    char saved_clip[64][PATH_MAX];
    int saved_n = a->n_clip, saved_cut = a->clip_cut;
    for (int i = 0; i < saved_n; i++)
        memcpy(saved_clip[i], a->clipboard[i], PATH_MAX);

    for (int i = 0; i < a->n_dnd_paths; i++)
        snprintf(a->clipboard[i], PATH_MAX, "%s", a->dnd_paths[i]);
    a->n_clip = a->n_dnd_paths;
    a->clip_cut = 1;              /* an internal drag is a move */

    char folder[PATH_MAX];
    const char *dir = NULL;
    Entry *e = (a->dnd_target_surface == a->surface)
                   ? dnd_drop_entry(a, a->dnd_drop_x, a->dnd_drop_y) : NULL;
    if (e) {
        snprintf(folder, sizeof folder, "%s", e->path);
        dir = folder;
    }
    do_paste(a, dir);
    /* The transfer is asynchronous; keep xfer_finish() from clearing the
     * clipboard we just restored for the user. */
    if (a->xfer.active) a->xfer.internal = 1;

    for (int i = 0; i < saved_n; i++)
        memcpy(a->clipboard[i], saved_clip[i], PATH_MAX);
    a->n_clip = saved_n;
    a->clip_cut = saved_cut;
    a->dnd_target_surface = NULL;
}

/* v3 offers must be finished, otherwise wlroots turns a successful drop into
 * wl_data_source.cancelled on the source side instead of dnd_finished. */
static void dnd_offer_release(App *a, int success)
{
    DataOffer *ctx = a->dnd_ctx;
    if (ctx) {
        struct wl_data_offer *offer = ctx->offer;
        if (success && a->dnd_accepted &&
            wl_data_offer_get_version(offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION &&
            a->dnd_offer_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
            a->dnd_offer_action != WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
            wl_data_offer_finish(offer);
        data_offer_destroy(a, ctx);
    }
    a->dnd_accept = a->dnd_accepted = 0;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_target_surface = NULL;
    data_offer_prune(a, NULL);
}

static void dnd_data_device_enter(void *data, struct wl_data_device *device,
                                  uint32_t serial, struct wl_surface *surface,
                                  wl_fixed_t x, wl_fixed_t y,
                                  struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    if (a->dnd_ctx) data_offer_destroy(a, a->dnd_ctx);
    DataOffer *ctx = data_offer_find(a, offer);
    if (!ctx) ctx = data_offer_create(a, offer);
    a->dnd_ctx = ctx;
    a->dnd_offer = ctx ? offer : NULL;
    a->dnd_enter_serial = serial;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_target_surface = (surface == a->surface) ? surface : NULL;
    a->dnd_drop_x = wl_fixed_to_double(x);
    a->dnd_drop_y = wl_fixed_to_double(y);
    if (!ctx) return;
    /* Mime events were delivered before enter(); use what we recorded. */
    a->dnd_accept = ctx->uris || ctx->gnome;
    a->dnd_have_gnome = ctx->gnome;
    a->dnd_accepted = 0;
    if (a->dnd_accept) {
        wl_data_offer_accept(offer, serial, a->dnd_have_gnome
                             ? "x-special/gnome-copied-files" : "text/uri-list");
        a->dnd_accepted = 1;
        if (wl_data_offer_get_version(offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
            wl_data_offer_set_actions(offer,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
    data_offer_prune(a, ctx);
}

static void dnd_data_device_motion(void *data, struct wl_data_device *device,
                                   uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)device; (void)time;
    App *a = data;
    a->dnd_drop_x = wl_fixed_to_double(x);
    a->dnd_drop_y = wl_fixed_to_double(y);
}

static void dnd_data_device_leave(void *data, struct wl_data_device *device)
{
    (void)device;
    App *a = data;
    if (a->dnd_ctx) data_offer_destroy(a, a->dnd_ctx);
    a->dnd_accept = a->dnd_accepted = 0;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    a->dnd_target_surface = NULL;
    data_offer_prune(a, NULL);
}

static void dnd_data_device_drop(void *data, struct wl_data_device *device)
{
    (void)device;
    App *a = data;
    int ok = 0;
    if (a->dnd_source) {
        dnd_internal_drop(a);
        ok = 1;                 /* no pipe transfer: no deadlock, no data lost */
    } else if (a->dnd_offer && a->dnd_accept) {
        ok = dnd_receive_uris(a, a->dnd_offer);
    }
    dnd_offer_release(a, ok);
}

static void dnd_data_device_selection(void *data, struct wl_data_device *device,
                                      struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    clip_read_cancel(a);
    a->clip_cache_valid = 0;
    if (a->clip_ctx) data_offer_destroy(a, a->clip_ctx);

    /* The compositor echoes our own selection back to us.  Keep the staged
     * clipboard payload: clearing it here would make the next local paste
     * fail after copy/cut. */
    if (a->clip_source) {
        if (offer) {
            DataOffer *ctx = data_offer_find(a, offer);
            if (ctx) data_offer_destroy(a, ctx);
            else data_offer_create(a, offer);
        }
        data_offer_prune(a, NULL);
        return;
    }

    a->n_clip = 0;
    a->clip_cut = 0;
    if (offer) {
        DataOffer *ctx = data_offer_find(a, offer);
        if (!ctx) ctx = data_offer_create(a, offer);
        if (ctx && (ctx->uris || ctx->gnome || ctx->text)) {
            a->clip_ctx = ctx;
            a->clip_offer = offer;
        } else if (ctx) {
            data_offer_destroy(a, ctx);
        }
    }
    data_offer_prune(a, NULL);
}

static void dnd_data_device_data_offer(void *data,
                                       struct wl_data_device *device,
                                       struct wl_data_offer *offer)
{
    (void)device;
    App *a = data;
    /* Create a context now; MIME events that follow are recorded on it. */
    data_offer_create(a, offer);
}

static const struct wl_data_device_listener dnd_data_device_listener = {
    .data_offer = dnd_data_device_data_offer,
    .enter = dnd_data_device_enter, .leave = dnd_data_device_leave,
    .motion = dnd_data_device_motion, .drop = dnd_data_device_drop,
    .selection = dnd_data_device_selection,
};

static void release_seat(App *a)
{
    clip_write_cancel_all(a);
    if (a->clip_source) {
        wl_data_source_destroy(a->clip_source);
        a->clip_source = NULL;
    }
    data_offer_destroy_all(a);
    if (a->data_device) {
        wl_data_device_destroy(a->data_device);
        a->data_device = NULL;
    }
    if (a->dnd_source) {
        wl_data_source_destroy(a->dnd_source);
        a->dnd_source = NULL;
    }
    a->dnd_pending = 0;
    a->n_dnd_paths = 0;
    a->dnd_target_surface = NULL;
    a->dnd_offer_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    if (a->cursor_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_device);
        a->cursor_device = NULL;
    }
    if (a->pointer) { wl_pointer_destroy(a->pointer); a->pointer = NULL; }
    if (a->keyboard) { wl_keyboard_destroy(a->keyboard); a->keyboard = NULL; }
    if (a->seat) { wl_seat_destroy(a->seat); a->seat = NULL; }
    a->seat_global = 0;
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;
    if (a->data_device_manager && !a->data_device) {
        a->data_device = wl_data_device_manager_get_data_device(
            a->data_device_manager, seat);
        if (a->data_device)
            wl_data_device_add_listener(a->data_device,
                                        &dnd_data_device_listener, a);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        if (a->pointer) wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        if (a->cursor_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_device);
            a->cursor_device = NULL;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        if (a->keyboard) wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver)
{
    App *a = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        if (!a->compositor)
            a->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                             ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        if (!a->shm) {
            a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
            if (a->shm) wl_shm_add_listener(a->shm, &shm_listener, a);
        }
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        if (!a->data_device_manager)
            a->data_device_manager = wl_registry_bind(reg, name,
                &wl_data_device_manager_interface, ver < 3 ? ver : 3);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        if (!a->wm_base) {
            a->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
            if (a->wm_base) xdg_wm_base_add_listener(a->wm_base, &wm_listener, a);
        }
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(reg, name,
            &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(reg, name,
            &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_manager = wl_registry_bind(reg, name,
            &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        if (a->seat) return;
        a->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                   ver < 5 ? ver : 5);
        if (a->seat) {
            a->seat_global = name;
            wl_seat_add_listener(a->seat, &seat_listener, a);
        }
    } else if (!strcmp(iface, wl_output_interface.name)) {
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            Output *o = &a->outputs[i];
            if (o->output) continue;
            o->output = wl_registry_bind(reg, name, &wl_output_interface,
                                         ver < 2 ? ver : 2);
            if (!o->output) return;
            o->global_name = name;
            o->scale = 1;
            o->entered = 0;
            o->app = a;
            wl_output_add_listener(o->output, &output_listener, o);
            break;
        }
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)reg;
    App *a = data;
    if (a->seat && name == a->seat_global) release_seat(a);
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        Output *o = &a->outputs[i];
        if (!o->output || o->global_name != name) continue;
        wl_output_destroy(o->output);
        o->output = NULL;
        o->entered = 0;
        recalc_scale(a);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static int wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtfiles: cannot connect to a Wayland display\n");
        return 0;
    }
    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtfiles: Wayland roundtrip failed\n");
        return 0;
    }
    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtfiles: missing wl_compositor, wl_shm or xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtfiles: compositor has no ARGB8888 shm format\n");
        return 0;
    }
    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) return 0;
    wl_surface_add_listener(a->surface, &surface_listener, a);
    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }
    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    if (!a->xdg_surface) return 0;
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    if (!a->toplevel) return 0;

    /* Restore the saved size before advertising window geometry. Otherwise
       the first geometry is the design fallback while the first buffer uses
       the restored size, so compositor-side decorations are laid out for the
       wrong width and never cover the whole window. */
    if (a->state_w > 0 && a->state_h > 0) {
        if (a->state_w < (int)MIN_W) a->state_w = (int)MIN_W;
        if (a->state_h < (int)MIN_H) a->state_h = (int)MIN_H;
        a->WIN_W = a->state_w;
        a->WIN_H = a->state_h;
    }
    xdg_surface_set_window_geometry(a->xdg_surface, 0, 0,
                                    (int)a->WIN_W, (int)a->WIN_H);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JT Files");
    xdg_toplevel_set_app_id(a->toplevel, "jtfiles");
    xdg_toplevel_set_min_size(a->toplevel, (int)MIN_W, (int)MIN_H);

    if (a->state_maximized) xdg_toplevel_set_maximized(a->toplevel);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        if (a->decoration) {
            zxdg_toplevel_decoration_v1_add_listener(a->decoration,
                                                     &deco_listener, a);
            zxdg_toplevel_decoration_v1_set_mode(a->decoration,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
    }
    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtfiles");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }
    /* An initial commit is needed for the compositor to create/map the view
       and send the first configure. Geometry is already set above with the
       restored size, so this commit carries the correct SSD frame extents. */
    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_commit(a->surface);
    return 1;
}

/* Stop any in-flight laps when the shared toggle is switched off. */
static void appearance_menu_stop_lap(Menu *m)
{
    if (!m) return;
    m->popup_anim_active = 0;
    m->popup_anim_start = 0.0;
    m->dirty = 1;
    for (int i = 0; i < m->n; i++)
        appearance_menu_stop_lap(m->items[i].sub);
}

static int appearance_config_bool(const char *value)
{
    if (!strcasecmp(value, "true")) return 1;
    if (!strcasecmp(value, "false")) return 0;
    return 0; /* off unless explicitly enabled, as in jtlabwc */
}

/* Read ~/.config/jt/appearance.conf with the same keys and default as
 * jtlabwc: a bare true/false, or accent_border_animation / the legacy
 * jtlabwc_border_anim_enabled key.  Off when unset. */
static void appearance_config_load(App *a)
{
    a->border_anim_enabled = 0;
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.config/jt/%s", home, APPEARANCE_CONFIG);
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof line, file)) {
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        char *end = text + strlen(text);
        while (end > text && (end[-1] == '\n' || end[-1] == '\r' ||
                              end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (!*text || *text == '#') continue;
        char *key = "";
        char *value = text;
        char *equals = strchr(text, '=');
        if (equals) {
            *equals = '\0';
            key = text;
            value = equals + 1;
            char *key_end = key + strlen(key);
            while (key_end > key && (key_end[-1] == ' ' ||
                                     key_end[-1] == '\t')) {
                *--key_end = '\0';
            }
            while (*value == ' ' || *value == '\t') value++;
        }
        if (!*value) continue;
        if (!*key || !strcmp(key, "accent_border_animation") ||
            !strcmp(key, "jtlabwc_border_anim_enabled")) {
            a->border_anim_enabled = appearance_config_bool(value);
        }
    }
    fclose(file);
    if (!a->border_anim_enabled) {
        a->post_sel_anim_start = -1;
        appearance_menu_stop_lap(a->menu);
        a->dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Live control socket                                                */
/* ------------------------------------------------------------------ */

static void ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtfiles-ctl.sock", rt);
}

static void ctl_handle_line(App *a, char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                 line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
    if (!strcmp(line, "quit")) { a->quit = 1; return; }
    if (!strcmp(line, "reload")) { appearance_config_load(a); return; }
    if (!strncmp(line, "rainbow", 7) &&
        (line[7] == '\0' || line[7] == ' ' || line[7] == '\t')) {
        const char *arg = line + 7;
        while (*arg == ' ' || *arg == '\t') arg++;
        int on;
        if (!*arg || !strcmp(arg, "toggle"))
            on = !rainbow_mode;
        else if (!strcmp(arg, "on"))
            on = 1;
        else if (!strcmp(arg, "off"))
            on = 0;
        else
            return;
        rainbow_set(on, mono_sec());
        accent_visual_refresh(a);
        return;
    }
    if (!strncmp(line, "accent", 6) && (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t') arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) accent_propagate(a, rgb);
    }
}

static void ctl_close_client(App *a)
{
    if (a->ctl_client_fd >= 0) { close(a->ctl_client_fd); a->ctl_client_fd = -1; }
    a->ctl_len = 0;
}

static void ctl_process(App *a, int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < a->ctl_len; i++) {
        if (a->ctl_buf[i] == '\n') {
            a->ctl_buf[i] = '\0';
            ctl_handle_line(a, a->ctl_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(a->ctl_buf, a->ctl_buf + start, a->ctl_len - start);
        a->ctl_len -= start;
    }
    if (flush && a->ctl_len > 0) {
        a->ctl_buf[a->ctl_len] = '\0';
        ctl_handle_line(a, a->ctl_buf);
        a->ctl_len = 0;
    }
}

static void ctl_read_client(App *a)
{
    char buf[128];
    for (int budget = 32; budget > 0; budget--) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof buf);
        if (n > 0) {
            size_t room = sizeof a->ctl_buf - 1 - a->ctl_len;
            if ((size_t)n > room) { ctl_close_client(a); return; }
            memcpy(a->ctl_buf + a->ctl_len, buf, (size_t)n);
            a->ctl_len += (size_t)n;
            ctl_process(a, 0);
            continue;
        }
        if (n == 0) { ctl_process(a, 1); ctl_close_client(a); return; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(a, 1);
            ctl_close_client(a);
        }
        return;
    }
}

static void ctl_accept(App *a)
{
    for (int budget = 8; budget > 0; budget--) {
        int fd = accept4(a->ctl_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        if (a->ctl_client_fd >= 0) { close(fd); continue; }
        a->ctl_client_fd = fd;
        a->ctl_len = 0;
    }
}

static int ctl_start(App *a)
{
    char path[256];
    ctl_path(path, sizeof path);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof addr.sun_path) return 0;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("jtfiles: control socket"); return 0; }
    mode_t old = umask(0077);
    int bound = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old);
    if (bound < 0 && errno == EADDRINUSE) {
        /* Another instance may already own a live socket. Probe it before
         * replacing the path: stealing it would leave the first instance
         * unreachable by jtfilesctl. */
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int live = 0;
        if (probe >= 0) {
            live = connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0;
            close(probe);
        }
        if (live) { close(fd); return 0; }  /* run without our own socket */
        unlink(path);                        /* stale socket: safe to replace */
        old = umask(0077);
        bound = bind(fd, (struct sockaddr *)&addr, sizeof addr);
        umask(old);
    }
    if (bound < 0) {
        fprintf(stderr, "jtfiles: cannot bind %s: %s\n", path, strerror(errno));
        close(fd);
        return 0;
    }
    if (listen(fd, 4) < 0) { close(fd); unlink(path); return 0; }
    a->ctl_fd = fd;
    return 1;
}

static void ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        struct stat st;
        int have_st = fstat(a->ctl_fd, &st) == 0;
        close(a->ctl_fd);
        a->ctl_fd = -1;
        if (have_st) {
            char path[256];
            ctl_path(path, sizeof path);
            struct stat pst;
            /* Only remove the path if it still points to our socket; a newer
             * instance may have legitimately replaced it after we bound. */
            if (stat(path, &pst) == 0 &&
                pst.st_dev == st.st_dev && pst.st_ino == st.st_ino)
                unlink(path);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Remembered state                                                   */
/* ------------------------------------------------------------------ */

static void state_path(char *out, size_t n)
{
    const char *base = getenv("XDG_STATE_HOME");
    if (base && *base) { snprintf(out, n, "%s/jt/jtfiles.conf", base); return; }
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    snprintf(out, n, "%s/.local/state/jt/jtfiles.conf", home);
}

static void state_mkdir_p(const char *path)
{
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void state_load(App *a)
{
    a->state_w = (int)DESIGN_W;
    a->state_h = (int)DESIGN_H;
    a->state_maximized = 0;
    a->sort_by = SORT_NAME;
    a->sort_desc = 0;
    a->group_by = GROUP_NONE;
    a->view = VIEW_DETAILS;
    a->show_hidden = 0;
    a->smooth_scroll = 1;
    char path[512];
    state_path(path, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], value[384];
        if (sscanf(line, " %63[^= \t] = %383[^\n]", key, value) != 2) continue;
        if (!strcmp(key, "favorite")) {
            if (a->n_favorites < 24 && value[0])
                snprintf(a->favorites[a->n_favorites++], PATH_MAX, "%s", value);
            continue;
        }
        int v = atoi(value);
        if (!strcmp(key, "width") && v > 0 && v <= 16384) a->state_w = v;
        else if (!strcmp(key, "height") && v > 0 && v <= 16384) a->state_h = v;
        else if (!strcmp(key, "maximized")) a->state_maximized = v != 0;
        else if (!strcmp(key, "sort")) a->sort_by = v;
        else if (!strcmp(key, "desc")) a->sort_desc = v != 0;
        else if (!strcmp(key, "group")) a->group_by = v;
        else if (!strcmp(key, "view")) a->view = v;
        else if (!strcmp(key, "hidden")) a->show_hidden = v != 0;
        else if (!strcmp(key, "smooth")) a->smooth_scroll = v != 0;
    }
    fclose(f);
}

static void state_save(App *a)
{
    if (a->state_w <= 0 || a->state_h <= 0) return;
    char path[512];
    state_path(path, sizeof path);
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; state_mkdir_p(dir); }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return; }
    fprintf(f, "width=%d\nheight=%d\nmaximized=%d\n", a->state_w, a->state_h,
            a->state_maximized ? 1 : 0);
    fprintf(f, "sort=%d\ndesc=%d\ngroup=%d\nview=%d\nhidden=%d\nsmooth=%d\n",
            a->sort_by, a->sort_desc ? 1 : 0, a->group_by, a->view,
            a->show_hidden ? 1 : 0, a->smooth_scroll ? 1 : 0);
    for (int i = 0; i < a->n_favorites; i++)
        fprintf(f, "favorite=%s\n", a->favorites[i]);
    fclose(f);
}

static void menu_handle_done(App *a, Menu *m)
{
    if (!m) return;
    if (m->done) {
        menu_close_tree(a);
        return;
    }
    for (int i = 0; i < m->n; i++) {
        Menu *sub = m->items[i].sub;
        if (!sub) continue;
        if (sub->done) {
            menu_close_tree(a);
            return;
        }
        menu_handle_done(a, sub);
    }
}

static int pump_events(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }

    /* Let GLib tell us which D-Bus descriptors to poll, using the same
     * poll(2) call as the Wayland and helper-pipe fds below. */
    GPollFD *dbus_fds = NULL;
    gint n_dbus_fds = 0, dbus_timeout = -1, max_priority = 0;
    gboolean dbus_ready = FALSE, dbus_acquired = FALSE;
    if (a->dbus_ctx && (dbus_acquired = g_main_context_acquire(a->dbus_ctx))) {
        dbus_ready = g_main_context_prepare(a->dbus_ctx, &max_priority);
        n_dbus_fds = g_main_context_query(a->dbus_ctx, max_priority,
                                          &dbus_timeout, NULL, 0);
        dbus_fds = g_new0(GPollFD, n_dbus_fds > 0 ? (guint)n_dbus_fds : 1);
        if (n_dbus_fds > 0)
            g_main_context_query(a->dbus_ctx, max_priority, &dbus_timeout,
                                 dbus_fds, n_dbus_fds);
        if (dbus_ready || dbus_timeout == 0)
            timeout_ms = 0;
        else if (dbus_timeout > 0 &&
                 (timeout_ms < 0 || dbus_timeout < timeout_ms))
            timeout_ms = dbus_timeout;
    }

    struct pollfd fds[64];
    int write_idx[16];
    int n_write_fds = 0;
    int nfds = 0;

    int wl_idx = nfds;
    fds[nfds].fd = wl_display_get_fd(a->display);
    fds[nfds].events = POLLIN | (want_write ? POLLOUT : 0);
    fds[nfds].revents = 0;
    nfds++;

    int ctl_idx = -1, client_idx = -1, thumb_idx = -1, search_idx = -1;
    int ino_idx = -1, clip_idx = -1, dbus_first = -1;
    if (a->ctl_fd >= 0) {
        ctl_idx = nfds;
        fds[nfds].fd = a->ctl_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (a->ctl_client_fd >= 0) {
        client_idx = nfds;
        fds[nfds].fd = a->ctl_client_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (a->xfer.active && a->xfer.fd >= 0) {
        fds[nfds].fd = a->xfer.fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (thumb_wake[0] >= 0) {
        thumb_idx = nfds;
        fds[nfds].fd = thumb_wake[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (search_wake[0] >= 0) {
        search_idx = nfds;
        fds[nfds].fd = search_wake[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (a->ino_fd >= 0) {
        ino_idx = nfds;
        fds[nfds].fd = a->ino_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }
    if (a->clip_read.fd >= 0) {
        clip_idx = nfds;
        fds[nfds].fd = a->clip_read.fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;
    }

    if (dbus_fds && n_dbus_fds > 0 &&
        nfds + n_dbus_fds <= (int)G_N_ELEMENTS(fds)) {
        dbus_first = nfds;
        for (int i = 0; i < n_dbus_fds; i++) {
            fds[nfds].fd = dbus_fds[i].fd;
            fds[nfds].events = dbus_fds[i].events;
            fds[nfds].revents = 0;
            dbus_fds[i].revents = 0;
            nfds++;
        }
    }

    /* Keep pasting into other apps from blocking our Wayland event loop. */
    for (ClipWrite *w = a->clip_writes; w && n_write_fds < 16; w = w->next) {
        write_idx[n_write_fds++] = nfds;
        fds[nfds].fd = w->fd;
        fds[nfds].events = POLLOUT;
        fds[nfds].revents = 0;
        nfds++;
    }

    if (clip_idx >= 0) {
        double left = a->clip_read.deadline - mono_sec();
        int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
        if (ms < timeout_ms || timeout_ms < 0) timeout_ms = ms;
    }
    if (a->xfer.active && a->xfer.show_pending) {
        double left = a->xfer.show_at - mono_sec();
        int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
        if (ms < timeout_ms || timeout_ms < 0) timeout_ms = ms;
    }
    if (a->repeat_active) {
        double left = a->repeat_at - mono_sec();
        int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
        if (ms < timeout_ms || timeout_ms < 0) timeout_ms = ms;
    }
    if (a->searching) {
        double left = a->caret_next - mono_sec();
        int ms = left <= 0.0 ? 0 : (int)(left * 1000.0);
        if (ms < timeout_ms || timeout_ms < 0) timeout_ms = ms;
    }

    int ret = poll(fds, (nfds_t)nfds, timeout_ms);
    int poll_errno = errno;
    if (dbus_acquired) {
        for (int i = 0; i < n_dbus_fds; i++)
            dbus_fds[i].revents = dbus_first >= 0 ?
                                  (gushort)fds[dbus_first + i].revents : 0;
        g_main_context_check(a->dbus_ctx, max_priority, dbus_fds, n_dbus_fds);
        g_main_context_dispatch(a->dbus_ctx);
        g_main_context_release(a->dbus_ctx);
        g_free(dbus_fds);
        jobs_dispatch(a);
    }
    if (ret < 0) return poll_errno == EINTR ? 0 : -1;

    if (fds[wl_idx].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN &&
            errno != EWOULDBLOCK)
            return -1;
    }
    if (fds[wl_idx].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (fds[wl_idx].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0) return -1;
    }
    if (ctl_idx >= 0 && (fds[ctl_idx].revents & POLLIN))
        ctl_accept(a);
    if (client_idx >= 0 && (fds[client_idx].revents & (POLLIN | POLLHUP | POLLERR)))
        ctl_read_client(a);

    xfer_poll(a);
    xfer_window_maybe_open(a);
    if (clip_idx >= 0 &&
        (fds[clip_idx].revents || a->clip_read.fd >= 0))
        clip_read_drain(a);
    for (int i = 0; i < n_write_fds; i++) {
        short re = fds[write_idx[i]].revents;
        if (re & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))
            clip_write_poll(a, fds[write_idx[i]].fd);
    }
    if (ino_idx >= 0 && (fds[ino_idx].revents &
                         (POLLIN | POLLERR | POLLHUP)))
        watch_drain(a);
    if (thumb_idx >= 0 && (fds[thumb_idx].revents & POLLIN)) {
        thumb_wake_drain();
        thumb_collect(a);
    }
    if (search_idx >= 0 && (fds[search_idx].revents & POLLIN))
        search_collect(a);
    menu_handle_done(a, a->menu);
    return 0;
}

static void menu_draw_tree(App *a, Menu *m)
{
    if (!m) return;
    if (m->dirty) {
        m->dirty = 0;
        menu_draw(a, m);
    }
    for (int i = 0; i < m->n; i++)
        if (m->items[i].sub)
            menu_draw_tree(a, m->items[i].sub);
}

static void app_cleanup(App *a)
{
    g_app_signal = NULL;
    menu_close_tree(a);
    ctl_cleanup(a);

    if (a->ino_fd >= 0) { close(a->ino_fd); a->ino_fd = -1; }
    a->ino_wd = -1;

    if (a->xfer.fd >= 0) { close(a->xfer.fd); a->xfer.fd = -1; }
    if (a->xfer.pid > 0) {
        kill(a->xfer.pid, SIGTERM);
        waitpid(a->xfer.pid, NULL, 0);
        a->xfer.pid = 0;
    }
    xfer_window_close(a);
    dialog_window_close(a);

    for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++) {
        Job *j = &a->jobs[i];
        if (j->active && j->cancel) g_cancellable_cancel(j->cancel);
    }
    if (a->dbus_ctx) {
        /* Let cancellation callbacks run before we drop the proxies. */
        for (int spin = 0; spin < 100; spin++) {
            int active = 0;
            for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++)
                active += a->jobs[i].active != 0;
            if (!active) break;
            g_main_context_iteration(a->dbus_ctx, FALSE);
        }
    }
    for (unsigned i = 0; i < sizeof a->jobs / sizeof a->jobs[0]; i++)
        job_release(&a->jobs[i]);

    if (a->udisks) { g_object_unref(a->udisks); a->udisks = NULL; }
    if (a->dbus_ctx) {
        g_main_context_pop_thread_default(a->dbus_ctx);
        g_main_context_unref(a->dbus_ctx);
        a->dbus_ctx = NULL;
    }

    search_thread_stop();
    thumb_stop_worker();
    icon_cache_clear(a);
    for (int i = 0; i < THUMB_CACHE_MAX; i++) {
        if (a->thumbs[i].sf) cairo_surface_destroy(a->thumbs[i].sf);
        a->thumbs[i].sf = NULL;
    }
    if (g_ambient_glow) {
        cairo_surface_destroy(g_ambient_glow);
        g_ambient_glow = NULL;
    }
    if (g_sidebar_glow) {
        cairo_surface_destroy(g_sidebar_glow);
        g_sidebar_glow = NULL;
    }
    g_glow_accent = 0;
    if (a->measure) { cairo_destroy(a->measure); a->measure = NULL; }
    if (a->measure_surf) {
        cairo_surface_destroy(a->measure_surf);
        a->measure_surf = NULL;
    }

    if (a->frame_cb) {
        wl_callback_destroy(a->frame_cb);
        a->frame_cb = NULL;
    }
    for (int i = 0; i < NBUFS; i++)
        shm_buffer_destroy(&a->bufs[i]);

    if (a->xkb_state) { xkb_state_unref(a->xkb_state); a->xkb_state = NULL; }
    if (a->xkb_keymap) { xkb_keymap_unref(a->xkb_keymap); a->xkb_keymap = NULL; }
    if (a->xkb_ctx) { xkb_context_unref(a->xkb_ctx); a->xkb_ctx = NULL; }

    release_seat(a);
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!a->outputs[i].output) continue;
        wl_output_destroy(a->outputs[i].output);
        a->outputs[i].output = NULL;
    }
    if (a->decoration) {
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
        a->decoration = NULL;
    }
    if (a->toplevel) { xdg_toplevel_destroy(a->toplevel); a->toplevel = NULL; }
    if (a->xdg_surface) { xdg_surface_destroy(a->xdg_surface); a->xdg_surface = NULL; }
    if (a->surface) { wl_surface_destroy(a->surface); a->surface = NULL; }
    if (a->icon_manager) {
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
        a->icon_manager = NULL;
    }
    if (a->deco_manager) {
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
        a->deco_manager = NULL;
    }
    if (a->cursor_manager) {
        wp_cursor_shape_manager_v1_destroy(a->cursor_manager);
        a->cursor_manager = NULL;
    }
    if (a->wm_base) { xdg_wm_base_destroy(a->wm_base); a->wm_base = NULL; }
    if (a->shm) { wl_shm_destroy(a->shm); a->shm = NULL; }
    if (a->registry) { wl_registry_destroy(a->registry); a->registry = NULL; }
    if (a->display) { wl_display_disconnect(a->display); a->display = NULL; }

    for (int i = 0; i < a->hist_n; i++) free(a->hist[i]);
    free(a->ents);
    free(a->dir_ents);
    free(a->search_ents);
    free(a->selected);
}

static void on_signal(int sig)
{
    (void)sig;
    if (g_app_signal) g_app_signal->signal_quit = 1;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [PATH] [--select FILE] [--accent #RRGGBB] [rainbow] [COLOR]\n"
            "\n"
            "Wayland file manager with native icons and drive mount/unmount.\n"
            "Right-click for the Windows 10 style context menu.\n"
            "  --select FILE  open FILE's folder and highlight FILE\n"
            "                 (a FILE given as PATH is selected too)\n"
            "  rainbow        cycle the accent through the colour wheel\n"
            "                 (or --rainbow; an explicit colour turns it off)\n"
            "Use jtfilesctl to change the accent while it is running.\n",
            prog);
}

int main(int argc, char **argv)
{
    /* Re-exec'd copy worker: no Wayland, no thumbnail threads, just the
     * length-framed request on stdin and progress on stdout. */
    if (argc >= 2 && !strcmp(argv[1], "--xfer-child")) {
        signal(SIGPIPE, SIG_IGN);
        uint32_t cut32 = 0, n32 = 0;
        if (fread(&cut32, sizeof cut32, 1, stdin) != 1 ||
            fread(&n32, sizeof n32, 1, stdin) != 1 ||
            n32 == 0 || n32 > 64)
            return 1;
        static char srcs[64][PATH_MAX], dsts[64][PATH_MAX];
        for (uint32_t i = 0; i < n32; i++)
            if (!xfer_read_path(srcs[i], sizeof srcs[i]) ||
                !xfer_read_path(dsts[i], sizeof dsts[i]))
                return 1;
        xfer_child(STDOUT_FILENO, (int)cut32, srcs, dsts, (int)n32);
        return 1; /* xfer_child normally _exit()s with its own status */
    }

    /* Re-exec'd trash/permanent-delete worker. Same framing as --xfer-child,
     * but only source paths follow the operation code. */
    if (argc >= 2 && !strcmp(argv[1], "--fs-child")) {
        signal(SIGPIPE, SIG_IGN);
        uint32_t op32 = 0, n32 = 0;
        if (fread(&op32, sizeof op32, 1, stdin) != 1 ||
            fread(&n32, sizeof n32, 1, stdin) != 1 ||
            n32 == 0 || n32 > 64 ||
            (op32 != XOP_TRASH && op32 != XOP_DELETE))
            return 1;
        static char paths[64][PATH_MAX];
        for (uint32_t i = 0; i < n32; i++)
            if (!xfer_read_path(paths[i], sizeof paths[i]))
                return 1;
        fs_child(STDOUT_FILENO, (int)op32, paths, (int)n32);
        return 1; /* fs_child normally _exit()s with its own status */
    }

    App app;
    memset(&app, 0, sizeof app);
    app.WIN_W = (double)DESIGN_W;
    app.WIN_H = (double)DESIGN_H;
    app.scale = 1;
    app.ctl_fd = -1;
    app.ino_fd = app.ino_wd = -1;
    app.ctl_client_fd = -1;
    app.xfer.fd = -1;
    app.clip_read.fd = -1;
    app.sel = -1;
    app.anchor = -1;
    app.hist_pos = -1;
    app.sidebar_drive = -1;
    app.hover_side = -1;
    app.side_hov_last = -2;
    app.pointer_x = -1.0;
    app.pointer_y = -1.0;
    app.last_click_t = 0.0;
    app.last_click_row = -1;
    app.border_anim_enabled = 0;
    app.ctx_kind = CTX_NONE;
    app.repeat_rate = 50.0;   /* overridden by wl_keyboard.repeat_info */
    app.repeat_delay = 0.25;
    g_app_signal = &app;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    thumb_start();
    search_thread_start();

    state_load(&app);
    app.saved_view = app.view;
    scroll_snap(&app);
    if (!accent_cli(&argc, argv)) {
        app_cleanup(&app);
        return 1;
    }
    if (!select_cli(&argc, argv, &app)) {
        app_cleanup(&app);
        return 1;
    }
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        print_usage(argv[0]);
        app_cleanup(&app);
        return 0;
    }

    app.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    app.measure_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    if (app.measure_surf)
        app.measure = cairo_create(app.measure_surf);
    if (!app.xkb_ctx || !app.measure) {
        fprintf(stderr, "jtfiles: cannot initialise input/text backend\n");
        app_cleanup(&app);
        return 1;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
    const char *home = getenv("HOME");
    const char *start = app.cli_start[0] ? app.cli_start :
                        (argc > 1 ? argv[1] : (home && *home ? home : cwd));
    snprintf(app.cwd, sizeof app.cwd, "%s", cwd);
    snprintf(app.path_edit, sizeof app.path_edit, "%s", start);

    appearance_config_load(&app);
    if (!wayland_start(&app)) {
        app_cleanup(&app);
        return 1;
    }
    ctl_start(&app);
    watch_init(&app);
    udisks_init(&app);

    navigate(&app, start, 1);
    if (app.pending_select[0]) {
        apply_pending_select(&app);
        app.pending_select_stage = 1;
    }
    drive_refresh(&app);
    app.dirty = 1;

    double next_drive_poll = mono_sec() + 15.0;
    double next_accent_poll = mono_sec() + 1.0;
    int animating = 0;
    while (!app.quit && !app.signal_quit) {
        /* The frame callback wakes us at presentation time. Dispatch events
         * first, then advance easing against the freshly-updated targets and
         * paint once. Ticking in frame_done_app instead painted a stale
         * position first and added a full extra frame of latency. */
        int lap_active = post_sel_anim_active(&app) ||
                         menu_tree_anim_active(app.menu);
        int timeout_ms = 100;
        if (animating || lap_active) timeout_ms = SEL_LAP_TICK_MS;
        else if (app.search_pending) timeout_ms = SEARCH_SPIN_TICK_MS;
        if (rainbow_mode && timeout_ms > 100) timeout_ms = 100;
        if (pump_events(&app, timeout_ms) < 0) break;

        if (accent_tick(mono_sec()))
            accent_visual_refresh(&app);
        keyboard_repeat_tick(&app);
        search_caret_tick(&app);
        if (app.search_pending) app.dirty = 1;   /* advance the spinner */
        watch_tick(&app);
        if (app.status[0] && mono_sec() - app.status_t >= 8.0) {
            app.status[0] = '\0';
            app.dirty = 1;
        }
        if (app.drives_dirty) {
            app.drives_dirty = 0;
            drive_refresh(&app);
        }
        if (mono_sec() >= next_drive_poll) {
            next_drive_poll = mono_sec() + 15.0;
            drive_refresh(&app);
        }
        if (mono_sec() >= next_accent_poll) {
            next_accent_poll = mono_sec() + 1.0;
            accent_conf_poll(&app);
        }

        animating = menu_scroll_tick(app.menu);
        menu_tree_tick(app.menu, mono_sec());
        if (scroll_tick(&app)) animating = 1;
        if (side_hover_tick(&app)) animating = 1;
        if (post_sel_anim_tick(&app)) animating = 1;

        if (app.dirty) {
            /* Only clear the bit after a real paint; a failed draw must not
             * silently drop the update. */
            if (app_draw(&app)) app.dirty = 0;
        }
        menu_draw_tree(&app, app.menu);
        if (app.xfer.active) xfer_draw(&app);
        if (app.dialog.type) dialog_window_draw(&app);
    }

    state_save(&app);
    app_cleanup(&app);
    return 0;
}
