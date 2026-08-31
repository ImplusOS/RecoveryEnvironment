#include "../Userland/API/File.h"
#include "../Userland/API/Graphics.h"
#include "../Userland/API/Input.h"
#include "../Userland/API/Process.h"
#include "../Userland/API/Serial.h"
#include "../Userland/API/SystemInfo.h"
#ifdef RECOVERY_AUDIO_TEST
#include "../Userland/API/Audio.h"
#endif
#include "../libc/I_libc/include/string.h"
#include "../libc/I_libc/include/stdlib.h"
#include "../libc/I_libc/include/math.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x,u)  ((void)(u),malloc(x))
#define STBTT_free(x,u)    ((void)(u),free(x))
#define STBTT_fmod(x,y)    fmod(x,y)
#include "../Vendor/Header/stb_truetype.h"

#define RECOVERY_PAYLOAD_PATH       "/Recovery/ImplusOS-root.tar.gz"
#define RECOVERY_INSTALL_IMAGE_PATH "/Recovery/ImplusOS-install.img"
#define RECOVERY_MANIFEST_PATH      "/Recovery/MANIFEST.txt"
#define RECOVERY_FONT_PATH          "/BootManager/Resource/Fonts/NotoSansJP-Regular.ttf"

#define INSTALL_CHUNK_SECTORS 2048u
#define INSTALL_SECTOR_SIZE     512u

#define PROGRESS_UPDATE_SECTORS 65536u

#define FONT_SIZE            14
#define FONT_SIZE_LARGE      20
#define MENU_ITEM_HEIGHT     34
#define PROGRESS_BAR_HEIGHT  22
#define RECOVERY_FONT_MAX_SIZE (6u * 1024u * 1024u)

#define KEY_UP   258
#define KEY_DOWN 259

#define C_BG      0xFF101820u
#define C_PANEL   0xFF1B2733u
#define C_BLUE    0xFF2F80EDu
#define C_CYAN    0xFF2D9CDBu
#define C_GREEN   0xFF27AE60u
#define C_RED     0xFFEB5757u
#define C_GRAY    0xFF7A869Au
#define C_MUTED   0xFF505A64u
#define C_WHITE   0xFFFFFFFFu
#define C_TEXT    0xFFE0E0E0u
#define C_SUBTEXT 0xFFA0A0A0u

static stbtt_fontinfo g_font_info     = {0};
static uint8_t       *g_font_data     = NULL;
static uint32_t       g_font_data_size = 0;
static bool           g_font_loaded   = false;

static void puts_serial(const char *s)
{
    if (!s) return;
    while (*s) {
        serial_write_char(*s++);
    }
}

