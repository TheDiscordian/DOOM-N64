// N64 runtime browser for selecting IWAD before DOOM startup.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

#include <libdragon.h>

#include "i_system.h"
#include "i_video.h"
#include "i_wad_browser_n64.h"
#include "n64_debug.h"

#define N64_WAD_MAX_ENTRIES 128
#define N64_WAD_PATH_LEN 256
#define N64_WAD_NAME_LEN 64

#define N64_WAD_VISIBLE_ROWS 8
#define N64_WAD_LIST_X 18
#define N64_WAD_LIST_Y 62
#define N64_WAD_LIST_WIDTH 284
#define N64_WAD_ROW_HEIGHT 12

#define N64_WAD_FONT_ID 1
#define N64_WAD_TEXT_CACHE_SIZE 96
#define N64_WAD_TEXT_CACHE_TEXT_MAX 96
#define N64_WAD_MAX_LOCAL_PLAYERS 4

enum
{
    N64_STYLE_TEXT = 0,
    N64_STYLE_TITLE = 1,
    N64_STYLE_SUBTITLE = 2,
    N64_STYLE_HINT = 3,
    N64_STYLE_SELECTED = 4,
    N64_STYLE_ERROR = 5
};

enum
{
    N64_WAD_COMPAT_OK = 0,
    N64_WAD_COMPAT_NOT_IWAD = 1,
    N64_WAD_COMPAT_CORRUPT = 2,
    N64_WAD_COMPAT_OPEN_FAILED = 3
};

typedef struct n64_wad_entry_s
{
    char path[N64_WAD_PATH_LEN];
    char name[N64_WAD_NAME_LEN];
    int64_t size_bytes;
    int compat;
} n64_wad_entry_t;

static n64_wad_entry_t n64_wad_entries[N64_WAD_MAX_ENTRIES];
static int n64_wad_count;
static int n64_wad_compatible_count;
static char n64_selected_wad[N64_WAD_PATH_LEN] = "rom:/doom.wad";
static char n64_selected_base_iwad[N64_WAD_PATH_LEN] = "rom:/doom.wad";
static int n64_selected_player_count = 1;
static char n64_browser_status_message[N64_WAD_TEXT_CACHE_TEXT_MAX];
static rdpq_font_t* n64_wad_font;
static bool n64_sd_mounted;

typedef struct n64_wad_text_cache_entry_s
{
    int style;
    int width;
    char text[N64_WAD_TEXT_CACHE_TEXT_MAX];
    rdpq_paragraph_t* layout;
    bool in_use;
} n64_wad_text_cache_entry_t;

static n64_wad_text_cache_entry_t n64_text_cache[N64_WAD_TEXT_CACHE_SIZE];
static int n64_text_cache_evict;

static color_t col_bg_dark;
static color_t col_bg_header;
static color_t col_panel;
static color_t col_border;
static color_t col_title;
static color_t col_subtitle;
static color_t col_text;
static color_t col_hint;
static color_t col_selected_bg;
static color_t col_selected_fg;
static color_t col_error;

static int I_IsAlphaUpper(char ch)
{
    return ch >= 'A' && ch <= 'Z';
}

static char I_ToLowerAscii(char ch)
{
    if (I_IsAlphaUpper(ch))
        return (char)(ch + ('a' - 'A'));
    return ch;
}

static int I_StrCaseCmp(const char* left, const char* right)
{
    unsigned char lc;
    unsigned char rc;

    if (!left || !right)
        return (left == right) ? 0 : (left ? 1 : -1);

    for (;;)
    {
        lc = (unsigned char)I_ToLowerAscii(*left++);
        rc = (unsigned char)I_ToLowerAscii(*right++);

        if (lc != rc || !lc || !rc)
            return (int)lc - (int)rc;
    }
}

static const char* I_BaseName(const char* path)
{
    const char* base = path;

    if (!path)
        return "";

    while (*path)
    {
        if (*path == '/' || *path == '\\')
            base = path + 1;
        path++;
    }

    return base;
}

static int I_HasWadExtension(const char* name)
{
    size_t len;

    if (!name)
        return 0;

    len = strlen(name);
    if (len < 4)
        return 0;

    return I_ToLowerAscii(name[len - 4]) == '.'
        && I_ToLowerAscii(name[len - 3]) == 'w'
        && I_ToLowerAscii(name[len - 2]) == 'a'
        && I_ToLowerAscii(name[len - 1]) == 'd';
}

