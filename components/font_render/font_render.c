/* components/font_render/font_render.c
 *
 * Runtime TTF renderer using stb_truetype.
 * Fonts live at /spiffs/fonts/<name>.ttf (LittleFS, mounted as /spiffs).
 * Glyph bitmaps are rasterised on first use and cached in a PSRAM ring-buffer.
 *
 * Requires stb_truetype.h alongside this file — download from
 *   https://github.com/nothings/stb/blob/master/stb_truetype.h
 */

#include "esp_heap_caps.h"      /* must precede the STBTT_malloc/free defines */
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u)  heap_caps_malloc((x), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBTT_free(x, u)    heap_caps_free(x)
#include "stb_truetype.h"

#include "font_render.h"

static const char *TAG = "font_render";

/* ── Faces ─────────────────────────────────────────────────────────────────── */
#define FR_GLYPH_MARGIN_PX  10   /* min pixels between glyph edge and tube edge */
#define FR_MAX_FACES  4

typedef struct {
    bool           loaded;
    char           path[128];
    uint8_t       *font_data;   /* PSRAM, freed on unload */
    stbtt_fontinfo info;
    /* For fr_draw_text: (ascent-descent) / outline_height_of_'0'.
     * Inflates px_size so digit '0' fills the caller's target height. */
    float          norm_ratio;
} fr_face_t;

static fr_face_t s_faces[FR_MAX_FACES];

/* ── Glyph cache (ring-buffer, FIFO eviction) ──────────────────────────────── */
#define FR_CACHE_SIZE  128

typedef struct {
    uint32_t   codepoint;
    uint16_t   px_size;
    uint8_t    face_id;
    bool       valid;
    fr_glyph_t g;
} fr_cache_entry_t;

static fr_cache_entry_t s_cache[FR_CACHE_SIZE];
static int s_cache_next = 0;

/* ── Init ───────────────────────────────────────────────────────────────────── */
void fr_init(void)
{
    memset(s_faces, 0, sizeof(s_faces));
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_next = 0;
}

/* ── Face management ────────────────────────────────────────────────────────── */
int fr_load_face(const char *spiffs_path)
{
    if (!spiffs_path || !spiffs_path[0]) return -1;

    /* Return existing face if already loaded */
    for (int i = 0; i < FR_MAX_FACES; i++) {
        if (s_faces[i].loaded && strcmp(s_faces[i].path, spiffs_path) == 0) {
            return i;
        }
    }

    /* Find a free slot; evict slot 0 if all occupied */
    int slot = -1;
    for (int i = 0; i < FR_MAX_FACES; i++) {
        if (!s_faces[i].loaded) { slot = i; break; }
    }
    if (slot < 0) {
        heap_caps_free(s_faces[0].font_data);
        s_faces[0].loaded = false;
        slot = 0;
    }

    FILE *f = fopen(spiffs_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", spiffs_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512L * 1024L) {
        fclose(f);
        ESP_LOGE(TAG, "font file unusable: %ld bytes", sz);
        return -1;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "PSRAM alloc failed (%ld bytes)", sz);
        return -1;
    }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    int offset = stbtt_GetFontOffsetForIndex(buf, 0);
    if (!stbtt_InitFont(&s_faces[slot].info, buf, offset)) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "stbtt_InitFont failed for %s", spiffs_path);
        return -1;
    }

    s_faces[slot].font_data = buf;
    s_faces[slot].loaded    = true;
    strncpy(s_faces[slot].path, spiffs_path, sizeof(s_faces[slot].path) - 1);
    s_faces[slot].path[sizeof(s_faces[slot].path) - 1] = '\0';

    /* ── Standard EM Scaling (Override Auto-Calibration) ───────────
     * Replaces the old '0' digit bounding-box calibration.
     * Fonts will now render true to their declared internal size,
     * maintaining standard proportions without unexpected scaling. */
    /* ── Cap-Height Normalization (Visual Scaling) ───────────
     * We measure a standard capital 'H' to find out how much of the EM
     * square the letters actually use. This gives us a multiplier so
     * that asking for size "40" yields 40-pixel tall visible letters
     * across all fonts, regardless of their internal descenders. */
    int asc, desc, lg;
    stbtt_GetFontVMetrics(&s_faces[slot].info, &asc, &desc, &lg);
    int em_span = asc - desc;

    int gx0, gy0, gx1, gy1;
    int cap_h = 0;
    // 'H' is the standard typographic anchor for Cap Height
    if (stbtt_GetCodepointBox(&s_faces[slot].info, 'H', &gx0, &gy0, &gx1, &gy1)) {
        cap_h = gy1 - gy0;
    } else if (stbtt_GetCodepointBox(&s_faces[slot].info, '0', &gx0, &gy0, &gx1, &gy1)) {
        cap_h = gy1 - gy0; // Fallback
    }

    s_faces[slot].norm_ratio = (cap_h > 0 && em_span > 0)
        ? (float)em_span / (float)cap_h
        : 1.0f;
    ESP_LOGI(TAG, "loaded %s -> face %d (%ld B PSRAM) norm_ratio=%.2f",
             spiffs_path, slot, sz, s_faces[slot].norm_ratio);

    return slot;
}

