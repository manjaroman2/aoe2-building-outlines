#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)                                                                                           \
    ((byte) & 0x80 ? '1' : '0'), ((byte) & 0x40 ? '1' : '0'), ((byte) & 0x20 ? '1' : '0'),                             \
        ((byte) & 0x10 ? '1' : '0'), ((byte) & 0x08 ? '1' : '0'), ((byte) & 0x04 ? '1' : '0'),                         \
        ((byte) & 0x02 ? '1' : '0'), ((byte) & 0x01 ? '1' : '0')
#define MAX_COMMANDS 1024

#ifdef DEBUG
#define DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...) ((void)0)
#endif

typedef struct
{
    char signature[4];
    uint16_t version;
    uint16_t frame_count;
    uint16_t unknown1;
    uint16_t unknown2;
    uint32_t unknown3;
} sld_header_t;

typedef struct
{
    uint16_t canvas_width;
    uint16_t canvas_height;
    int16_t canvas_hotspot_x;
    int16_t canvas_hotspot_y;
    char frame_type;
    char unknown5;
    uint16_t frame_index;
} sld_frame_header_t;

typedef struct
{
    uint16_t offset_x1;
    uint16_t offset_y1;
    uint16_t offset_x2;
    uint16_t offset_y2;
    uint8_t flag1;
    uint8_t unknown1;
} sld_main_header_t, sld_shadow_header_t;

typedef struct
{
    uint8_t flag1;
    uint8_t unknown1;
} sld_damage_mask_header_t, sld_playercolor_mask_header_t;

typedef struct
{
    uint8_t skipped;
    uint8_t drawn;
} command_t;

typedef struct
{
    uint16_t color0;
    uint16_t color1;
    uint32_t pix_indices;
} bc1_t;

typedef struct
{
    uint8_t color0;
    uint8_t color1;
    uint8_t pix_indices[6];
} bc4_t;

typedef void (*dxt1_pixel_writer_t)(uint8_t *pixel, uint16_t c, int transparent);
typedef void (*dxt4_pixel_writer_t)(uint8_t *pixel, uint8_t c);
typedef void (*decode_method_t)(const void *block, int block_x, int block_y, uint8_t *canvas, uint16_t offset_x1,
                                uint16_t offset_y1, uint16_t canvas_width, void *pixel_writer);

typedef struct
{
    long read_start;
    uint32_t content_length;
    int actual_length;
    uint16_t command_array_length;
    command_t commands[MAX_COMMANDS];
} sld_layer_t;

typedef struct
{
    long start;
    int actual_length;
    int present;
} layer_slice_t;

typedef struct
{
    int present;
    long layer_start;
    int actual_length;
    int width;
    int height;
    sld_main_header_t header;
    uint16_t command_array_length;
    command_t commands[MAX_COMMANDS];
} main_layer_info_t;

typedef struct
{
    int present;
    long layer_start;
    int actual_length;
    int width;
    int height;
    uint16_t offset_x1;
    uint16_t offset_y1;
    size_t header_size;
    uint16_t command_array_length;
    command_t commands[MAX_COMMANDS];
} dxt1_layer_info_t;

typedef struct
{
    int present;
    long layer_start;
    int actual_length;
    int width;
    int height;
    uint16_t offset_x1;
    uint16_t offset_y1;
    size_t header_size;
    uint16_t command_array_length;
    command_t commands[MAX_COMMANDS];
} bc4_layer_info_t;

typedef struct
{
    int x1;
    int y1;
    int x2;
    int y2;
} layer_rect_t;

typedef struct
{
    command_t *commands;
    int command_count;
    void *blocks;
    int block_count;
    size_t block_size;
    uint32_t content_length;
    int actual_length;
} rebuilt_layer_t;

typedef uint8_t (*bc4_value_reader_t)(const uint8_t *pixel);

typedef struct
{
    const char *name;
    int tiles_u;
    int tiles_v;
} building_footprint_t;

static const int TILE_HALF_HEIGHT_X1 = 24;
static const int TILE_HALF_HEIGHT_X2 = 48;
static const int TILE_WIDTH_X1 = 96;
static const int TILE_WIDTH_X2 = 192;

static const building_footprint_t BUILDING_FOOTPRINTS[] = {
    {"archery_range", 3, 3}, {"lumber_camp", 2, 2},   {"mining_camp", 2, 2}, {"siege_workshop", 4, 4},
    {"trade_workshop", 4, 4}, {"town_center", 4, 4},  {"caravanserai", 4, 4}, {"blacksmith", 3, 3},
    {"monastery", 3, 3},      {"settlement", 3, 3},   {"university", 4, 4},   {"shipyard", 3, 3},
    {"outpost", 1, 1},        {"barracks", 3, 3},     {"stable", 3, 3},       {"market", 4, 4},
    {"castle", 4, 4},         {"wonder", 5, 5},       {"house", 2, 2},        {"tower", 1, 1},
    {"mill", 2, 2},           {"donjon", 2, 2},       {"folwark", 3, 3},      {"dock", 3, 3},
    {"krepost", 3, 3},        {"mule_cart", 1, 1},    {"wall", 1, 1},
};

int write_bytes(FILE *f, const void *data, size_t size);

uint8_t interpolate_single_channel_8(uint8_t c0, uint8_t c1, double f0, double f1)
{
    return (uint8_t)(c0 * f0 + c1 * f1);
}

uint16_t interpolate_r5g6b5(uint16_t c0, uint16_t c1, double f0, double f1)
{
    uint8_t r0 = (c0 >> 11) & 0x1F;
    uint8_t g0 = (c0 >> 5) & 0x3F;
    uint8_t b0 = c0 & 0x1F;

    uint8_t r1 = (c1 >> 11) & 0x1F;
    uint8_t g1 = (c1 >> 5) & 0x3F;
    uint8_t b1 = c1 & 0x1F;

    uint8_t r = (uint8_t)(f0 * r0 + f1 * r1 + 0.5);
    uint8_t g = (uint8_t)(f0 * g0 + f1 * g1 + 0.5);
    uint8_t b = (uint8_t)(f0 * b0 + f1 * b1 + 0.5);

    return ((uint16_t)r << 11) | ((uint16_t)g << 5) | b;
}

