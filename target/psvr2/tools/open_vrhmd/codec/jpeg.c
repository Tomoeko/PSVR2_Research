#include "jpeg.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    JPEG_MAX_DIMENSION = 8192,
    JPEG_MAX_FILE = 128 * 1024 * 1024,
    JPEG_MAX_COEFFICIENT_BYTES = 128 * 1024 * 1024,
    JPEG_MAX_DECODE_BYTES = 192 * 1024 * 1024
};

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct JpegHuffman {
    uint16_t count[17];
    uint32_t first[17];
    uint16_t offset[17];
    uint8_t values[256];
    bool valid;
} JpegHuffman;

typedef struct JpegComponent {
    unsigned id, h, v, quant;
    unsigned width, height, blocks_x, blocks_y, stride_blocks;
    int32_t predictor;
    int32_t *coefficients;
    uint8_t *samples;
    int8_t approximation[64];
    uint16_t quant_values[64];
    bool quant_captured;
} JpegComponent;

typedef struct Jpeg {
    const uint8_t *data;
    size_t size, position;
    unsigned bits, byte;
    unsigned width, height, count, max_h, max_v, mcu_cols, mcu_rows;
    unsigned restart_interval, restart_marker, eob_run;
    unsigned adobe_transform;
    bool frame, progressive, adobe;
    uint16_t quant[4][64];
    bool quant_valid[4];
    JpegHuffman huffman[2][4];
    JpegComponent component[4];
} Jpeg;

typedef struct JpegScan {
    unsigned count, component[4], dc[4], ac[4];
    unsigned first, last, high, low;
} JpegScan;

static unsigned big16(const uint8_t *data) {
    return ((unsigned)data[0] << 8) | data[1];
}

static bool marker(Jpeg *jpeg, unsigned *value) {
    if (jpeg->position >= jpeg->size || jpeg->data[jpeg->position++] != 0xff)
        return false;
    while (jpeg->position < jpeg->size && jpeg->data[jpeg->position] == 0xff)
        ++jpeg->position;
    if (jpeg->position == jpeg->size) return false;
    *value = jpeg->data[jpeg->position++];
    return *value != 0;
}

static bool segment(Jpeg *jpeg, const uint8_t **body, size_t *length) {
    if (jpeg->size - jpeg->position < 2) return false;
    unsigned total = big16(jpeg->data + jpeg->position);
    if (total < 2 || total > jpeg->size - jpeg->position) return false;
    *body = jpeg->data + jpeg->position + 2;
    *length = total - 2;
    jpeg->position += total;
    return true;
}

static bool read_bits(Jpeg *jpeg, unsigned count, unsigned *value) {
    if (count > 16) return false;
    unsigned result = 0;
    while (count--) {
        if (!jpeg->bits) {
            if (jpeg->position == jpeg->size) return false;
            unsigned byte = jpeg->data[jpeg->position];
            if (byte == 0xff) {
                if (jpeg->size - jpeg->position < 2 ||
                    jpeg->data[jpeg->position + 1] != 0) return false;
                jpeg->position += 2;
            } else ++jpeg->position;
            jpeg->byte = byte;
            jpeg->bits = 8;
        }
        result = (result << 1) | ((jpeg->byte >> --jpeg->bits) & 1u);
    }
    *value = result;
    return true;
}

static bool symbol(Jpeg *jpeg, const JpegHuffman *table, unsigned *value) {
    if (!table->valid) return false;
    unsigned code = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        unsigned bit;
        if (!read_bits(jpeg, 1, &bit)) return false;
        code = (code << 1) | bit;
        if (code >= table->first[length] &&
            code - table->first[length] < table->count[length]) {
            *value = table->values[table->offset[length] + code - table->first[length]];
            return true;
        }
    }
    return false;
}

static bool amplitude(Jpeg *jpeg, unsigned count, int32_t *value) {
    unsigned bits;
    if (count > 11 || !read_bits(jpeg, count, &bits)) return false;
    *value = count && bits < (1u << (count - 1))
        ? (int32_t)bits - (int32_t)((1u << count) - 1) : (int32_t)bits;
    return true;
}

