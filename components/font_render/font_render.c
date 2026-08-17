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
    /* LRU eviction tick (see s_face_use_ctr) — bumped on load and on every
     * subsequent use, so fr_load_face() can evict the actually-coldest slot
     * instead of always slot 0. */
    uint32_t       last_used;
} fr_face_t;

static fr_face_t s_faces[FR_MAX_FACES];
/* Monotonically increasing use-order counter for face LRU tracking — a
 * simple tick, not wall-clock time, so this needs no time-source dependency. */
static uint32_t s_face_use_ctr = 0;

static inline void fr_face_touch(int face_id)
{
    if (face_id >= 0 && face_id < FR_MAX_FACES)
        s_faces[face_id].last_used = ++s_face_use_ctr;
}

/* ── Glyph cache (ring-buffer, FIFO eviction) ──────────────────────────────── */
#define FR_CACHE_SIZE  128

typedef struct {
    uint32_t   codepoint;
    uint16_t   px_size;
    uint8_t    face_id;
    bool       valid;
    bool       failed;   /* true = rasterisation OOM'd; g.bitmap is NULL and
                           * MUST NOT be retried — see fr_get_glyph's comment */
    fr_glyph_t g;
} fr_cache_entry_t;

static fr_cache_entry_t s_cache[FR_CACHE_SIZE];
static int s_cache_next = 0;
static int s_cache_mru  = -1;   /* most-recently-hit slot; see fr_get_glyph */

/* Per-(face,size,fb) layout memo for fr_draw_glyph_centered().  The universal
 * scale (univ_adj) and the digit baseline depend only on the face, target pixel
 * height and framebuffer size — not on the codepoint — yet computing them runs
 * a 0-9 glyph-box scan (10× stbtt_GetCodepointBox) plus the font v-metrics.
 * Memoise so a steady clock face pays that once instead of once per digit per
 * frame.  Invalidated by fr_cache_flush(), which runs on every face change. */
typedef struct {
    bool     valid;
    uint8_t  face_id;
    uint16_t px_size;
    int16_t  fb_w, fb_h;
    uint16_t univ_adj;
    int      universal_baseline;
    bool     baseline_valid;
} fr_layout_memo_t;
#define FR_LAYOUT_MEMO_N 4
static fr_layout_memo_t s_layout_memo[FR_LAYOUT_MEMO_N];
static int s_layout_memo_next = 0;

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
            fr_face_touch(i);
            return i;
        }
    }

    /* Find a free slot; evict the least-recently-used face if all occupied */
    int slot = -1;
    for (int i = 0; i < FR_MAX_FACES; i++) {
        if (!s_faces[i].loaded) { slot = i; break; }
    }
    if (slot < 0) {
        int lru = 0;
        for (int i = 1; i < FR_MAX_FACES; i++) {
            if (s_faces[i].last_used < s_faces[lru].last_used) lru = i;
        }
        /* Evicted face_id is REUSED for the new font, so any cached glyph
         * bitmaps / layout memos keyed to that face_id would be served for the
         * wrong font (stale glyphs, wrong baseline).  The display side skips
         * its own flush when the returned id is unchanged (see
         * wl_refresh_ft_face) — so the flush must happen here. */
        fr_cache_flush();
        heap_caps_free(s_faces[lru].font_data);
        s_faces[lru].loaded = false;
        slot = lru;
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
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "short read on %s (%u/%ld B)", spiffs_path, (unsigned)rd, sz);
        return -1;
    }

    int offset = stbtt_GetFontOffsetForIndex(buf, 0);
    if (!stbtt_InitFont(&s_faces[slot].info, buf, offset)) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "stbtt_InitFont failed for %s", spiffs_path);
        return -1;
    }

    s_faces[slot].font_data = buf;
    s_faces[slot].loaded    = true;
    fr_face_touch(slot);
    strncpy(s_faces[slot].path, spiffs_path, sizeof(s_faces[slot].path) - 1);
    s_faces[slot].path[sizeof(s_faces[slot].path) - 1] = '\0';

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
    s_cache_mru  = -1;
    /* The layout memo is keyed by face_id, which may now point at a different
     * font — drop it so univ_adj / baseline are recomputed for the new face. */
    memset(s_layout_memo, 0, sizeof(s_layout_memo));
    s_layout_memo_next = 0;
}