char *filename(char *path)
{
    char *fn = path;
    for (char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            fn = ++p;
    return fn;
}

void stem(char *path, char *out, size_t out_size)
{
    char *fn = filename(path);
    snprintf(out, out_size, "%s", fn);

    char *dot = strrchr(out, '.');
    if (dot != NULL)
    {
        *dot = '\0';
    }
}

void lowercase_copy(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0;
    if (dst_size == 0)
    {
        return;
    }

    while (src[i] != '\0' && i + 1 < dst_size)
    {
        dst[i] = (char)tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

int get_tile_half_height(const char *filename)
{
    return strstr(filename, "_x2") != NULL ? TILE_HALF_HEIGHT_X2 : TILE_HALF_HEIGHT_X1;
}

int get_tile_width(const char *filename)
{
    return strstr(filename, "_x2") != NULL ? TILE_WIDTH_X2 : TILE_WIDTH_X1;
}

void get_building_tiles(const char *filename, int layer_w, int tile_w, int *tiles_u, int *tiles_v)
{
    char name[256];
    lowercase_copy(filename, name, sizeof(name));

    if (strstr(name, "gate") != NULL)
    {
        if (strstr(name, "_n_") != NULL)
        {
            *tiles_u = 1;
            *tiles_v = 1;
            return;
        }
        if (strstr(name, "_ne_") != NULL)
        {
            *tiles_u = 1;
            *tiles_v = 2;
            return;
        }
        if (strstr(name, "_se_") != NULL)
        {
            *tiles_u = 2;
            *tiles_v = 1;
            return;
        }
        if (strstr(name, "_e_") != NULL)
        {
            *tiles_u = 1;
            *tiles_v = 1;
            return;
        }

        *tiles_u = 1;
        *tiles_v = 1;
        return;
    }

    size_t footprint_count = sizeof(BUILDING_FOOTPRINTS) / sizeof(BUILDING_FOOTPRINTS[0]);
    for (size_t i = 0; i < footprint_count; i++)
    {
        if (strstr(name, BUILDING_FOOTPRINTS[i].name) != NULL)
        {
            *tiles_u = BUILDING_FOOTPRINTS[i].tiles_u;
            *tiles_v = BUILDING_FOOTPRINTS[i].tiles_v;
            return;
        }
    }

    int n = (layer_w + tile_w / 2) / tile_w;
    if (n < 1)
    {
        n = 1;
    }
    *tiles_u = n;
    *tiles_v = n;
}

int get_gate_compound_offsets(const char *filename, int tile_hh, int offsets[2][2])
{
    char name[256];
    lowercase_copy(filename, name, sizeof(name));

    if (strstr(name, "gate") == NULL)
    {
        return 0;
    }
    if (strstr(name, "_n_") != NULL)
    {
        offsets[0][0] = 0;
        offsets[0][1] = -tile_hh;
        offsets[1][0] = 0;
        offsets[1][1] = tile_hh;
        return 2;
    }
    if (strstr(name, "_e_") != NULL)
    {
        int tile_hw = tile_hh * 2;
        offsets[0][0] = -tile_hw;
        offsets[0][1] = 0;
        offsets[1][0] = tile_hw;
        offsets[1][1] = 0;
        return 2;
    }

    return 0;
}

void output_path(char *out, size_t out_size, const char *stemmed, const char *filename, const char *suffix)
{
    snprintf(out, out_size, "out/%s/%s%s", stemmed, filename, suffix);
}

void output_frame_path(char *out, size_t out_size, const char *stemmed, const char *filename, int frame_count,
                       int frame_number, const char *suffix)
{
    if (frame_count <= 1)
    {
        output_path(out, out_size, stemmed, filename, suffix);
        return;
    }

    snprintf(out, out_size, "out/%s/%s_f%d%s", stemmed, filename, frame_number, suffix);
}

void draw_pixel(uint8_t *canvas, int canvas_width, int canvas_height, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= canvas_width || y < 0 || y >= canvas_height)
    {
        return;
    }

    int idx = (y * canvas_width + x) * 4;
    canvas[idx + 0] = r;
    canvas[idx + 1] = g;
    canvas[idx + 2] = b;
    canvas[idx + 3] = 255;
}

void draw_line(uint8_t *canvas, int canvas_width, int canvas_height, int x0, int y0, int x1, int y1, uint8_t r,
               uint8_t g, uint8_t b)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        draw_pixel(canvas, canvas_width, canvas_height, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_main_pixel(uint8_t *canvas, int canvas_width, int canvas_height, int x, int y, uint8_t r, uint8_t g,
                     uint8_t b, uint16_t offset_x1, uint16_t offset_y1, int width, int height, int blocks_wide,
                     uint8_t *dirty_blocks)
{
    int local_x = x - offset_x1;
    int local_y = y - offset_y1;
    if (local_x < 0 || local_x >= width || local_y < 0 || local_y >= height)
    {
        return;
    }

    draw_pixel(canvas, canvas_width, canvas_height, x, y, r, g, b);
    dirty_blocks[(local_y / 4) * blocks_wide + (local_x / 4)] = 1;
}

void draw_main_line(uint8_t *canvas, int canvas_width, int canvas_height, int x0, int y0, int x1, int y1, uint8_t r,
                    uint8_t g, uint8_t b, uint16_t offset_x1, uint16_t offset_y1, int width, int height,
                    int blocks_wide, uint8_t *dirty_blocks)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        draw_main_pixel(canvas, canvas_width, canvas_height, x0, y0, r, g, b, offset_x1, offset_y1, width, height,
                        blocks_wide, dirty_blocks);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_rect_outline(uint8_t *canvas, int canvas_width, int canvas_height, int x1, int y1, int x2, int y2, uint8_t r,
                       uint8_t g, uint8_t b)
{
    if (x1 >= x2 || y1 >= y2)
    {
        return;
    }

    draw_line(canvas, canvas_width, canvas_height, x1, y1, x2 - 1, y1, r, g, b);
    draw_line(canvas, canvas_width, canvas_height, x2 - 1, y1, x2 - 1, y2 - 1, r, g, b);
    draw_line(canvas, canvas_width, canvas_height, x2 - 1, y2 - 1, x1, y2 - 1, r, g, b);
    draw_line(canvas, canvas_width, canvas_height, x1, y2 - 1, x1, y1, r, g, b);
}

void write_debug_png_with_sprite_boxes(const char *path, const uint8_t *canvas, int canvas_width, int canvas_height,
                                       const layer_rect_t *rects, int rect_count)
{
    size_t canvas_size = (size_t)canvas_width * canvas_height * 4;
    uint8_t *png_canvas = malloc(canvas_size);
    if (png_canvas == NULL)
    {
        return;
    }

    memcpy(png_canvas, canvas, canvas_size);
    for (int i = 0; i < rect_count; i++)
    {
        draw_rect_outline(png_canvas, canvas_width, canvas_height, rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2,
                          255, 0, 0);
    }
    stbi_write_png(path, canvas_width, canvas_height, 4, png_canvas, canvas_width * 4);
    free(png_canvas);
}

void mark_layer_outline_pixel(int x, int y, layer_rect_t rect, int blocks_wide, uint16_t *outline_masks)
{
    int local_x = x - rect.x1;
    int local_y = y - rect.y1;
    if (local_x < 0 || local_x >= rect.x2 - rect.x1 || local_y < 0 || local_y >= rect.y2 - rect.y1)
    {
        return;
    }

    int block_idx = (local_y / 4) * blocks_wide + (local_x / 4);
    int pixel_idx = (local_y % 4) * 4 + (local_x % 4);
    outline_masks[block_idx] |= (uint16_t)(1u << pixel_idx);
}

void mark_layer_outline_line(int x0, int y0, int x1, int y1, layer_rect_t rect, int blocks_wide, uint16_t *outline_masks)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        mark_layer_outline_pixel(x0, y0, rect, blocks_wide, outline_masks);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

uint16_t compute_diamond_outline_mask_for_block(layer_rect_t rect, int block_x, int block_y, int center_x, int center_y,
                                                int margin_u, int margin_v, int outline_width)
{
    if (outline_width <= 0 || margin_u <= 0 || margin_v <= 0)
    {
        return 0;
    }

    int outer_margin_u2 = margin_u * 2;
    int outer_margin_v2 = margin_v * 2;
    int inner_margin_u = margin_u - outline_width;
    int inner_margin_v = margin_v - outline_width;
    int inner_margin_u2 = inner_margin_u * 2;
    int inner_margin_v2 = inner_margin_v * 2;
    uint16_t mask = 0;

    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int x = rect.x1 + block_x * 4 + px;
            int y = rect.y1 + block_y * 4 + py;
            int dx = x - center_x;
            int dy = y - center_y;
            int u2 = dx + 2 * dy;
            int v2 = -dx + 2 * dy;
            int outer = abs(u2) <= outer_margin_u2 && abs(v2) <= outer_margin_v2;
            int inner = inner_margin_u > 0 && inner_margin_v > 0 && abs(u2) < inner_margin_u2 &&
                        abs(v2) < inner_margin_v2;

            if (outer && !inner)
            {
                mask |= (uint16_t)(1u << (py * 4 + px));
            }
        }
    }

    return mask;
}

uint16_t compute_compound_outline_mask_for_block(layer_rect_t rect, int block_x, int block_y, int center_x, int center_y,
                                                 int tile_hh, const int offsets[2][2], int offset_count,
                                                 int outline_width)
{
    uint16_t mask = 0;

    for (int i = 0; i < offset_count; i++)
    {
        mask |= compute_diamond_outline_mask_for_block(rect, block_x, block_y, center_x + offsets[i][0],
                                                       center_y + offsets[i][1], tile_hh, tile_hh, outline_width);
    }

    return mask;
}

void draw_outline_mask_to_layer(uint8_t *canvas, int canvas_width, int canvas_height, layer_rect_t rect, int block_x,
                                int block_y, int blocks_wide, uint16_t mask, uint8_t value, uint8_t *dirty_blocks)
{
    if (mask == 0)
    {
        return;
    }

    int wrote_pixel = 0;
    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int bit = py * 4 + px;
            if ((mask & (uint16_t)(1u << bit)) == 0)
            {
                continue;
            }

            int x = rect.x1 + block_x * 4 + px;
            int y = rect.y1 + block_y * 4 + py;
            draw_pixel(canvas, canvas_width, canvas_height, x, y, value, value, value);
            wrote_pixel = 1;
        }
    }

    if (wrote_pixel)
    {
        dirty_blocks[block_y * blocks_wide + block_x] = 1;
    }
}

void draw_layer_pixel(uint8_t *canvas, int canvas_width, int canvas_height, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, layer_rect_t rect, int blocks_wide, uint8_t *dirty_blocks)
{
    int local_x = x - rect.x1;
    int local_y = y - rect.y1;
    if (local_x < 0 || local_x >= rect.x2 - rect.x1 || local_y < 0 || local_y >= rect.y2 - rect.y1)
    {
        return;
    }

    draw_pixel(canvas, canvas_width, canvas_height, x, y, r, g, b);
    dirty_blocks[(local_y / 4) * blocks_wide + (local_x / 4)] = 1;
}

void draw_layer_line(uint8_t *canvas, int canvas_width, int canvas_height, int x0, int y0, int x1, int y1, uint8_t r,
                     uint8_t g, uint8_t b, layer_rect_t rect, int blocks_wide, uint8_t *dirty_blocks)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        draw_layer_pixel(canvas, canvas_width, canvas_height, x0, y0, r, g, b, rect, blocks_wide, dirty_blocks);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

uint16_t rgb888_to_r5g6b5(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

void r5g6b5_to_rgb888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = ((c >> 11) & 0x1F) * 255 / 31;
    *g = ((c >> 5) & 0x3F) * 255 / 63;
    *b = (c & 0x1F) * 255 / 31;
}

int rgb_distance_sq(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1)
{
    int dr = (int)r0 - r1;
    int dg = (int)g0 - g1;
    int db = (int)b0 - b1;
    return dr * dr + dg * dg + db * db;
}

void write_main_pixel(uint8_t *pixel, uint16_t c, int transparent)
{
    pixel[0] = ((c >> 11) & 0x1F) * 255 / 31;
    pixel[1] = ((c >> 5) & 0x3F) * 255 / 63;
    pixel[2] = (c & 0x1F) * 255 / 31;
    pixel[3] = transparent ? 0 : 255;
}

void write_shadow_pixel(uint8_t *pixel, uint8_t c)
{
    pixel[0] = 0;
    pixel[1] = 0;
    pixel[2] = 0;
    pixel[3] = c;
}

void write_playercolor_pixel(uint8_t *pixel, uint8_t c)
{
    pixel[0] = c;
    pixel[1] = c;
    pixel[2] = c;
    pixel[3] = c == 0 ? 0 : 255;
}

void write_damage_pixel(uint8_t *pixel, uint8_t c)
{
    pixel[0] = c;
    pixel[1] = c;
    pixel[2] = c;
    pixel[3] = c == 0 ? 0 : 255;
}

uint8_t read_shadow_value(const uint8_t *pixel)
{
    return pixel[3];
}

uint8_t read_gray_value(const uint8_t *pixel)
{
    return pixel[0];
}

layer_rect_t layer_rect(int x1, int y1, int x2, int y2)
{
    layer_rect_t rect = {x1, y1, x2, y2};
    return rect;
}

layer_rect_t layer_rect_from_header(sld_main_header_t header)
{
    return layer_rect(header.offset_x1, header.offset_y1, header.offset_x2, header.offset_y2);
}

void expand_rect(layer_rect_t *rect, layer_rect_t other)
{
    if (other.x1 < rect->x1)
    {
        rect->x1 = other.x1;
    }
    if (other.y1 < rect->y1)
    {
        rect->y1 = other.y1;
    }
    if (other.x2 > rect->x2)
    {
        rect->x2 = other.x2;
    }
    if (other.y2 > rect->y2)
    {
        rect->y2 = other.y2;
    }
}

layer_rect_t align_layer_rect(layer_rect_t rect, int canvas_width, int canvas_height)
{
    rect.x1 &= ~3;
    rect.y1 &= ~3;
    rect.x2 = (rect.x2 + 3) & ~3;
    rect.y2 = (rect.y2 + 3) & ~3;

    if (rect.x1 < 0)
    {
        rect.x1 = 0;
    }
    if (rect.y1 < 0)
    {
        rect.y1 = 0;
    }
    if (rect.x2 > canvas_width)
    {
        rect.x2 = canvas_width;
    }
    if (rect.y2 > canvas_height)
    {
        rect.y2 = canvas_height;
    }

    return rect;
}

int begin_layer(FILE *f, const char *name, sld_layer_t *layer)
{
    (void)name;
    layer->read_start = ftell(f);
    if (fread(&layer->content_length, sizeof(uint32_t), 1, f) != 1)
    {
        return 0;
    }

    layer->actual_length = (layer->content_length + 3) & ~3;
    DEBUG_PRINTF("    %s\n      sld_layer_length=%d\n      actual_length=%d\n", name, layer->content_length,
                 layer->actual_length);
    return 1;
}