static int uint64_to_str(uint64_t val, char *buf)
{
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[20];
    int len = 0;
    while (val > 0) { tmp[len++] = (char)('0' + val % 10); val /= 10; }
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static int int64_to_str(int64_t val, char *buf)
{
    if (val < 0) {
        buf[0] = '-';
        return 1 + uint64_to_str((uint64_t)(-(val + 1)) + 1u, buf + 1);
    }
    return uint64_to_str((uint64_t)val, buf);
}

static void format_size(uint64_t bytes, char *buf)
{
    const uint64_t GB = (uint64_t)1000 * 1000 * 1000;
    const uint64_t MB = (uint64_t)1000 * 1000;

    if (bytes >= GB) {
        uint64_t whole = bytes / GB;
        uint64_t frac  = (bytes % GB) / (GB / 10);
        int n = uint64_to_str(whole, buf);
        buf[n++] = '.';
        buf[n++] = (char)('0' + frac % 10);
        buf[n++] = ' '; buf[n++] = 'G'; buf[n++] = 'B'; buf[n] = '\0';
    } else if (bytes >= MB) {
        int n = uint64_to_str(bytes / MB, buf);
        buf[n++] = ' '; buf[n++] = 'M'; buf[n++] = 'B'; buf[n] = '\0';
    } else {
        int n = uint64_to_str(bytes, buf);
        buf[n++] = ' '; buf[n++] = 'B'; buf[n] = '\0';
    }
}

static void format_percent(int pct, char *buf)
{
    int i = 0;
    if      (pct >= 100) { buf[i++] = '1'; buf[i++] = '0'; buf[i++] = '0'; }
    else if (pct >=  10) { buf[i++] = (char)('0' + pct / 10);
                           buf[i++] = (char)('0' + pct % 10); }
    else                 { buf[i++] = (char)('0' + pct); }
    buf[i++] = '%'; buf[i] = '\0';
}

static int load_font_from_path(const char *path)
{
    file_stat_t stat;
    memset(&stat, 0, sizeof(stat));
    if (file_stat(path, &stat) < 0 || !stat.exists || stat.size == 0) {
        return -1;
    }
    if (stat.size > RECOVERY_FONT_MAX_SIZE) {
        puts_serial("Warning: font file too large, skipping\n");
        return -1;
    }

    uint8_t *font_data = (uint8_t *)malloc((size_t)stat.size);
    if (!font_data) {
        puts_serial("Error: cannot allocate font buffer\n");
        return -1;
    }

    int32_t fd = file_open(path, 0);
    if (fd < 0) {
        free(font_data);
        puts_serial("Error: cannot open font\n");
        return -1;
    }

    uint32_t total = 0;
    while (total < stat.size) {
        int64_t n = file_read(fd, font_data + total, stat.size - total);
        if (n <= 0) {
            break;
        }
        total += (uint32_t)n;
    }
    file_close(fd);

    if (total != stat.size) {
        puts_serial("Error: short font read\n");
        g_font_data_size = 0;
        free(font_data);
        return -1;
    }

    if (!stbtt_InitFont(&g_font_info, font_data,
                        stbtt_GetFontOffsetForIndex(font_data, 0))) {
        puts_serial("Error: stbtt_InitFont failed\n");
        g_font_data_size = 0;
        free(font_data);
        return -1;
    }

    free(g_font_data);
    g_font_data = font_data;
    g_font_data_size = stat.size;
    g_font_loaded = true;
    puts_serial("Font loaded: ");
    puts_serial(path);
    puts_serial("\n");
    return 0;
}

static int load_font_from_boot_info(void)
{
    int64_t size = os_get_boot_font(NULL, 0);
    if (size <= 0) {
        return -1;
    }
    if ((uint64_t)size > RECOVERY_FONT_MAX_SIZE) {
        puts_serial("Warning: boot font too large, skipping\n");
        return -1;
    }

    uint8_t *font_data = (uint8_t *)malloc((size_t)size);
    if (!font_data) {
        puts_serial("Error: cannot allocate boot font buffer\n");
        return -1;
    }

    int64_t copied = os_get_boot_font(font_data, (uint64_t)size);
    if (copied != size) {
        puts_serial("Error: cannot copy boot font\n");
        g_font_data_size = 0;
        free(font_data);
        return -1;
    }

    int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
    if (offset < 0 ||
        !stbtt_InitFont(&g_font_info, font_data, offset)) {
        puts_serial("Error: boot font is invalid\n");
        g_font_data_size = 0;
        free(font_data);
        return -1;
    }

    free(g_font_data);
    g_font_data = font_data;
    g_font_data_size = (uint32_t)size;
    g_font_loaded = true;
    puts_serial("Font loaded: boot info\n");
    return 0;
}

static int init_font(void)
{
    static const char *paths[] = {
        RECOVERY_FONT_PATH,
        "/Userland/com.ImplusOS.windowmanager/Resource/Fonts/NotoSansJP-Regular.ttf",
        "/BootManager/Fonts/NotoSansJP-Regular.ttf",
        "/Fonts/NotoSansJP-Regular.ttf",
        NULL
    };

    if (g_font_loaded) return 0;

    for (int i = 0; paths[i]; i++) {
        if (load_font_from_path(paths[i]) == 0) {
            return 0;
        }
    }

    if (load_font_from_boot_info() == 0) {
        return 0;
    }

    puts_serial("Warning: recovery font file not found\n");
    return -1;
}

static uint32_t blend_pixel(uint32_t src, uint32_t dst, uint8_t alpha)
{
    uint32_t sa = alpha, ia = 255 - sa;
    uint32_t r = (((src >> 16) & 0xFF) * sa + ((dst >> 16) & 0xFF) * ia) / 255;
    uint32_t g = (((src >>  8) & 0xFF) * sa + ((dst >>  8) & 0xFF) * ia) / 255;
    uint32_t b = (( src        & 0xFF) * sa + ( dst        & 0xFF) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

static void draw_text_at(int x, int y, const char *text, uint32_t color, int font_size)
{
    if (!g_font_loaded) return;
    uint32_t sw = get_display_width(), sh = get_display_height();
    if (!sw || !sh) return;

    float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)font_size);
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&g_font_info, &ascent, &descent, &linegap);
    int baseline  = (int)(ascent * scale);
    int current_x = x;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 32 || *p >= 127) continue;

        int advance, lsb, ix0, iy0, ix1, iy1;
        stbtt_GetCodepointHMetrics(&g_font_info, *p, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font_info, *p, scale, scale,
                                    &ix0, &iy0, &ix1, &iy1);

        int bw = ix1 - ix0, bh = iy1 - iy0;
        if (bw > 0 && bh > 0) {
            unsigned char *bmp = stbtt_GetCodepointBitmap(
                &g_font_info, scale, scale, *p, &bw, &bh, NULL, NULL);
            if (bmp) {
                for (int py = 0; py < bh; py++) {
                    for (int px = 0; px < bw; px++) {
                        int sx = current_x + ix0 + px;
                        int sy = y + baseline + iy0 + py;
                        if (sx < 0 || sy < 0 || sx >= (int)sw || sy >= (int)sh)
                            continue;
                        uint8_t a = bmp[py * bw + px];
                        if (!a) continue;
                        draw_pixel(sx, sy, blend_pixel(color, get_pixel(sx, sy), a));
                    }
                }
                free(bmp);
            }
        }
        current_x += (int)(advance * scale);
    }
}