void fr_unload_face(int face_id)
{
    if (face_id < 0 || face_id >= FR_MAX_FACES) return;
    fr_face_t *face = &s_faces[face_id];
    if (!face->loaded) return;
    heap_caps_free(face->font_data);
    face->font_data = NULL;
    face->loaded    = false;
    face->path[0]   = '\0';
}

bool fr_face_valid(int face_id)
{
    return face_id >= 0 && face_id < FR_MAX_FACES && s_faces[face_id].loaded;
}

/* ── Cache ──────────────────────────────────────────────────────────────────── */
void fr_cache_flush(void)
{
    for (int i = 0; i < FR_CACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].g.bitmap) {
            heap_caps_free((void *)s_cache[i].g.bitmap);
        }
        memset(&s_cache[i], 0, sizeof(fr_cache_entry_t));
    }
    s_cache_next = 0;
}

/* ── Glyph lookup / rasterise ───────────────────────────────────────────────── */
const fr_glyph_t *fr_get_glyph(uint8_t face_id, uint32_t codepoint, uint16_t px_size)
{
    if (!fr_face_valid(face_id)) return NULL;

    /* Cache hit */
    for (int i = 0; i < FR_CACHE_SIZE; i++) {
        fr_cache_entry_t *e = &s_cache[i];
        if (e->valid && e->face_id == face_id &&
            e->codepoint == codepoint && e->px_size == px_size) {
            return &e->g;
        }
    }

    /* Cache miss — rasterise */
    fr_face_t *face = &s_faces[face_id];
    float scale = stbtt_ScaleForPixelHeight(&face->info, (float)px_size);

    int advance_u, lsb;
    stbtt_GetCodepointHMetrics(&face->info, (int)codepoint, &advance_u, &lsb);

    int ix0, iy0, ix1, iy1;
    stbtt_GetCodepointBitmapBox(&face->info, (int)codepoint,
                                scale, scale, &ix0, &iy0, &ix1, &iy1);
    int bw = ix1 - ix0;
    int bh = iy1 - iy0;

    uint8_t *bitmap = NULL;
    if (bw > 0 && bh > 0) {
        bitmap = (uint8_t *)heap_caps_malloc((size_t)(bw * bh),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bitmap) {
            ESP_LOGE(TAG, "glyph alloc failed (%d B)", bw * bh);
            return NULL;
        }
        stbtt_MakeCodepointBitmap(&face->info, bitmap, bw, bh, bw,
                                  scale, scale, (int)codepoint);
    }

    /* Evict oldest ring-buffer slot */
    fr_cache_entry_t *slot = &s_cache[s_cache_next];
    if (slot->valid && slot->g.bitmap) {
        heap_caps_free((void *)slot->g.bitmap);
    }
    slot->face_id    = face_id;
    slot->codepoint  = codepoint;
    slot->px_size    = px_size;
    slot->valid      = true;
    slot->g.bitmap   = bitmap;
    slot->g.width    = (int16_t)bw;
    slot->g.rows     = (int16_t)bh;
    slot->g.bearing_x = (int16_t)ix0;
    slot->g.bearing_y = (int16_t)(-iy0); /* positive = above baseline */
    slot->g.advance  = (int16_t)(int)(advance_u * scale + 0.5f);
    s_cache_next = (s_cache_next + 1) % FR_CACHE_SIZE;
    return &slot->g;
}