int read_layer_commands(FILE *f, sld_layer_t *layer)
{
    if (fread(&layer->command_array_length, sizeof(uint16_t), 1, f) != 1)
    {
        return 0;
    }

    if (layer->command_array_length > MAX_COMMANDS)
    {
        printf("command_array_length %d is bigger than %d\n", layer->command_array_length, MAX_COMMANDS);
        return 0;
    }
    DEBUG_PRINTF("      command_array_length=%d\n", layer->command_array_length);
    if (fread(layer->commands, sizeof(command_t), layer->command_array_length, f) != layer->command_array_length)
    {
        printf("commands err\n");
        return 0;
    }
    return 1;
}

int finish_layer(FILE *f, sld_layer_t *layer)
{
    long read = ftell(f) - layer->read_start;
    DEBUG_PRINTF("      null bytes %ld\n", layer->actual_length - read);
    return fseek(f, layer->actual_length - read, SEEK_CUR) == 0;
}

int extract_main_blocks(const uint8_t *file_bytes, const main_layer_info_t *main_info, bc1_t *blocks, uint8_t *present)
{
    int blocks_wide = main_info->width / 4;
    const uint8_t *p = file_bytes + main_info->layer_start + sizeof(uint32_t) + sizeof(sld_main_header_t) +
                       sizeof(uint16_t) + main_info->command_array_length * sizeof(command_t);
    int block_x = 0;
    int block_y = 0;

    for (int i = 0; i < main_info->command_array_length; i++)
    {
        command_t cmd = main_info->commands[i];
        block_y += (block_x + cmd.skipped) / blocks_wide;
        block_x = (block_x + cmd.skipped) % blocks_wide;

        for (int j = 0; j < cmd.drawn; j++)
        {
            int idx = block_y * blocks_wide + block_x;
            memcpy(&blocks[idx], p, sizeof(bc1_t));
            present[idx] = 1;
            p += sizeof(bc1_t);
            block_y += (block_x + 1) / blocks_wide;
            block_x = (block_x + 1) % blocks_wide;
        }
    }

    return 1;
}

int extract_dxt1_blocks(const uint8_t *file_bytes, const dxt1_layer_info_t *layer_info, bc1_t *blocks, uint8_t *present)
{
    int blocks_wide = layer_info->width / 4;
    const uint8_t *p = file_bytes + layer_info->layer_start + sizeof(uint32_t) + layer_info->header_size +
                       sizeof(uint16_t) + layer_info->command_array_length * sizeof(command_t);
    int block_x = 0;
    int block_y = 0;

    for (int i = 0; i < layer_info->command_array_length; i++)
    {
        command_t cmd = layer_info->commands[i];
        block_y += (block_x + cmd.skipped) / blocks_wide;
        block_x = (block_x + cmd.skipped) % blocks_wide;

        for (int j = 0; j < cmd.drawn; j++)
        {
            int idx = block_y * blocks_wide + block_x;
            memcpy(&blocks[idx], p, sizeof(bc1_t));
            present[idx] = 1;
            p += sizeof(bc1_t);
            block_y += (block_x + 1) / blocks_wide;
            block_x = (block_x + 1) % blocks_wide;
        }
    }

    return 1;
}

int extract_bc4_blocks(const uint8_t *file_bytes, const bc4_layer_info_t *layer_info, bc4_t *blocks, uint8_t *present)
{
    int blocks_wide = layer_info->width / 4;
    const uint8_t *p = file_bytes + layer_info->layer_start + sizeof(uint32_t) + layer_info->header_size +
                       sizeof(uint16_t) + layer_info->command_array_length * sizeof(command_t);
    int block_x = 0;
    int block_y = 0;

    for (int i = 0; i < layer_info->command_array_length; i++)
    {
        command_t cmd = layer_info->commands[i];
        block_y += (block_x + cmd.skipped) / blocks_wide;
        block_x = (block_x + cmd.skipped) % blocks_wide;

        for (int j = 0; j < cmd.drawn; j++)
        {
            int idx = block_y * blocks_wide + block_x;
            memcpy(&blocks[idx], p, sizeof(bc4_t));
            present[idx] = 1;
            p += sizeof(bc4_t);
            block_y += (block_x + 1) / blocks_wide;
            block_x = (block_x + 1) % blocks_wide;
        }
    }

    return 1;
}

int rebuild_copied_bc1_layer(const uint8_t *file_bytes, const dxt1_layer_info_t *layer_info, layer_rect_t rect,
                             rebuilt_layer_t *rebuilt)
{
    int old_blocks_wide = layer_info->width / 4;
    int old_blocks_tall = layer_info->height / 4;
    int old_total_blocks = old_blocks_wide * old_blocks_tall;
    int new_blocks_wide = (rect.x2 - rect.x1) / 4;
    int new_blocks_tall = (rect.y2 - rect.y1) / 4;
    int new_total_blocks = new_blocks_wide * new_blocks_tall;
    bc1_t *original_blocks = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(bc1_t));
    uint8_t *original_present = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(uint8_t));
    bc1_t *block_grid = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc1_t));
    uint8_t *present = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(command_t));
    bc1_t *blocks = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc1_t));
    if (original_blocks == NULL || original_present == NULL || block_grid == NULL || present == NULL ||
        commands == NULL || blocks == NULL)
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    if (!extract_dxt1_blocks(file_bytes, layer_info, original_blocks, original_present))
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    int delta_x_blocks = ((int)layer_info->offset_x1 - rect.x1) / 4;
    int delta_y_blocks = ((int)layer_info->offset_y1 - rect.y1) / 4;
    for (int old_y = 0; old_y < old_blocks_tall; old_y++)
    {
        for (int old_x = 0; old_x < old_blocks_wide; old_x++)
        {
            int old_idx = old_y * old_blocks_wide + old_x;
            if (!original_present[old_idx])
            {
                continue;
            }

            int new_x = old_x + delta_x_blocks;
            int new_y = old_y + delta_y_blocks;
            if (new_x < 0 || new_x >= new_blocks_wide || new_y < 0 || new_y >= new_blocks_tall)
            {
                continue;
            }
            int new_idx = new_y * new_blocks_wide + new_x;
            block_grid[new_idx] = original_blocks[old_idx];
            present[new_idx] = 1;
        }
    }

    int pos = 0;
    while (pos < new_total_blocks)
    {
        int skipped = 0;
        while (pos < new_total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < new_total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= new_total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc1_t);
    rebuilt->content_length = sizeof(uint32_t) + layer_info->header_size + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(original_blocks);
    free(original_present);
    free(block_grid);
    free(present);
    return 1;
}

uint32_t expand_outline_mask(uint16_t mask_bits)
{
    uint32_t expanded = 0;
    for (int bit = 0; bit < 16; bit++)
    {
        if (mask_bits & (uint16_t)(1u << bit))
        {
            expanded |= 0x3u << (2 * bit);
        }
    }
    return expanded;
}

bc1_t force_opaque_outline_pixels(bc1_t block, uint16_t mask_bits)
{
    if (mask_bits == 0 || block.color0 > block.color1)
    {
        return block;
    }

    block.pix_indices &= ~expand_outline_mask(mask_bits);
    return block;
}

bc1_t create_outline_support_block(uint16_t mask_bits)
{
    bc1_t block = {0};
    uint16_t gray = rgb888_to_r5g6b5(82, 85, 82);
    uint16_t non_outline = (uint16_t)(~mask_bits);

    block.color0 = gray;
    block.color1 = gray;
    block.pix_indices = expand_outline_mask(non_outline);
    return block;
}

int main_block_is_empty(const uint8_t *canvas, int canvas_width, uint16_t offset_x1, uint16_t offset_y1, int block_x,
                        int block_y)
{
    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int x = offset_x1 + block_x * 4 + px;
            int y = offset_y1 + block_y * 4 + py;
            int idx = (y * canvas_width + x) * 4;
            if (canvas[idx + 3] != 0)
            {
                return 0;
            }
        }
    }

    return 1;
}

int bc4_block_is_empty(const uint8_t *canvas, int canvas_width, uint16_t offset_x1, uint16_t offset_y1, int block_x,
                       int block_y, bc4_value_reader_t read_value)
{
    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int x = offset_x1 + block_x * 4 + px;
            int y = offset_y1 + block_y * 4 + py;
            int idx = (y * canvas_width + x) * 4;
            if (read_value(canvas + idx) != 0)
            {
                return 0;
            }
        }
    }

    return 1;
}

bc1_t encode_main_block(const uint8_t *canvas, int canvas_width, uint16_t offset_x1, uint16_t offset_y1, int block_x,
                        int block_y)
{
    uint8_t min_r = 255, min_g = 255, min_b = 255;
    uint8_t max_r = 0, max_g = 0, max_b = 0;
    int has_transparent = 0;
    int has_opaque = 0;

    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int x = offset_x1 + block_x * 4 + px;
            int y = offset_y1 + block_y * 4 + py;
            int idx = (y * canvas_width + x) * 4;
            if (canvas[idx + 3] == 0)
            {
                has_transparent = 1;
                continue;
            }

            has_opaque = 1;
            if (canvas[idx + 0] < min_r)
                min_r = canvas[idx + 0];
            if (canvas[idx + 1] < min_g)
                min_g = canvas[idx + 1];
            if (canvas[idx + 2] < min_b)
                min_b = canvas[idx + 2];
            if (canvas[idx + 0] > max_r)
                max_r = canvas[idx + 0];
            if (canvas[idx + 1] > max_g)
                max_g = canvas[idx + 1];
            if (canvas[idx + 2] > max_b)
                max_b = canvas[idx + 2];
        }
    }

    bc1_t block = {0};
    if (!has_opaque)
    {
        return block;
    }

    block.color0 = rgb888_to_r5g6b5(max_r, max_g, max_b);
    block.color1 = rgb888_to_r5g6b5(min_r, min_g, min_b);

    if (has_transparent)
    {
        if (block.color0 > block.color1)
        {
            uint16_t tmp = block.color0;
            block.color0 = block.color1;
            block.color1 = tmp;
        }
    }
    else
    {
        if (block.color0 == block.color1)
        {
            if (block.color1 > 0)
            {
                block.color1--;
            }
            else
            {
                block.color0 = 1;
            }
        }
        if (block.color0 <= block.color1)
        {
            uint16_t tmp = block.color0;
            block.color0 = block.color1;
            block.color1 = tmp;
        }
    }

    uint16_t palette565[4];
    uint8_t palette[4][3];
    palette565[0] = block.color0;
    palette565[1] = block.color1;
    if (block.color0 > block.color1)
    {
        palette565[2] = interpolate_r5g6b5(block.color0, block.color1, 2.0 / 3.0, 1.0 / 3.0);
        palette565[3] = interpolate_r5g6b5(block.color0, block.color1, 1.0 / 3.0, 2.0 / 3.0);
    }
    else
    {
        palette565[2] = interpolate_r5g6b5(block.color0, block.color1, 0.5, 0.5);
        palette565[3] = 0;
    }

    for (int i = 0; i < 4; i++)
    {
        r5g6b5_to_rgb888(palette565[i], &palette[i][0], &palette[i][1], &palette[i][2]);
    }

    uint32_t indices = 0;
    for (int k = 0; k < 16; k++)
    {
        int px = k % 4;
        int py = k / 4;
        int x = offset_x1 + block_x * 4 + px;
        int y = offset_y1 + block_y * 4 + py;
        int idx = (y * canvas_width + x) * 4;

        uint32_t color_idx = 0;
        if (canvas[idx + 3] == 0 && block.color0 <= block.color1)
        {
            color_idx = 3;
        }
        else
        {
            int best_idx = 0;
            int best_dist = rgb_distance_sq(canvas[idx + 0], canvas[idx + 1], canvas[idx + 2], palette[0][0],
                                            palette[0][1], palette[0][2]);
            int max_palette = block.color0 > block.color1 ? 4 : 3;
            for (int i = 1; i < max_palette; i++)
            {
                int dist = rgb_distance_sq(canvas[idx + 0], canvas[idx + 1], canvas[idx + 2], palette[i][0],
                                           palette[i][1], palette[i][2]);
                if (dist < best_dist)
                {
                    best_dist = dist;
                    best_idx = i;
                }
            }
            color_idx = (uint32_t)best_idx;
        }

        indices |= color_idx << (2 * k);
    }

    block.pix_indices = indices;
    return block;
}