static void draw_bordered_rect(int x, int y, int w, int h,
                               uint32_t fill, uint32_t border)
{
    draw_fill_rect(x,     y,     w,     h,     border);
    draw_fill_rect(x + 1, y + 1, w - 2, h - 2, fill);
}

static void draw_progress_bar(int x, int y, int width, int height,
                               float progress,
                               uint32_t bg_color, uint32_t fg_color)
{
    draw_bordered_rect(x, y, width, height, bg_color, C_WHITE);
    if (progress > 0.0f) {
        int inner = width - 2;
        int fill  = (int)((float)inner * (progress < 1.0f ? progress : 1.0f));
        if (fill > 0)
            draw_fill_rect(x + 1, y + 1, fill, height - 2, fg_color);
    }
}

static int read_key_input(void)
{
    static bool s_held[512];
    static bool s_initialized = false;
    if (!s_initialized) {
        memset(s_held, 0, sizeof(s_held));
        s_initialized = true;
    }

    while (1) {
        input_keyboard_event_t ev;
        if (input_read_keyboard(&ev) < 0) {
            process_yield();
            continue;
        }

        int kc  = ev.keycode;
        int idx = (kc >= 0 && kc < 512) ? kc : -1;

        if (!ev.pressed) {
            if (idx >= 0) s_held[idx] = false;
            continue;
        }

        if (idx >= 0 && s_held[idx]) continue;
        if (idx >= 0) s_held[idx] = true;

        if (ev.ascii != 0) return (unsigned char)ev.ascii;

        if (kc == KEY_UP)   return KEY_UP;
        if (kc == KEY_DOWN) return KEY_DOWN;
    }
}

