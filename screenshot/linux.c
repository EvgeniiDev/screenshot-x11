#ifdef PLATFORM_LINUX

#include "common.h"
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>
#include <numa.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

/* ========== Linux платформо-зависимые данные ========== */
typedef struct {
    Display            *dpy;
    Window              root;
    XImage             *ximg[MAX_SLOTS];
    XShmSegmentInfo     shm[MAX_SLOTS];
} LinuxPlatformData;

/* ========== Linux квантизация ========== */

/* void platform_quantize_rgba_to_rgb332( */
/*     const uint8_t *rgba_data, uint8_t *quant_data, */
/*     int width, int height, int padded_width, int bytes_per_line) { */
/*     // Однократный детект SIMD возможностей */
/*     static bool inited = false; */
/*     static bool use256; */
/*     if (!inited) { */
/*         use256 = cpu_has_avx2(); */
/*         inited = true; */
/*         printf("[init] AVX2 support for RGBA quantization: %s\n", use256 ? "yes" : "no"); */
/*     } */

/*     // Векторное квантование RGBA → RGB332 */
/*     for (int y = 0; y < height; ++y) { */
/*         const uint8_t *row_src = rgba_data + (size_t)y * bytes_per_line;  // Используем bytes_per_line */
/*         uint8_t *row_q = quant_data + (size_t)y * padded_width; */
/*         int x = 0; */

/* #if defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) */
/*         if (use256) { */
/*             __m256i mR = _mm256_set1_epi32(0xE0); */
/*             __m256i mB = _mm256_set1_epi32(0xC0); */
/*             int lim8 = (width/8)*8; */
/*             for (; x < lim8; x += 8) { */
/*                 __m256i pix = _mm256_loadu_si256((__m256i*)(row_src + x*4)); */
/*                 __m256i r   = _mm256_and_si256(_mm256_srli_epi32(pix,16), mR); */
/*                 __m256i g2  = _mm256_and_si256(_mm256_srli_epi32(pix, 8), mR); */
/*                 __m256i b   = _mm256_and_si256(pix, mB); */
/*                 __m256i rg  = _mm256_or_si256(r, _mm256_srli_epi32(g2,3)); */
/*                 __m256i rgb = _mm256_or_si256(rg, _mm256_srli_epi32(b,6)); */
/*                 __m128i lo  = _mm256_castsi256_si128(rgb); */
/*                 __m128i hi  = _mm256_extracti128_si256(rgb,1); */
/*                 __m128i p16 = _mm_packus_epi32(lo, hi); */
/*                 __m128i p8  = _mm_packus_epi16(p16, _mm_setzero_si128()); */
/*                 _mm_storel_epi64((__m128i*)(row_q + x), p8); */
/*             } */
/*         } */
/* #endif */

/*         // SSE2 путь (обрабатывает хвосты и если AVX2 нет) */
/* #if defined(__SSE2__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) */
/*         { */
/*             __m128i mR4 = _mm_set1_epi32(0xE0); */
/*             __m128i mB4 = _mm_set1_epi32(0xC0); */
/*             int lim4 = (width/4)*4; */
/*             for (; x < lim4; x += 4) { */
/*                 __m128i pix = _mm_loadu_si128((__m128i*)(row_src + x*4)); */
/*                 __m128i r   = _mm_and_si128(_mm_srli_epi32(pix,16), mR4); */
/*                 __m128i g2  = _mm_and_si128(_mm_srli_epi32(pix, 8), mR4); */
/*                 __m128i b   = _mm_and_si128(pix, mB4); */
/*                 __m128i rg  = _mm_or_si128(r, _mm_srli_epi32(g2,3)); */
/*                 __m128i rgb = _mm_or_si128(rg,_mm_srli_epi32(b,6)); */
/*                 __m128i p16 = _mm_packus_epi32(rgb, _mm_setzero_si128()); */
/*                 __m128i p8  = _mm_packus_epi16(p16, _mm_setzero_si128()); */
/*                 *(uint32_t*)(row_q + x) = _mm_cvtsi128_si32(p8); */
/*             } */
/*         } */
/* #endif */

/*         // Скалярный хвост + паддинг (RGBA) */
/*         for (; x < width; ++x) { */
/*             uint8_t R = row_src[x*4+0];  // RGBA */
/*             uint8_t G = row_src[x*4+1]; */
/*             uint8_t B = row_src[x*4+2]; */
/*             // A игнорируем */
/*             uint8_t R3 = R >> 5, G3 = G >> 5, B2 = B >> 6; */
/*             row_q[x] = (uint8_t)((R3<<5)|(G3<<2)|B2); */
/*         } */
/*         for (; x < padded_width; ++x) { */
/*             row_q[x] = 0; */
/*         } */
/*     } */

