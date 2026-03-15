#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
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

void output_path(char *out, size_t out_size, const char *stemmed, const char *filename, const char *suffix)
{
    snprintf(out, out_size, "out/%s/%s%s", stemmed, filename, suffix);
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
    pixel[0] = 0;
    pixel[1] = 0;
    pixel[2] = c;
    pixel[3] = c == 0 ? 0 : 255;
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
    if (argc != 2)
    {
        printf("usage\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL)
    {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    char *fn = filename(argv[1]);
    char stemmed[200];
    stem(argv[1], stemmed, sizeof(stemmed));
    DEBUG_PRINTF("%s\n", fn);

    sld_header_t sld_header = {0};
    if (fread(&sld_header, sizeof(sld_header_t), 1, f) != 1)
    {
        return 1;
    }

    DEBUG_PRINTF("%.4s version=%d\nframes=%d\n", sld_header.signature, sld_header.version, sld_header.frame_count);

    sld_frame_header_t sld_frame_header = {0};
    if (fread(&sld_frame_header, sizeof(sld_frame_header_t), 1, f) != 1)
    {
        return 1;
    }
    DEBUG_PRINTF("  frame_type=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_frame_header.frame_type));

    size_t canvas_size = sld_frame_header.canvas_width * sld_frame_header.canvas_height * 4;
    uint8_t *main_canvas = calloc(canvas_size, sizeof(uint8_t));
    uint8_t *shadow_canvas = calloc(canvas_size, sizeof(uint8_t));
    uint8_t *playercolor_canvas = calloc(canvas_size, sizeof(uint8_t));
    sld_layer_t layer = {0};

    int main_width = 0;
    uint16_t main_offset_x1 = 0, main_offset_y1 = 0, main_offset_x2 = 0;

    if (sld_frame_header.frame_type & 1) // main graphic
    {
        memset(&layer, 0, sizeof(layer));
        if (!begin_layer(f, "main", &layer))
        {
            return 1;
        }

        sld_main_header_t sld_main_header = {0};
        if (fread(&sld_main_header, sizeof(sld_main_header_t), 1, f) != 1)
        {
            return 1;
        }

        DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_main_header.flag1));
        main_offset_x1 = sld_main_header.offset_x1;
        main_offset_y1 = sld_main_header.offset_y1;
        main_offset_x2 = sld_main_header.offset_x2;
        main_width = sld_main_header.offset_x2 - sld_main_header.offset_x1;
        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        if (!render_blocks(f, &layer, main_width, sizeof(bc1_t), main_canvas, sld_main_header.offset_x1,
                           sld_main_header.offset_y1, sld_frame_header.canvas_width, decode_dxt1,
                           (void *)write_main_pixel))
        {
            return 1;
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 2) // shadow
    {
        memset(&layer, 0, sizeof(layer));
        if (!begin_layer(f, "shadow", &layer))
        {
            return 1;
        }

        sld_shadow_header_t sld_shadow_header = {0};
        if (fread(&sld_shadow_header, sizeof(sld_shadow_header_t), 1, f) != 1)
        {
            return 1;
        }
        DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_shadow_header.flag1));
        int width = sld_shadow_header.offset_x2 - sld_shadow_header.offset_x1;
        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        if (!render_blocks(f, &layer, width, sizeof(bc4_t), shadow_canvas, sld_shadow_header.offset_x1,
                           sld_shadow_header.offset_y1, sld_frame_header.canvas_width, decode_dxt4,
                           (void *)write_shadow_pixel))
        {
            return 1;
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 4) // ???
    {
        memset(&layer, 0, sizeof(layer));
        if (!begin_layer(f, "???", &layer))
        {
            return 1;
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 8) // damage mask
    {
        memset(&layer, 0, sizeof(layer));
        if (!begin_layer(f, "damage", &layer))
        {
            return 1;
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 16) // player color mask
    {
        if ((sld_frame_header.frame_type & 1) == 0)
        {
            printf("playercolor mask requires main graphic layer\n");
            return 1;
        }

        memset(&layer, 0, sizeof(layer));
        if (!begin_layer(f, "playercolor", &layer))
        {
            return 1;
        }

        sld_playercolor_mask_header_t sld_playercolor_header = {0};
        if (fread(&sld_playercolor_header, sizeof(sld_playercolor_mask_header_t), 1, f) != 1)
        {
            return 1;
        }
        DEBUG_PRINTF("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_playercolor_header.flag1));

        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        if (!render_blocks(f, &layer, main_width, sizeof(bc4_t), playercolor_canvas, main_offset_x1, main_offset_y1,
                           sld_frame_header.canvas_width, decode_dxt4, (void *)write_playercolor_pixel))
        {
            return 1;
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }

    uint8_t *image_canvas = calloc(canvas_size, sizeof(uint8_t));

    for (size_t idx = 0; idx < canvas_size; idx += 4)
    {
        uint8_t *dst = image_canvas + idx;
        const uint8_t *src_layers[3] = {shadow_canvas + idx, main_canvas + idx, playercolor_canvas + idx};

        for (int layer = 0; layer < 3; layer++)
        {
            const uint8_t *src = src_layers[layer];
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

    long final_pos = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0)
    {
        return 1;
    }
    long file_size = ftell(f);
    if (file_size < 0 || final_pos < 0)
    {
        return 1;
    }
    DEBUG_PRINTF("file read check: %s (read=%ld size=%ld)\n", final_pos == file_size ? "complete" : "incomplete",
                 final_pos, file_size);
    fclose(f);

    char png[256];
    char out_dir[256];
    mkdir("out", 0755);
    snprintf(out_dir, sizeof(out_dir), "out/%s", stemmed);
    mkdir(out_dir, 0755);

    output_path(png, sizeof(png), stemmed, fn, "_main.png");
    DEBUG_PRINTF("writing %s\n", png);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, main_canvas,
                   sld_frame_header.canvas_width * 4);

    output_path(png, sizeof(png), stemmed, fn, "_shadow.png");
    DEBUG_PRINTF("writing %s\n", png);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, shadow_canvas,
                   sld_frame_header.canvas_width * 4);

    output_path(png, sizeof(png), stemmed, fn, "_playercolor.png");
    DEBUG_PRINTF("writing %s\n", png);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, playercolor_canvas,
                   sld_frame_header.canvas_width * 4);

    output_path(png, sizeof(png), stemmed, fn, ".png");
    if (sld_frame_header.frame_type & 1)
    {
        int left_x = main_offset_x1;
        int right_x = main_offset_x2 - 1;
        int center_y = sld_frame_header.canvas_hotspot_y;
        int mid_x = (left_x + right_x) / 2;
        int half_height = (right_x - left_x) / 4;
        int top_y = center_y - half_height;
        int bottom_y = center_y + half_height;

        draw_line(image_canvas, sld_frame_header.canvas_width, sld_frame_header.canvas_height, left_x, center_y, mid_x,
                  top_y, 0, 0, 255);
        draw_line(image_canvas, sld_frame_header.canvas_width, sld_frame_header.canvas_height, mid_x, top_y, right_x,
                  center_y, 0, 0, 255);
        draw_line(image_canvas, sld_frame_header.canvas_width, sld_frame_header.canvas_height, right_x, center_y, mid_x,
                  bottom_y, 0, 0, 255);
        draw_line(image_canvas, sld_frame_header.canvas_width, sld_frame_header.canvas_height, mid_x, bottom_y, left_x,
                  center_y, 0, 0, 255);
    }
    DEBUG_PRINTF("writing %s\n", png);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, image_canvas,
                   sld_frame_header.canvas_width * 4);

    free(image_canvas);
    free(main_canvas);
    free(shadow_canvas);
    free(playercolor_canvas);

    return 0;
}