static bool quantization(Jpeg *jpeg, const uint8_t *body, size_t size) {
    size_t position = 0;
    while (position < size) {
        unsigned descriptor = body[position++], precision = descriptor >> 4;
        unsigned table = descriptor & 15;
        size_t bytes = precision == 0 ? 64u : 128u;
        if (precision > 1 || table > 3 || bytes > size - position) return false;
        for (unsigned index = 0; index < 64; ++index) {
            unsigned value = precision ? big16(body + position + index * 2) : body[position + index];
            if (!value) return false;
            jpeg->quant[table][zigzag[index]] = (uint16_t)value;
        }
        jpeg->quant_valid[table] = true;
        position += bytes;
    }
    return size != 0;
}

static bool huffman_tables(Jpeg *jpeg, const uint8_t *body, size_t size) {
    size_t position = 0;
    while (position < size) {
        unsigned descriptor = body[position++];
        if (descriptor >> 4 > 1 || (descriptor & 15) > 3 || size - position < 16)
            return false;
        JpegHuffman *table = &jpeg->huffman[descriptor >> 4][descriptor & 15];
        memset(table, 0, sizeof(*table));
        unsigned used = 0, code = 0;
        for (unsigned length = 1; length <= 16; ++length) {
            unsigned count = body[position++];
            table->count[length] = (uint16_t)count;
            table->offset[length] = (uint16_t)used;
            table->first[length] = code;
            /* JPEG reserves the all-ones word for entropy padding. */
            if (code + count >= (1u << length) || count > 256 - used) return false;
            used += count;
            code = (code + count) << 1;
        }
        if (!used || used > size - position) return false;
        memcpy(table->values, body + position, used);
        position += used;
        table->valid = true;
    }
    return size != 0;
}

static bool frame_header(Jpeg *jpeg, unsigned kind, const uint8_t *body, size_t size) {
    if (jpeg->frame || size < 6 || body[0] != 8) return false;
    jpeg->height = big16(body + 1);
    jpeg->width = big16(body + 3);
    jpeg->count = body[5];
    if (!jpeg->width || !jpeg->height || jpeg->width > JPEG_MAX_DIMENSION ||
        jpeg->height > JPEG_MAX_DIMENSION ||
        (jpeg->count != 1 && jpeg->count != 3 && jpeg->count != 4) ||
        size != 6u + jpeg->count * 3u) return false;
    unsigned blocks_per_mcu = 0;
    for (unsigned index = 0; index < jpeg->count; ++index) {
        JpegComponent *component = &jpeg->component[index];
        component->id = body[6 + index * 3];
        component->h = body[7 + index * 3] >> 4;
        component->v = body[7 + index * 3] & 15;
        component->quant = body[8 + index * 3];
        if (!component->h || component->h > 4 || !component->v || component->v > 4 ||
            component->quant > 3) return false;
        for (unsigned previous = 0; previous < index; ++previous)
            if (jpeg->component[previous].id == component->id) return false;
        if (component->h > jpeg->max_h) jpeg->max_h = component->h;
        if (component->v > jpeg->max_v) jpeg->max_v = component->v;
        blocks_per_mcu += component->h * component->v;
        memset(component->approximation, -1, sizeof(component->approximation));
    }
    if (jpeg->count > 1 && blocks_per_mcu > 10) return false;
    jpeg->mcu_cols = (jpeg->width + jpeg->max_h * 8 - 1) / (jpeg->max_h * 8);
    jpeg->mcu_rows = (jpeg->height + jpeg->max_v * 8 - 1) / (jpeg->max_v * 8);
    uint64_t bytes = (uint64_t)jpeg->mcu_cols * jpeg->mcu_rows * blocks_per_mcu * 64 * sizeof(int32_t);
    if (bytes > JPEG_MAX_COEFFICIENT_BYTES) return false;
    uint64_t samples = (uint64_t)jpeg->mcu_cols * jpeg->mcu_rows * blocks_per_mcu * 64;
    uint64_t output = (uint64_t)jpeg->width * jpeg->height * 4;
    if (bytes + samples + output + jpeg->size > JPEG_MAX_DECODE_BYTES) return false;
    for (unsigned index = 0; index < jpeg->count; ++index) {
        JpegComponent *component = &jpeg->component[index];
        component->width = (jpeg->width * component->h + jpeg->max_h - 1) / jpeg->max_h;
        component->height = (jpeg->height * component->v + jpeg->max_v - 1) / jpeg->max_v;
        component->blocks_x = (component->width + 7) / 8;
        component->blocks_y = (component->height + 7) / 8;
        component->stride_blocks = jpeg->mcu_cols * component->h;
        size_t count = (size_t)component->stride_blocks * jpeg->mcu_rows * component->v * 64;
        component->coefficients = calloc(count, sizeof(*component->coefficients));
        if (!component->coefficients) return false;
    }
    jpeg->frame = true;
    jpeg->progressive = kind == 0xc2;
    return true;
}