static void draw_status_screen(uint32_t header_color, const char *title,
                               const char *message, const char *footer_hint)
{
    uint32_t w = get_display_width(), h = get_display_height();
    if (!w || !h) return;

    draw_fill_rect(0, 0, w, h, C_BG);
    draw_fill_rect(0, 0, w, 72, header_color);
    draw_text_at(32, 14, "ImplusOS Recovery", C_WHITE, FONT_SIZE);
    if (title)   draw_text_at(32, 36, title, C_WHITE, FONT_SIZE_LARGE);
    if (message) draw_text_at(32, 110, message, C_TEXT, FONT_SIZE);

    const char *hint = footer_hint ? footer_hint : "Press 'r' to reboot";
    draw_fill_rect(0, (int)h - 36, w, 36, C_PANEL);
    draw_text_at(32, (int)h - 22, hint, C_SUBTEXT, FONT_SIZE - 2);

    draw_present();
}

static void print_disk_info_serial(uint32_t index, const system_disk_info_t *d)
{
    char buf[21];
    puts_serial("  ["); uint64_to_str(index, buf); puts_serial(buf); puts_serial("] ");
    puts_serial(d->disk_name);   puts_serial("  ");
    puts_serial(d->manufacturer); puts_serial(" "); puts_serial(d->model);
    puts_serial("  bytes="); uint64_to_str(d->total_bytes, buf); puts_serial(buf);
    puts_serial("  sector="); uint64_to_str(d->sector_size, buf); puts_serial(buf);
    if (d->flags & SYSTEM_DISK_FLAG_BOOT)     puts_serial(" [boot]");
    if (d->flags & SYSTEM_DISK_FLAG_WRITABLE)  puts_serial(" [writable]");
    else                                        puts_serial(" [read-only]");
    puts_serial("\n");
}

static bool disk_is_installable(const system_disk_info_t *d)
{
    return (d->flags & SYSTEM_DISK_FLAG_WRITABLE) &&
           !(d->flags & SYSTEM_DISK_FLAG_BOOT);
}

static void draw_disk_menu(const system_disk_info_t *disks, uint32_t count,
                           int selected)
{
    uint32_t w = get_display_width(), h = get_display_height();
    if (!w || !h) return;

    uint32_t display_count = count < 8 ? count : 8;

    draw_fill_rect(0, 0, w, h, C_BG);
    draw_fill_rect(0, 0, w, 72, C_BLUE);
    draw_text_at(32, 14, "ImplusOS Recovery",        C_WHITE, FONT_SIZE);
    draw_text_at(32, 36, "Select Installation Target", C_WHITE, FONT_SIZE_LARGE);

    draw_text_at(52,  82, "Device", C_SUBTEXT, FONT_SIZE - 2);
    draw_text_at(180, 82, "Size",   C_SUBTEXT, FONT_SIZE - 2);
    draw_text_at(290, 82, "Model",  C_SUBTEXT, FONT_SIZE - 2);

    int start_y = 96;
    for (uint32_t i = 0; i < display_count; i++) {
        const system_disk_info_t *d = &disks[i];
        bool installable = disk_is_installable(d);
        bool is_sel      = ((int)i == selected);
        int  y           = start_y + (int)(i * MENU_ITEM_HEIGHT);

        uint32_t bg   = is_sel ? C_BLUE  : C_PANEL;
        uint32_t text = is_sel ? C_WHITE : (installable ? C_TEXT    : C_MUTED);
        uint32_t sub  = is_sel ? 0xFFBBDDFFu : (installable ? C_SUBTEXT : C_MUTED);

        draw_fill_rect(32, y, (int)w - 64, MENU_ITEM_HEIGHT - 2, bg);

        draw_text_at(52, y + 9, d->disk_name, text, FONT_SIZE);

        char size_buf[16];
        format_size(d->total_bytes, size_buf);
        draw_text_at(180, y + 9, size_buf, sub, FONT_SIZE);

        if (!installable) {
            const char *reason = (d->flags & SYSTEM_DISK_FLAG_BOOT)
                                 ? "[boot media – cannot overwrite]"
                                 : "[read-only]";
            draw_text_at(290, y + 9, reason, C_MUTED, FONT_SIZE - 2);
        } else {
            draw_text_at(290, y + 9, d->model, sub, FONT_SIZE - 2);
        }
    }

    draw_fill_rect(0, (int)h - 36, w, 36, C_PANEL);
    draw_text_at(32, (int)h - 22,
                 "Arrow keys: move   Enter / number key: confirm   q: cancel",
                 C_SUBTEXT, FONT_SIZE - 2);

    draw_present();
}