/*     // Заполняем padding строки */
/*     int block_rows = (height + BS - 1) / BS; */
/*     int padded_height = block_rows * BS; */
/*     for (int y = height; y < padded_height; ++y) { */
/*         uint8_t *row_q = quant_data + (size_t)y * padded_width; */
/*         memset(row_q, 0, padded_width); */
/*     } */

/* #ifdef __SSE2__ */
/*     _mm_sfence();  // дождаться store */
/* #endif */
/* } */


/**
 * quantize_and_analyze — в одном проходе:
 *  1) квантизация RGBA→RGB332 SIMD/скаляр
 *  2) сбор гистограмм по блокам
 *  3) заполнение fg/bg и маски сразу
 *
 * Выход:
 *
 * slot->quant — весь кадр в формате RGB332,
 * slot->bg и slot->fg — массивы «фоновых» и «текстовых» цветов по 32×32,
 * slot->mask — битовые маски смешанных блоков (1 бит на пиксель внутри блока).
 *
 */
void quantize_and_analyze(
    const uint8_t *rgba, FrameSlot *slot, GlobalContext *ctx)
{
    const int W  = ctx->w;
    const int H  = ctx->h;
    const int pw = ctx->padded_w;
    const int stride_bytes = ctx->stride_rgba;
    const int bc = ctx->block_cols;
    const int br = ctx->block_rows;
    const size_t mask_bytes = (BS*BS + 7) / 8;
    const int total_blocks = bc * br;

    // 1) Выделяем и обнуляем гистограммы размером [total_blocks][256],
    // выровнено на 128 байт
    int *hist;
    if (posix_memalign(
            (void**)&hist, ALIGNMENT,
            sizeof(int) * 256 * total_blocks) != 0)
    {
        hist = malloc(sizeof(int) * 256 * total_blocks);
    }
    memset(hist, 0, sizeof(int) * 256 * total_blocks);

    // 2) Проход по кадру: квантование + сбор гистограмм
    bool have_avx2 = cpu_has_avx2();

    for (int y = 0; y < H; ++y) {
        const uint8_t *row = rgba + (size_t)y * stride_bytes;
        uint8_t       *qrow = slot->quant + (size_t)y * pw;
        int x = 0;

        // --- AVX2: по 8 пикселей за итерацию ---
        if (have_avx2) {
            __m256i mR = _mm256_set1_epi32(0xE0);
            __m256i mB = _mm256_set1_epi32(0xC0);
            int lim8 = (W / 8) * 8;
            for (; x < lim8; x += 8) {
                __m256i pix = _mm256_loadu_si256((__m256i*)(row + x*4));
                __m256i r   = _mm256_and_si256(_mm256_srli_epi32(pix,16), mR);
                __m256i g2  = _mm256_and_si256(_mm256_srli_epi32(pix, 8), mR);
                __m256i b   = _mm256_and_si256(pix,               mB);
                __m256i rg  = _mm256_or_si256(r, _mm256_srli_epi32(g2,3));
                __m256i rgb = _mm256_or_si256(rg, _mm256_srli_epi32(b,6));
                __m128i lo   = _mm256_castsi256_si128(rgb);
                __m128i hi   = _mm256_extracti128_si256(rgb,1);
                __m128i p16  = _mm_packus_epi32(lo, hi);
                __m128i p8   = _mm_packus_epi16(p16, _mm_setzero_si128());
                // unaligned store, т.к. qrow+x выровнен только на 8 байт
                _mm_storeu_si128((__m128i*)(qrow + x), p8);
            }
        }

        // --- SSE2 (или хвост после AVX2) по 4 пикселя ---
        {
            __m128i mR4 = _mm_set1_epi32(0xE0);
            __m128i mB4 = _mm_set1_epi32(0xC0);
            int lim4 = (W / 4) * 4;
            for (; x < lim4; x += 4) {
                __m128i pix = _mm_loadu_si128((__m128i*)(row + x*4));
                __m128i r   = _mm_and_si128(_mm_srli_epi32(pix,16), mR4);
                __m128i g2  = _mm_and_si128(_mm_srli_epi32(pix, 8), mR4);
                __m128i b   = _mm_and_si128(pix,               mB4);
                __m128i rg  = _mm_or_si128(r, _mm_srli_epi32(g2,3));
                __m128i rgb = _mm_or_si128(rg, _mm_srli_epi32(b,6));
                __m128i p16 = _mm_packus_epi32(rgb, _mm_setzero_si128());
                __m128i p8  = _mm_packus_epi16(p16, _mm_setzero_si128());
                *(uint32_t*)(qrow + x) = _mm_cvtsi128_si32(p8);
            }
        }

        // --- Скалярный хвост ---
        for (; x < W; ++x) {
            uint8_t R = row[x*4 + 0];
            uint8_t G = row[x*4 + 1];
            uint8_t B = row[x*4 + 2];
            uint8_t r3 = R >> 5, g3 = G >> 5, b2 = B >> 6;
            qrow[x] = (uint8_t)((r3<<5)|(g3<<2)|b2);
        }

        // --- Сбор гистограмм по блокам 32×32 ---
        int by = y / BS;
        for (int bx = 0; bx < bc; ++bx) {
            int idx = by * bc + bx;
            int x0  = bx * BS;
            int x1  = x0 + BS;
            if (x1 > W) x1 = W;
            for (int xi = x0; xi < x1; ++xi) {
                uint8_t v = qrow[xi];
                hist[idx*256 + v]++;
            }
        }
    }

    // 3) Обработка каждого блока: bg, fg и маска
    for (int by = 0; by < br; ++by) {
        for (int bx = 0; bx < bc; ++bx) {
            int idx = by * bc + bx;
            int *h  = hist + idx*256;

            // находим самый частый цвет
            int best = 0;
            for (int c = 1; c < 256; ++c)
                if (h[c] > h[best]) best = c;
            slot->bg[idx] = (uint8_t)best;

            // ищем второй по частоте, если mixed
            int total_pixels = MIN(BS, W - bx*BS)
                * MIN(BS, H - by*BS);
            int second = best, sc = -1;
            if (h[best] != total_pixels) {
                for (int c = 0; c < 256; ++c) {
                    if (c == best) continue;
                    if (h[c] > sc) { sc = h[c]; second = c; }
                }
            }
            slot->fg[idx] = (uint8_t)second;

            // строим битовую маску, читая из slot->quant
            uint8_t *mask  = slot->mask + (size_t)idx * mask_bytes;
            uint8_t *quant = slot->quant;
            memset(mask, 0, mask_bytes);
            if (second != best) {
                int bit = 0;
                for (int dy = 0; dy < BS; ++dy) {
                    int y = by * BS + dy;
                    if (y >= H) break;
                    for (int dx = 0; dx < BS; ++dx, ++bit) {
                        int x = bx * BS + dx;
                        if (x < W) {
                            uint8_t v = quant[(size_t)y * pw + x];
                            if (v != best) {
                                mask[bit>>3] |= (uint8_t)(1u << (bit&7));
                            }
                        }
                    }
                    // поправка, если блок выходит за границу по X
                    if (bx*BS + BS > W) {
                        bit += BS - (W - bx*BS);
                    }
                }
            }
        }
    }

    // Гарантия завершения всех store-инструкций
    _mm_sfence();

    free(hist);
}

