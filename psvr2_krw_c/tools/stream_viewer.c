#include "psvr2/device.h"

#include <SDL.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

typedef enum { MODE_VRH2, MODE_ET } stream_mode;

typedef struct {
    psvr2_device device;
    int interface_number;
    uint8_t endpoint;
    stream_mode mode;
    unsigned scale;
    bool activate;
    psvr2_buffer stream;
    size_t cursor;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t texture_w;
    uint32_t texture_h;
    uint64_t frames;
} viewer;

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [--mode cinema|mirror|et] [--interface N] [--endpoint N]\n"
        "          [--scale N] [--activate]\n"
        "  Defaults: ET, automatic EP 0x87 discovery, and scale 2.\n"
        "  ET contains both 200x200 eye cameras in one 400x200 frame.\n"
        "  cinema/mirror parse auto-sized VRH2 frames and default to scale 1.\n",
        p);
}

static bool find_endpoint(viewer *v) {
    const psvr2_endpoint_query query = {
        .interface_number = v->interface_number,
        .alternate_setting = -1,
        .direction = LIBUSB_ENDPOINT_IN,
        .transfer_type = LIBUSB_TRANSFER_TYPE_BULK,
        .address = v->endpoint
    };
    psvr2_endpoint endpoint;
    if (psvr2_device_find_endpoints(
            &v->device, &query, &endpoint, 1) != 1)
        return false;
    v->interface_number = endpoint.interface_number;
    v->endpoint = endpoint.address;
    return psvr2_device_claim_interface(
        &v->device, v->interface_number);
}

static bool ensure_texture(viewer *v, uint32_t w, uint32_t h) {
    if (v->texture && v->texture_w == w && v->texture_h == h) return true;
    if (v->texture) SDL_DestroyTexture(v->texture);
    v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_RGB24,
                                   SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
    v->texture_w = w;
    v->texture_h = h;
    SDL_SetWindowSize(v->window, (int)(w * v->scale), (int)(h * v->scale));
    return v->texture != NULL;
}

static bool finish_frame(viewer *v, uint32_t w, uint32_t h) {
    SDL_UnlockTexture(v->texture);
    SDL_RenderClear(v->renderer);
    SDL_RenderCopy(v->renderer, v->texture, NULL, NULL);
    SDL_RenderPresent(v->renderer);
    ++v->frames;
    if (v->frames % 30 == 1)
        printf("[+] Frame %llu (%ux%u)\n", (unsigned long long)v->frames, w, h);
    return true;
}

static bool lock_frame(
    viewer *v, uint32_t w, uint32_t h,
    uint8_t **pixels, int *pitch) {
    void *locked = NULL;
    if (!ensure_texture(v, w, h) ||
        SDL_LockTexture(v->texture, NULL, &locked, pitch) != 0)
        return false;
    *pixels = locked;
    return true;
}

static bool present_rgb(viewer *v, uint32_t w, uint32_t h,
                        const uint8_t *pixels, size_t pitch) {
    size_t row_size = (size_t)w * 3U;
    uint8_t *target;
    int target_pitch;
    if (pitch < row_size ||
        !lock_frame(v, w, h, &target, &target_pitch))
        return false;
    if (target_pitch < 0 || (size_t)target_pitch < row_size) {
        SDL_UnlockTexture(v->texture);
        return false;
    }
    for (uint32_t y = 0; y < h; ++y)
        memcpy(
            target + (size_t)y * (size_t)target_pitch,
            pixels + (size_t)y * pitch, row_size);
    return finish_frame(v, w, h);
}

static bool present_ar30(viewer *v, uint32_t w, uint32_t h, uint32_t pitch,
                         const uint8_t *raw) {
    size_t row_size = (size_t)w * 3U;
    uint8_t *rgb;
    int rgb_pitch;
    if (pitch < (uint64_t)w * 4U ||
        !lock_frame(v, w, h, &rgb, &rgb_pitch))
        return false;
    if (rgb_pitch < 0 || (size_t)rgb_pitch < row_size) {
        SDL_UnlockTexture(v->texture);
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t p = psvr2_load_le32(raw + (size_t)y * pitch + x * 4U);
            size_t d = (size_t)y * (size_t)rgb_pitch + x * 3U;
            rgb[d] = (uint8_t)(p >> 22);
            rgb[d + 1] = (uint8_t)(p >> 12);
            rgb[d + 2] = (uint8_t)(p >> 2);
        }
    }
    return finish_frame(v, w, h);
}