/* ── Pixel blender (big-endian RGB565) ──────────────────────────────────────── */
static inline void fr_blend_px(uint8_t *px,
                                uint8_t r, uint8_t g, uint8_t b, int a)
{
    if (a <= 0) return;
    if (a >= 255) {
        uint16_t c = (((uint16_t)(r >> 3)) << 11) |
                     (((uint16_t)(g >> 2)) << 5)  |
                      ((uint16_t)(b >> 3));
        px[0] = (uint8_t)(c >> 8);
        px[1] = (uint8_t)(c & 0xFF);
        return;
    }
    uint16_t bg = ((uint16_t)px[0] << 8) | px[1];
    int br  = ((bg >> 11) & 0x1F) << 3;
    int bg2 = ((bg >>  5) & 0x3F) << 2;
    int bb  = ((bg >>  0) & 0x1F) << 3;
    int inv = 255 - a;
    int nr = (r * a + br  * inv) >> 8;
    int ng = (g * a + bg2 * inv) >> 8;
    int nb = (b * a + bb  * inv) >> 8;
    uint16_t c = (((uint16_t)(nr >> 3)) << 11) |
                 (((uint16_t)(ng >> 2)) << 5)  |
                  ((uint16_t)(nb >> 3));
    px[0] = (uint8_t)(c >> 8);
    px[1] = (uint8_t)(c & 0xFF);
}

/* ── Blit ───────────────────────────────────────────────────────────────────── */
void fr_blit(uint8_t *fb, int fb_w, int fb_h,
             const fr_glyph_t *glyph,
             int x0, int y0,
             uint8_t cr, uint8_t cg, uint8_t cb,
             bool shadow, uint8_t sr, uint8_t sg, uint8_t sb)
{
    if (!glyph || !glyph->bitmap || glyph->width <= 0 || glyph->rows <= 0) return;

    /* Pass 1: shadow bloom */
    if (shadow) {
        for (int gy = 0; gy < glyph->rows; gy++) {
            for (int gx = 0; gx < glyph->width; gx++) {
                if (glyph->bitmap[gy * glyph->width + gx] < 32) continue;
                for (int dy = -2; dy <= 2; dy++) {
                    int fy = y0 + gy + dy;
                    if (fy < 0 || fy >= fb_h) continue;
                    for (int dx = -2; dx <= 2; dx++) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 == 0 || d2 > 5) continue;
                        int fx = x0 + gx + dx;
                        if (fx < 0 || fx >= fb_w) continue;
                        int sa = (d2 <= 2) ? 180 : 90;
                        fr_blend_px(fb + (fy * fb_w + fx) * 2, sr, sg, sb, sa);
                    }
                }
            }
        }
    }

    /* Pass 2: glyph fill */
    for (int gy = 0; gy < glyph->rows; gy++) {
        int fy = y0 + gy;
        if (fy < 0 || fy >= fb_h) continue;
        for (int gx = 0; gx < glyph->width; gx++) {
            int fx = x0 + gx;
            if (fx < 0 || fx >= fb_w) continue;
            int a = glyph->bitmap[gy * glyph->width + gx];
            if (a > 0) {
                fr_blend_px(fb + (fy * fb_w + fx) * 2, cr, cg, cb, a);
            }
        }
    }
}

/* ── Drawing helpers ────────────────────────────────────────────────────────── */