static void I_CopyTruncated(char* dst, size_t dst_size, const char* src)
{
    size_t copy_len;

    if (!dst || !dst_size)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    copy_len = strlen(src);
    if (copy_len >= dst_size)
        copy_len = dst_size - 1;

    if (copy_len > 0)
        memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static joypad_port_t I_FirstConnectedPort(void)
{
    JOYPAD_PORT_FOREACH(port)
    {
        if (joypad_is_connected(port))
            return port;
    }

    return JOYPAD_PORT_1;
}

static int I_GetConnectedControllerCount(void)
{
    int count;

    count = 0;
    JOYPAD_PORT_FOREACH(port)
    {
        if (joypad_is_connected(port))
            count++;
    }

    return count;
}

static void I_ClampSelectedPlayerCount(void)
{
    if (n64_selected_player_count < 1)
        n64_selected_player_count = 1;
    else if (n64_selected_player_count > N64_WAD_MAX_LOCAL_PLAYERS)
        n64_selected_player_count = N64_WAD_MAX_LOCAL_PLAYERS;
}

static int I_PortHasInputEvent(joypad_port_t port)
{
    joypad_buttons_t pressed;
    joypad_inputs_t inputs;

    if (!joypad_is_connected(port))
        return 0;

    pressed = joypad_get_buttons_pressed(port);
    if (pressed.raw)
        return 1;

    inputs = joypad_get_inputs(port);
    return (inputs.stick_x > 48 || inputs.stick_x < -48
        || inputs.stick_y > 48 || inputs.stick_y < -48);
}

static joypad_port_t I_UpdateActivePort(joypad_port_t current_port)
{
    joypad_port_t first_connected = JOYPAD_PORT_1;
    int have_connected = 0;

    if (joypad_is_connected(current_port) && I_PortHasInputEvent(current_port))
        return current_port;

    JOYPAD_PORT_FOREACH(port)
    {
        if (!joypad_is_connected(port))
            continue;

        if (!have_connected)
        {
            first_connected = port;
            have_connected = 1;
        }

        if (I_PortHasInputEvent(port))
            return port;
    }

    if (have_connected)
        return first_connected;

    return JOYPAD_PORT_1;
}

static uint32_t I_ReadLE32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static const char* I_CompatTag(int compat)
{
    switch (compat)
    {
        case N64_WAD_COMPAT_OK:
            return "IWAD";
        case N64_WAD_COMPAT_NOT_IWAD:
            return "PWAD";
        case N64_WAD_COMPAT_CORRUPT:
            return "BAD";
        default:
            return "ERR";
    }
}

static const char* I_CompatMessage(int compat)
{
    switch (compat)
    {
        case N64_WAD_COMPAT_NOT_IWAD:
            return "PWAD: PRESS A TO CHOOSE BASE IWAD";
        case N64_WAD_COMPAT_CORRUPT:
            return "NOT COMPATIBLE: IWAD HEADER OR DIRECTORY INVALID";
        case N64_WAD_COMPAT_OPEN_FAILED:
            return "NOT COMPATIBLE: FAILED TO READ FILE";
        default:
            return "NOT COMPATIBLE";
    }
}

static void I_SetRejectNotice(char* text,
                              size_t text_size,
                              int* ticks,
                              const char* message)
{
    if (!text || !text_size || !ticks)
        return;

    if (!message || !message[0])
        message = "LAUNCH BLOCKED";

    I_CopyTruncated(text, text_size, message);
    *ticks = 90;
}

static int I_ClassifyWadCompatibility(const char* path, int64_t size_bytes)
{
    int fd;
    int bytes_read;
    int is_iwad;
    int is_pwad;
    uint8_t header[12];
    uint32_t numlumps;
    uint32_t infotableofs;
    uint64_t dir_end;
    struct stat st;
    off_t file_end;

    if (!path || !path[0])
        return N64_WAD_COMPAT_OPEN_FAILED;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0)
        return N64_WAD_COMPAT_OPEN_FAILED;

    if (size_bytes < 0)
    {
        if (!fstat(fd, &st) && st.st_size >= 0)
        {
            size_bytes = (int64_t)st.st_size;
        }
        else
        {
            file_end = lseek(fd, 0, SEEK_END);
            if (file_end >= 0)
            {
                size_bytes = (int64_t)file_end;
                lseek(fd, 0, SEEK_SET);
            }
        }
    }

    if (size_bytes >= 0 && size_bytes < (int64_t)sizeof(header))
    {
        close(fd);
        return N64_WAD_COMPAT_CORRUPT;
    }

    bytes_read = (int)read(fd, header, sizeof(header));
    close(fd);

    if (bytes_read != (int)sizeof(header))
        return N64_WAD_COMPAT_OPEN_FAILED;

    is_iwad = (header[0] == 'I' && header[1] == 'W' && header[2] == 'A' && header[3] == 'D');
    is_pwad = (header[0] == 'P' && header[1] == 'W' && header[2] == 'A' && header[3] == 'D');

    if (!is_iwad && !is_pwad)
        return N64_WAD_COMPAT_CORRUPT;

    numlumps = I_ReadLE32(header + 4);
    infotableofs = I_ReadLE32(header + 8);

    if (!numlumps)
        return N64_WAD_COMPAT_CORRUPT;

    if (size_bytes >= 0)
    {
        dir_end = (uint64_t)infotableofs + (uint64_t)numlumps * 16ULL;
        if (dir_end > (uint64_t)size_bytes)
            return N64_WAD_COMPAT_CORRUPT;
    }

    if (is_iwad)
        return N64_WAD_COMPAT_OK;

    return N64_WAD_COMPAT_NOT_IWAD;
}

static int I_WadPriority(const char* name)
{
    if (!I_StrCaseCmp(name, "doom.wad"))
        return 0;
    if (!I_StrCaseCmp(name, "doomu.wad"))
        return 1;
    if (!I_StrCaseCmp(name, "doom2.wad"))
        return 2;
    if (!I_StrCaseCmp(name, "plutonia.wad"))
        return 3;
    if (!I_StrCaseCmp(name, "tnt.wad"))
        return 4;
    if (!I_StrCaseCmp(name, "doom2f.wad"))
        return 5;
    if (!I_StrCaseCmp(name, "doom1.wad"))
        return 6;

    return 20;
}

static int I_CompareWadEntry(const void* lhs_ptr, const void* rhs_ptr)
{
    const n64_wad_entry_t* lhs = (const n64_wad_entry_t*)lhs_ptr;
    const n64_wad_entry_t* rhs = (const n64_wad_entry_t*)rhs_ptr;
    int lhs_priority = I_WadPriority(lhs->name);
    int rhs_priority = I_WadPriority(rhs->name);
    int cmp;

    if (lhs_priority != rhs_priority)
        return lhs_priority - rhs_priority;

    cmp = I_StrCaseCmp(lhs->name, rhs->name);
    if (cmp)
        return cmp;

    return I_StrCaseCmp(lhs->path, rhs->path);
}

static int I_WadScanCallback(const char* fn, dir_t* dir, void* data)
{
    const char* name;
    n64_wad_entry_t* entry;

    (void)data;

    if (!fn || !dir || dir->d_type != DT_REG)
        return DIR_WALK_CONTINUE;

    name = I_BaseName(fn);
    if (!I_HasWadExtension(name))
        return DIR_WALK_CONTINUE;

    if (n64_wad_count >= N64_WAD_MAX_ENTRIES)
        return DIR_WALK_CONTINUE;

    entry = &n64_wad_entries[n64_wad_count];
    I_CopyTruncated(entry->path, sizeof(entry->path), fn);
    I_CopyTruncated(entry->name, sizeof(entry->name), name);
    entry->size_bytes = dir->d_size;
    entry->compat = I_ClassifyWadCompatibility(fn, dir->d_size);

    if (entry->compat == N64_WAD_COMPAT_OK)
        n64_wad_compatible_count++;

    n64_wad_count++;

    return DIR_WALK_CONTINUE;
}

// Mounts the flashcart SD card filesystem under the "sd:/" prefix using
// libdragon's FAT backend. Done lazily/on demand (never at startup) because
// probing SD without a card inserted can crash the USB stack on some carts
// (e.g. 64drive). The mount is kept open for the rest of the session so the
// engine can stream the selected WAD directly from the card.
static bool I_N64TryMountSd(void)
{
    if (n64_sd_mounted)
        return true;

    if (debug_init_sdfs("sd:/", -1))
    {
        n64_sd_mounted = true;
        N64_DEBUGF("WAD browser: SD filesystem mounted (sd:/)\n");
        return true;
    }

    N64_DEBUGF("WAD browser: SD mount failed (no card or unsupported flashcart)\n");
    return false;
}

static int I_HasSdPrefix(const char* path)
{
    return path
        && path[0] == 's'
        && path[1] == 'd'
        && path[2] == ':'
        && path[3] == '/';
}

static int I_CountSdEntries(void)
{
    int i;
    int count = 0;

    for (i = 0; i < n64_wad_count; i++)
    {
        if (I_HasSdPrefix(n64_wad_entries[i].path))
            count++;
    }

    return count;
}