static int choose_disk(const system_disk_info_t *disks, uint32_t count,
                       uint32_t *out_index)
{
    uint32_t display_count = count < 8 ? count : 8;

    int selected = -1;
    for (uint32_t i = 0; i < display_count; i++) {
        if (disk_is_installable(&disks[i])) { selected = (int)i; break; }
    }

    if (selected < 0) {
        draw_status_screen(C_RED,
                           "No installable disk found",
                           "All detected disks are read-only or are the boot medium. "
                           "Attach a target disk and reboot.",
                           "Press 'r' to reboot");
        puts_serial("Error: no installable disks available.\n");
        return -1;
    }

    while (1) {
        draw_disk_menu(disks, count, selected);

        int ch = read_key_input();

        if (ch == 'q' || ch == 'Q') {
            puts_serial("User canceled disk selection.\n");
            return -1;
        }

        if (ch == KEY_DOWN) {
            for (int t = 0; t < (int)display_count; t++) {
                selected = (selected + 1) % (int)display_count;
                if (disk_is_installable(&disks[selected])) break;
            }
        } else if (ch == KEY_UP) {
            for (int t = 0; t < (int)display_count; t++) {
                selected = (selected - 1 + (int)display_count) % (int)display_count;
                if (disk_is_installable(&disks[selected])) break;
            }
        } else if (ch == '\r' || ch == '\n') {
            if (disk_is_installable(&disks[selected])) {
                *out_index = (uint32_t)selected;
                return 0;
            }
        }
        else if (ch >= '1' && ch <= '8') {
            uint32_t idx = (uint32_t)(ch - '1');
            if (idx < display_count && disk_is_installable(&disks[idx])) {
                *out_index = idx;
                return 0;
            }
        }
    }
}

static int confirm_install(const system_disk_info_t *disk)
{
    uint32_t w = get_display_width(), h = get_display_height();
    char size_buf[16];
    format_size(disk->total_bytes, size_buf);

    while (1) {
        draw_fill_rect(0, 0, w, h, C_BG);

        draw_fill_rect(0, 0, w, 72, C_RED);
        draw_text_at(32, 14, "ImplusOS Recovery", C_WHITE, FONT_SIZE);
        draw_text_at(32, 36, "WARNING: This disk will be erased", C_WHITE, FONT_SIZE_LARGE);

        draw_fill_rect(32, 90, (int)w - 64, 54, C_PANEL);
        draw_text_at(52, 100, "Target disk:", C_SUBTEXT, FONT_SIZE - 2);
        draw_text_at(52, 118, disk->disk_name,   C_WHITE,   FONT_SIZE);
        draw_text_at(160, 118, size_buf,          C_SUBTEXT, FONT_SIZE);
        draw_text_at(260, 118, disk->manufacturer, C_SUBTEXT, FONT_SIZE);
        draw_text_at(400, 118, disk->model,        C_SUBTEXT, FONT_SIZE - 2);

        draw_text_at(32, 170, "All data on this disk will be permanently destroyed.", C_TEXT, FONT_SIZE);
        draw_text_at(32, 196, "This action cannot be undone.", C_RED,  FONT_SIZE);

        int btn_y = 240;
        draw_bordered_rect(80,            btn_y, 170, 50, C_GREEN, 0xFF00CC44u);
        draw_text_at(110, btn_y + 15, "Confirm (Y)", C_WHITE, FONT_SIZE);

        draw_bordered_rect((int)w - 250, btn_y, 170, 50, 0xFF3A2020u, 0xFF993333u);
        draw_text_at((int)w - 225, btn_y + 15, "Cancel (N)", C_TEXT, FONT_SIZE);

        draw_fill_rect(0, (int)h - 36, w, 36, C_PANEL);
        draw_text_at(32, (int)h - 22,
                     "Press 'y' to confirm and begin installation, "
                     "'n' or 'q' to cancel",
                     C_SUBTEXT, FONT_SIZE - 2);

        draw_present();

        int ch = read_key_input();
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 'q' || ch == 'Q') return 0;
    }
}
static void show_payload_status(void)
{
    const char *paths[] = {
        RECOVERY_PAYLOAD_PATH,
        RECOVERY_INSTALL_IMAGE_PATH,
        RECOVERY_MANIFEST_PATH,
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        file_stat_t stat;
        memset(&stat, 0, sizeof(stat));
        bool ok = (file_stat(paths[i], &stat) == 0 && stat.exists);
        puts_serial(ok ? "  OK  " : " MISS ");
        puts_serial(paths[i]);
        if (ok) {
            char buf[21]; uint64_to_str(stat.size, buf);
            puts_serial("  ("); puts_serial(buf); puts_serial(" bytes)");
        } else {
            puts_serial("  [MISSING]");
        }
        puts_serial("\n");
    }
}