/* Render a single codepoint centred in the framebuffer.
 *
 * px_size  — desired cap height in pixels (pass FR_DIGIT_CAP_PX for full-tube
 *             clock digits).  The function scales from the per-font digit_px
 *             calibration to achieve this cap height automatically for any font.
 *
 * Universal behaviour guarantee:
 *   • Any TTF dropped in /spiffs/fonts/ is auto-calibrated at fr_load_face
 *     time; no per-font tuning is required here.
 *   • '0' is the size reference.  Glyphs taller than '0' (decorative flourishes,
 *     outlier '1' stems, etc.) are height-capped to match '0'.  Descenders are
 *     NOT capped — they extend below the baseline without shifting the cap upward.
 *   • All glyphs share the same baseline, which is positioned so the px_size-pixel
 *     cap band is vertically centred in the framebuffer.  This eliminates the
 *     "1 and 9 appear taller" visual inconsistency caused by bitmap-bounds
 *     centering. */
void fr_draw_glyph_centered(uint8_t *fb, int fb_w, int fb_h,
                             uint8_t face_id, uint32_t codepoint, uint16_t px_size,
                             uint8_t cr, uint8_t cg, uint8_t cb,
                             bool shadow, uint8_t sr, uint8_t sg, uint8_t sb)
{
    if (!fr_face_valid(face_id)) return;
    const fr_face_t *face = &s_faces[face_id];

    /* 1. MASTER ANCHOR CALCULATION
     * We measure '0' as the ultimate source of truth. We figure out
     * exactly what scale is needed to make '0' fit the height requested. */
    int zx0, zy0, zx1, zy1;
    stbtt_GetCodepointBox(&face->info, '0', &zx0, &zy0, &zx1, &zy1);
    int zero_unscaled_h = zy1 - zy0;
    if (zero_unscaled_h <= 0) return;

    int asc, desc, lg;
    stbtt_GetFontVMetrics(&face->info, &asc, &desc, &lg);
    int em_span = asc - desc;
    if (em_span <= 0) em_span = 2048;

    long calc_adj = (long)px_size * em_span / zero_unscaled_h;
    uint16_t univ_adj = (calc_adj < 8) ? 8 : (uint16_t)calc_adj;

    /* 2. UNIVERSAL WIDTH CAP
     * Scan digits 0-9 to find the absolute widest character.
     * If the widest digit overflows the tube, we shrink the universal scale 
     * so EVERYTHING scales down proportionally, preserving uniform stroke weight. */
    float scale = stbtt_ScaleForPixelHeight(&face->info, (float)univ_adj);
    int max_digit_w = 0;
    
    for (char c = '0'; c <= '9'; c++) {
        int cx0, cy0, cx1, cy1;
        if (stbtt_GetCodepointBox(&face->info, c, &cx0, &cy0, &cx1, &cy1)) {
            int cw = (int)((cx1 - cx0) * scale);
            if (cw > max_digit_w) max_digit_w = cw;
        }
    }

    int max_w = fb_w - FR_GLYPH_MARGIN_PX * 2;
    if (max_digit_w > max_w && max_digit_w > 0) {
        long w_adj = (long)univ_adj * max_w / max_digit_w;
        univ_adj = (w_adj < 8) ? 8 : (uint16_t)w_adj;
    }

    /* 3. Fetch the requested glyph at the universally locked size */
    const fr_glyph_t *probe = fr_get_glyph(face_id, codepoint, univ_adj);
    if (!probe) return;

    /* 3b. Per-glyph width cap — catches letters wider than '0' (e.g. 'K', 'M').
     * The '0' check above keeps digits consistent; this second pass ensures any
     * suffix letter (K/M for social counters, C/F for temperature, etc.) also
     * stays within the tube margins. */
    if (probe->width > 0 && (int)probe->width > max_w) {
        long w_adj = (long)univ_adj * max_w / (int)probe->width;
        univ_adj = (w_adj < 8) ? 8 : (uint16_t)w_adj;
        probe = fr_get_glyph(face_id, codepoint, univ_adj);
        if (!probe) return;
    }

    /* Horizontal Center */
    int x0 = (fb_w - probe->width) / 2;
    int y0;

    /* 4. UNIVERSAL BASELINE ALIGNMENT
     * We physically center the '0' in the display, find its baseline, 
     * and force all other numbers to share that exact same invisible line. */
    const fr_glyph_t *zero_glyph = fr_get_glyph(face_id, '0', univ_adj);
    if (zero_glyph) {
        // Find where '0' naturally sits when mathematically centered
        int zero_centered_y = (fb_h - zero_glyph->rows) / 2;
        int universal_baseline = zero_centered_y + zero_glyph->bearing_y;

        if (codepoint >= '0' && codepoint <= '9') {
            /* All numbers stand perfectly on the shared baseline */
            y0 = universal_baseline - probe->bearing_y;
        } else {
            /* Punctuation (like ':') ignores baselines and perfectly centers itself */
            y0 = (fb_h - probe->rows) / 2;
        }
    } else {
        /* Failsafe */
        y0 = (fb_h - probe->rows) / 2;
    }

    /* Prevent off-screen clipping */
    if (y0 < 0) y0 = 0;

    fr_blit(fb, fb_w, fb_h, probe, x0, y0, cr, cg, cb, shadow, sr, sg, sb);
}