/* ========== Реализация платформо-зависимых функций ========== */

bool platform_init(GlobalContext *ctx, int slots_arg) {
    LinuxPlatformData *pdata = malloc(sizeof(LinuxPlatformData));
    if (!pdata) return false;

    ctx->platform_data = pdata;

    if (!XInitThreads()) {
        free(pdata);
        return false;
    }

    pdata->dpy = XOpenDisplay(NULL);
    if (!pdata->dpy) {
        free(pdata);
        return false;
    }

    int scr = DefaultScreen(pdata->dpy);
    pdata->root = RootWindow(pdata->dpy, scr);
    ctx->w = DisplayWidth(pdata->dpy, scr);
    ctx->h = DisplayHeight(pdata->dpy, scr);

    // Если кол-во слотов не определено, вычисляем их автоматически
    if (slots_arg > 0) {
        ctx->slots = slots_arg;
    } else {
        ctx->slots = calculate_slots(ctx);
    }

    if (!XShmQueryExtension(pdata->dpy)) {
        XCloseDisplay(pdata->dpy);
        free(pdata);
        return false;
    }

    /* Для каждого слота создаём свой XImage/SHM */
    for (uint32_t i = 0; i < ctx->slots; ++i) {
        XShmSegmentInfo *s = &pdata->shm[i];
        pdata->ximg[i] = XShmCreateImage(pdata->dpy,
                                        DefaultVisual(pdata->dpy, scr),
                                        DefaultDepth(pdata->dpy, scr),
                                        ZPixmap,
                                        NULL, s,
                                        ctx->w, ctx->h);
        printf("[init] XImage: depth=%d, bpp=%d, bytes_per_line=%d, byte_order=%s\n",
               pdata->ximg[i]->depth,
               pdata->ximg[i]->bits_per_pixel/8,
               pdata->ximg[i]->bytes_per_line,
               pdata->ximg[i]->byte_order == LSBFirst ? "LSB" : "MSB");
        if (!pdata->ximg[i]) {
            // Очистка уже созданных ресурсов
            for (uint32_t j = 0; j < i; ++j) {
                XShmDetach(pdata->dpy, &pdata->shm[j]);
                shmdt(pdata->shm[j].shmaddr);
                shmctl(pdata->shm[j].shmid, IPC_RMID, NULL);
                XDestroyImage(pdata->ximg[j]);
            }
            XCloseDisplay(pdata->dpy);
            free(pdata);
            return false;
        }

        size_t shmsz = pdata->ximg[i]->bytes_per_line * pdata->ximg[i]->height;
        s->shmid = shmget(IPC_PRIVATE, shmsz, IPC_CREAT | 0600);
        if (s->shmid < 0) {
            XDestroyImage(pdata->ximg[i]);
            XCloseDisplay(pdata->dpy);
            free(pdata);
            return false;
        }

        s->shmaddr = pdata->ximg[i]->data = shmat(s->shmid, NULL, 0);
        if (s->shmaddr == (char*)-1) {
            shmctl(s->shmid, IPC_RMID, NULL);
            XDestroyImage(pdata->ximg[i]);
            XCloseDisplay(pdata->dpy);
            free(pdata);
            return false;
        }

        if (!XShmAttach(pdata->dpy, s)) {
            shmdt(s->shmaddr);
            shmctl(s->shmid, IPC_RMID, NULL);
            XDestroyImage(pdata->ximg[i]);
            XCloseDisplay(pdata->dpy);
            free(pdata);
            return false;
        }
    }

    XSync(pdata->dpy, False);
    printf("[init] Linux X11 %dx%d\n", ctx->w, ctx->h);

    // сохраняем реальный шаг строки RGBA для дальнейшего квантования
    ctx->stride_rgba = pdata->ximg[0]->bytes_per_line;

    return true;
}