static int install_image_to_disk(uint32_t disk_index)
{
    file_stat_t stat;
    memset(&stat, 0, sizeof(stat));
    if (file_stat(RECOVERY_INSTALL_IMAGE_PATH, &stat) < 0 ||
        !stat.exists || stat.size == 0) {
        draw_status_screen(C_RED, "Install image not found",
                           "The recovery media may be incomplete.",
                           "Press 'r' to reboot");
        puts_serial("Error: install image missing or empty.\n");
        return -1;
    }
    if ((stat.size % INSTALL_SECTOR_SIZE) != 0) {
        draw_status_screen(C_RED, "Install image is not sector-aligned",
                           "The recovery media may be corrupted.",
                           "Press 'r' to reboot");
        puts_serial("Error: install image not sector-aligned.\n");
        return -1;
    }

    int32_t fd = file_open(RECOVERY_INSTALL_IMAGE_PATH, 0);
    if (fd < 0) {
        draw_status_screen(C_RED, "Cannot open install image",
                           "The recovery media may be corrupted.",
                           "Press 'r' to reboot");
        puts_serial("Error: cannot open install image.\n");
        return -1;
    }

    uint32_t max_chunk_bytes = INSTALL_CHUNK_SECTORS * INSTALL_SECTOR_SIZE;
    uint8_t *buffer = (uint8_t *)malloc(max_chunk_bytes);
    if (!buffer) {
        file_close(fd);
        draw_status_screen(C_RED, "Out of memory",
                           "Recovery could not allocate the install buffer.",
                           "Press 'r' to reboot");
        puts_serial("Error: cannot allocate install buffer.\n");
        return -1;
    }

    uint32_t total_sectors   = (uint32_t)(stat.size / INSTALL_SECTOR_SIZE);
    uint32_t written_sectors = 0;
    uint32_t w = get_display_width(), h = get_display_height();
    char     buf[21];

    puts_serial("Installing: total sectors = ");
    uint64_to_str(total_sectors, buf); puts_serial(buf); puts_serial("\n");

    draw_fill_rect(0, 0, w, h, C_BG);
    draw_fill_rect(0, 0, w, 72, C_CYAN);
    draw_text_at(32, 22, "Installing ImplusOS", C_WHITE, FONT_SIZE_LARGE);
    draw_text_at(32, 110, "Writing image to disk...", C_TEXT, FONT_SIZE);
    draw_progress_bar(64, 160, (int)w - 128, PROGRESS_BAR_HEIGHT, 0.0f, C_PANEL, C_GREEN);
    draw_fill_rect(0, (int)h - 36, w, 36, C_RED);
    draw_text_at(32, (int)h - 22, "Do not power off or remove any disk!", C_WHITE, FONT_SIZE - 2);
    draw_present();

    while (written_sectors < total_sectors) {
        uint32_t remaining   = total_sectors - written_sectors;
        uint32_t chunk       = remaining < INSTALL_CHUNK_SECTORS
                               ? remaining : INSTALL_CHUNK_SECTORS;
        uint32_t chunk_bytes = chunk * INSTALL_SECTOR_SIZE;

        int64_t n = file_read(fd, buffer, chunk_bytes);
        if (n != (int64_t)chunk_bytes) {
            free(buffer);
            file_close(fd);
            draw_status_screen(C_RED, "Read error",
                               "Could not read from the install image. "
                               "The recovery media may be damaged.",
                               "Press 'r' to reboot");
            puts_serial("Error: short read from install image.\n");
            return -1;
        }

        int64_t status = os_raw_block_write(disk_index, written_sectors,
                                            buffer, chunk);
        if (status < 0) {
            free(buffer);
            file_close(fd);
            puts_serial("Error: block write failed at sector ");
            uint64_to_str(written_sectors, buf); puts_serial(buf); puts_serial("\n");
            draw_status_screen(C_RED, "Write error",
                               "A disk write error occurred. "
                               "The target disk may be faulty.",
                               "Press 'r' to reboot");
            return -1;
        }

        written_sectors += chunk;

        if ((written_sectors % PROGRESS_UPDATE_SECTORS) == 0 ||
            written_sectors == total_sectors) {

            float progress = (float)written_sectors / (float)total_sectors;
            int   bar_w    = (int)w - 128;

            draw_fill_rect(0, 0, w, h, C_BG);
            draw_fill_rect(0, 0, w, 72, C_CYAN);
            draw_text_at(32, 22, "Installing ImplusOS", C_WHITE, FONT_SIZE_LARGE);
            draw_text_at(32, 110, "Writing image to disk...", C_TEXT, FONT_SIZE);

            draw_progress_bar(64, 160, bar_w, PROGRESS_BAR_HEIGHT,
                              progress, C_PANEL, C_GREEN);

            char pct_buf[8];
            format_percent((int)(progress * 100.0f), pct_buf);
            draw_text_at(64 + bar_w - 48, 190, pct_buf, C_WHITE, FONT_SIZE);

            draw_fill_rect(0, (int)h - 36, w, 36, C_RED);
            draw_text_at(32, (int)h - 22,
                         "Do not power off or remove any disk!",
                         C_WHITE, FONT_SIZE - 2);

            draw_present();
        }

        if ((written_sectors % 2048u) == 0 || written_sectors == total_sectors) {
            puts_serial("  ");
            uint64_to_str(written_sectors, buf); puts_serial(buf);
            puts_serial(" / ");
            uint64_to_str(total_sectors, buf);   puts_serial(buf);
            puts_serial(" sectors written\n");
        }
    }

    free(buffer);
    file_close(fd);
    puts_serial("Installation complete.\n");
    return 0;
}

