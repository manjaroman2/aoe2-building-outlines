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

typedef void (*dxt4_pixel_writer_t)(uint8_t *pixel, uint8_t c);

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
    layer->read_start = ftell(f);
    if (fread(&layer->content_length, sizeof(uint32_t), 1, f) != 1)
    {
        return 0;
    }

    layer->actual_length = (layer->content_length + 3) & ~3;
    printf("    %s\n      sld_layer_length=%d\n      actual_length=%d\n", name, layer->content_length,
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
    printf("      command_array_length=%d\n", layer->command_array_length);
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
    printf("      null bytes %ld\n", layer->actual_length - read);
    return fseek(f, layer->actual_length - read, SEEK_CUR) == 0;
}

void decode_dxt4(bc4_t block, int block_x, int block_y, uint8_t *canvas, uint16_t offset_x1, uint16_t offset_y1,
                 uint16_t canvas_width, dxt4_pixel_writer_t write_pixel)
{
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

void decode_dxt1(bc1_t block, int block_x, int block_y, uint8_t *canvas, uint16_t offset_x1, uint16_t offset_y1,
                 uint16_t canvas_width)
{
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
        canvas[canvas_idx + 0] = ((c >> 11) & 0x1F) * 255 / 31;
        canvas[canvas_idx + 1] = ((c >> 5) & 0x3F) * 255 / 63;
        canvas[canvas_idx + 2] = (c & 0x1F) * 255 / 31;
        canvas[canvas_idx + 3] = (colors[0] <= colors[1] && color_idx == 3) ? 0 : 255;
    }
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
    printf("%s\n", fn);

    sld_header_t sld_header = {0};
    if (fread(&sld_header, sizeof(sld_header_t), 1, f) != 1)
    {
        return 1;
    }

    printf("%.4s version=%d\nframes=%d\n", sld_header.signature, sld_header.version, sld_header.frame_count);

    sld_frame_header_t sld_frame_header = {0};
    if (fread(&sld_frame_header, sizeof(sld_frame_header_t), 1, f) != 1)
    {
        return 1;
    }
    printf("  frame_type=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_frame_header.frame_type));

    size_t canvas_size = sld_frame_header.canvas_width * sld_frame_header.canvas_height * 4;
    uint8_t *main_canvas = calloc(canvas_size, sizeof(uint8_t));
    uint8_t *shadow_canvas = calloc(canvas_size, sizeof(uint8_t));
    uint8_t *playercolor_canvas = calloc(canvas_size, sizeof(uint8_t));

    int main_width = 0;
    uint16_t main_offset_x1 = 0, main_offset_y1 = 0;

    if (sld_frame_header.frame_type & 1) // main graphic
    {
        sld_layer_t layer = {0};
        if (!begin_layer(f, "main", &layer))
        {
            return 1;
        }

        sld_main_header_t sld_main_header = {0};
        if (fread(&sld_main_header, sizeof(sld_main_header_t), 1, f) != 1)
        {
            return 1;
        }

        printf("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_main_header.flag1));
        main_offset_x1 = sld_main_header.offset_x1;
        main_offset_y1 = sld_main_header.offset_y1;
        main_width = sld_main_header.offset_x2 - sld_main_header.offset_x1;
        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        int blocks_wide = main_width / 4;
        // printf("width=%d height=%d\n", width, height);
        // printf("blocks=%dx%d\n", blocks_wide, blocks_tall);

        int block_x = 0;
        int block_y = 0;
        for (int i = 0; i < layer.command_array_length; i++)
        {
            command_t cmd = layer.commands[i];

            int drawn = cmd.drawn;
            int skipped = cmd.skipped;
            // printf("command %d %d\n", drawn, skipped);

            block_y += (block_x + skipped) / blocks_wide;
            block_x = (block_x + skipped) % blocks_wide;

            for (int j = 0; j < drawn; j++)
            {
                bc1_t block;
                if (fread(&block, sizeof(bc1_t), 1, f) != 1)
                {
                    printf("block err\n");
                    return 1;
                }
                decode_dxt1(block, block_x, block_y, main_canvas, sld_main_header.offset_x1, sld_main_header.offset_y1,
                            sld_frame_header.canvas_width);

                block_y += (block_x + 1) / blocks_wide;
                block_x = (block_x + 1) % blocks_wide;
            }
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 2) // shadow
    {
        sld_layer_t layer = {0};
        if (!begin_layer(f, "shadow", &layer))
        {
            return 1;
        }

        sld_shadow_header_t sld_shadow_header = {0};
        if (fread(&sld_shadow_header, sizeof(sld_shadow_header_t), 1, f) != 1)
        {
            return 1;
        }
        printf("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_shadow_header.flag1));
        int width = sld_shadow_header.offset_x2 - sld_shadow_header.offset_x1;
        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        int blocks_wide = width / 4;
        // printf("width=%d height=%d\n", width, height);
        // printf("blocks=%dx%d\n", blocks_wide, blocks_tall);

        int block_x = 0;
        int block_y = 0;
        for (int i = 0; i < layer.command_array_length; i++)
        {
            command_t cmd = layer.commands[i];

            int drawn = cmd.drawn;
            int skipped = cmd.skipped;
            // printf("command %d %d\n", drawn, skipped);

            block_y += (block_x + skipped) / blocks_wide;
            block_x = (block_x + skipped) % blocks_wide;

            for (int j = 0; j < drawn; j++)
            {
                bc4_t block;
                if (fread(&block, sizeof(bc4_t), 1, f) != 1)
                {
                    printf("block err\n");
                    return 1;
                }

                decode_dxt4(block, block_x, block_y, shadow_canvas, sld_shadow_header.offset_x1,
                            sld_shadow_header.offset_y1, sld_frame_header.canvas_width, write_shadow_pixel);

                block_y += (block_x + 1) / blocks_wide;
                block_x = (block_x + 1) % blocks_wide;
            }
        }
        if (!finish_layer(f, &layer))
        {
            return 1;
        }
    }
    if (sld_frame_header.frame_type & 4) // ???
    {
        sld_layer_t layer = {0};
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
        sld_layer_t layer = {0};
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

        sld_layer_t layer = {0};
        if (!begin_layer(f, "playercolor", &layer))
        {
            return 1;
        }

        sld_playercolor_mask_header_t sld_playercolor_header = {0};
        if (fread(&sld_playercolor_header, sizeof(sld_playercolor_mask_header_t), 1, f) != 1)
        {
            return 1;
        }
        printf("      flag1=" BYTE_TO_BINARY_PATTERN "\n", BYTE_TO_BINARY(sld_playercolor_header.flag1));

        if (!read_layer_commands(f, &layer))
        {
            return 1;
        }

        int blocks_wide = main_width / 4;
        // printf("width=%d height=%d\n", width, height);
        // printf("blocks=%dx%d\n", blocks_wide, blocks_tall);

        int block_x = 0;
        int block_y = 0;
        for (int i = 0; i < layer.command_array_length; i++)
        {
            command_t cmd = layer.commands[i];

            int drawn = cmd.drawn;
            int skipped = cmd.skipped;
            // printf("command %d %d\n", drawn, skipped);

            block_y += (block_x + skipped) / blocks_wide;
            block_x = (block_x + skipped) % blocks_wide;

            for (int j = 0; j < drawn; j++)
            {
                bc4_t block;
                if (fread(&block, sizeof(bc4_t), 1, f) != 1)
                {
                    printf("block err\n");
                    return 1;
                }

                decode_dxt4(block, block_x, block_y, playercolor_canvas, main_offset_x1, main_offset_y1,
                            sld_frame_header.canvas_width, write_playercolor_pixel);

                block_y += (block_x + 1) / blocks_wide;
                block_x = (block_x + 1) % blocks_wide;
            }
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

    char png[128];
    mkdir("out", 0755);

    snprintf(png, sizeof(png), "out/%s_main.png", fn);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, main_canvas,
                   sld_frame_header.canvas_width * 4);

    snprintf(png, sizeof(png), "out/%s_shadow.png", fn);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, shadow_canvas,
                   sld_frame_header.canvas_width * 4);

    snprintf(png, sizeof(png), "out/%s_playercolor.png", fn);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, playercolor_canvas,
                   sld_frame_header.canvas_width * 4);

    snprintf(png, sizeof(png), "out/%s.png", fn);
    stbi_write_png(png, sld_frame_header.canvas_width, sld_frame_header.canvas_height, 4, image_canvas,
                   sld_frame_header.canvas_width * 4);

    free(image_canvas);
    free(main_canvas);
    free(shadow_canvas);
    free(playercolor_canvas);

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
    printf("file read check: %s (read=%ld size=%ld)\n", final_pos == file_size ? "complete" : "incomplete", final_pos,
           file_size);

    fclose(f);
    return 0;
}