static bool scan_header(Jpeg *jpeg, const uint8_t *body, size_t size, JpegScan *scan) {
    if (!jpeg->frame || !size) return false;
    scan->count = body[0];
    if (!scan->count || scan->count > jpeg->count || size != 4u + scan->count * 2u)
        return false;
    for (unsigned index = 0; index < scan->count; ++index) {
        unsigned id = body[1 + index * 2], component = 0;
        while (component < jpeg->count && jpeg->component[component].id != id) ++component;
        if (component == jpeg->count) return false;
        for (unsigned previous = 0; previous < index; ++previous)
            if (scan->component[previous] == component) return false;
        scan->component[index] = component;
        scan->dc[index] = body[2 + index * 2] >> 4;
        scan->ac[index] = body[2 + index * 2] & 15;
        if (scan->dc[index] > 3 || scan->ac[index] > 3 ||
            !jpeg->quant_valid[jpeg->component[component].quant]) return false;
    }
    scan->first = body[1 + scan->count * 2];
    scan->last = body[2 + scan->count * 2];
    scan->high = body[3 + scan->count * 2] >> 4;
    scan->low = body[3 + scan->count * 2] & 15;
    if (scan->first > scan->last || scan->last > 63 || scan->high > 13 || scan->low > 13)
        return false;
    if (!jpeg->progressive && (scan->first || scan->last != 63 || scan->high || scan->low))
        return false;
    if (jpeg->progressive && ((!scan->first && scan->last) ||
        (scan->first && scan->count != 1) || (scan->high && scan->high != scan->low + 1)))
        return false;
    for (unsigned index = 0; index < scan->count; ++index) {
        JpegComponent *component = &jpeg->component[scan->component[index]];
        if (component->quant_captured) {
            if (memcmp(component->quant_values, jpeg->quant[component->quant],
                       sizeof(component->quant_values))) return false;
        } else {
            memcpy(component->quant_values, jpeg->quant[component->quant],
                   sizeof(component->quant_values));
            component->quant_captured = true;
        }
        for (unsigned coefficient = scan->first; coefficient <= scan->last; ++coefficient) {
            int expected = scan->high ? (int)scan->high : -1;
            if (component->approximation[coefficient] != expected) return false;
            component->approximation[coefficient] = (int8_t)scan->low;
        }
    }
    return true;
}

static bool refine(Jpeg *jpeg, int32_t *coefficient, unsigned low) {
    unsigned bit;
    if (!read_bits(jpeg, 1, &bit)) return false;
    int32_t increment = (int32_t)(1u << low);
    if (bit && ((uint32_t)abs(*coefficient) & (unsigned)increment) == 0)
        *coefficient += *coefficient < 0 ? -increment : increment;
    return true;
}