bc4_t encode_bc4_block(const uint8_t *canvas, int canvas_width, uint16_t offset_x1, uint16_t offset_y1, int block_x,
                       int block_y, bc4_value_reader_t read_value)
{
    uint8_t min_c = 255;
    uint8_t max_c = 0;
    int has_nonzero = 0;
    int has_zero = 0;
    int has_255 = 0;

    for (int py = 0; py < 4; py++)
    {
        for (int px = 0; px < 4; px++)
        {
            int x = offset_x1 + block_x * 4 + px;
            int y = offset_y1 + block_y * 4 + py;
            int idx = (y * canvas_width + x) * 4;
            uint8_t c = read_value(canvas + idx);
            if (c == 0)
            {
                has_zero = 1;
                continue;
            }

            has_nonzero = 1;
            if (c == 255)
            {
                has_255 = 1;
            }
            if (c < min_c)
            {
                min_c = c;
            }
            if (c > max_c)
            {
                max_c = c;
            }
        }
    }

    bc4_t block = {0};
    if (!has_nonzero)
    {
        return block;
    }

    int use_special_mode = has_zero || has_255;
    if (use_special_mode)
    {
        block.color0 = min_c;
        block.color1 = max_c;
    }
    else
    {
        block.color0 = max_c;
        block.color1 = min_c;
        if (block.color0 <= block.color1)
        {
            if (block.color0 < 255)
            {
                block.color0++;
            }
            else if (block.color1 > 0)
            {
                block.color1--;
            }
        }
    }

    uint8_t colors[8];
    colors[0] = block.color0;
    colors[1] = block.color1;
    if (use_special_mode)
    {
        colors[2] = interpolate_single_channel_8(colors[0], colors[1], 4.0 / 5.0, 1.0 / 5.0);
        colors[3] = interpolate_single_channel_8(colors[0], colors[1], 3.0 / 5.0, 2.0 / 5.0);
        colors[4] = interpolate_single_channel_8(colors[0], colors[1], 2.0 / 5.0, 3.0 / 5.0);
        colors[5] = interpolate_single_channel_8(colors[0], colors[1], 1.0 / 5.0, 4.0 / 5.0);
        colors[6] = 0;
        colors[7] = 255;
    }
    else
    {
        colors[2] = interpolate_single_channel_8(colors[0], colors[1], 6.0 / 7.0, 1.0 / 7.0);
        colors[3] = interpolate_single_channel_8(colors[0], colors[1], 5.0 / 7.0, 2.0 / 7.0);
        colors[4] = interpolate_single_channel_8(colors[0], colors[1], 4.0 / 7.0, 3.0 / 7.0);
        colors[5] = interpolate_single_channel_8(colors[0], colors[1], 3.0 / 7.0, 4.0 / 7.0);
        colors[6] = interpolate_single_channel_8(colors[0], colors[1], 2.0 / 7.0, 5.0 / 7.0);
        colors[7] = interpolate_single_channel_8(colors[0], colors[1], 1.0 / 7.0, 6.0 / 7.0);
    }

    uint64_t indices = 0;
    for (int k = 0; k < 16; k++)
    {
        int px = k % 4;
        int py = k / 4;
        int x = offset_x1 + block_x * 4 + px;
        int y = offset_y1 + block_y * 4 + py;
        int idx = (y * canvas_width + x) * 4;
        uint8_t c = read_value(canvas + idx);

        int best_idx = 0;
        int best_dist = abs((int)c - colors[0]);
        for (int i = 1; i < 8; i++)
        {
            int dist = abs((int)c - colors[i]);
            if (dist < best_dist)
            {
                best_dist = dist;
                best_idx = i;
            }
        }
        indices |= (uint64_t)best_idx << (3 * k);
    }

    for (int i = 0; i < 6; i++)
    {
        block.pix_indices[i] = (uint8_t)((indices >> (8 * i)) & 0xFF);
    }
    return block;
}

int rebuild_bc1_layer(const uint8_t *canvas, int canvas_width, layer_rect_t rect, rebuilt_layer_t *rebuilt)
{
    int blocks_wide = (rect.x2 - rect.x1) / 4;
    int blocks_tall = (rect.y2 - rect.y1) / 4;
    int total_blocks = blocks_wide * blocks_tall;
    bc1_t *block_grid = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(bc1_t));
    uint8_t *present = calloc(total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(command_t));
    bc1_t *blocks = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(bc1_t));
    if (block_grid == NULL || present == NULL || commands == NULL || blocks == NULL)
    {
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    for (int block_y = 0; block_y < blocks_tall; block_y++)
    {
        for (int block_x = 0; block_x < blocks_wide; block_x++)
        {
            int idx = block_y * blocks_wide + block_x;
            if (main_block_is_empty(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y))
            {
                continue;
            }

            present[idx] = 1;
            block_grid[idx] = encode_main_block(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y);
        }
    }

    int pos = 0;
    while (pos < total_blocks)
    {
        int skipped = 0;
        while (pos < total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc1_t);
    rebuilt->content_length = sizeof(uint32_t) + sizeof(sld_main_header_t) + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(block_grid);
    free(present);
    return 1;
}

int rebuild_bc4_layer(const uint8_t *canvas, int canvas_width, layer_rect_t rect, bc4_value_reader_t read_value,
                      size_t header_size, rebuilt_layer_t *rebuilt)
{
    int blocks_wide = (rect.x2 - rect.x1) / 4;
    int blocks_tall = (rect.y2 - rect.y1) / 4;
    int total_blocks = blocks_wide * blocks_tall;
    bc4_t *block_grid = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(bc4_t));
    uint8_t *present = calloc(total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(command_t));
    bc4_t *blocks = calloc(total_blocks == 0 ? 1 : total_blocks, sizeof(bc4_t));
    if (block_grid == NULL || present == NULL || commands == NULL || blocks == NULL)
    {
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    for (int block_y = 0; block_y < blocks_tall; block_y++)
    {
        for (int block_x = 0; block_x < blocks_wide; block_x++)
        {
            int idx = block_y * blocks_wide + block_x;
            if (bc4_block_is_empty(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y, read_value))
            {
                continue;
            }

            present[idx] = 1;
            block_grid[idx] = encode_bc4_block(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y, read_value);
        }
    }

    int pos = 0;
    while (pos < total_blocks)
    {
        int skipped = 0;
        while (pos < total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc4_t);
    rebuilt->content_length = sizeof(uint32_t) + header_size + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(block_grid);
    free(present);
    return 1;
}

int rebuild_modified_bc1_layer(const uint8_t *file_bytes, const main_layer_info_t *main_info, layer_rect_t rect,
                               const uint16_t *outline_masks, int allow_new_blocks, rebuilt_layer_t *rebuilt)
{
    int old_blocks_wide = main_info->width / 4;
    int old_blocks_tall = main_info->height / 4;
    int old_total_blocks = old_blocks_wide * old_blocks_tall;
    int new_blocks_wide = (rect.x2 - rect.x1) / 4;
    int new_blocks_tall = (rect.y2 - rect.y1) / 4;
    int new_total_blocks = new_blocks_wide * new_blocks_tall;
    bc1_t *original_blocks = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(bc1_t));
    uint8_t *original_present = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(uint8_t));
    bc1_t *block_grid = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc1_t));
    uint8_t *present = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(command_t));
    bc1_t *blocks = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc1_t));
    if (original_blocks == NULL || original_present == NULL || block_grid == NULL || present == NULL ||
        commands == NULL || blocks == NULL)
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    if (!extract_main_blocks(file_bytes, main_info, original_blocks, original_present))
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    int block_x_offset = (rect.x1 - main_info->header.offset_x1) / 4;
    int block_y_offset = (rect.y1 - main_info->header.offset_y1) / 4;
    for (int block_y = 0; block_y < new_blocks_tall; block_y++)
    {
        for (int block_x = 0; block_x < new_blocks_wide; block_x++)
        {
            int idx = block_y * new_blocks_wide + block_x;
            uint16_t mask_bits = outline_masks[idx];
            int old_block_x = block_x + block_x_offset;
            int old_block_y = block_y + block_y_offset;
            int has_original = old_block_x >= 0 && old_block_x < old_blocks_wide && old_block_y >= 0 &&
                               old_block_y < old_blocks_tall && original_present[old_block_y * old_blocks_wide + old_block_x];

            if (mask_bits == 0)
            {
                if (has_original)
                {
                    present[idx] = 1;
                    block_grid[idx] = original_blocks[old_block_y * old_blocks_wide + old_block_x];
                }
                continue;
            }

            present[idx] = 1;
            if (has_original)
            {
                block_grid[idx] =
                    force_opaque_outline_pixels(original_blocks[old_block_y * old_blocks_wide + old_block_x], mask_bits);
            }
            else if (allow_new_blocks)
            {
                block_grid[idx] = create_outline_support_block(mask_bits);
            }
            else
            {
                present[idx] = 0;
            }
        }
    }

    int pos = 0;
    while (pos < new_total_blocks)
    {
        int skipped = 0;
        while (pos < new_total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < new_total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= new_total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc1_t);
    rebuilt->content_length = sizeof(uint32_t) + sizeof(sld_main_header_t) + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(original_blocks);
    free(original_present);
    free(block_grid);
    free(present);
    return 1;
}