static void wait_for_reboot(uint32_t color, const char *title,
                             const char *message)
{
    draw_status_screen(color, title, message, "Press 'r' to reboot");
    while (1) {
        int ch = read_key_input();
        if (ch == 'r' || ch == 'R') system_reboot();
    }
}

#ifdef RECOVERY_AUDIO_TEST
static void run_audio_self_test(void)
{
    static int16_t pcm[2048u * 2u];
    os_audio_info_t info;

    if (os_audio_open() < 0) {
        puts_serial("Audio self-test: unavailable\n");
        return;
    }
    memset(&info, 0, sizeof(info));
    if (os_audio_get_info(&info) < 0 ||
        info.sample_rate != 48000u ||
        info.channels != 2u ||
        info.format != OS_AUDIO_FORMAT_S16_LE) {
        puts_serial("Audio self-test: unsupported format\n");
        os_audio_close();
        return;
    }

    for (uint32_t frame = 0u; frame < 2048u; ++frame) {
        int16_t sample = ((frame / 50u) & 1u) != 0u ? 6000 : -6000;
        pcm[frame * 2u] = sample;
        pcm[frame * 2u + 1u] = sample;
    }

    uint64_t offset = 0u;
    while (offset < sizeof(pcm)) {
        int64_t written = os_audio_write(
            (const uint8_t *)pcm + offset, sizeof(pcm) - offset);
        if (written <= 0) {
            puts_serial("Audio self-test: write failed\n");
            os_audio_close();
            return;
        }
        offset += (uint64_t)written;
    }
    if (os_audio_drain(3000u) < 0) {
        puts_serial("Audio self-test: drain failed\n");
        os_audio_close();
        return;
    }
    os_audio_close();
    puts_serial("Audio self-test: PASS\n");
}
#endif