static void I_ScanWadEntries(void)
{
    int i;

    n64_wad_count = 0;
    n64_wad_compatible_count = 0;
    memset(n64_wad_entries, 0, sizeof(n64_wad_entries));

    dir_walk("rom:/", I_WadScanCallback, NULL);

    // dir_walk recurses into every subdirectory, so a single walk from the
    // card root finds .wad files in any folder (no fixed layout required).
    if (n64_sd_mounted)
        dir_walk("sd:/", I_WadScanCallback, NULL);

    if (n64_wad_count > 1)
    {
        qsort(n64_wad_entries,
              n64_wad_count,
              sizeof(n64_wad_entries[0]),
              I_CompareWadEntry);
    }

    N64_DEBUGF("WAD browser: found %d .wad entries (%d compatible)\n",
               n64_wad_count,
               n64_wad_compatible_count);
    for (i = 0; i < n64_wad_count; i++)
    {
        N64_DEBUGF("  [%02d] %s (%lld bytes, %s)\n",
                   i,
                   n64_wad_entries[i].path,
                   (long long)n64_wad_entries[i].size_bytes,
                   I_CompatTag(n64_wad_entries[i].compat));
    }
}

static int I_FindEntryByPath(const char* path)
{
    int i;

    if (!path)
        return -1;

    for (i = 0; i < n64_wad_count; i++)
    {
        if (!I_StrCaseCmp(path, n64_wad_entries[i].path))
            return i;
    }

    return -1;
}

static int I_FindDefaultEntry(void)
{
    int i;

    for (i = 0; i < n64_wad_count; i++)
    {
        if (!I_StrCaseCmp(n64_wad_entries[i].name, "doom.wad")
            && n64_wad_entries[i].compat == N64_WAD_COMPAT_OK)
            return i;
    }

    return -1;
}

static int I_FindFirstCompatibleEntry(void)
{
    int i;

    for (i = 0; i < n64_wad_count; i++)
    {
        if (n64_wad_entries[i].compat == N64_WAD_COMPAT_OK)
            return i;
    }

    return -1;
}

static void I_ClipLabel(char* dst, size_t dst_size, const char* src, size_t max_len)
{
    size_t src_len;

    if (!dst || !dst_size)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    src_len = strlen(src);
    if (src_len <= max_len)
    {
        I_CopyTruncated(dst, dst_size, src);
        return;
    }

    if (max_len < 4)
    {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, max_len - 3);
    strcpy(dst + (max_len - 3), "...");
}

static void I_FillRect(int x, int y, int width, int height, color_t color)
{
    if (width <= 0 || height <= 0)
        return;

    rdpq_set_mode_fill(color);
    rdpq_fill_rectangle(x, y, x + width, y + height);
}

// rdpq_paragraph_build treats '^' as a style escape and '$' as a font escape
// (each expects two following hex digits, and is doubled to render literally).
// UI labels (e.g. "^ MORE ^") and arbitrary SD WAD filenames can contain these
// characters, which would otherwise trip an assertion and crash. Double them so
// they render as literal glyphs.
static void I_EscapeRdpqControls(char* dst, size_t dst_size, const char* src)
{
    size_t di = 0;

    if (!dst || dst_size == 0)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    while (*src)
    {
        char c = *src++;

        if (c == '^' || c == '$')
        {
            if (di + 2 >= dst_size)
                break;
            dst[di++] = c;
            dst[di++] = c;
        }
        else
        {
            if (di + 1 >= dst_size)
                break;
            dst[di++] = c;
        }
    }

    dst[di] = '\0';
}

static void I_DrawTextSafe(int x,
                           int y,
                           int width,
                           int style,
                           const char* text)
{
    rdpq_paragraph_t* layout;
    bool transient_layout;
    float draw_x;
    float draw_y;
    int i;
    int index;
    size_t text_len;
    n64_wad_text_cache_entry_t* entry;
    rdpq_textparms_t parms;
    int nbytes;
    char build_text[2 * N64_WAD_PATH_LEN + 1];

    if (!text || width <= 0)
        return;

    // The cache is keyed on the original text, but the paragraph is built from
    // an escaped copy so rdpq control characters cannot crash the build.
    I_EscapeRdpqControls(build_text, sizeof(build_text), text);

    layout = NULL;
    transient_layout = false;
    text_len = strlen(text);

    if (text_len < N64_WAD_TEXT_CACHE_TEXT_MAX)
    {
        for (i = 0; i < N64_WAD_TEXT_CACHE_SIZE; i++)
        {
            entry = &n64_text_cache[i];
            if (!entry->in_use)
                continue;

            if (entry->style == style
                && entry->width == width
                && !strcmp(entry->text, text))
            {
                layout = entry->layout;
                break;
            }
        }

        if (!layout)
        {
            index = -1;
            for (i = 0; i < N64_WAD_TEXT_CACHE_SIZE; i++)
            {
                if (!n64_text_cache[i].in_use)
                {
                    index = i;
                    break;
                }
            }

            if (index < 0)
            {
                index = n64_text_cache_evict;
                n64_text_cache_evict = (n64_text_cache_evict + 1) % N64_WAD_TEXT_CACHE_SIZE;
            }

            entry = &n64_text_cache[index];
            if (entry->layout)
                rdpq_paragraph_free(entry->layout);

            memset(&parms, 0, sizeof(parms));
            parms.style_id = style;
            parms.width = width;
            parms.align = ALIGN_LEFT;
            parms.valign = VALIGN_TOP;
            parms.wrap = WRAP_ELLIPSES;

            nbytes = (int)strlen(build_text);
            entry->layout = rdpq_paragraph_build(&parms, N64_WAD_FONT_ID, build_text, &nbytes);
            if (!entry->layout)
            {
                entry->in_use = false;
                entry->text[0] = '\0';
                return;
            }

            entry->style = style;
            entry->width = width;
            strcpy(entry->text, text);
            entry->in_use = true;
            layout = entry->layout;
        }
    }
    else
    {
        memset(&parms, 0, sizeof(parms));
        parms.style_id = style;
        parms.width = width;
        parms.align = ALIGN_LEFT;
        parms.valign = VALIGN_TOP;
        parms.wrap = WRAP_ELLIPSES;

        nbytes = (int)strlen(build_text);
        layout = rdpq_paragraph_build(&parms, N64_WAD_FONT_ID, build_text, &nbytes);
        if (!layout)
            return;

        transient_layout = true;
    }

    draw_x = (float)x - layout->bbox.x0;
    draw_y = (float)y - layout->bbox.y0;

    rdpq_paragraph_render(layout, draw_x, draw_y);

    if (transient_layout)
        rdpq_paragraph_free(layout);
}