/* ── Glyph lookup / rasterise ───────────────────────────────────────────────── */

/* s_cache_mru (declared with the cache above): the render paths look the
 * SAME glyph up twice in a row (shadow pass then foreground pass), and
 * measure+draw makes several passes over the same string — a one-slot MRU
 * check skips most of the 128-entry linear scans. */
const fr_glyph_t *fr_get_glyph(uint8_t face_id, uint32_t codepoint, uint16_t px_size)
{
    if (!fr_face_valid(face_id)) return NULL;

    /* Every glyph lookup is a use of this face — keep LRU eviction in
     * fr_load_face() from picking a face that's actually still active. */
    fr_face_touch(face_id);

    /* MRU fast path */
    if (s_cache_mru >= 0) {
        fr_cache_entry_t *m = &s_cache[s_cache_mru];
        if (m->valid && m->face_id == face_id &&
            m->codepoint == codepoint && m->px_size == px_size) {
            return &m->g;
        }
    }

    /* Cache hit */
    for (int i = 0; i < FR_CACHE_SIZE; i++) {
        fr_cache_entry_t *e = &s_cache[i];
        if (e->valid && e->face_id == face_id &&
            e->codepoint == codepoint && e->px_size == px_size) {
            s_cache_mru = i;
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
    bool raster_failed = false;
    if (bw > 0 && bh > 0) {
        bitmap = (uint8_t *)heap_caps_malloc((size_t)(bw * bh),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bitmap) {
            /* Cache this as a permanent (until reboot) failure below instead
             * of returning NULL here — otherwise every subsequent draw of
             * this glyph re-attempts the same doomed allocation and re-logs
             * this error every frame.  bw/bh are dropped to 0 so callers
             * (fr_blit et al.) treat this exactly like a blank glyph; only
             * bearing/advance stay valid so text layout is unaffected. */
            ESP_LOGE(TAG, "glyph alloc failed (%d B) — caching as failed, no retry", bw * bh);
            raster_failed = true;
        } else {
            stbtt_MakeCodepointBitmap(&face->info, bitmap, bw, bh, bw,
                                      scale, scale, (int)codepoint);
        }
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
    slot->failed     = raster_failed;
    slot->g.bitmap   = bitmap;
    slot->g.width    = raster_failed ? 0 : (int16_t)bw;
    slot->g.rows     = raster_failed ? 0 : (int16_t)bh;
    slot->g.bearing_x = (int16_t)ix0;
    slot->g.bearing_y = (int16_t)(-iy0); /* positive = above baseline */
    slot->g.advance  = (int16_t)(int)(advance_u * scale + 0.5f);
    s_cache_mru  = s_cache_next;   /* the shadow/foreground pair re-hits this */
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
/* Compute (or fetch from the memo) the codepoint-independent layout for a
 * face/size/framebuffer: the universal scale univ_adj (size that makes '0' the
 * requested height, then capped so the widest digit fits the tube) and the
 * universal baseline (where '0' sits when vertically centred).  These are the
 * expensive parts of fr_draw_glyph_centered() — a 0-9 box scan + v-metrics —
 * and they do not depend on which glyph is being drawn, so memoising them turns
 * per-digit-per-frame work into once-per-face/size.  Returns NULL only when '0'
 * has no usable bounding box (degenerate font). */
static fr_layout_memo_t *fr_get_layout(uint8_t face_id, uint16_t px_size,
                                       int fb_w, int fb_h)
{
    for (int i = 0; i < FR_LAYOUT_MEMO_N; i++) {
        fr_layout_memo_t *m = &s_layout_memo[i];
        if (m->valid && m->face_id == face_id && m->px_size == px_size &&
            m->fb_w == (int16_t)fb_w && m->fb_h == (int16_t)fb_h)
            return m;
    }

    const fr_face_t *face = &s_faces[face_id];

    /* MASTER ANCHOR — measure '0', derive the scale that makes it `px_size` tall. */
    int zx0, zy0, zx1, zy1;
    stbtt_GetCodepointBox(&face->info, '0', &zx0, &zy0, &zx1, &zy1);
    int zero_unscaled_h = zy1 - zy0;
    if (zero_unscaled_h <= 0) return NULL;

    int asc, desc, lg;
    stbtt_GetFontVMetrics(&face->info, &asc, &desc, &lg);
    int em_span = asc - desc;
    if (em_span <= 0) em_span = 2048;

    long calc_adj = (long)px_size * em_span / zero_unscaled_h;
    uint16_t univ_adj = (calc_adj < 8) ? 8 : (uint16_t)calc_adj;

    /* UNIVERSAL WIDTH CAP — shrink uniformly if the widest digit overflows. */
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

    /* UNIVERSAL BASELINE — where '0' sits when vertically centred. */
    int  universal_baseline = 0;
    bool baseline_valid = false;
    const fr_glyph_t *zero_glyph = fr_get_glyph(face_id, '0', univ_adj);
    if (zero_glyph) {
        int zero_centered_y = (fb_h - zero_glyph->rows) / 2;
        universal_baseline  = zero_centered_y + zero_glyph->bearing_y;
        baseline_valid      = true;
    }

    fr_layout_memo_t *m = &s_layout_memo[s_layout_memo_next];
    m->valid              = true;
    m->face_id            = face_id;
    m->px_size            = px_size;
    m->fb_w               = (int16_t)fb_w;
    m->fb_h               = (int16_t)fb_h;
    m->univ_adj           = univ_adj;
    m->universal_baseline = universal_baseline;
    m->baseline_valid     = baseline_valid;
    s_layout_memo_next = (s_layout_memo_next + 1) % FR_LAYOUT_MEMO_N;
    return m;
}

void fr_draw_glyph_centered(uint8_t *fb, int fb_w, int fb_h,
                             uint8_t face_id, uint32_t codepoint, uint16_t px_size,
                             uint8_t cr, uint8_t cg, uint8_t cb,
                             bool shadow, uint8_t sr, uint8_t sg, uint8_t sb)
{
    if (!fr_face_valid(face_id)) return;

    /* Universal scale + baseline are codepoint-independent — pull them from the
     * memo (computed once per face/size/fb) instead of re-running the 0-9 box
     * scan and v-metrics on every glyph. */
    fr_layout_memo_t *L = fr_get_layout(face_id, px_size, fb_w, fb_h);
    if (!L) return;
    uint16_t univ_adj = L->univ_adj;
    int      max_w    = fb_w - FR_GLYPH_MARGIN_PX * 2;

    /* Fetch the requested glyph at the universally locked size. */
    const fr_glyph_t *probe = fr_get_glyph(face_id, codepoint, univ_adj);
    if (!probe) return;

    /* Per-glyph width cap — catches letters wider than '0' (e.g. 'K', 'M').
     * Never fires for digits (the 0-9 scan already capped width), so the
     * memoised baseline stays exact for the digit case below. */
    if (probe->width > 0 && (int)probe->width > max_w) {
        long w_adj = (long)univ_adj * max_w / (int)probe->width;
        univ_adj = (w_adj < 8) ? 8 : (uint16_t)w_adj;
        probe = fr_get_glyph(face_id, codepoint, univ_adj);
        if (!probe) return;
    }

    int x0 = (fb_w - probe->width) / 2;     /* horizontal centre */
    int y0;
    if (L->baseline_valid && codepoint >= '0' && codepoint <= '9') {
        /* All digits stand on the shared baseline so they don't jitter. */
        y0 = L->universal_baseline - probe->bearing_y;
    } else {
        /* Punctuation / letters centre by their own height. */
        y0 = (fb_h - probe->rows) / 2;
    }
    if (y0 < 0) y0 = 0;

    fr_blit(fb, fb_w, fb_h, probe, x0, y0, cr, cg, cb, shadow, sr, sg, sb);
}

/* ── UTF-8 decoder ──────────────────────────────────────────────────────────── */
/* Consume one continuation byte; a NUL (string byte-truncated mid-codepoint,
 * e.g. by a snprintf cut) must NOT be stepped over — without this check the
 * decoder walked past the terminator and kept reading out-of-bounds until it
 * happened to hit a zero byte. */
static inline uint32_t fr_utf8_cont(const char **p)
{
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;          /* leave *p on the NUL so the caller loop ends */
    (*p)++;
    return (uint32_t)(c & 0x3F);
}

static uint32_t fr_utf8_next(const char **p)
{
    unsigned char c = (unsigned char)**p;
    if (!c) return 0;
    (*p)++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {
        uint32_t cp = (uint32_t)(c & 0x1F) << 6;
        cp |= fr_utf8_cont(p);
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        uint32_t cp = (uint32_t)(c & 0x0F) << 12;
        cp |= fr_utf8_cont(p) << 6;
        cp |= fr_utf8_cont(p);
        return cp;
    }
    if ((c & 0xF8) == 0xF0) {
        uint32_t cp = (uint32_t)(c & 0x07) << 18;
        cp |= fr_utf8_cont(p) << 12;
        cp |= fr_utf8_cont(p) << 6;
        cp |= fr_utf8_cont(p);
        return cp;
    }
    return '?';
}

/* Shared sizing computation for fr_draw_text() / fr_measure_text(): inflates
 * requested_px by the face's norm_ratio (so digit '0' fills the caller's
 * target height), then applies width-fit (shrink to the tube's max_w if the
 * string overflows) and height-fit (shrink further if any glyph's native
 * bounding box renders taller than the caller's original target).  Both
 * functions used to duplicate this ~40-line pass sequence almost verbatim,
 * with only a comment noting they had to be kept in sync manually — factored
 * here so fr_measure_text()'s reported advance can never desync from what
 * fr_draw_text() actually renders.
 * Returns the total pixel advance at the final fitted size; *out_adj_px
 * receives that final px size (needed by fr_draw_text()'s render pass). */
static int fr_compute_fit_size(int fb_w, uint8_t face_id, uint16_t requested_px,
                                const char *utf8_str, uint16_t *out_adj_px)
{
    const uint16_t orig_px = requested_px;   /* caller's target before norm_ratio */

    /* Inflate px_size by norm_ratio so that digit '0' fills orig_px pixels.
     * The height-fit pass below then scales everything back down if any glyph
     * exceeds orig_px, keeping the whole string within the caller's target. */
    uint16_t px_size = requested_px;
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

    *out_adj_px = adj_px;
    return total_adv;
}

void fr_draw_text(uint8_t *fb, int fb_w, int fb_h,
                  int cx, int baseline_y,
                  uint8_t face_id, uint16_t px_size,
                  const char *utf8_str,
                  uint8_t cr, uint8_t cg, uint8_t cb,
                  bool shadow, uint8_t sr, uint8_t sg, uint8_t sb)
{
    if (!utf8_str || !utf8_str[0] || !fr_face_valid(face_id)) return;

    uint16_t adj_px;
    int total_adv = fr_compute_fit_size(fb_w, face_id, px_size, utf8_str, &adj_px);

    /* Pass 2: render */
    int pen_x = cx - total_adv / 2;
    const char *p = utf8_str;
    uint32_t cp;
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
 * the same norm_ratio, width-fit and height-fit as fr_draw_text (both share
 * fr_compute_fit_size() so the two can never desync).  Callers use the
 * result to compute label center positions before rendering.  Returns 0 on
 * error. */
int fr_measure_text(int fb_w, uint8_t face_id, uint16_t px_size, const char *utf8_str)
{
    if (!utf8_str || !utf8_str[0] || !fr_face_valid(face_id)) return 0;

    uint16_t adj_px;
    return fr_compute_fit_size(fb_w, face_id, px_size, utf8_str, &adj_px);
}