void platform_cleanup(GlobalContext *ctx) {
    if (!ctx->platform_data) return;

    LinuxPlatformData *pdata = (LinuxPlatformData*)ctx->platform_data;

    for (uint32_t i = 0; i < ctx->slots; ++i) {
        if (pdata->ximg[i]) {
            XShmDetach(pdata->dpy, &pdata->shm[i]);
            shmdt(pdata->shm[i].shmaddr);
            shmctl(pdata->shm[i].shmid, IPC_RMID, NULL);
            XDestroyImage(pdata->ximg[i]);
        }
    }

    if (pdata->dpy) {
        XCloseDisplay(pdata->dpy);
    }

    free(pdata);
    ctx->platform_data = NULL;
}

bool platform_capture_screen(GlobalContext *ctx, int slot_index) {
    LinuxPlatformData *pdata = (LinuxPlatformData*)ctx->platform_data;

    if (!XShmGetImage(pdata->dpy, pdata->root, pdata->ximg[slot_index], 0, 0, AllPlanes)) {
        return false;
    }

    // Устанавливаем указатель на raw данные для отладки
    ctx->slot[slot_index].raw = (uint8_t*)pdata->ximg[slot_index]->data;

    // Прямая квантизация RGBA → RGB332
    /* platform_quantize_rgba_to_rgb332((uint8_t*)pdata->ximg[slot_index]->data, */
    /*                                   ctx->slot[slot_index].quant, */
    /*                                   ctx->w, ctx->h, ctx->padded_w, */
    /*                                   pdata->ximg[slot_index]->bytes_per_line); */

    quantize_and_analyze((uint8_t*)pdata->ximg[slot_index]->data,
                         &ctx->slot[slot_index], ctx);

    return true;
}

#endif /* PLATFORM_LINUX */