int rebuild_modified_bc4_layer(const uint8_t *file_bytes, const uint8_t *canvas, int canvas_width,
                               const bc4_layer_info_t *layer_info, layer_rect_t rect, const uint8_t *dirty_blocks,
                               bc4_value_reader_t read_value, int allow_new_blocks, rebuilt_layer_t *rebuilt)
{
    int old_blocks_wide = layer_info->width / 4;
    int old_blocks_tall = layer_info->height / 4;
    int old_total_blocks = old_blocks_wide * old_blocks_tall;
    int new_blocks_wide = (rect.x2 - rect.x1) / 4;
    int new_blocks_tall = (rect.y2 - rect.y1) / 4;
    int new_total_blocks = new_blocks_wide * new_blocks_tall;
    bc4_t *original_blocks = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(bc4_t));
    uint8_t *original_present = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(uint8_t));
    bc4_t *block_grid = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc4_t));
    uint8_t *present = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(command_t));
    bc4_t *blocks = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc4_t));
    if (original_blocks == NULL || original_present == NULL || block_grid == NULL || present == NULL ||
        commands == NULL || blocks == NULL)
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    if (!extract_bc4_blocks(file_bytes, layer_info, original_blocks, original_present))
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    int block_x_offset = (rect.x1 - layer_info->offset_x1) / 4;
    int block_y_offset = (rect.y1 - layer_info->offset_y1) / 4;
    for (int block_y = 0; block_y < new_blocks_tall; block_y++)
    {
        for (int block_x = 0; block_x < new_blocks_wide; block_x++)
        {
            int idx = block_y * new_blocks_wide + block_x;
            int old_block_x = block_x + block_x_offset;
            int old_block_y = block_y + block_y_offset;
            int has_original = old_block_x >= 0 && old_block_x < old_blocks_wide && old_block_y >= 0 &&
                               old_block_y < old_blocks_tall && original_present[old_block_y * old_blocks_wide + old_block_x];
            if (bc4_block_is_empty(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y, read_value))
            {
                continue;
            }
            if (!allow_new_blocks && !has_original)
            {
                continue;
            }

            present[idx] = 1;
            if (!dirty_blocks[idx] && has_original)
            {
                block_grid[idx] = original_blocks[old_block_y * old_blocks_wide + old_block_x];
            }
            else
            {
                block_grid[idx] =
                    encode_bc4_block(canvas, canvas_width, rect.x1, rect.y1, block_x, block_y, read_value);
            }
        }
    }

    int pos = 0;
    while (pos < new_total_blocks)
    {
        int skipped = 0;
        while (pos < new_total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < new_total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= new_total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc4_t);
    rebuilt->content_length = sizeof(uint32_t) + layer_info->header_size + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(original_blocks);
    free(original_present);
    free(block_grid);
    free(present);
    return 1;
}

int rebuild_copied_bc4_layer(const uint8_t *file_bytes, const bc4_layer_info_t *layer_info, layer_rect_t rect,
                             rebuilt_layer_t *rebuilt)
{
    int old_blocks_wide = layer_info->width / 4;
    int old_blocks_tall = layer_info->height / 4;
    int old_total_blocks = old_blocks_wide * old_blocks_tall;
    int new_blocks_wide = (rect.x2 - rect.x1) / 4;
    int new_blocks_tall = (rect.y2 - rect.y1) / 4;
    int new_total_blocks = new_blocks_wide * new_blocks_tall;
    bc4_t *original_blocks = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(bc4_t));
    uint8_t *original_present = calloc(old_total_blocks == 0 ? 1 : old_total_blocks, sizeof(uint8_t));
    bc4_t *block_grid = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc4_t));
    uint8_t *present = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(uint8_t));
    command_t *commands = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(command_t));
    bc4_t *blocks = calloc(new_total_blocks == 0 ? 1 : new_total_blocks, sizeof(bc4_t));
    if (original_blocks == NULL || original_present == NULL || block_grid == NULL || present == NULL ||
        commands == NULL || blocks == NULL)
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    if (!extract_bc4_blocks(file_bytes, layer_info, original_blocks, original_present))
    {
        free(original_blocks);
        free(original_present);
        free(block_grid);
        free(present);
        free(commands);
        free(blocks);
        return 0;
    }

    int delta_x_blocks = ((int)layer_info->offset_x1 - rect.x1) / 4;
    int delta_y_blocks = ((int)layer_info->offset_y1 - rect.y1) / 4;
    for (int old_y = 0; old_y < old_blocks_tall; old_y++)
    {
        for (int old_x = 0; old_x < old_blocks_wide; old_x++)
        {
            int old_idx = old_y * old_blocks_wide + old_x;
            if (!original_present[old_idx])
            {
                continue;
            }

            int new_x = old_x + delta_x_blocks;
            int new_y = old_y + delta_y_blocks;
            if (new_x < 0 || new_x >= new_blocks_wide || new_y < 0 || new_y >= new_blocks_tall)
            {
                continue;
            }
            int new_idx = new_y * new_blocks_wide + new_x;
            block_grid[new_idx] = original_blocks[old_idx];
            present[new_idx] = 1;
        }
    }

    int pos = 0;
    while (pos < new_total_blocks)
    {
        int skipped = 0;
        while (pos < new_total_blocks && !present[pos] && skipped < 255)
        {
            skipped++;
            pos++;
        }

        int drawn = 0;
        while (pos < new_total_blocks && present[pos] && drawn < 255)
        {
            blocks[rebuilt->block_count++] = block_grid[pos];
            drawn++;
            pos++;
        }

        if (drawn == 0 && pos >= new_total_blocks)
        {
            break;
        }

        commands[rebuilt->command_count].skipped = skipped;
        commands[rebuilt->command_count].drawn = drawn;
        rebuilt->command_count++;
    }

    rebuilt->commands = commands;
    rebuilt->blocks = blocks;
    rebuilt->block_size = sizeof(bc4_t);
    rebuilt->content_length = sizeof(uint32_t) + layer_info->header_size + sizeof(uint16_t) +
                              rebuilt->command_count * sizeof(command_t) + rebuilt->block_count * rebuilt->block_size;
    rebuilt->actual_length = (rebuilt->content_length + 3) & ~3;

    free(original_blocks);
    free(original_present);
    free(block_grid);
    free(present);
    return 1;
}

int write_rebuilt_layer(FILE *f, const rebuilt_layer_t *rebuilt, const void *header, size_t header_size)
{
    if (!write_bytes(f, &rebuilt->content_length, sizeof(uint32_t)) || !write_bytes(f, header, header_size))
    {
        return 0;
    }

    uint16_t command_count = (uint16_t)rebuilt->command_count;
    if (!write_bytes(f, &command_count, sizeof(uint16_t)) ||
        !write_bytes(f, rebuilt->commands, rebuilt->command_count * sizeof(command_t)) ||
        !write_bytes(f, rebuilt->blocks, rebuilt->block_count * rebuilt->block_size))
    {
        return 0;
    }

    uint8_t zero[4] = {0};
    int padding = rebuilt->actual_length - rebuilt->content_length;
    return padding <= 0 || write_bytes(f, zero, (size_t)padding);
}

void free_rebuilt_layer(rebuilt_layer_t *rebuilt)
{
    free(rebuilt->commands);
    free(rebuilt->blocks);
    rebuilt->commands = NULL;
    rebuilt->blocks = NULL;
    rebuilt->command_count = 0;
    rebuilt->block_count = 0;
    rebuilt->block_size = 0;
    rebuilt->content_length = 0;
    rebuilt->actual_length = 0;
}

int write_bytes(FILE *f, const void *data, size_t size)
{
    return fwrite(data, 1, size, f) == size;
}

void decode_dxt4(const void *block_ptr, int block_x, int block_y, uint8_t *canvas, uint16_t offset_x1,
                 uint16_t offset_y1, uint16_t canvas_width, void *pixel_writer)
{
    bc4_t block = *(const bc4_t *)block_ptr;
    dxt4_pixel_writer_t write_pixel = (dxt4_pixel_writer_t)pixel_writer;
    uint16_t colors[8];
    colors[0] = block.color0;
    colors[1] = block.color1;

    if (colors[0] > colors[1])
    {
        colors[2] = interpolate_single_channel_8(colors[0], colors[1], 6.0 / 7.0, 1.0 / 7.0);
        colors[3] = interpolate_single_channel_8(colors[0], colors[1], 5.0 / 7.0, 2.0 / 7.0);
        colors[4] = interpolate_single_channel_8(colors[0], colors[1], 4.0 / 7.0, 3.0 / 7.0);
        colors[5] = interpolate_single_channel_8(colors[0], colors[1], 3.0 / 7.0, 4.0 / 7.0);
        colors[6] = interpolate_single_channel_8(colors[0], colors[1], 2.0 / 7.0, 5.0 / 7.0);
        colors[7] = interpolate_single_channel_8(colors[0], colors[1], 1.0 / 7.0, 6.0 / 7.0);
    }
    else
    {
        colors[2] = interpolate_single_channel_8(colors[0], colors[1], 4.0 / 5.0, 1.0 / 5.0);
        colors[3] = interpolate_single_channel_8(colors[0], colors[1], 3.0 / 5.0, 2.0 / 5.0);
        colors[4] = interpolate_single_channel_8(colors[0], colors[1], 2.0 / 5.0, 3.0 / 5.0);
        colors[5] = interpolate_single_channel_8(colors[0], colors[1], 1.0 / 5.0, 4.0 / 5.0);
        colors[6] = 0;
        colors[7] = 255;
    }

    uint64_t pix = 0;
    int shift = 0;
    for (int p = 0; p < 6; p++)
    {
        pix |= (uint64_t)block.pix_indices[p] << shift;
        shift += 8;
    }
    for (int k = 0; k < 16; k++)
    {
        uint8_t color_idx = pix & 0b111;
        uint8_t c = colors[color_idx];
        pix >>= 3;

        int px = block_x * 4 + (k % 4);
        int py = block_y * 4 + (k / 4);

        int canvas_px = offset_x1 + px;
        int canvas_py = offset_y1 + py;
        int canvas_idx = (canvas_py * canvas_width + canvas_px) * 4;
        write_pixel(canvas + canvas_idx, c);
    }
}