/* ── UTF-8 decoder ──────────────────────────────────────────────────────────── */
static uint32_t fr_utf8_next(const char **p)
{
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    (*p)++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {
        uint32_t cp = (uint32_t)(c & 0x1F) << 6;
        cp |= (unsigned char)*((*p)++) & 0x3F;
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        uint32_t cp = (uint32_t)(c & 0x0F) << 12;
        cp |= ((unsigned char)*((*p)++) & 0x3F) << 6;
        cp |= (unsigned char)*((*p)++) & 0x3F;
        return cp;
    }
    if ((c & 0xF8) == 0xF0) {
        uint32_t cp = (uint32_t)(c & 0x07) << 18;
        cp |= ((unsigned char)*((*p)++) & 0x3F) << 12;
        cp |= ((unsigned char)*((*p)++) & 0x3F) << 6;
        cp |= (unsigned char)*((*p)++) & 0x3F;
        return cp;
    }
    return '?';
}

void fr_draw_text(uint8_t *fb, int fb_w, int fb_h,
                  int cx, int baseline_y,
                  uint8_t face_id, uint16_t px_size,
                  const char *utf8_str,
                  uint8_t cr, uint8_t cg, uint8_t cb,
                  bool shadow, uint8_t sr, uint8_t sg, uint8_t sb)
{
    if (!utf8_str || !utf8_str[0] || !fr_face_valid(face_id)) return;

    const uint16_t orig_px = px_size;   /* caller's target before norm_ratio */

    /* Inflate px_size by norm_ratio so that digit '0' fills orig_px pixels.
     * The height-fit pass below then scales everything back down if any glyph
     * exceeds orig_px, keeping the whole string within the caller's target. */
    {
        float ratio = s_faces[face_id].norm_ratio;
        if (ratio > 1.001f) {
            uint16_t np = (uint16_t)((float)px_size * ratio + 0.5f);
            px_size = (np < 4) ? 4 : np;
        }
    }

    const int max_w = fb_w - FR_GLYPH_MARGIN_PX * 2;

    /* Pass 1: measure total advance at requested size */
    int total_adv = 0;
    const char *p = utf8_str;
    uint32_t cp;
    while ((cp = fr_utf8_next(&p)) != 0) {
        const fr_glyph_t *g = fr_get_glyph(face_id, cp, px_size);
        if (g) total_adv += g->advance;
    }

    /* Width-fit: if text overflows the tube (minus margins), scale px_size down
     * and re-measure so centering uses the accurate shrunken advance total. */
    uint16_t adj_px = px_size;
    if (total_adv > max_w && total_adv > 0) {
        adj_px = (uint16_t)((long)px_size * max_w / total_adv);
        if (adj_px < 4) adj_px = 4;
        total_adv = 0;
        p = utf8_str;
        while ((cp = fr_utf8_next(&p)) != 0) {
            const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
            if (g) total_adv += g->advance;
        }
    }

    /* Height-fit: norm_ratio is calibrated to digit "0"; glyphs with a taller
     * native bounding box (e.g. "1" in Jim Nightshade) render above orig_px.
     * Find the tallest glyph and scale adj_px back down so it fits. */
    {
        int max_h = 0;
        p = utf8_str;
        while ((cp = fr_utf8_next(&p)) != 0) {
            const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
            if (g && (int)g->rows > max_h) max_h = (int)g->rows;
        }
        if (max_h > (int)orig_px && max_h > 0) {
            uint16_t h_adj = (uint16_t)((long)adj_px * (int)orig_px / max_h);
            if (h_adj < 4) h_adj = 4;
            adj_px = h_adj;
            total_adv = 0;
            p = utf8_str;
            while ((cp = fr_utf8_next(&p)) != 0) {
                const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
                if (g) total_adv += g->advance;
            }
        }
    }

    /* Pass 2: render */
    int pen_x = cx - total_adv / 2;
    p = utf8_str;
    while ((cp = fr_utf8_next(&p)) != 0) {
        const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
        if (!g) continue;
        int x0 = pen_x + g->bearing_x;
        int y0 = baseline_y - g->bearing_y;

        /* TTF UI Fix: Visually center the colon with the digits */
        if (cp == ':') {
            // Nudge the Y-coordinate UP by a fraction of the font size
            y0 -= (adj_px / 8); 
        }

        fr_blit(fb, fb_w, fb_h, g, x0, y0, cr, cg, cb, shadow, sr, sg, sb);
        pen_x += g->advance;
    }
}

