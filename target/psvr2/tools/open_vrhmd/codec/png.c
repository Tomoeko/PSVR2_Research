#include "png.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PNG_MAX_DIMENSION = 8192,
    PNG_MAX_COMPRESSED = 128 * 1024 * 1024,
    PNG_MAX_DECOMPRESSED = 128 * 1024 * 1024,
    PNG_MAX_DECODE_BYTES = 192 * 1024 * 1024,
    DEFLATE_TABLE_BITS = 15,
    DEFLATE_TABLE_SIZE = 1 << DEFLATE_TABLE_BITS
};

typedef struct BitStream {
    const uint8_t *data;
    size_t size;
    size_t bit;
} BitStream;

typedef struct HuffmanEntry {
    uint16_t symbol;
    uint8_t length;
} HuffmanEntry;

typedef struct Huffman {
    HuffmanEntry *entries;
} Huffman;

static uint32_t be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size) {
    for (size_t index = 0; index < size; index++) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; bit++)
            crc = crc & 1 ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
    }
    return crc;
}

static uint32_t adler32(const uint8_t *data, size_t size) {
    uint32_t first = 1;
    uint32_t second = 0;
    for (size_t index = 0; index < size; index++) {
        first = (first + data[index]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

static bool read_bits(BitStream *stream, unsigned count, unsigned *value) {
    if (count > 16 || stream->bit > stream->size * 8 ||
        count > stream->size * 8 - stream->bit) return false;
    unsigned result = 0;
    for (unsigned index = 0; index < count; index++) {
        size_t position = stream->bit++;
        result |= (unsigned)((stream->data[position / 8] >>
                              (position % 8)) & 1u) << index;
    }
    *value = result;
    return true;
}

static unsigned reverse_bits(unsigned code, unsigned width) {
    unsigned reversed = 0;
    for (unsigned index = 0; index < width; index++) {
        reversed = (reversed << 1) | (code & 1u);
        code >>= 1;
    }
    return reversed;
}

static bool build_huffman(Huffman *table, const uint8_t *lengths,
                          unsigned count) {
    unsigned totals[16] = {0};
    unsigned next_code[16] = {0};
    for (unsigned symbol = 0; symbol < count; symbol++) {
        if (lengths[symbol] > DEFLATE_TABLE_BITS) return false;
        totals[lengths[symbol]]++;
    }
    unsigned code = 0;
    for (unsigned width = 1; width <= DEFLATE_TABLE_BITS; width++) {
        unsigned previous = width == 1 ? 0 : totals[width - 1];
        code = (code + previous) << 1;
        next_code[width] = code;
        if (code + totals[width] > (1u << width)) return false;
    }
    table->entries = calloc(DEFLATE_TABLE_SIZE, sizeof(*table->entries));
    if (!table->entries) return false;
    for (unsigned symbol = 0; symbol < count; symbol++) {
        unsigned width = lengths[symbol];
        if (!width) continue;
        unsigned reversed = reverse_bits(next_code[width]++, width);
        for (unsigned index = reversed; index < DEFLATE_TABLE_SIZE;
             index += 1u << width) {
            table->entries[index].symbol = (uint16_t)symbol;
            table->entries[index].length = (uint8_t)width;
        }
    }
    return true;
}

static bool huffman_symbol(BitStream *stream, const Huffman *table,
                           unsigned *symbol) {
    if (!table->entries || stream->bit >= stream->size * 8) return false;
    unsigned index = 0;
    size_t available = stream->size * 8 - stream->bit;
    unsigned bits = available < DEFLATE_TABLE_BITS
        ? (unsigned)available : DEFLATE_TABLE_BITS;
    for (unsigned offset = 0; offset < bits; offset++) {
        size_t position = stream->bit + offset;
        index |= (unsigned)((stream->data[position / 8] >>
                             (position % 8)) & 1u) << offset;
    }
    HuffmanEntry entry = table->entries[index];
    if (!entry.length || entry.length > available) return false;
    stream->bit += entry.length;
    *symbol = entry.symbol;
    return true;
}

static bool dynamic_tables(BitStream *stream, Huffman *literal,
                           Huffman *distance) {
    static const unsigned order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned h_lit, h_dist, h_code;
    if (!read_bits(stream, 5, &h_lit) ||
        !read_bits(stream, 5, &h_dist) ||
        !read_bits(stream, 4, &h_code)) return false;
    h_lit += 257;
    h_dist += 1;
    h_code += 4;
    if (h_lit > 286 || h_dist > 32) return false;
    uint8_t code_lengths[19] = {0};
    for (unsigned index = 0; index < h_code; index++) {
        unsigned length;
        if (!read_bits(stream, 3, &length)) return false;
        code_lengths[order[index]] = (uint8_t)length;
    }
    Huffman code_table = {0};
    if (!build_huffman(&code_table, code_lengths, 19)) return false;
    uint8_t lengths[286 + 32] = {0};
    unsigned total = h_lit + h_dist;
    unsigned used = 0;
    bool valid = true;
    while (used < total) {
        unsigned symbol;
        if (!huffman_symbol(stream, &code_table, &symbol)) {
            valid = false;
            break;
        }
        if (symbol <= 15) {
            lengths[used++] = (uint8_t)symbol;
            continue;
        }
        unsigned extra = symbol == 16 ? 2 : symbol == 17 ? 3 : 7;
        unsigned base = symbol == 16 ? 3 : symbol == 17 ? 3 : 11;
        unsigned value;
        if (symbol > 18 || (symbol == 16 && used == 0) ||
            !read_bits(stream, extra, &value) ||
            base + value > total - used) {
            valid = false;
            break;
        }
        uint8_t length = symbol == 16 ? lengths[used - 1] : 0;
        for (unsigned repeat = 0; repeat < base + value; repeat++)
            lengths[used++] = length;
    }
    free(code_table.entries);
    if (!valid || !lengths[256] ||
        !build_huffman(literal, lengths, h_lit) ||
        !build_huffman(distance, lengths + h_lit, h_dist)) return false;
    return true;
}

static bool fixed_tables(Huffman *literal, Huffman *distance) {
    uint8_t lengths[288];
    for (unsigned symbol = 0; symbol < 288; symbol++)
        lengths[symbol] = symbol <= 143 ? 8 : symbol <= 255 ? 9
                          : symbol <= 279 ? 7 : 8;
    uint8_t distances[32];
    memset(distances, 5, sizeof(distances));
    return build_huffman(literal, lengths, 288) &&
           build_huffman(distance, distances, 32);
}

static bool inflate_block(BitStream *stream, uint8_t *output,
                          size_t capacity, size_t *used,
                          const Huffman *literal,
                          const Huffman *distance) {
    static const uint16_t length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
        15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
        67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
        1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
        4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
        33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
        16385, 24577
    };
    static const uint8_t distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
        4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
        9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    for (;;) {
        unsigned symbol;
        if (!huffman_symbol(stream, literal, &symbol)) return false;
        if (symbol < 256) {
            if (*used >= capacity) return false;
            output[(*used)++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return true;
        } else {
            if (symbol > 285) return false;
            unsigned length_index = symbol - 257;
            unsigned extra_length;
            unsigned distance_symbol;
            if (!read_bits(stream, length_extra[length_index],
                           &extra_length) ||
                !huffman_symbol(stream, distance, &distance_symbol) ||
                distance_symbol >= 30) return false;
            unsigned extra_distance;
            if (!read_bits(stream, distance_extra[distance_symbol],
                           &extra_distance)) return false;
            size_t length = length_base[length_index] + extra_length;
            size_t offset = distance_base[distance_symbol] + extra_distance;
            if (offset > *used || length > capacity - *used) return false;
            for (size_t index = 0; index < length; index++) {
                output[*used] = output[*used - offset];
                (*used)++;
            }
        }
    }
}

static bool inflate_zlib(const uint8_t *compressed, size_t size,
                         uint8_t *output, size_t expected) {
    if (size < 6 || (compressed[0] & 15u) != 8 ||
        (compressed[0] >> 4) > 7 || (compressed[1] & 0x20u) != 0 ||
        (((unsigned)compressed[0] << 8) | compressed[1]) % 31 != 0)
        return false;
    BitStream stream = {
        .data = compressed + 2,
        .size = size - 6
    };
    size_t used = 0;
    bool final = false;
    while (!final) {
        unsigned last, type;
        if (!read_bits(&stream, 1, &last) ||
            !read_bits(&stream, 2, &type)) return false;
        final = last != 0;
        if (type == 0) {
            stream.bit = (stream.bit + 7u) & ~(size_t)7u;
            unsigned length, complement;
            if (!read_bits(&stream, 16, &length) ||
                !read_bits(&stream, 16, &complement) ||
                ((length ^ complement) & 0xffffu) != 0xffffu ||
                length > expected - used) return false;
            for (unsigned index = 0; index < length; index++) {
                unsigned value;
                if (!read_bits(&stream, 8, &value)) return false;
                output[used++] = (uint8_t)value;
            }
        } else if (type == 1 || type == 2) {
            Huffman literal = {0};
            Huffman distance = {0};
            bool valid = type == 1
                ? fixed_tables(&literal, &distance)
                : dynamic_tables(&stream, &literal, &distance);
            if (valid)
                valid = inflate_block(&stream, output, expected, &used,
                                      &literal, &distance);
            free(literal.entries);
            free(distance.entries);
            if (!valid) return false;
        } else {
            return false;
        }
    }
    return used == expected && (stream.bit + 7) / 8 == stream.size &&
           adler32(output, expected) == be32(compressed + size - 4);
}

static uint8_t paeth(uint8_t left, uint8_t above, uint8_t diagonal) {
    int prediction = (int)left + above - diagonal;
    int left_distance = abs(prediction - left);
    int above_distance = abs(prediction - above);
    int diagonal_distance = abs(prediction - diagonal);
    if (left_distance <= above_distance &&
        left_distance <= diagonal_distance) return left;
    return above_distance <= diagonal_distance ? above : diagonal;
}

typedef struct PngInfo {
    uint32_t width;
    uint32_t height;
    unsigned depth;
    unsigned color;
    unsigned channels;
    bool interlaced;
    unsigned palette_count;
    uint8_t palette[256][4];
    bool transparency;
    uint16_t transparent[3];
} PngInfo;

static uint16_t be16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static bool valid_header(const uint8_t *body, PngInfo *info) {
    info->width = be32(body);
    info->height = be32(body + 4);
    info->depth = body[8];
    info->color = body[9];
    info->interlaced = body[12] == 1;
    if (!info->width || !info->height ||
        info->width > PNG_MAX_DIMENSION || info->height > PNG_MAX_DIMENSION ||
        body[10] || body[11] || body[12] > 1) return false;
    if ((uint64_t)info->width * info->height * 4 > PNG_MAX_DECOMPRESSED)
        return false;
    switch (info->color) {
    case 0:
        info->channels = 1;
        return info->depth == 1 || info->depth == 2 || info->depth == 4 ||
               info->depth == 8 || info->depth == 16;
    case 3:
        info->channels = 1;
        return info->depth == 1 || info->depth == 2 ||
               info->depth == 4 || info->depth == 8;
    case 2: info->channels = 3; break;
    case 4: info->channels = 2; break;
    case 6: info->channels = 4; break;
    default: return false;
    }
    return info->depth == 8 || info->depth == 16;
}

static void pass_geometry(const PngInfo *info, unsigned pass,
                          uint32_t *left, uint32_t *top,
                          uint32_t *step_x, uint32_t *step_y,
                          uint32_t *width, uint32_t *height) {
    static const uint8_t origins_x[7] = {0, 4, 0, 2, 0, 1, 0};
    static const uint8_t origins_y[7] = {0, 0, 4, 0, 2, 0, 1};
    static const uint8_t steps_x[7] = {8, 8, 4, 4, 2, 2, 1};
    static const uint8_t steps_y[7] = {8, 8, 8, 4, 4, 2, 2};
    *left = info->interlaced ? origins_x[pass] : 0;
    *top = info->interlaced ? origins_y[pass] : 0;
    *step_x = info->interlaced ? steps_x[pass] : 1;
    *step_y = info->interlaced ? steps_y[pass] : 1;
    *width = info->width > *left ?
        (info->width - *left + *step_x - 1) / *step_x : 0;
    *height = info->height > *top ?
        (info->height - *top + *step_y - 1) / *step_y : 0;
}

static size_t filtered_size(const PngInfo *info) {
    size_t total = 0;
    unsigned passes = info->interlaced ? 7 : 1;
    for (unsigned pass = 0; pass < passes; ++pass) {
        uint32_t left, top, step_x, step_y, width, height;
        pass_geometry(info, pass, &left, &top, &step_x, &step_y, &width, &height);
        if (!width || !height) continue;
        size_t row = ((size_t)width * info->channels * info->depth + 7) / 8;
        size_t bytes = (row + 1) * height;
        if (bytes > PNG_MAX_DECOMPRESSED - total) return 0;
        total += bytes;
    }
    return total;
}

static unsigned sample_value(const uint8_t *row, size_t sample, unsigned depth) {
    if (depth == 16) return be16(row + sample * 2);
    if (depth == 8) return row[sample];
    size_t bit = sample * depth;
    return (row[bit / 8] >> (8 - depth - (unsigned)(bit % 8))) &
           ((1u << depth) - 1);
}

static uint8_t sample_byte(unsigned sample, unsigned depth) {
    if (depth == 16) return (uint8_t)(sample >> 8);
    return (uint8_t)(sample * 255u / ((1u << depth) - 1));
}

static bool rgba_pixel(const PngInfo *info, const uint8_t *row,
                       uint32_t x, uint8_t *pixel) {
    unsigned values[4] = {0, 0, 0, 0};
    for (unsigned channel = 0; channel < info->channels; ++channel)
        values[channel] = sample_value(row, (size_t)x * info->channels + channel,
                                      info->depth);
    pixel[3] = 255;
    if (info->color == 3) {
        if (values[0] >= info->palette_count) return false;
        memcpy(pixel, info->palette[values[0]], 4);
    } else if (info->color == 0 || info->color == 4) {
        pixel[0] = pixel[1] = pixel[2] = sample_byte(values[0], info->depth);
        if (info->color == 4) pixel[3] = sample_byte(values[1], info->depth);
        else if (info->transparency && values[0] == info->transparent[0]) pixel[3] = 0;
    } else {
        for (unsigned channel = 0; channel < 3; ++channel)
            pixel[channel] = sample_byte(values[channel], info->depth);
        if (info->color == 6) pixel[3] = sample_byte(values[3], info->depth);
        else if (info->transparency && values[0] == info->transparent[0] &&
                 values[1] == info->transparent[1] &&
                 values[2] == info->transparent[2]) pixel[3] = 0;
    }
    return true;
}

static bool reconstruct(const PngInfo *info, const uint8_t *filtered,
                        Psvr2PngImage *image) {
    size_t position = 0;
    unsigned passes = info->interlaced ? 7 : 1;
    unsigned pixel_bytes = (info->channels * info->depth + 7) / 8;
    for (unsigned pass = 0; pass < passes; ++pass) {
        uint32_t left, top, step_x, step_y, width, height;
        pass_geometry(info, pass, &left, &top, &step_x, &step_y, &width, &height);
        if (!width || !height) continue;
        size_t row_size = ((size_t)width * info->channels * info->depth + 7) / 8;
        uint8_t *rows = calloc(2, row_size);
        if (!rows) return false;
        uint8_t *row = rows, *previous = rows + row_size;
        bool valid = true;
        for (uint32_t y = 0; y < height && valid; ++y) {
            unsigned filter = filtered[position++];
            if (filter > 4) { valid = false; break; }
            for (size_t byte = 0; byte < row_size; ++byte) {
                uint8_t a = byte >= pixel_bytes ? row[byte - pixel_bytes] : 0;
                uint8_t b = previous[byte];
                uint8_t c = byte >= pixel_bytes ? previous[byte - pixel_bytes] : 0;
                uint8_t predictor = filter == 1 ? a : filter == 2 ? b :
                    filter == 3 ? (uint8_t)(((unsigned)a + b) / 2) :
                    filter == 4 ? paeth(a, b, c) : 0;
                row[byte] = (uint8_t)(filtered[position++] + predictor);
            }
            for (uint32_t x = 0; x < width && valid; ++x) {
                size_t index = ((size_t)(top + y * step_y) * info->width +
                                left + x * step_x) * 4;
                valid = rgba_pixel(info, row, x, image->pixels + index);
            }
            uint8_t *swap = row; row = previous; previous = swap;
        }
        free(rows);
        if (!valid) return false;
    }
    return true;
}

void psvr2_png_free(Psvr2PngImage *image) {
    if (!image) return;
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}

bool psvr2_png_decode(const uint8_t *data, size_t size, Psvr2PngImage *image) {
    static const uint8_t signature[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    if (!image) { errno = EINVAL; return false; }
    memset(image, 0, sizeof(*image));
    if (!data || size < 8 || size > PNG_MAX_COMPRESSED ||
        memcmp(data, signature, 8)) { errno = EINVAL; return false; }
    PngInfo info = {0};
    for (unsigned index = 0; index < 256; ++index) info.palette[index][3] = 255;
    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    bool header = false, palette = false, transparency = false;
    bool started = false, ended = false, finished = false, valid = true;
    size_t offset = 8;
    while (valid && offset < size) {
        if (size - offset < 12) break;
        size_t length = be32(data + offset);
        if (length > size - offset - 12) break;
        const uint8_t *kind = data + offset + 4, *body = data + offset + 8;
        for (unsigned index = 0; index < 4; ++index)
            if (!((kind[index] >= 'A' && kind[index] <= 'Z') ||
                  (kind[index] >= 'a' && kind[index] <= 'z'))) valid = false;
        if (!valid || (kind[2] & 0x20)) break;
        uint32_t checksum = crc32_update(UINT32_C(0xffffffff), kind, length + 4) ^
                            UINT32_C(0xffffffff);
        if (checksum != be32(body + length)) break;
        if (!memcmp(kind, "IHDR", 4)) {
            if (header || offset != 8 || length != 13 || !valid_header(body, &info)) break;
            header = true;
        } else if (!header) {
            break;
        } else if (!memcmp(kind, "PLTE", 4)) {
            if (palette || transparency || started || info.color == 0 || info.color == 4 ||
                !length || length % 3 || length > 768 ||
                (info.color == 3 && length / 3 > (1u << info.depth))) break;
            palette = true;
            info.palette_count = (unsigned)(length / 3);
            for (unsigned index = 0; index < info.palette_count; ++index)
                memcpy(info.palette[index], body + index * 3, 3);
        } else if (!memcmp(kind, "tRNS", 4)) {
            if (transparency || started) break;
            transparency = true;
            if (info.color == 3) {
                if (!palette || !length || length > info.palette_count) break;
                for (unsigned index = 0; index < length; ++index)
                    info.palette[index][3] = body[index];
            } else if (info.color == 0 || info.color == 2) {
                if (length != (info.color == 0 ? 2u : 6u)) break;
                info.transparency = true;
                for (unsigned index = 0; index < info.channels; ++index) {
                    info.transparent[index] = be16(body + index * 2);
                    if (info.transparent[index] >= (1u << info.depth)) valid = false;
                }
            } else break;
        } else if (!memcmp(kind, "IDAT", 4)) {
            if (ended || (info.color == 3 && !palette) ||
                length > PNG_MAX_COMPRESSED - compressed_size ||
                /* realloc may temporarily retain the old allocation. */
                (uint64_t)size + compressed_size * 2u + length > PNG_MAX_DECODE_BYTES) break;
            started = true;
            if (length) {
                uint8_t *grown = realloc(compressed, compressed_size + length);
                if (!grown) { valid = false; break; }
                compressed = grown;
                memcpy(compressed + compressed_size, body, length);
                compressed_size += length;
            }
        } else if (!memcmp(kind, "IEND", 4)) {
            finished = !length && started && compressed_size && offset + 12 == size;
            break;
        } else {
            if (!(kind[0] & 0x20u)) break;
            if (started) ended = true;
        }
        offset += length + 12;
    }
    size_t expected = finished && valid ? filtered_size(&info) : 0;
    uint64_t output_size = (uint64_t)info.width * info.height * 4;
    uint64_t row_workspace = ((uint64_t)info.width * info.channels * info.depth + 7) / 8 * 2;
    uint64_t tables = 3u * DEFLATE_TABLE_SIZE * sizeof(HuffmanEntry);
    if ((uint64_t)size + compressed_size + expected + output_size + row_workspace + tables >
        PNG_MAX_DECODE_BYTES) expected = 0;
    uint8_t *filtered = expected ? malloc(expected) : NULL;
    image->width = info.width;
    image->height = info.height;
    image->pixels = filtered ? malloc((size_t)info.width * info.height * 4) : NULL;
    valid = filtered && image->pixels &&
            inflate_zlib(compressed, compressed_size, filtered, expected) &&
            reconstruct(&info, filtered, image);
    free(compressed);
    free(filtered);
    if (!valid) { psvr2_png_free(image); errno = EINVAL; }
    return valid;
}

bool psvr2_png_load(const char *path, Psvr2PngImage *image) {
    if (!image) { errno = EINVAL; return false; }
    memset(image, 0, sizeof(*image));
    if (!path) { errno = EINVAL; return false; }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool valid = fseek(file, 0, SEEK_END) == 0;
    long length = valid ? ftell(file) : -1;
    valid = length >= 8 && length <= PNG_MAX_COMPRESSED &&
            fseek(file, 0, SEEK_SET) == 0;
    uint8_t *data = valid ? malloc((size_t)length) : NULL;
    valid = data && fread(data, 1, (size_t)length, file) == (size_t)length &&
            fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) valid = false;
    if (valid) valid = psvr2_png_decode(data, (size_t)length, image);
    free(data);
    if (!valid) { psvr2_png_free(image); errno = EINVAL; }
    return valid;
}