void decode_dxt1(const void *block_ptr, int block_x, int block_y, uint8_t *canvas, uint16_t offset_x1,
                 uint16_t offset_y1, uint16_t canvas_width, void *pixel_writer)
{
    bc1_t block = *(const bc1_t *)block_ptr;
    dxt1_pixel_writer_t write_pixel = (dxt1_pixel_writer_t)pixel_writer;
    uint16_t colors[4];
    colors[0] = block.color0;
    colors[1] = block.color1;

    if (colors[0] > colors[1])
    {
        colors[2] = interpolate_r5g6b5(colors[0], colors[1], 2.0 / 3.0, 1.0 / 3.0);
        colors[3] = interpolate_r5g6b5(colors[0], colors[1], 1.0 / 3.0, 2.0 / 3.0);
    }
    else
    {
        colors[2] = interpolate_r5g6b5(colors[0], colors[1], 0.5, 0.5);
        colors[3] = 0;
    }

    uint32_t pix = block.pix_indices;
    for (int k = 0; k < 16; k++)
    {
        uint8_t color_idx = pix & 0b11;
        uint16_t c = colors[color_idx];
        pix >>= 2;

        int px = block_x * 4 + (k % 4);
        int py = block_y * 4 + (k / 4);

        int canvas_px = offset_x1 + px;
        int canvas_py = offset_y1 + py;
        int canvas_idx = (canvas_py * canvas_width + canvas_px) * 4;
        write_pixel(canvas + canvas_idx, c, colors[0] <= colors[1] && color_idx == 3);
    }
}

int render_blocks(FILE *f, sld_layer_t *layer, int width, size_t block_size, uint8_t *canvas, uint16_t offset_x1,
                  uint16_t offset_y1, uint16_t canvas_width, decode_method_t decode_method, void *pixel_writer)
{
    int blocks_wide = width / 4;
    int block_x = 0;
    int block_y = 0;
    uint8_t block[sizeof(bc4_t)];

    for (int i = 0; i < layer->command_array_length; i++)
    {
        command_t cmd = layer->commands[i];
        block_y += (block_x + cmd.skipped) / blocks_wide;
        block_x = (block_x + cmd.skipped) % blocks_wide;

        for (int j = 0; j < cmd.drawn; j++)
        {
            if (fread(block, block_size, 1, f) != 1)
            {
                printf("block err\n");
                return 0;
            }
            decode_method(block, block_x, block_y, canvas, offset_x1, offset_y1, canvas_width, pixel_writer);
            block_y += (block_x + 1) / blocks_wide;
            block_x = (block_x + 1) % blocks_wide;
        }
    }

    return 1;
}