static bool decode_block(Jpeg *jpeg, const JpegScan *scan, unsigned index, int32_t *block) {
    JpegComponent *component = &jpeg->component[scan->component[index]];
    if (!scan->first) {
        if (scan->high) {
            /* DC refinement appends one bit to the signed two's-complement value. */
            unsigned bit;
            if (!read_bits(jpeg, 1, &bit)) return false;
            block[0] |= (int32_t)(bit << scan->low);
        } else {
            unsigned category;
            int32_t difference;
            if (!symbol(jpeg, &jpeg->huffman[0][scan->dc[index]], &category) ||
                !amplitude(jpeg, category, &difference)) return false;
            int64_t predictor = (int64_t)component->predictor + difference;
            if (predictor < -32768 || predictor > 32767) return false;
            component->predictor = (int32_t)predictor;
            block[0] = component->predictor * (int32_t)(1u << scan->low);
        }
        if (jpeg->progressive) return true;
    }
    unsigned position = scan->first ? scan->first : 1;
    if (!scan->high) {
        if (jpeg->eob_run) { --jpeg->eob_run; return true; }
        while (position <= scan->last) {
            unsigned rs;
            if (!symbol(jpeg, &jpeg->huffman[1][scan->ac[index]], &rs)) return false;
            unsigned zeros = rs >> 4, category = rs & 15;
            if (!category) {
                if (zeros == 15) {
                    if (16 > scan->last + 1 - position) return false;
                    position += 16;
                } else if (jpeg->progressive) {
                    unsigned extra;
                    if (!read_bits(jpeg, zeros, &extra)) return false;
                    jpeg->eob_run = (1u << zeros) + extra - 1;
                    return true;
                } else return zeros == 0;
            } else {
                if (category > 10 || zeros > scan->last - position) return false;
                position += zeros;
                int32_t value;
                if (!amplitude(jpeg, category, &value)) return false;
                block[zigzag[position++]] = value * (int32_t)(1u << scan->low);
            }
        }
        return true;
    }
    if (!jpeg->eob_run) {
        while (position <= scan->last) {
            unsigned rs;
            if (!symbol(jpeg, &jpeg->huffman[1][scan->ac[index]], &rs)) return false;
            unsigned zeros = rs >> 4, category = rs & 15;
            int32_t value = 0;
            if (category) {
                unsigned sign;
                if (category != 1 || !read_bits(jpeg, 1, &sign)) return false;
                value = sign ? (int32_t)(1u << scan->low) : -(int32_t)(1u << scan->low);
            } else if (zeros != 15) {
                unsigned extra;
                if (!read_bits(jpeg, zeros, &extra)) return false;
                jpeg->eob_run = (1u << zeros) + extra;
                break;
            } else zeros = 16;
            while (position <= scan->last) {
                int32_t *coefficient = block + zigzag[position];
                if (*coefficient) {
                    if (!refine(jpeg, coefficient, scan->low)) return false;
                } else if (zeros) --zeros;
                else break;
                ++position;
                if (!zeros && !value) break;
            }
            if (zeros || (value && position > scan->last)) return false;
            if (value) block[zigzag[position++]] = value;
        }
    }
    if (jpeg->eob_run) {
        for (; position <= scan->last; ++position)
            if (block[zigzag[position]] && !refine(jpeg, block + zigzag[position], scan->low))
                return false;
        --jpeg->eob_run;
    }
    return true;
}

static bool decode_scan(Jpeg *jpeg, const JpegScan *scan) {
    jpeg->bits = jpeg->eob_run = jpeg->restart_marker = 0;
    for (unsigned index = 0; index < jpeg->count; ++index) jpeg->component[index].predictor = 0;
    bool interleaved = scan->count > 1;
    JpegComponent *single = &jpeg->component[scan->component[0]];
    unsigned columns = interleaved ? jpeg->mcu_cols : single->blocks_x;
    unsigned rows = interleaved ? jpeg->mcu_rows : single->blocks_y;
    unsigned processed = 0;
    for (unsigned y = 0; y < rows; ++y) {
        for (unsigned x = 0; x < columns; ++x) {
            if (processed && jpeg->restart_interval && processed % jpeg->restart_interval == 0) {
                unsigned restart;
                jpeg->bits = 0;
                if (jpeg->eob_run || !marker(jpeg, &restart) || restart != 0xd0 + jpeg->restart_marker)
                    return false;
                jpeg->restart_marker = (jpeg->restart_marker + 1) & 7;
                for (unsigned index = 0; index < jpeg->count; ++index)
                    jpeg->component[index].predictor = 0;
            }
            for (unsigned index = 0; index < scan->count; ++index) {
                JpegComponent *component = &jpeg->component[scan->component[index]];
                unsigned h = interleaved ? component->h : 1, v = interleaved ? component->v : 1;
                for (unsigned by = 0; by < v; ++by)
                    for (unsigned bx = 0; bx < h; ++bx) {
                        size_t block = ((size_t)(y * v + by) * component->stride_blocks + x * h + bx) * 64;
                        if (!decode_block(jpeg, scan, index, component->coefficients + block)) return false;
                    }
            }
            ++processed;
        }
    }
    jpeg->bits = 0;
    return jpeg->eob_run == 0;
}

static uint8_t bounded_sample(double value) {
    if (value <= 0) return 0;
    if (value >= 255) return 255;
    return (uint8_t)floor(value + 0.5);
}