/* Return the total pixel advance of utf8_str at the given face/size, applying
 * the same norm_ratio and width-fit as fr_draw_text.  Callers use the result
 * to compute label center positions before rendering.  Returns 0 on error.  */
int fr_measure_text(int fb_w, uint8_t face_id, uint16_t px_size, const char *utf8_str)
{
    if (!utf8_str || !utf8_str[0] || !fr_face_valid(face_id)) return 0;

    const uint16_t orig_px = px_size;   /* caller's target before norm_ratio */

    {
        float ratio = s_faces[face_id].norm_ratio;
        if (ratio > 1.001f) {
            uint16_t np = (uint16_t)((float)px_size * ratio + 0.5f);
            px_size = (np < 4) ? 4 : np;
        }
    }

    const int max_w = fb_w - FR_GLYPH_MARGIN_PX * 2;

    int total_adv = 0;
    const char *p = utf8_str;
    uint32_t cp;
    while ((cp = fr_utf8_next(&p)) != 0) {
        const fr_glyph_t *g = fr_get_glyph(face_id, cp, px_size);
        if (g) total_adv += g->advance;
    }

    uint16_t adj_px = px_size;
    if (total_adv > max_w && total_adv > 0) {
        adj_px = (uint16_t)((long)px_size * max_w / total_adv);
        if (adj_px < 4) adj_px = 4;
        total_adv = 0;
        p = utf8_str;
        while ((cp = fr_utf8_next(&p)) != 0) {
            const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
            if (g) total_adv += g->advance;
        }
    }

    /* Height-fit: apply the same pass as fr_draw_text so the returned advance
     * matches what fr_draw_text will actually use for centering.  Without this,
     * fonts whose glyphs exceed orig_px (e.g. "1" in many display fonts) trigger
     * a further px_size reduction inside fr_draw_text that makes the rendered
     * text narrower than fr_measure_text reported — causing label overlap when
     * the measured half-widths are used to position two adjacent labels.       */
    {
        int max_h = 0;
        p = utf8_str;
        while ((cp = fr_utf8_next(&p)) != 0) {
            const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
            if (g && (int)g->rows > max_h) max_h = (int)g->rows;
        }
        if (max_h > (int)orig_px && max_h > 0) {
            uint16_t h_adj = (uint16_t)((long)adj_px * (int)orig_px / max_h);
            if (h_adj < 4) h_adj = 4;
            adj_px = h_adj;
            total_adv = 0;
            p = utf8_str;
            while ((cp = fr_utf8_next(&p)) != 0) {
                const fr_glyph_t *g = fr_get_glyph(face_id, cp, adj_px);
                if (g) total_adv += g->advance;
            }
        }
    }

    return total_adv;
}