int main(int argc, char **argv)
{
    int add_outlines = 0;
    int outline_width = 2;
    int export_pngs = 0;
    int resize_layers = 0;
    char *input_path = NULL;
    FILE *f = NULL;
    FILE *out = NULL;
    FILE *outlined_out = NULL;
    long input_file_size = 0;
    long final_pos = 0;
    uint8_t *file_bytes = NULL;
    char *fn = NULL;
    char stemmed[200];
    sld_header_t sld_header = {0};
    char png[256];
    char out_dir[256];
    char sld_path[512];
    int outlined_output_started = 0;
    int status = 1;

    size_t canvas_size = 0;
    uint8_t *main_canvas = NULL;
    uint8_t *shadow_canvas = NULL;
    uint8_t *damage_canvas = NULL;
    uint8_t *playercolor_canvas = NULL;
    uint8_t *image_canvas = NULL;
    sld_frame_header_t sld_frame_header = {0};
    sld_layer_t layer = {0};
    main_layer_info_t main_info = {0};
    bc4_layer_info_t shadow_info = {0};
    dxt1_layer_info_t damage_info = {0};
    bc4_layer_info_t playercolor_info = {0};
    layer_slice_t unknown_slice = {0};
    sld_shadow_header_t shadow_header = {0};
    sld_damage_mask_header_t damage_header = {0};
    sld_playercolor_mask_header_t playercolor_header = {0};
    int shadow_present = 0;
    int damage_present = 0;
    int playercolor_present = 0;
    int main_width = 0;
    int main_height = 0;
    uint16_t main_offset_x1 = 0;
    uint16_t main_offset_y1 = 0;
    rebuilt_layer_t rebuilt_main = {0};
    rebuilt_layer_t rebuilt_shadow = {0};
    rebuilt_layer_t rebuilt_damage = {0};
    rebuilt_layer_t rebuilt_playercolor = {0};
    layer_rect_t outlined_rect = {0};
    layer_rect_t last_resized_rect = {0};
    int have_last_resized_rect = 0;
    long frame_start = 0;
    long frame_end = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--add-outlines") == 0)
        {
            add_outlines = 1;
            continue;
        }
        if (strcmp(argv[i], "--export-pngs") == 0)
        {
            export_pngs = 1;
            continue;
        }
        if (strcmp(argv[i], "--resize-layers") == 0)
        {
            resize_layers = 1;
            continue;
        }
        if (strcmp(argv[i], "--outline-width") == 0)
        {
            char *end = NULL;
            long parsed = 0;
            if (i + 1 >= argc)
            {
                printf("usage\n");
                return 1;
            }

            parsed = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || parsed <= 0 || parsed > 255)
            {
                printf("usage\n");
                return 1;
            }
            outline_width = (int)parsed;
            continue;
        }

        if (input_path == NULL)
        {
            input_path = argv[i];
            continue;
        }

        printf("usage\n");
        return 1;
    }

    if (input_path == NULL)
    {
        printf("usage\n");
        return 1;
    }

    f = fopen(input_path, "rb");
    if (f == NULL)
    {
        printf("cannot open %s\n", input_path);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        goto cleanup;
    }
    input_file_size = ftell(f);
    if (input_file_size < 0)
    {
        goto cleanup;
    }
    rewind(f);
    file_bytes = malloc((size_t)input_file_size);
    if (file_bytes == NULL)
    {
        goto cleanup;
    }
    if (fread(file_bytes, 1, (size_t)input_file_size, f) != (size_t)input_file_size)
    {
        goto cleanup;
    }
    rewind(f);

    fn = filename(input_path);
    stem(input_path, stemmed, sizeof(stemmed));
    DEBUG_PRINTF("%s\n", fn);

    if (fread(&sld_header, sizeof(sld_header_t), 1, f) != 1)
    {
        goto cleanup;
    }

    DEBUG_PRINTF("%.4s version=%d\nframes=%d\n", sld_header.signature, sld_header.version, sld_header.frame_count);

    mkdir("out", 0755);
    snprintf(out_dir, sizeof(out_dir), "out/%s", stemmed);
    mkdir(out_dir, 0755);

    snprintf(sld_path, sizeof(sld_path), "out/%s/%s.sld", stemmed, stemmed);
    DEBUG_PRINTF("writing %s\n", sld_path);
    out = fopen(sld_path, "wb");
    if (out == NULL)
    {
        goto cleanup;
    }
    if (!write_bytes(out, file_bytes, (size_t)input_file_size))
    {
        goto cleanup;
    }
    fclose(out);
    out = NULL;

    for (int frame_num = 0; frame_num < sld_header.frame_count; frame_num++)
    {
        memset(&sld_frame_header, 0, sizeof(sld_frame_header));
        memset(&layer, 0, sizeof(layer));
        memset(&main_info, 0, sizeof(main_info));
        memset(&shadow_info, 0, sizeof(shadow_info));
        memset(&damage_info, 0, sizeof(damage_info));
        memset(&playercolor_info, 0, sizeof(playercolor_info));
        memset(&unknown_slice, 0, sizeof(unknown_slice));
        memset(&shadow_header, 0, sizeof(shadow_header));
        memset(&damage_header, 0, sizeof(damage_header));
        memset(&playercolor_header, 0, sizeof(playercolor_header));
        shadow_present = 0;
        damage_present = 0;
        playercolor_present = 0;
        main_width = 0;
        main_height = 0;
        main_offset_x1 = 0;
        main_offset_y1 = 0;
        outlined_rect = layer_rect(0, 0, 0, 0);
        frame_end = 0;
        canvas_size = 0;

        frame_start = ftell(f);
        if (frame_start < 0)
        {
            goto cleanup;
        }
        if (fread(&sld_frame_header, sizeof(sld_frame_header_t), 1, f) != 1)
        {
            goto cleanup;
        }
        DEBUG_PRINTF("  frame %d frame_type=" BYTE_TO_BINARY_PATTERN "\n", frame_num,
                     BYTE_TO_BINARY(sld_frame_header.frame_type));

        canvas_size = sld_frame_header.canvas_width * sld_frame_header.canvas_height * 4;
        main_canvas = calloc(canvas_size, sizeof(uint8_t));
        shadow_canvas = calloc(canvas_size, sizeof(uint8_t));
        damage_canvas = calloc(canvas_size, sizeof(uint8_t));
        playercolor_canvas = calloc(canvas_size, sizeof(uint8_t));
        if (main_canvas == NULL || shadow_canvas == NULL || damage_canvas == NULL || playercolor_canvas == NULL)
        {
            goto cleanup;
        }

        if (sld_frame_header.frame_type & 1) // main graphic
        {
            memset(&layer, 0, sizeof(layer));
            if (!begin_layer(f, "main", &layer))
            {
                goto cleanup;
            }

            sld_main_header_t sld_main_header = {0};
            if (fread(&sld_main_header, sizeof(sld_main_header_t), 1, f) != 1)
            {
                goto cleanup;
            }

            DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_main_header.flag1));
            main_offset_x1 = sld_main_header.offset_x1;
            main_offset_y1 = sld_main_header.offset_y1;
            main_width = sld_main_header.offset_x2 - sld_main_header.offset_x1;
            main_height = sld_main_header.offset_y2 - sld_main_header.offset_y1;
            if (!read_layer_commands(f, &layer))
            {
                goto cleanup;
            }

            main_info.present = 1;
            main_info.layer_start = layer.read_start;
            main_info.actual_length = layer.actual_length;
            main_info.width = main_width;
            main_info.height = main_height;
            main_info.header = sld_main_header;
            main_info.command_array_length = layer.command_array_length;
            memcpy(main_info.commands, layer.commands, layer.command_array_length * sizeof(command_t));

            if (!render_blocks(f, &layer, main_width, sizeof(bc1_t), main_canvas, sld_main_header.offset_x1,
                               sld_main_header.offset_y1, sld_frame_header.canvas_width, decode_dxt1,
                               (void *)write_main_pixel))
            {
                goto cleanup;
            }
            if (!finish_layer(f, &layer))
            {
                goto cleanup;
            }
        }
        if (sld_frame_header.frame_type & 2) // shadow
        {
            memset(&layer, 0, sizeof(layer));
            if (!begin_layer(f, "shadow", &layer))
            {
                goto cleanup;
            }

            if (fread(&shadow_header, sizeof(sld_shadow_header_t), 1, f) != 1)
            {
                goto cleanup;
            }
            DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(shadow_header.flag1));
            int width = shadow_header.offset_x2 - shadow_header.offset_x1;
            if (!read_layer_commands(f, &layer))
            {
                goto cleanup;
            }

            if (!render_blocks(f, &layer, width, sizeof(bc4_t), shadow_canvas, shadow_header.offset_x1,
                               shadow_header.offset_y1, sld_frame_header.canvas_width, decode_dxt4,
                               (void *)write_shadow_pixel))
            {
                goto cleanup;
            }
            if (!finish_layer(f, &layer))
            {
                goto cleanup;
            }
            shadow_info.present = 1;
            shadow_info.layer_start = layer.read_start;
            shadow_info.actual_length = layer.actual_length;
            shadow_info.width = shadow_header.offset_x2 - shadow_header.offset_x1;
            shadow_info.height = shadow_header.offset_y2 - shadow_header.offset_y1;
            shadow_info.offset_x1 = shadow_header.offset_x1;
            shadow_info.offset_y1 = shadow_header.offset_y1;
            shadow_info.header_size = sizeof(sld_shadow_header_t);
            shadow_info.command_array_length = layer.command_array_length;
            memcpy(shadow_info.commands, layer.commands, layer.command_array_length * sizeof(command_t));
            shadow_present = 1;
        }
        if (sld_frame_header.frame_type & 4) // ???
        {
            memset(&layer, 0, sizeof(layer));
            if (!begin_layer(f, "???", &layer))
            {
                goto cleanup;
            }
            if (!finish_layer(f, &layer))
            {
                goto cleanup;
            }
            unknown_slice.start = layer.read_start;
            unknown_slice.actual_length = layer.actual_length;
            unknown_slice.present = 1;
        }
        if (sld_frame_header.frame_type & 8) // damage mask
        {
            memset(&layer, 0, sizeof(layer));
            if (!begin_layer(f, "damage", &layer))
            {
                goto cleanup;
            }
            if (fread(&damage_header, sizeof(sld_damage_mask_header_t), 1, f) != 1)
            {
                goto cleanup;
            }
            DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(damage_header.flag1));
            if (!read_layer_commands(f, &layer))
            {
                goto cleanup;
            }
            if (!render_blocks(f, &layer, main_width, sizeof(bc1_t), damage_canvas, main_offset_x1, main_offset_y1,
                               sld_frame_header.canvas_width, decode_dxt1, (void *)write_main_pixel))
            {
                goto cleanup;
            }
            if (!finish_layer(f, &layer))
            {
                goto cleanup;
            }
            damage_info.present = 1;
            damage_info.layer_start = layer.read_start;
            damage_info.actual_length = layer.actual_length;
            damage_info.width = main_width;
            damage_info.height = main_height;
            damage_info.offset_x1 = main_offset_x1;
            damage_info.offset_y1 = main_offset_y1;
            damage_info.header_size = sizeof(sld_damage_mask_header_t);
            damage_info.command_array_length = layer.command_array_length;
            memcpy(damage_info.commands, layer.commands, layer.command_array_length * sizeof(command_t));
            damage_present = 1;
        }
        if (sld_frame_header.frame_type & 16) // player color mask
        {
            memset(&layer, 0, sizeof(layer));
            if (!begin_layer(f, "playercolor", &layer))
            {
                goto cleanup;
            }

            if (fread(&playercolor_header, sizeof(sld_playercolor_mask_header_t), 1, f) != 1)
            {
                goto cleanup;
            }
            DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(playercolor_header.flag1));

            if (!read_layer_commands(f, &layer))
            {
                goto cleanup;
            }

            if (!render_blocks(f, &layer, main_width, sizeof(bc4_t), playercolor_canvas, main_offset_x1,
                               main_offset_y1, sld_frame_header.canvas_width, decode_dxt4,
                               (void *)write_playercolor_pixel))
            {
                goto cleanup;
            }
            if (!finish_layer(f, &layer))
            {
                goto cleanup;
            }
            playercolor_info.present = 1;
            playercolor_info.layer_start = layer.read_start;
            playercolor_info.actual_length = layer.actual_length;
            playercolor_info.width = main_width;
            playercolor_info.height = main_height;
            playercolor_info.offset_x1 = main_offset_x1;
            playercolor_info.offset_y1 = main_offset_y1;
            playercolor_info.header_size = sizeof(sld_playercolor_mask_header_t);
            playercolor_info.command_array_length = layer.command_array_length;
            memcpy(playercolor_info.commands, layer.commands, layer.command_array_length * sizeof(command_t));
            playercolor_present = 1;
        }

        if (main_info.present && add_outlines)
        {
            int center_x = sld_frame_header.canvas_hotspot_x;
            int center_y = sld_frame_header.canvas_hotspot_y;
            int is_delta_frame = (main_info.header.flag1 & 0x80) != 0;
            int allow_new_outline_blocks = !is_delta_frame;
            int tile_hh = get_tile_half_height(fn);
            int tile_w = get_tile_width(fn);
            int tiles_u = 1;
            int tiles_v = 1;
            int gate_compound_offsets[2][2] = {{0, 0}, {0, 0}};
            int gate_compound_count = get_gate_compound_offsets(fn, tile_hh, gate_compound_offsets);
            get_building_tiles(fn, main_width, tile_w, &tiles_u, &tiles_v);
            int margin_u = tiles_u * tile_hh;
            int margin_v = tiles_v * tile_hh;
            int left_x = 0;
            int right_x = 0;
            int top_y = 0;
            int bottom_y = 0;

            if (gate_compound_count > 0)
            {
                left_x = center_x + gate_compound_offsets[0][0] - 2 * tile_hh;
                right_x = center_x + gate_compound_offsets[0][0] + 2 * tile_hh - 1;
                top_y = center_y + gate_compound_offsets[0][1] - tile_hh;
                bottom_y = center_y + gate_compound_offsets[0][1] + tile_hh - 1;

                for (int i = 1; i < gate_compound_count; i++)
                {
                    int diamond_left = center_x + gate_compound_offsets[i][0] - 2 * tile_hh;
                    int diamond_right = center_x + gate_compound_offsets[i][0] + 2 * tile_hh - 1;
                    int diamond_top = center_y + gate_compound_offsets[i][1] - tile_hh;
                    int diamond_bottom = center_y + gate_compound_offsets[i][1] + tile_hh - 1;

                    if (diamond_left < left_x)
                    {
                        left_x = diamond_left;
                    }
                    if (diamond_right > right_x)
                    {
                        right_x = diamond_right;
                    }
                    if (diamond_top < top_y)
                    {
                        top_y = diamond_top;
                    }
                    if (diamond_bottom > bottom_y)
                    {
                        bottom_y = diamond_bottom;
                    }
                }
            }
            else
            {
                int horizontal_radius = margin_u + margin_v;
                int vertical_radius = margin_u > margin_v ? margin_u : margin_v;
                left_x = center_x - horizontal_radius;
                right_x = center_x + horizontal_radius - 1;
                top_y = center_y - vertical_radius;
                bottom_y = center_y + vertical_radius - 1;
            }

            outlined_rect = layer_rect_from_header(main_info.header);
            if (resize_layers)
            {
                if (allow_new_outline_blocks)
                {
                    // Match the Python reference: resize only for missing support blocks
                    // at the horizontal sides and below the sprite footprint. Do not grow
                    // upward to the top of the diamond, or split-layer sprites like town
                    // center slices pick up stray outline support in upper layers.
                    int expanded_x1 = left_x & ~3;
                    int expanded_x2 = (right_x + 4) & ~3;
                    int expanded_y2 = (bottom_y + 4) & ~3;

                    if (expanded_x1 < outlined_rect.x1)
                    {
                        outlined_rect.x1 = expanded_x1;
                    }
                    if (expanded_x2 > outlined_rect.x2)
                    {
                        outlined_rect.x2 = expanded_x2;
                    }
                    if (expanded_y2 > outlined_rect.y2)
                    {
                        outlined_rect.y2 = expanded_y2;
                    }
                    outlined_rect =
                        align_layer_rect(outlined_rect, sld_frame_header.canvas_width, sld_frame_header.canvas_height);
                    last_resized_rect = outlined_rect;
                    have_last_resized_rect = 1;
                }
                else if (have_last_resized_rect)
                {
                    // Delta frames must stay on the running resized grid so inherited
                    // outline support persists, but the grid still has to contain the
                    // current frame's own rect because some animations grow beyond the
                    // previous full frame.
                    outlined_rect = last_resized_rect;
                    expand_rect(&outlined_rect, layer_rect_from_header(main_info.header));
                    outlined_rect =
                        align_layer_rect(outlined_rect, sld_frame_header.canvas_width, sld_frame_header.canvas_height);
                }
            }

            int outlined_blocks_wide = (outlined_rect.x2 - outlined_rect.x1) / 4;
            int outlined_blocks_tall = (outlined_rect.y2 - outlined_rect.y1) / 4;
            uint16_t *main_outline_masks = calloc(outlined_blocks_wide * outlined_blocks_tall, sizeof(uint16_t));
            uint8_t *playercolor_dirty_blocks = calloc(outlined_blocks_wide * outlined_blocks_tall, sizeof(uint8_t));
            if (main_outline_masks == NULL || playercolor_dirty_blocks == NULL)
            {
                free(main_outline_masks);
                free(playercolor_dirty_blocks);
                goto cleanup;
            }

            for (int block_y = 0; block_y < outlined_blocks_tall; block_y++)
            {
                for (int block_x = 0; block_x < outlined_blocks_wide; block_x++)
                {
                    int idx = block_y * outlined_blocks_wide + block_x;
                    uint16_t mask = 0;
                    if (gate_compound_count > 0)
                    {
                        mask = compute_compound_outline_mask_for_block(outlined_rect, block_x, block_y, center_x,
                                                                       center_y, tile_hh, gate_compound_offsets,
                                                                       gate_compound_count, outline_width);
                    }
                    else
                    {
                        mask = compute_diamond_outline_mask_for_block(outlined_rect, block_x, block_y, center_x,
                                                                      center_y, margin_u, margin_v, outline_width);
                    }
                    main_outline_masks[idx] = mask;
                    if (playercolor_present || allow_new_outline_blocks)
                    {
                        draw_outline_mask_to_layer(playercolor_canvas, sld_frame_header.canvas_width,
                                                   sld_frame_header.canvas_height, outlined_rect, block_x, block_y,
                                                   outlined_blocks_wide, mask, 255, playercolor_dirty_blocks);
                    }
                }
            }

            if (!rebuild_modified_bc1_layer(file_bytes, &main_info, outlined_rect, main_outline_masks,
                                            allow_new_outline_blocks, &rebuilt_main))
            {
                free(main_outline_masks);
                free(playercolor_dirty_blocks);
                goto cleanup;
            }
            if (resize_layers && damage_present &&
                !rebuild_copied_bc1_layer(file_bytes, &damage_info, outlined_rect, &rebuilt_damage))
            {
                free(main_outline_masks);
                free(playercolor_dirty_blocks);
                goto cleanup;
            }
            if (playercolor_present)
            {
                if (!rebuild_modified_bc4_layer(file_bytes, playercolor_canvas, sld_frame_header.canvas_width,
                                                &playercolor_info, outlined_rect, playercolor_dirty_blocks,
                                                read_gray_value, allow_new_outline_blocks, &rebuilt_playercolor))
                {
                    free(main_outline_masks);
                    free(playercolor_dirty_blocks);
                    goto cleanup;
                }
            }
            else if (allow_new_outline_blocks)
            {
                playercolor_header.flag1 = 1;
                playercolor_header.unknown1 = 0;
                if (!rebuild_bc4_layer(playercolor_canvas, sld_frame_header.canvas_width, outlined_rect,
                                       read_gray_value, sizeof(sld_playercolor_mask_header_t), &rebuilt_playercolor))
                {
                    free(main_outline_masks);
                    free(playercolor_dirty_blocks);
                    goto cleanup;
                }
            }
            free(main_outline_masks);
            free(playercolor_dirty_blocks);
        }

        image_canvas = calloc(canvas_size, sizeof(uint8_t));
        if (image_canvas == NULL)
        {
            goto cleanup;
        }

        for (size_t idx = 0; idx < canvas_size; idx += 4)
        {
            uint8_t *dst = image_canvas + idx;
            const uint8_t *src_layers[3] = {shadow_canvas + idx, main_canvas + idx, playercolor_canvas + idx};

            for (int layer_idx = 0; layer_idx < 3; layer_idx++)
            {
                const uint8_t *src = src_layers[layer_idx];
                uint32_t src_a = src[3];
                uint32_t dst_a = dst[3];
                uint32_t out_a = src_a + ((dst_a * (255 - src_a) + 127) / 255);

                if (out_a == 0)
                {
                    dst[0] = 0;
                    dst[1] = 0;
                    dst[2] = 0;
                    dst[3] = 0;
                    continue;
                }

                for (int channel = 0; channel < 3; channel++)
                {
                    uint32_t src_premul = src[channel] * src_a;
                    uint32_t dst_premul = dst[channel] * dst_a;
                    uint32_t out_premul = src_premul + ((dst_premul * (255 - src_a) + 127) / 255);
                    dst[channel] = (uint8_t)((out_premul + out_a / 2) / out_a);
                }

                dst[3] = (uint8_t)out_a;
            }
        }

        frame_end = ftell(f);
        if (frame_end < 0)
        {
            goto cleanup;
        }

        if (export_pngs)
        {
            layer_rect_t main_rect =
                main_info.present ? layer_rect_from_header(main_info.header) : layer_rect(0, 0, 0, 0);
            layer_rect_t shadow_rect =
                shadow_present ? layer_rect_from_header(shadow_header) : layer_rect(0, 0, 0, 0);
            int playercolor_debug_present = playercolor_present || (main_info.present && add_outlines);
            layer_rect_t playercolor_rect =
                playercolor_debug_present
                    ? ((main_info.present && add_outlines)
                           ? outlined_rect
                           : layer_rect(main_offset_x1, main_offset_y1, main_offset_x1 + main_width,
                                        main_offset_y1 + main_height))
                    : layer_rect(0, 0, 0, 0);
            layer_rect_t composite_rects[3];
            int composite_rect_count = 0;

            if (main_info.present)
            {
                composite_rects[composite_rect_count++] = main_rect;
            }
            if (shadow_present)
            {
                composite_rects[composite_rect_count++] = shadow_rect;
            }
            if (playercolor_debug_present)
            {
                composite_rects[composite_rect_count++] = playercolor_rect;
            }

            output_frame_path(png, sizeof(png), stemmed, fn, sld_header.frame_count, frame_num, "_main.png");
            DEBUG_PRINTF("writing %s\n", png);
            write_debug_png_with_sprite_boxes(png, main_canvas, sld_frame_header.canvas_width,
                                              sld_frame_header.canvas_height, &main_rect, main_info.present ? 1 : 0);

            output_frame_path(png, sizeof(png), stemmed, fn, sld_header.frame_count, frame_num, "_shadow.png");
            DEBUG_PRINTF("writing %s\n", png);
            write_debug_png_with_sprite_boxes(png, shadow_canvas, sld_frame_header.canvas_width,
                                              sld_frame_header.canvas_height, &shadow_rect, shadow_present ? 1 : 0);

            output_frame_path(png, sizeof(png), stemmed, fn, sld_header.frame_count, frame_num, "_playercolor.png");
            DEBUG_PRINTF("writing %s\n", png);
            write_debug_png_with_sprite_boxes(png, playercolor_canvas, sld_frame_header.canvas_width,
                                              sld_frame_header.canvas_height, &playercolor_rect,
                                              playercolor_debug_present ? 1 : 0);

            output_frame_path(png, sizeof(png), stemmed, fn, sld_header.frame_count, frame_num, ".png");
            DEBUG_PRINTF("writing %s\n", png);
            write_debug_png_with_sprite_boxes(png, image_canvas, sld_frame_header.canvas_width,
                                              sld_frame_header.canvas_height, composite_rects, composite_rect_count);
        }

        if (main_info.present && add_outlines)
        {
            sld_frame_header_t outlined_frame_header = sld_frame_header;
            sld_main_header_t outlined_main_header = main_info.header;

            if (!outlined_output_started)
            {
                long prelude_size = frame_start - (long)sizeof(sld_header_t);

                snprintf(sld_path, sizeof(sld_path), "out/%s/%s_outlined.sld", stemmed, stemmed);
                DEBUG_PRINTF("writing %s\n", sld_path);
                outlined_out = fopen(sld_path, "wb");
                if (outlined_out == NULL)
                {
                    goto cleanup;
                }
                if (!write_bytes(outlined_out, &sld_header, sizeof(sld_header_t)))
                {
                    goto cleanup;
                }
                if (prelude_size > 0 &&
                    !write_bytes(outlined_out, file_bytes + sizeof(sld_header_t), (size_t)prelude_size))
                {
                    goto cleanup;
                }
                outlined_output_started = 1;
            }

            if (!playercolor_present && rebuilt_playercolor.command_count > 0)
            {
                outlined_frame_header.frame_type |= 16;
            }
            if (!write_bytes(outlined_out, &outlined_frame_header, sizeof(sld_frame_header_t)))
            {
                goto cleanup;
            }
            DEBUG_PRINTF("    write frame %d header -> %ld\n", frame_num, ftell(outlined_out));

            outlined_main_header.offset_x1 = (uint16_t)outlined_rect.x1;
            outlined_main_header.offset_y1 = (uint16_t)outlined_rect.y1;
            outlined_main_header.offset_x2 = (uint16_t)outlined_rect.x2;
            outlined_main_header.offset_y2 = (uint16_t)outlined_rect.y2;

            if (!write_rebuilt_layer(outlined_out, &rebuilt_main, &outlined_main_header, sizeof(sld_main_header_t)))
            {
                goto cleanup;
            }
            DEBUG_PRINTF("    write frame %d main len=%u blocks=%d cmds=%d -> %ld\n", frame_num,
                         rebuilt_main.content_length, rebuilt_main.block_count, rebuilt_main.command_count,
                         ftell(outlined_out));

            if (shadow_present &&
                     !write_bytes(outlined_out, file_bytes + shadow_info.layer_start, (size_t)shadow_info.actual_length))
            {
                goto cleanup;
            }
            if (unknown_slice.present &&
                !write_bytes(outlined_out, file_bytes + unknown_slice.start, (size_t)unknown_slice.actual_length))
            {
                goto cleanup;
            }
            if (resize_layers && damage_present &&
                !write_rebuilt_layer(outlined_out, &rebuilt_damage, &damage_header, sizeof(damage_header)))
            {
                goto cleanup;
            }
            if (resize_layers && damage_present)
            {
                DEBUG_PRINTF("    write frame %d damage len=%u blocks=%d cmds=%d -> %ld\n", frame_num,
                             rebuilt_damage.content_length, rebuilt_damage.block_count, rebuilt_damage.command_count,
                             ftell(outlined_out));
            }
            else if (damage_present &&
                     !write_bytes(outlined_out, file_bytes + damage_info.layer_start, (size_t)damage_info.actual_length))
            {
                goto cleanup;
            }
            if ((playercolor_present || rebuilt_playercolor.command_count > 0) &&
                !write_rebuilt_layer(outlined_out, &rebuilt_playercolor, &playercolor_header, sizeof(playercolor_header)))
            {
                goto cleanup;
            }
            if (playercolor_present || rebuilt_playercolor.command_count > 0)
            {
                DEBUG_PRINTF("    write frame %d playercolor len=%u blocks=%d cmds=%d -> %ld\n", frame_num,
                             rebuilt_playercolor.content_length, rebuilt_playercolor.block_count,
                             rebuilt_playercolor.command_count, ftell(outlined_out));
            }
        }
        else if (outlined_output_started &&
                 !write_bytes(outlined_out, file_bytes + frame_start, (size_t)(frame_end - frame_start)))
        {
            goto cleanup;
        }

        free(image_canvas);
        image_canvas = NULL;
        free(main_canvas);
        main_canvas = NULL;
        free(shadow_canvas);
        shadow_canvas = NULL;
        free(damage_canvas);
        damage_canvas = NULL;
        free(playercolor_canvas);
        playercolor_canvas = NULL;
        free_rebuilt_layer(&rebuilt_main);
        free_rebuilt_layer(&rebuilt_shadow);
        free_rebuilt_layer(&rebuilt_damage);
        free_rebuilt_layer(&rebuilt_playercolor);
    }

    final_pos = ftell(f);
    if (final_pos < 0)
    {
        goto cleanup;
    }
    DEBUG_PRINTF("file read check: %s (read=%ld size=%ld)\n", final_pos == input_file_size ? "complete" : "incomplete",
                 final_pos, input_file_size);

    if (outlined_output_started && final_pos < input_file_size &&
        !write_bytes(outlined_out, file_bytes + final_pos, (size_t)(input_file_size - final_pos)))
    {
        goto cleanup;
    }

    status = 0;

cleanup:
    free(image_canvas);
    free(main_canvas);
    free(shadow_canvas);
    free(damage_canvas);
    free(playercolor_canvas);
    free_rebuilt_layer(&rebuilt_main);
    free_rebuilt_layer(&rebuilt_shadow);
    free_rebuilt_layer(&rebuilt_damage);
    free_rebuilt_layer(&rebuilt_playercolor);
    if (out != NULL)
    {
        fclose(out);
    }
    if (outlined_out != NULL)
    {
        fclose(outlined_out);
    }
    if (f != NULL)
    {
        fclose(f);
    }
    free(file_bytes);

    return status;
}