static bool present_gray(viewer *v, const uint8_t *gray) {
    const uint32_t w = 400, h = 200;
    const size_t row_size = (size_t)w * 3U;
    uint8_t *rgb;
    int rgb_pitch;
    if (!lock_frame(v, w, h, &rgb, &rgb_pitch))
        return false;
    if (rgb_pitch < 0 || (size_t)rgb_pitch < row_size) {
        SDL_UnlockTexture(v->texture);
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = rgb + (size_t)y * (size_t)rgb_pitch;
        for (uint32_t x = 0; x < w; ++x)
            row[x * 3U] = row[x * 3U + 1U] =
                row[x * 3U + 2U] = gray[(size_t)y * w + x];
    }
    return finish_frame(v, w, h);
}

static bool present_rgbx(
    viewer *v, uint32_t w, uint32_t h, const uint8_t *pixels) {
    const size_t row_size = (size_t)w * 3U;
    uint8_t *rgb;
    int rgb_pitch;
    if (!lock_frame(v, w, h, &rgb, &rgb_pitch))
        return false;
    if (rgb_pitch < 0 || (size_t)rgb_pitch < row_size) {
        SDL_UnlockTexture(v->texture);
        return false;
    }
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = rgb + (size_t)y * (size_t)rgb_pitch;
        const uint8_t *source = pixels + (size_t)y * w * 4U;
        for (uint32_t x = 0; x < w; ++x)
            memcpy(row + x * 3U, source + x * 4U, 3);
    }
    return finish_frame(v, w, h);
}

static size_t find_magic(const uint8_t *data, size_t len, uint32_t magic) {
    uint8_t bytes[4];
    psvr2_store_le32(bytes, magic);
    for (size_t i = 0; i + 4 <= len; ++i)
        if (!memcmp(data + i, bytes, 4)) return i;
    return SIZE_MAX;
}

static size_t find_vrh2_magic(const uint8_t *data, size_t len) {
    size_t a = find_magic(data, len, UINT32_C(0x56524832));
    size_t b = find_magic(data, len, UINT32_C(0x32485256));
    if (a == SIZE_MAX) return b;
    if (b == SIZE_MAX) return a;
    return a < b ? a : b;
}

static bool process_vrh2(viewer *v) {
    while (v->stream.len - v->cursor >= 32) {
        uint8_t *p = v->stream.data + v->cursor;
        size_t avail = v->stream.len - v->cursor;
        uint32_t magic = psvr2_load_le32(p);
        if (magic != UINT32_C(0x56524832) &&
            magic != UINT32_C(0x32485256)) {
            size_t index = find_vrh2_magic(p + 1, avail - 1);
            if (index == SIZE_MAX) {
                v->cursor = v->stream.len > 3 ? v->stream.len - 3 : 0;
                break;
            }
            v->cursor += index + 1;
            continue;
        }
        uint32_t w = psvr2_load_le32(p + 8);
        uint32_t h = psvr2_load_le32(p + 12);
        uint32_t pitch = psvr2_load_le32(p + 16);
        uint32_t format = psvr2_load_le32(p + 20);
        uint32_t size = psvr2_load_le32(p + 24);
        (void)format;
        if (!w || !h || w > 8192 || h > 8192 || size > 128U * 1024U * 1024U) {
            v->cursor += 4;
            continue;
        }
        if (avail < (size_t)32 + size) break;
        const uint8_t *pixels = p + 32;
        uint64_t count = (uint64_t)w * h;
        if (pitch >= w * 4U && size >= (uint64_t)pitch * h)
            (void)present_ar30(v, w, h, pitch, pixels);
        else if (size == count * 3)
            (void)present_rgb(v, w, h, pixels, (size_t)w * 3);
        else if (size == count * 4)
            (void)present_rgbx(v, w, h, pixels);
        v->cursor += (size_t)32 + size;
    }
    return true;
}

static bool process_et(viewer *v) {
    const uint32_t magic = UINT32_C(0x02004956);
    const size_t frame_size = 80256, header = 256;
    while (v->stream.len - v->cursor >= frame_size) {
        uint8_t *p = v->stream.data + v->cursor;
        size_t avail = v->stream.len - v->cursor;
        if (psvr2_load_le32(p) != magic) {
            size_t index = find_magic(p + 1, avail - 1, magic);
            if (index == SIZE_MAX) {
                v->cursor = v->stream.len > 3 ? v->stream.len - 3 : 0;
                break;
            }
            v->cursor += index + 1;
            continue;
        }
        (void)present_gray(v, p + header);
        v->cursor += frame_size;
    }
    return true;
}

static void compact(viewer *v) {
    if (v->cursor > 0 && (v->cursor > v->stream.len / 2 ||
                          v->cursor > 16U * 1024U * 1024U)) {
        memmove(v->stream.data, v->stream.data + v->cursor,
                v->stream.len - v->cursor);
        v->stream.len -= v->cursor;
        v->cursor = 0;
    }
}