static bool transform_components(Jpeg *jpeg) {
    double basis[8][8];
    for (unsigned x = 0; x < 8; ++x)
        for (unsigned frequency = 0; frequency < 8; ++frequency)
            basis[x][frequency] = cos((double)((2 * x + 1) * frequency) *
                3.14159265358979323846 / 16.0) * (frequency ? 1.0 : 0.70710678118654752440);
    for (unsigned index = 0; index < jpeg->count; ++index) {
        JpegComponent *component = &jpeg->component[index];
        if (component->approximation[0] < 0) return false;
        unsigned stride = component->stride_blocks * 8;
        component->samples = malloc((size_t)stride * jpeg->mcu_rows * component->v * 8);
        if (!component->samples) return false;
        for (unsigned by = 0; by < component->blocks_y; ++by) {
            for (unsigned bx = 0; bx < component->blocks_x; ++bx) {
                int32_t *coefficients = component->coefficients + ((size_t)by * component->stride_blocks + bx) * 64;
                double rows[8][8];
                for (unsigned v = 0; v < 8; ++v)
                    for (unsigned x = 0; x < 8; ++x) {
                        double sum = 0;
                        for (unsigned u = 0; u < 8; ++u)
                            sum += (double)coefficients[v * 8 + u] * component->quant_values[v * 8 + u] * basis[x][u];
                        rows[v][x] = sum;
                    }
                for (unsigned y = 0; y < 8; ++y)
                    for (unsigned x = 0; x < 8; ++x) {
                        double sum = 0;
                        for (unsigned v = 0; v < 8; ++v) sum += rows[v][x] * basis[y][v];
                        component->samples[(size_t)(by * 8 + y) * stride + bx * 8 + x] = bounded_sample(128 + sum * 0.25);
                    }
            }
        }
    }
    return true;
}

static double component_sample(const Jpeg *jpeg, unsigned index, unsigned x, unsigned y) {
    const JpegComponent *component = &jpeg->component[index];
    double fx = ((double)x + 0.5) * component->h / jpeg->max_h - 0.5;
    double fy = ((double)y + 0.5) * component->v / jpeg->max_v - 0.5;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > component->width - 1) fx = component->width - 1;
    if (fy > component->height - 1) fy = component->height - 1;
    unsigned left = (unsigned)fx, top = (unsigned)fy;
    unsigned right = left + 1 < component->width ? left + 1 : left;
    unsigned bottom = top + 1 < component->height ? top + 1 : top;
    unsigned stride = component->stride_blocks * 8;
    double dx = fx - left, dy = fy - top;
    double above = component->samples[(size_t)top * stride + left] * (1 - dx) +
                   component->samples[(size_t)top * stride + right] * dx;
    double below = component->samples[(size_t)bottom * stride + left] * (1 - dx) +
                   component->samples[(size_t)bottom * stride + right] * dx;
    return above * (1 - dy) + below * dy;
}

static bool output_image(Jpeg *jpeg, Psvr2PngImage *image) {
    if (jpeg->adobe && ((jpeg->count == 1 && jpeg->adobe_transform != 0) ||
        (jpeg->count == 3 && jpeg->adobe_transform > 1) ||
        (jpeg->count == 4 && jpeg->adobe_transform == 1))) return false;
    if (!transform_components(jpeg)) return false;
    image->pixels = malloc((size_t)jpeg->width * jpeg->height * 4);
    if (!image->pixels) return false;
    image->width = jpeg->width;
    image->height = jpeg->height;
    bool direct_rgb = jpeg->count == 3 && ((jpeg->adobe && jpeg->adobe_transform == 0) ||
        (jpeg->component[0].id == 'R' && jpeg->component[1].id == 'G' && jpeg->component[2].id == 'B'));
    for (unsigned y = 0; y < jpeg->height; ++y) {
        for (unsigned x = 0; x < jpeg->width; ++x) {
            uint8_t *pixel = image->pixels + ((size_t)y * jpeg->width + x) * 4;
            double values[4] = {0};
            for (unsigned index = 0; index < jpeg->count; ++index)
                values[index] = component_sample(jpeg, index, x, y);
            if (jpeg->count == 1) pixel[0] = pixel[1] = pixel[2] = bounded_sample(values[0]);
            else if (direct_rgb) {
                for (unsigned index = 0; index < 3; ++index) pixel[index] = bounded_sample(values[index]);
            } else if (jpeg->count == 3 || (jpeg->adobe && jpeg->adobe_transform == 2)) {
                double r = values[0] + 1.402 * (values[2] - 128);
                double g = values[0] - 0.344136 * (values[1] - 128) - 0.714136 * (values[2] - 128);
                double b = values[0] + 1.772 * (values[1] - 128);
                if (jpeg->count == 4) {
                    pixel[0] = bounded_sample((255 - bounded_sample(r)) * values[3] / 255);
                    pixel[1] = bounded_sample((255 - bounded_sample(g)) * values[3] / 255);
                    pixel[2] = bounded_sample((255 - bounded_sample(b)) * values[3] / 255);
                } else {
                    pixel[0] = bounded_sample(r); pixel[1] = bounded_sample(g); pixel[2] = bounded_sample(b);
                }
            } else {
                for (unsigned index = 0; index < 3; ++index) {
                    double color = jpeg->adobe ? values[index] : 255 - values[index];
                    double black = jpeg->adobe ? values[3] : 255 - values[3];
                    pixel[index] = bounded_sample(color * black / 255);
                }
            }
            pixel[3] = 255;
        }
    }
    return true;
}