static void I_InitTextRenderer(void)
{
    int i;

    for (i = 0; i < N64_WAD_TEXT_CACHE_SIZE; i++)
    {
        n64_text_cache[i].style = 0;
        n64_text_cache[i].width = 0;
        n64_text_cache[i].text[0] = '\0';
        n64_text_cache[i].layout = NULL;
        n64_text_cache[i].in_use = false;
    }
    n64_text_cache_evict = 0;

    n64_wad_font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
    assertf(n64_wad_font != NULL, "WAD browser: failed loading builtin font");

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_TEXT,
                    &(rdpq_fontstyle_t){
                        .color = col_text,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_TITLE,
                    &(rdpq_fontstyle_t){
                        .color = col_title,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_SUBTITLE,
                    &(rdpq_fontstyle_t){
                        .color = col_subtitle,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_HINT,
                    &(rdpq_fontstyle_t){
                        .color = col_hint,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_SELECTED,
                    &(rdpq_fontstyle_t){
                        .color = col_selected_fg,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_font_style(n64_wad_font,
                    N64_STYLE_ERROR,
                    &(rdpq_fontstyle_t){
                        .color = col_error,
                        .outline_color = RGBA32(0, 0, 0, 0)
                    });

    rdpq_text_register_font(N64_WAD_FONT_ID, n64_wad_font);
}

static void I_ShutdownTextRenderer(void)
{
    int i;

    if (!n64_wad_font)
        return;

    for (i = 0; i < N64_WAD_TEXT_CACHE_SIZE; i++)
    {
        if (n64_text_cache[i].layout)
        {
            rdpq_paragraph_free(n64_text_cache[i].layout);
            n64_text_cache[i].layout = NULL;
        }

        n64_text_cache[i].in_use = false;
        n64_text_cache[i].text[0] = '\0';
    }

    rdpq_text_unregister_font(N64_WAD_FONT_ID);
    rdpq_font_free(n64_wad_font);
    n64_wad_font = NULL;
}

static void I_InitPalette(void)
{
    col_bg_dark = RGBA32(18, 10, 8, 255);
    col_bg_header = RGBA32(52, 18, 13, 255);
    col_panel = RGBA32(30, 17, 12, 255);
    col_border = RGBA32(114, 61, 24, 255);
    col_title = RGBA32(228, 179, 74, 255);
    col_subtitle = RGBA32(201, 116, 50, 255);
    col_text = RGBA32(240, 224, 180, 255);
    col_hint = RGBA32(177, 148, 101, 255);
    col_selected_bg = RGBA32(128, 45, 15, 255);
    col_selected_fg = RGBA32(255, 231, 168, 255);
    col_error = RGBA32(232, 106, 90, 255);
}

static void I_DrawStaticFrame(void)
{
    I_FillRect(0, 0, 320, 200, col_bg_dark);
    I_FillRect(0, 0, 320, 44, col_bg_header);
    I_FillRect(0, 44, 320, 2, col_border);

    I_FillRect(10, 50, 300, 112, col_panel);
    I_FillRect(10, 50, 300, 1, col_border);
    I_FillRect(10, 161, 300, 1, col_border);
    I_FillRect(10, 50, 1, 112, col_border);
    I_FillRect(309, 50, 1, 112, col_border);

    I_DrawTextSafe(18, 7, 152, N64_STYLE_TITLE, "DOOM N64");
    I_DrawTextSafe(18, 19, 152, N64_STYLE_SUBTITLE, "SELECT WAD");

    I_DrawTextSafe(176, 3, 142, N64_STYLE_HINT, "D-PAD: MOVE");
    I_DrawTextSafe(176, 11, 142, N64_STYLE_HINT, "C-UP/DOWN: PAGE");
    I_DrawTextSafe(176, 19, 142, N64_STYLE_HINT, "A/START: SELECT");
    I_DrawTextSafe(176, 27, 142, N64_STYLE_HINT, "B: QUICK IWAD");
    I_DrawTextSafe(176, 35, 142, N64_STYLE_HINT, "L/R: PLAYERS");
}

static void I_DrawEntryRow(int row, int entry_index, int selected)
{
    char label[32];
    char size_label[16];
    const char* compat_tag;
    int x;
    int y;
    int style;
    n64_wad_entry_t* entry;
    unsigned long size_kib;

    x = N64_WAD_LIST_X;
    y = N64_WAD_LIST_Y + row * N64_WAD_ROW_HEIGHT;
    entry = &n64_wad_entries[entry_index];

    if (selected)
        I_FillRect(x - 2, y - 1, N64_WAD_LIST_WIDTH, N64_WAD_ROW_HEIGHT - 1, col_selected_bg);

    I_ClipLabel(label, sizeof(label), entry->name, 22);

    style = selected ? N64_STYLE_SELECTED : N64_STYLE_TEXT;
    I_DrawTextSafe(x, y, 222, style, label);

    if (entry->compat == N64_WAD_COMPAT_OK && entry->size_bytes >= 0)
    {
        size_kib = (unsigned long)((entry->size_bytes + 1023) / 1024);
        snprintf(size_label, sizeof(size_label), "%5luK", size_kib);
        I_DrawTextSafe(248, y, 62, style, size_label);
    }
    else
    {
        int compat_style;

        compat_tag = I_CompatTag(entry->compat);
        if (entry->compat == N64_WAD_COMPAT_NOT_IWAD)
            compat_style = selected ? N64_STYLE_SELECTED : N64_STYLE_HINT;
        else
            compat_style = N64_STYLE_ERROR;

        I_DrawTextSafe(248, y, 62, compat_style, compat_tag);
    }
}

static void I_DrawBrowserFrame(int cursor,
                               int page,
                               int reject_notice_ticks,
                               const char* reject_notice_text)
{
    char footer[64];
    char player_label[48];
    int i;
    int index;
    int connected_count;
    n64_wad_entry_t* selected_entry;

    I_DrawStaticFrame();

    connected_count = I_GetConnectedControllerCount();
    snprintf(player_label,
             sizeof(player_label),
             "PLAYERS: %d/%d",
             n64_selected_player_count,
             connected_count ? connected_count : 1);
    I_DrawTextSafe(18, 33, 152, N64_STYLE_HINT, player_label);

    selected_entry = &n64_wad_entries[cursor];

    if (reject_notice_ticks > 0)
    {
        I_DrawTextSafe(16,
                       46,
                       304,
                       N64_STYLE_ERROR,
                       (reject_notice_text && reject_notice_text[0])
                           ? reject_notice_text
                           : "SELECTED FILE IS NOT A COMPATIBLE IWAD");
    }

    for (i = 0; i < N64_WAD_VISIBLE_ROWS; i++)
    {
        index = page + i;
        if (index >= n64_wad_count)
            break;
        I_DrawEntryRow(i, index, index == cursor);
    }

    if (page > 0)
    {
        I_DrawTextSafe(132, 52, 120, N64_STYLE_HINT, "^ MORE ^");
    }

    if ((page + N64_WAD_VISIBLE_ROWS) < n64_wad_count)
    {
        I_DrawTextSafe(132, 152, 120, N64_STYLE_HINT, "v MORE v");
    }

    snprintf(footer,
             sizeof(footer),
             "WADS:%d  IWAD:%d  [Z]%s",
             n64_wad_count,
             n64_wad_compatible_count,
             n64_sd_mounted ? "RESCAN SD" : "SCAN SD");
    I_DrawTextSafe(16, 170, 304, N64_STYLE_HINT, footer);

    if (selected_entry->compat == N64_WAD_COMPAT_OK)
    {
        I_DrawTextSafe(16, 182, 304, N64_STYLE_TEXT, selected_entry->path);
    }
    else if (selected_entry->compat == N64_WAD_COMPAT_NOT_IWAD)
    {
        I_DrawTextSafe(16,
                       182,
                       304,
                       N64_STYLE_HINT,
                       I_CompatMessage(selected_entry->compat));
    }
    else
    {
        I_DrawTextSafe(16,
                       182,
                       304,
                       N64_STYLE_ERROR,
                       I_CompatMessage(selected_entry->compat));
    }
}

static void I_DrawFallbackFrame(void)
{
    I_DrawStaticFrame();

    I_DrawTextSafe(18, 70, 280, N64_STYLE_TEXT, "NO .WAD FILES FOUND IN ROM FS");
    I_DrawTextSafe(18, 84, 280, N64_STYLE_HINT, "ADD IWADS UNDER filesystem/");
    I_DrawTextSafe(18, 94, 280, N64_STYLE_HINT, "AND REBUILD ROM IMAGE");

    I_DrawTextSafe(18, 112, 280, N64_STYLE_TEXT, "PRESS A OR START TO CONTINUE");
    I_DrawTextSafe(18, 124, 280, N64_STYLE_HINT, "FALLBACK: rom:/doom.wad");

    I_DrawTextSafe(18, 140, 280, N64_STYLE_TITLE, "PRESS Z TO SCAN SD CARD");
    I_DrawTextSafe(18, 152, 280, N64_STYLE_HINT, "FINDS .WAD FILES IN ANY SD FOLDER");

    I_DrawTextSafe(16, 182, 304, N64_STYLE_HINT, "B ALSO CONTINUES WITH FALLBACK");
}

static void I_DrawScanningFrame(const char* message)
{
    surface_t* disp;

    disp = display_get();
    rdpq_attach_clear(disp, NULL);
    I_DrawStaticFrame();
    I_DrawTextSafe(18, 84, 280, N64_STYLE_TITLE, "SCANNING SD CARD...");
    I_DrawTextSafe(18, 100, 280, N64_STYLE_HINT,
                   (message && message[0]) ? message : "READING FILESYSTEM, PLEASE WAIT");
    rdpq_detach_show();
}

static void I_DrawLoadingFrame(void)
{
    surface_t* disp;
    char wad_label[96];
    char base_label[96];
    char wad_name_clipped[56];
    char base_name_clipped[56];
    const char* wad_name;
    const char* base_name;

    wad_name = I_BaseName(n64_selected_wad);
    base_name = I_BaseName(n64_selected_base_iwad);

    I_ClipLabel(wad_name_clipped,
                sizeof(wad_name_clipped),
                (wad_name && wad_name[0]) ? wad_name : n64_selected_wad,
                48);

    I_ClipLabel(base_name_clipped,
                sizeof(base_name_clipped),
                (base_name && base_name[0]) ? base_name : n64_selected_base_iwad,
                48);

    snprintf(wad_label,
             sizeof(wad_label),
             "WAD: %s",
             wad_name_clipped);

    snprintf(base_label,
             sizeof(base_label),
             "BASE: %s",
             base_name_clipped);

    disp = display_get();
    rdpq_attach_clear(disp, NULL);
    I_DrawStaticFrame();
    I_DrawTextSafe(18, 82, 280, N64_STYLE_TITLE, "LOADING WAD...");
    I_DrawTextSafe(18, 96, 280, N64_STYLE_HINT, "PLEASE WAIT");
    I_DrawTextSafe(18, 114, 280, N64_STYLE_TEXT, wad_label);

    if (I_StrCaseCmp(n64_selected_wad, n64_selected_base_iwad))
        I_DrawTextSafe(18, 126, 280, N64_STYLE_HINT, base_label);

    I_DrawTextSafe(18, 182, 280, N64_STYLE_HINT, "STARTUP IN PROGRESS");
    rdpq_detach_show();
}

static void I_ClampCursorAndPageForCount(int* cursor, int* page, int count)
{
    int max_page;

    if (!cursor || !page)
        return;

    if (count <= 0)
    {
        *cursor = 0;
        *page = 0;
        return;
    }

    if (*cursor < 0)
        *cursor = 0;
    if (*cursor >= count)
        *cursor = count - 1;

    if (*page > *cursor)
        *page = *cursor;

    if (*cursor >= (*page + N64_WAD_VISIBLE_ROWS))
        *page = *cursor - N64_WAD_VISIBLE_ROWS + 1;

    max_page = count - N64_WAD_VISIBLE_ROWS;
    if (max_page < 0)
        max_page = 0;
    if (*page > max_page)
        *page = max_page;

    if (*page < 0)
        *page = 0;
}

static void I_ClampCursorAndPage(int* cursor, int* page)
{
    I_ClampCursorAndPageForCount(cursor, page, n64_wad_count);
}

static int I_BuildIwadIndexList(int* out_indices, int max_entries)
{
    int i;
    int count;

    if (!out_indices || max_entries <= 0)
        return 0;

    count = 0;
    for (i = 0; i < n64_wad_count; i++)
    {
        if (n64_wad_entries[i].compat != N64_WAD_COMPAT_OK)
            continue;

        if (count < max_entries)
            out_indices[count] = i;

        count++;
    }

    if (count > max_entries)
        count = max_entries;

    return count;
}

static void I_DrawIwadPickerFrame(const char* pwad_name,
                                  const int* iwad_indices,
                                  int iwad_count,
                                  int cursor,
                                  int page)
{
    char footer[64];
    char pwad_label[48];
    int i;
    int index;
    int entry_index;
    n64_wad_entry_t* selected_iwad;

    if (!iwad_indices || iwad_count <= 0)
        return;

    I_DrawStaticFrame();
    I_DrawTextSafe(18, 23, 152, N64_STYLE_SUBTITLE, "SELECT BASE IWAD");
    I_DrawTextSafe(176, 36, 142, N64_STYLE_HINT, "B: CANCEL");

    if (pwad_name && pwad_name[0])
    {
        I_ClipLabel(pwad_label, sizeof(pwad_label), pwad_name, 34);
        I_DrawTextSafe(16, 55, 304, N64_STYLE_HINT, pwad_label);
    }

    for (i = 0; i < N64_WAD_VISIBLE_ROWS; i++)
    {
        index = page + i;
        if (index >= iwad_count)
            break;

        entry_index = iwad_indices[index];
        I_DrawEntryRow(i, entry_index, index == cursor);
    }

    if (page > 0)
        I_DrawTextSafe(132, 64, 120, N64_STYLE_HINT, "^ MORE ^");

    if ((page + N64_WAD_VISIBLE_ROWS) < iwad_count)
        I_DrawTextSafe(132, 193, 120, N64_STYLE_HINT, "v MORE v");

    snprintf(footer, sizeof(footer), "BASE IWADS: %d", iwad_count);
    I_DrawTextSafe(16, 210, 304, N64_STYLE_HINT, footer);

    selected_iwad = &n64_wad_entries[iwad_indices[cursor]];
    I_DrawTextSafe(16, 221, 304, N64_STYLE_TEXT, selected_iwad->path);
}

static int I_RunIwadPickerLoop(int pwad_index)
{
    int iwad_indices[N64_WAD_MAX_ENTRIES];
    int iwad_count;
    joypad_buttons_t pressed;
    joypad_buttons_t held;
    joypad_inputs_t inputs;
    surface_t* disp;
    int cursor;
    int page;
    int move;
    int preferred_iwad;
    int i;
    bool stick_up_latch;
    bool stick_down_latch;
    bool input_armed;
    int input_arm_timeout;
    joypad_port_t active_port;
    joypad_port_t next_port;

    iwad_count = I_BuildIwadIndexList(iwad_indices, N64_WAD_MAX_ENTRIES);
    if (iwad_count <= 0)
        return -1;

    preferred_iwad = I_FindEntryByPath(n64_selected_base_iwad);
    if (preferred_iwad < 0)
        preferred_iwad = I_FindDefaultEntry();
    if (preferred_iwad < 0)
        preferred_iwad = I_FindFirstCompatibleEntry();

    cursor = 0;
    if (preferred_iwad >= 0)
    {
        for (i = 0; i < iwad_count; i++)
        {
            if (iwad_indices[i] == preferred_iwad)
            {
                cursor = i;
                break;
            }
        }
    }

    page = 0;
    stick_up_latch = false;
    stick_down_latch = false;
    input_armed = false;
    input_arm_timeout = 30;
    active_port = I_FirstConnectedPort();

    I_ClampCursorAndPageForCount(&cursor, &page, iwad_count);

    for (;;)
    {
        joypad_poll();
        next_port = I_UpdateActivePort(active_port);
        if (next_port != active_port)
        {
            active_port = next_port;
            N64_DEBUGF("WAD browser: IWAD picker active port=%d\n",
                       (int)active_port + 1);
        }

        pressed = joypad_get_buttons_pressed(active_port);
        held = joypad_get_buttons_held(active_port);
        inputs = joypad_get_inputs(active_port);
        move = 0;

        if (!input_armed)
        {
            if (!held.raw)
            {
                input_armed = true;
            }
            else if (input_arm_timeout > 0)
            {
                input_arm_timeout--;
                if (!input_arm_timeout)
                {
                    input_armed = true;
                    N64_DEBUGF("WAD browser: IWAD picker input arm timeout fallback (held=0x%08x)\n",
                               (unsigned int)held.raw);
                }
            }
        }

        if (pressed.d_up)
            move = -1;
        else if (pressed.d_down)
            move = 1;
        else if (pressed.d_left)
            move = -N64_WAD_VISIBLE_ROWS;
        else if (pressed.d_right)
            move = N64_WAD_VISIBLE_ROWS;
        else if (pressed.c_up)
            move = -N64_WAD_VISIBLE_ROWS;
        else if (pressed.c_down)
            move = N64_WAD_VISIBLE_ROWS;

        if (inputs.stick_y > 48)
        {
            if (!stick_up_latch)
                move = -1;
            stick_up_latch = true;
        }
        else
        {
            stick_up_latch = false;
        }

        if (inputs.stick_y < -48)
        {
            if (!stick_down_latch)
                move = 1;
            stick_down_latch = true;
        }
        else
        {
            stick_down_latch = false;
        }

        if (move)
        {
            cursor += move;
            I_ClampCursorAndPageForCount(&cursor, &page, iwad_count);
        }

        if (input_armed && (pressed.a || pressed.start))
            return iwad_indices[cursor];

        if (input_armed && pressed.b)
            return -1;

        disp = display_get();
        rdpq_attach_clear(disp, NULL);
        I_DrawIwadPickerFrame(n64_wad_entries[pwad_index].name,
                              iwad_indices,
                              iwad_count,
                              cursor,
                              page);
        rdpq_detach_show();
    }
}

// User-initiated SD scan from the main selection screen. Preserves the current
// cursor selection by path across the re-scan so the highlight does not jump.
static void I_HandleSdScanRequest(int* cursor,
                                  int* page,
                                  char* notice,
                                  size_t notice_size,
                                  int* notice_ticks)
{
    char prev_path[N64_WAD_PATH_LEN];
    char msg[N64_WAD_TEXT_CACHE_TEXT_MAX];
    int sd_found;
    int restored;

    if (cursor && *cursor >= 0 && *cursor < n64_wad_count)
        I_CopyTruncated(prev_path, sizeof(prev_path), n64_wad_entries[*cursor].path);
    else
        prev_path[0] = '\0';

    I_DrawScanningFrame("MOUNTING SD / SEARCHING ALL FOLDERS");

    if (!I_N64TryMountSd())
    {
        I_SetRejectNotice(notice, notice_size, notice_ticks,
                          "SD NOT DETECTED (NEEDS FLASHCART + CARD)");
        return;
    }

    I_ScanWadEntries();
    sd_found = I_CountSdEntries();

    restored = I_FindEntryByPath(prev_path);
    if (cursor && restored >= 0)
        *cursor = restored;
    I_ClampCursorAndPage(cursor, page);

    snprintf(msg, sizeof(msg), "SD SCAN: %d WAD%s FOUND",
             sd_found, (sd_found == 1) ? "" : "S");
    I_SetRejectNotice(notice, notice_size, notice_ticks, msg);

    N64_DEBUGF("WAD browser: SD scan done sd_wads=%d total=%d\n",
               sd_found, n64_wad_count);
}

static int I_RunSelectionLoop(int* out_base_iwad_index)
{
    joypad_buttons_t pressed;
    joypad_buttons_t held;
    joypad_inputs_t inputs;
    surface_t* disp;
    int cursor;
    int page;
    int move;
    int default_entry;
    int selected_base_iwad;
    bool done;
    bool used_default;
    bool stick_up_latch;
    bool stick_down_latch;
    bool input_armed;
    int reject_notice_ticks;
    char reject_notice_text[N64_WAD_TEXT_CACHE_TEXT_MAX];
    int input_arm_timeout;
    joypad_port_t active_port;
    joypad_port_t next_port;

    if (!n64_wad_count)
        return -1;

    cursor = I_FindEntryByPath(n64_selected_wad);
    if (cursor < 0)
        cursor = 0;
    page = 0;
    stick_up_latch = false;
    stick_down_latch = false;
    done = false;
    used_default = false;
    input_armed = false;
    input_arm_timeout = 30;
    reject_notice_ticks = 0;
    reject_notice_text[0] = '\0';
    selected_base_iwad = -1;
    active_port = I_FirstConnectedPort();

    if (n64_browser_status_message[0])
    {
        I_SetRejectNotice(reject_notice_text,
                          sizeof(reject_notice_text),
                          &reject_notice_ticks,
                          n64_browser_status_message);
        n64_browser_status_message[0] = '\0';
    }

    while (!done)
    {
        joypad_poll();
        next_port = I_UpdateActivePort(active_port);
        if (next_port != active_port)
        {
            active_port = next_port;
            N64_DEBUGF("WAD browser: selection active port=%d\n",
                       (int)active_port + 1);
        }

        pressed = joypad_get_buttons_pressed(active_port);
        held = joypad_get_buttons_held(active_port);
        inputs = joypad_get_inputs(active_port);
        move = 0;

        if (!input_armed)
        {
            if (!held.raw)
            {
                input_armed = true;
            }
            else if (input_arm_timeout > 0)
            {
                input_arm_timeout--;
                if (!input_arm_timeout)
                {
                    input_armed = true;
                    N64_DEBUGF("WAD browser: selection input arm timeout fallback (held=0x%08x)\n",
                               (unsigned int)held.raw);
                }
            }
        }

        if (!input_armed && (pressed.a || pressed.start || pressed.b))
        {
            I_SetRejectNotice(reject_notice_text,
                              sizeof(reject_notice_text),
                              &reject_notice_ticks,
                              "INPUT LOCK: RELEASE HELD BUTTONS");
            N64_DEBUGF("WAD browser: selection input ignored while unarmed (pressed=0x%08x held=0x%08x)\n",
                       (unsigned int)pressed.raw,
                       (unsigned int)held.raw);
        }

        if (pressed.d_up)
            move = -1;
        else if (pressed.d_down)
            move = 1;
        else if (pressed.d_left)
            move = -N64_WAD_VISIBLE_ROWS;
        else if (pressed.d_right)
            move = N64_WAD_VISIBLE_ROWS;
        else if (pressed.c_up)
            move = -N64_WAD_VISIBLE_ROWS;
        else if (pressed.c_down)
            move = N64_WAD_VISIBLE_ROWS;

        if (inputs.stick_y > 48)
        {
            if (!stick_up_latch)
                move = -1;
            stick_up_latch = true;
        }
        else
        {
            stick_up_latch = false;
        }

        if (inputs.stick_y < -48)
        {
            if (!stick_down_latch)
                move = 1;
            stick_down_latch = true;
        }
        else
        {
            stick_down_latch = false;
        }

        if (move)
        {
            cursor += move;
            I_ClampCursorAndPage(&cursor, &page);
        }

        if (input_armed && pressed.l)
        {
            if (n64_selected_player_count > 1)
                n64_selected_player_count--;
            else
                I_SetRejectNotice(reject_notice_text,
                                  sizeof(reject_notice_text),
                                  &reject_notice_ticks,
                                  "PLAYERS: MINIMUM IS 1");
        }

        if (input_armed && pressed.r)
        {
            if (n64_selected_player_count < N64_WAD_MAX_LOCAL_PLAYERS)
                n64_selected_player_count++;
            else
                I_SetRejectNotice(reject_notice_text,
                                  sizeof(reject_notice_text),
                                  &reject_notice_ticks,
                                  "PLAYERS: MAXIMUM IS 4");
        }

        if (input_armed && pressed.z)
        {
            I_HandleSdScanRequest(&cursor,
                                  &page,
                                  reject_notice_text,
                                  sizeof(reject_notice_text),
                                  &reject_notice_ticks);
        }

        if (input_armed && (pressed.a || pressed.start))
        {
            if (n64_wad_entries[cursor].compat == N64_WAD_COMPAT_OK)
            {
                selected_base_iwad = cursor;
                done = true;
            }
            else if (n64_wad_entries[cursor].compat == N64_WAD_COMPAT_NOT_IWAD)
            {
                if (I_FindFirstCompatibleEntry() < 0)
                {
                    I_SetRejectNotice(reject_notice_text,
                                      sizeof(reject_notice_text),
                                      &reject_notice_ticks,
                                      "PWAD BLOCKED: NO COMPATIBLE BASE IWAD FOUND");
                    N64_DEBUGF("WAD browser: PWAD selection blocked (no compatible base IWAD) index=%d path=%s\n",
                               cursor,
                               n64_wad_entries[cursor].path);
                }
                else
                {
                    selected_base_iwad = I_RunIwadPickerLoop(cursor);
                    if (selected_base_iwad >= 0)
                    {
                        done = true;
                    }
                    else
                    {
                        I_SetRejectNotice(reject_notice_text,
                                          sizeof(reject_notice_text),
                                          &reject_notice_ticks,
                                          "PWAD CANCELED: BASE IWAD NOT SELECTED");
                        N64_DEBUGF("WAD browser: PWAD base IWAD selection canceled index=%d path=%s\n",
                                   cursor,
                                   n64_wad_entries[cursor].path);
                    }
                }
            }
            else
            {
                I_SetRejectNotice(reject_notice_text,
                                  sizeof(reject_notice_text),
                                  &reject_notice_ticks,
                                  I_CompatMessage(n64_wad_entries[cursor].compat));
                N64_DEBUGF("WAD browser: rejected incompatible selection index=%d path=%s (%s)\n",
                           cursor,
                           n64_wad_entries[cursor].path,
                           I_CompatTag(n64_wad_entries[cursor].compat));
            }
        }

        if (input_armed && pressed.b)
        {
            default_entry = I_FindDefaultEntry();
            if (default_entry < 0)
                default_entry = I_FindFirstCompatibleEntry();

            if (default_entry >= 0)
            {
                cursor = default_entry;
                selected_base_iwad = default_entry;
                used_default = true;
                done = true;
            }
            else
            {
                I_SetRejectNotice(reject_notice_text,
                                  sizeof(reject_notice_text),
                                  &reject_notice_ticks,
                                  "QUICK IWAD FAILED: NO COMPATIBLE IWAD FOUND");
                N64_DEBUGF("WAD browser: no compatible IWAD available for default selection\n");
            }
        }

        disp = display_get();
        rdpq_attach_clear(disp, NULL);
        I_DrawBrowserFrame(cursor, page, reject_notice_ticks, reject_notice_text);
        rdpq_detach_show();

        if (reject_notice_ticks > 0)
            reject_notice_ticks--;
    }

    if (selected_base_iwad < 0)
        selected_base_iwad = I_FindFirstCompatibleEntry();

    if (out_base_iwad_index)
        *out_base_iwad_index = selected_base_iwad;

    if (!used_default)
    {
        if (n64_wad_entries[cursor].compat == N64_WAD_COMPAT_NOT_IWAD
            && selected_base_iwad >= 0)
        {
            N64_DEBUGF("WAD browser: selected PWAD index=%d path=%s base_iwad=%s\n",
                       cursor,
                       n64_wad_entries[cursor].path,
                       n64_wad_entries[selected_base_iwad].path);
        }
        else
        {
            N64_DEBUGF("WAD browser: selected index=%d path=%s\n",
                       cursor,
                       n64_wad_entries[cursor].path);
        }
        return cursor;
    }

    N64_DEBUGF("WAD browser: using quick IWAD index=%d path=%s\n",
               selected_base_iwad,
               n64_wad_entries[selected_base_iwad].path);
    return cursor;
}

static int I_RunFallbackLoop(void)
{
    joypad_buttons_t pressed;
    joypad_buttons_t held;
    surface_t* disp;
    bool input_armed;
    int input_arm_timeout;
    int notice_ticks;
    char notice[N64_WAD_TEXT_CACHE_TEXT_MAX];
    joypad_port_t active_port;
    joypad_port_t next_port;

    input_armed = false;
    input_arm_timeout = 30;
    notice_ticks = 0;
    notice[0] = '\0';
    active_port = I_FirstConnectedPort();

    for (;;)
    {
        joypad_poll();
        next_port = I_UpdateActivePort(active_port);
        if (next_port != active_port)
        {
            active_port = next_port;
            N64_DEBUGF("WAD browser: fallback active port=%d\n",
                       (int)active_port + 1);
        }

        pressed = joypad_get_buttons_pressed(active_port);
        held = joypad_get_buttons_held(active_port);

        if (!input_armed)
        {
            if (!held.raw)
            {
                input_armed = true;
            }
            else if (input_arm_timeout > 0)
            {
                input_arm_timeout--;
                if (!input_arm_timeout)
                    input_armed = true;
            }
        }

        if (input_armed && pressed.z)
        {
            I_DrawScanningFrame("MOUNTING SD / SEARCHING ALL FOLDERS");
            if (I_N64TryMountSd())
            {
                I_ScanWadEntries();
                if (n64_wad_count > 0)
                {
                    N64_DEBUGF("WAD browser: SD scan from fallback found %d entries\n",
                               n64_wad_count);
                    return 1;
                }

                I_CopyTruncated(notice, sizeof(notice), "SD SCAN: NO WAD FILES FOUND");
                notice_ticks = 120;
            }
            else
            {
                I_CopyTruncated(notice, sizeof(notice),
                                "SD NOT DETECTED (NEEDS FLASHCART + CARD)");
                notice_ticks = 120;
            }
        }

        disp = display_get();
        rdpq_attach_clear(disp, NULL);
        I_DrawFallbackFrame();
        if (notice_ticks > 0)
        {
            I_DrawTextSafe(16, 168, 304, N64_STYLE_ERROR, notice);
            notice_ticks--;
        }
        rdpq_detach_show();

        if (input_armed && (pressed.a || pressed.start || pressed.b))
        {
            N64_DEBUGF("WAD browser: fallback continue with path=%s\n", n64_selected_wad);
            return 0;
        }
    }
}

void I_N64RunWadBrowser(void)
{
    int selected_index;
    int selected_base_iwad_index;
    uint32_t existing_buffers;

    I_ScanWadEntries();
    I_ClampSelectedPlayerCount();
    N64_DEBUGF("WAD browser: entering UI with %d entries\n", n64_wad_count);
    I_N64LogMemoryStats("wad_browser:enter");

    existing_buffers = display_get_num_buffers();
    N64_DEBUGF("WAD browser: display_get_num_buffers()=%u\n", (unsigned)existing_buffers);
    if (existing_buffers == 0)
    {
        N64_DEBUGF("WAD browser: calling display_init (fresh)\n");
        display_init(N64_DISPLAY_RESOLUTION, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    }
    else
    {
        N64_DEBUGF("WAD browser: reusing existing display\n");
    }

    display_set_fps_limit(60.0f);
    rdpq_init();
    I_InitPalette();
    I_InitTextRenderer();

    if (!n64_wad_count)
    {
        if (!I_RunFallbackLoop())
        {
            I_CopyTruncated(n64_selected_base_iwad,
                            sizeof(n64_selected_base_iwad),
                            n64_selected_wad);
            N64_DEBUGF("WAD browser: no entries, selected path=%s\n", n64_selected_wad);
            I_DrawLoadingFrame();
            I_N64LogMemoryStats("wad_browser:before_exit");
            rspq_wait();
            I_ShutdownTextRenderer();
            rdpq_close();
            I_N64LogMemoryStats("wad_browser:after_exit");
            return;
        }

        N64_DEBUGF("WAD browser: SD scan provided %d entries, entering selection\n",
                   n64_wad_count);
    }

    selected_base_iwad_index = -1;
    selected_index = I_RunSelectionLoop(&selected_base_iwad_index);
    if (selected_index >= 0 && selected_index < n64_wad_count)
    {
        I_CopyTruncated(n64_selected_wad,
                        sizeof(n64_selected_wad),
                        n64_wad_entries[selected_index].path);

        if (n64_wad_entries[selected_index].compat == N64_WAD_COMPAT_NOT_IWAD)
        {
            if (selected_base_iwad_index < 0 || selected_base_iwad_index >= n64_wad_count)
                selected_base_iwad_index = I_FindFirstCompatibleEntry();

            if (selected_base_iwad_index >= 0)
            {
                I_CopyTruncated(n64_selected_base_iwad,
                                sizeof(n64_selected_base_iwad),
                                n64_wad_entries[selected_base_iwad_index].path);
            }
            else
            {
                I_CopyTruncated(n64_selected_base_iwad,
                                sizeof(n64_selected_base_iwad),
                                "rom:/doom.wad");
            }
        }
        else
        {
            I_CopyTruncated(n64_selected_base_iwad,
                            sizeof(n64_selected_base_iwad),
                            n64_selected_wad);
        }
    }

    N64_DEBUGF("WAD browser: final selected path=%s base_iwad=%s\n",
               n64_selected_wad,
               n64_selected_base_iwad);

    I_DrawLoadingFrame();
    I_N64LogMemoryStats("wad_browser:before_exit");
    rspq_wait();
    I_ShutdownTextRenderer();
    rdpq_close();
    I_N64LogMemoryStats("wad_browser:after_exit");
}

#ifdef N64_BENCH
void I_N64ForceSelectedWad(const char* path)
{
    I_CopyTruncated(n64_selected_wad, sizeof(n64_selected_wad), path);
    I_CopyTruncated(n64_selected_base_iwad, sizeof(n64_selected_base_iwad), path);
}
#endif

const char* I_N64GetSelectedWadPath(void)
{
    return n64_selected_wad;
}

const char* I_N64GetSelectedBaseIwadPath(void)
{
    return n64_selected_base_iwad;
}

int I_N64GetSelectedPlayerCount(void)
{
    I_ClampSelectedPlayerCount();
    return n64_selected_player_count;
}

void I_N64SetBrowserStatusMessage(const char* message)
{
    I_CopyTruncated(n64_browser_status_message,
                    sizeof(n64_browser_status_message),
                    message ? message : "");

    if (n64_browser_status_message[0])
    {
        N64_DEBUGF("WAD browser: status message set: %s\n",
                   n64_browser_status_message);
    }
}