int main(int argc, char **argv) {
    viewer v = {.mode = MODE_ET};
    bool interface_explicit = false;
    bool endpoint_explicit = false;
    bool scale_explicit = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "et")) v.mode = MODE_ET;
            else if (!strcmp(m, "cinema") || !strcmp(m, "mirror")) v.mode = MODE_VRH2;
            else { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--interface") && i + 1 < argc) {
            v.interface_number = atoi(argv[++i]);
            interface_explicit = true;
        } else if (!strcmp(argv[i], "--endpoint") && i + 1 < argc) {
            uint64_t ep;
            if (!psvr2_parse_u64(argv[++i], &ep) || ep > 255) return 1;
            v.endpoint = (uint8_t)ep;
            endpoint_explicit = true;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            v.scale = (unsigned)atoi(argv[++i]);
            if (!v.scale || v.scale > 8) return 1;
            scale_explicit = true;
        } else if (!strcmp(argv[i], "--activate")) v.activate = true;
        else { usage(argv[0]); return 1; }
    }
    /*
     * Stage3 can renumber Sony interfaces when it adds ACM functions.
     * Find EP7 by searching all active interfaces.
     */
    if (!interface_explicit)
        v.interface_number = v.mode == MODE_ET ? -1 : 5;
    if (!endpoint_explicit)
        v.endpoint = v.mode == MODE_ET ? 0x87 : 0;
    if (!scale_explicit)
        v.scale = v.mode == MODE_ET ? 2 : 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    bool device_initialized = false;
    bool interface_claimed = false;
    bool sdl_initialized = false;
    uint8_t *read_buf = NULL;
    int result = 1;
    if (psvr2_device_init(&v.device) != 0) {
        fputs("[-] Could not initialize libusb.\n", stderr);
        goto cleanup;
    }
    device_initialized = true;
    if (!psvr2_device_wait_ready(
            &v.device, PSVR2_DEVICE_STARTUP_TIMEOUT_MS)) {
        fputs("[-] PSVR2 not found.\n", stderr);
        goto cleanup;
    }
    if (v.activate && v.mode == MODE_ET &&
        !psvr2_device_activate_eye_tracking(&v.device)) {
        fputs("[-] ET activation failed.\n", stderr);
        goto cleanup;
    }
    if (!find_endpoint(&v)) {
        fputs("[-] Bulk IN endpoint not found or claim failed.\n", stderr);
        goto cleanup;
    }
    interface_claimed = true;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        goto cleanup;
    }
    sdl_initialized = true;
    v.window = SDL_CreateWindow(v.mode == MODE_ET ? "PSVR2 Eye Tracking" :
                                "PSVR2 Video Stream",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                800, 450, SDL_WINDOW_RESIZABLE);
    if (v.window)
        v.renderer = SDL_CreateRenderer(
            v.window, -1, SDL_RENDERER_ACCELERATED);
    if (!v.window || !v.renderer) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        goto cleanup;
    }
    printf("[+] Reading EP 0x%02x on interface %d\n", v.endpoint, v.interface_number);
    read_buf = malloc(2U * 1024U * 1024U);
    if (!read_buf) {
        fputs("[-] Could not allocate USB receive buffer.\n", stderr);
        goto cleanup;
    }
    bool running = read_buf != NULL;
    double last_ping = psvr2_now();
    result = 0;
    while (running && !interrupted) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN &&
                 (event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_ESCAPE)))
                running = false;
        int transferred = 0;
        int rc = libusb_bulk_transfer(v.device.handle, v.endpoint, read_buf,
                                      2U * 1024U * 1024U, &transferred,
                                      v.mode == MODE_ET ? 100 : 200);
        if (rc == LIBUSB_ERROR_TIMEOUT) continue;
        if (rc != 0) {
            fprintf(stderr, "USB: %s\n", libusb_error_name(rc));
            result = 1;
            break;
        }
        if (transferred > 0 &&
            !psvr2_buffer_append(
                &v.stream, read_buf, (size_t)transferred)) {
            result = 1;
            break;
        }
        if (!(v.mode == MODE_ET ? process_et(&v) : process_vrh2(&v))) {
            result = 1;
            break;
        }
        compact(&v);
        if (v.mode == MODE_ET && psvr2_now() - last_ping >= 2) {
            (void)psvr2_device_vendor_set(&v.device, 0x0c, 1, NULL, 0, 50);
            last_ping = psvr2_now();
        }
    }
cleanup:
    free(read_buf);
    psvr2_buffer_free(&v.stream);
    if (v.texture) SDL_DestroyTexture(v.texture);
    if (v.renderer) SDL_DestroyRenderer(v.renderer);
    if (v.window) SDL_DestroyWindow(v.window);
    if (sdl_initialized) SDL_Quit();
    if (interface_claimed)
        psvr2_device_release_interface(
            &v.device, v.interface_number);
    if (device_initialized) psvr2_device_close(&v.device);
    printf("[+] Closed after %llu frames.\n", (unsigned long long)v.frames);
    return result;
}