bool psvr2_jpeg_decode(const uint8_t *data, size_t size, Psvr2PngImage *image) {
    if (!image) { errno = EINVAL; return false; }
    memset(image, 0, sizeof(*image));
    if (!data || size < 4 || size > JPEG_MAX_FILE || data[0] != 0xff || data[1] != 0xd8) {
        errno = EINVAL; return false;
    }
    Jpeg jpeg = {.data = data, .size = size, .position = 2};
    bool valid = true, complete = false;
    unsigned scans = 0;
    while (valid) {
        unsigned kind;
        if (!marker(&jpeg, &kind)) break;
        if (kind == 0xd9) { complete = scans && jpeg.position == size; break; }
        if (kind == 0xd8 || kind == 0x01 || (kind >= 0xd0 && kind <= 0xd7)) break;
        const uint8_t *body;
        size_t length;
        if (!segment(&jpeg, &body, &length)) break;
        if (kind == 0xc0 || kind == 0xc1 || kind == 0xc2)
            valid = frame_header(&jpeg, kind, body, length);
        else if (kind == 0xdb) valid = quantization(&jpeg, body, length);
        else if (kind == 0xc4) valid = huffman_tables(&jpeg, body, length);
        else if (kind == 0xdd) {
            valid = length == 2;
            if (valid) jpeg.restart_interval = big16(body);
        } else if (kind == 0xda) {
            JpegScan scan = {0};
            valid = ++scans <= 256 && scan_header(&jpeg, body, length, &scan) && decode_scan(&jpeg, &scan);
        } else if (kind == 0xee && length >= 12 && !memcmp(body, "Adobe", 5)) {
            jpeg.adobe = true;
            jpeg.adobe_transform = body[11];
            valid = jpeg.adobe_transform <= 2;
        } else valid = (kind >= 0xe0 && kind <= 0xef) || kind == 0xfe;
    }
    valid = valid && complete && output_image(&jpeg, image);
    for (unsigned index = 0; index < 4; ++index) {
        free(jpeg.component[index].coefficients);
        free(jpeg.component[index].samples);
    }
    if (!valid) { psvr2_png_free(image); errno = EINVAL; }
    return valid;
}

bool psvr2_jpeg_load(const char *path, Psvr2PngImage *image) {
    if (!image) { errno = EINVAL; return false; }
    memset(image, 0, sizeof(*image));
    if (!path) { errno = EINVAL; return false; }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool valid = fseek(file, 0, SEEK_END) == 0;
    long length = valid ? ftell(file) : -1;
    valid = length >= 4 && length <= JPEG_MAX_FILE && fseek(file, 0, SEEK_SET) == 0;
    uint8_t *data = valid ? malloc((size_t)length) : NULL;
    valid = data && fread(data, 1, (size_t)length, file) == (size_t)length &&
            fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) valid = false;
    if (valid) valid = psvr2_jpeg_decode(data, (size_t)length, image);
    free(data);
    if (!valid) { psvr2_png_free(image); errno = EINVAL; }
    return valid;
}