static void run_recovery(void)
{
    init_font();

#ifdef RECOVERY_AUDIO_TEST
    run_audio_self_test();
#endif

    puts_serial("\nImplusOS Recovery Environment\n");
    puts_serial("Recovery media contents:\n");
    show_payload_status();
    puts_serial("\n");

    uint32_t count = 0;
    int64_t disk_count_status = os_get_disk_count(&count);
    if (disk_count_status < 0 || count == 0) {
        char buf[24];
        puts_serial("Disk count syscall status: ");
        int64_to_str(disk_count_status, buf); puts_serial(buf);
        puts_serial(", count: ");
        uint64_to_str(count, buf); puts_serial(buf);
        puts_serial("\n");
        puts_serial("Error: no disks reported by kernel.\n");
        wait_for_reboot(C_RED, "No disks detected",
                        "No storage devices were found. "
                        "Check connections and reboot.");
        return;
    }
    puts_serial("Disk count: ");
    char count_buf[21];
    uint64_to_str(count, count_buf); puts_serial(count_buf); puts_serial("\n");

    system_disk_info_t *disks =
        (system_disk_info_t *)malloc(sizeof(system_disk_info_t) * count);
    if (!disks) {
        puts_serial("Error: OOM allocating disk info.\n");
        wait_for_reboot(C_RED, "Out of memory",
                        "Recovery could not allocate memory for disk information.");
        return;
    }

    puts_serial("Detected disks:\n");
    for (uint32_t i = 0; i < count; i++) {
        memset(&disks[i], 0, sizeof(disks[i]));
        if (os_get_disk_info(i, &disks[i]) == 0)
            print_disk_info_serial(i, &disks[i]);
    }

    uint32_t disk_index = 0;
    if (choose_disk(disks, count, &disk_index) < 0) {
        free(disks);
        wait_for_reboot(C_GRAY, "Installation canceled",
                        "No disk was selected. Reboot to try again.");
        return;
    }

    system_disk_info_t selected;
    memset(&selected, 0, sizeof(selected));
    if (os_get_disk_info(disk_index, &selected) < 0) {
        free(disks);
        wait_for_reboot(C_RED, "Disk disappeared",
                        "The selected disk is no longer available.");
        return;
    }

    free(disks);

    if (selected.flags & SYSTEM_DISK_FLAG_BOOT) {
        puts_serial("Refused: selected disk is boot media.\n");
        wait_for_reboot(C_RED, "Cannot install to boot media",
                        "Attach the target disk and reboot the installer.");
        return;
    }
    if (!(selected.flags & SYSTEM_DISK_FLAG_WRITABLE)) {
        puts_serial("Refused: selected disk is read-only.\n");
        wait_for_reboot(C_RED, "Disk is read-only",
                        "The selected disk cannot be written to.");
        return;
    }
    
    if (!confirm_install(&selected)) {
        wait_for_reboot(C_GRAY, "Installation canceled",
                        "Reboot to try again.");
        return;
    }
    
    char buf[21];
    puts_serial("Installing to disk index ");
    uint64_to_str(disk_index, buf); puts_serial(buf); puts_serial("\n");

    if (install_image_to_disk(disk_index) == 0) {
        wait_for_reboot(C_GREEN, "Installation complete!",
                        "Remove the recovery media, then press 'r' to reboot "
                        "into your new ImplusOS system.");
    } else {
        wait_for_reboot(C_RED, "Installation failed",
                        "See serial output for details. "
                        "Press 'r' to reboot.");
    }
}

void _start(void)
{
    run_recovery();
    while (1) {
        int ch = read_key_input();
        process_yield();
    }
}
