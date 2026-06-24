#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Target cap height (pixels) for full-tube clock-digit glyphs.
 * Pass as px_size to fr_draw_glyph_centered.  fr_load_face calibrates every
 * font so '0' renders at exactly this height; the function then baseline-aligns
 * all other digits against it.  Changing this value requires a full reflash of
 * the fonts partition (digit_px is computed at load time against this constant). */
#define FR_DIGIT_CAP_PX  130

/* Rasterised glyph descriptor.
 * bitmap   – 8-bpp grayscale alpha mask, row-major, width×rows bytes in PSRAM.
 * bearing_x – x offset from pen to left edge of bitmap (pixels; raw stbtt ix0).
 * bearing_y – y offset from baseline to TOP edge of bitmap, positive = above
 *              baseline (i.e. -iy0 from stbtt).
 * advance  – advance width in pixels. */
typedef struct {
    const uint8_t *bitmap;
    int16_t  width;
    int16_t  rows;
    int16_t  bearing_x;
    int16_t  bearing_y;
    int16_t  advance;
} fr_glyph_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */
void  fr_init(void);

/* Load a TTF from the VFS (e.g. "/spiffs/fonts/MyFont.ttf").
 * Returns a face_id (0 .. FR_MAX_FACES-1) on success, or -1.
 * If the same path is already loaded, the existing id is returned immediately. */
int   fr_load_face(const char *spiffs_path);

/* Release a face.  Cached glyphs for that face remain in PSRAM until evicted
 * by the ring-buffer or an explicit fr_cache_flush() call. */
void  fr_unload_face(int face_id);

/* Evict all cached glyph bitmaps (loaded faces are kept). */
void  fr_cache_flush(void);

/* Returns true when face_id was loaded successfully and is still valid. */
bool  fr_face_valid(int face_id);

/* ── Glyph access ─────────────────────────────────────────────────────────────
 * Returns a cached (or freshly rasterised) glyph, or NULL on error.
 * The pointer is valid until the ring-buffer evicts the slot or
 * fr_cache_flush() is called — do not free it. */
const fr_glyph_t *fr_get_glyph(uint8_t face_id, uint32_t codepoint, uint16_t px_size);

/* ── Blit ─────────────────────────────────────────────────────────────────────
 * Composite a glyph into a big-endian RGB565 framebuffer.
 * x0, y0  : top-left corner of the glyph bitmap in framebuffer coordinates.
 * cr/cg/cb: glyph fill colour.
 * shadow  : when true, paints a dark bloom ring around the glyph. */
void  fr_blit(uint8_t *fb, int fb_w, int fb_h,
              const fr_glyph_t *glyph,
              int x0, int y0,
              uint8_t cr, uint8_t cg, uint8_t cb,
              bool shadow, uint8_t sr, uint8_t sg, uint8_t sb);

/* Render a single codepoint visually centred (both axes) in the framebuffer. */
void  fr_draw_glyph_centered(uint8_t *fb, int fb_w, int fb_h,
                              uint8_t face_id, uint32_t codepoint, uint16_t px_size,
                              uint8_t cr, uint8_t cg, uint8_t cb,
                              bool shadow, uint8_t sr, uint8_t sg, uint8_t sb);

/* Render a UTF-8 string horizontally centred at column cx, baseline at baseline_y. */
void  fr_draw_text(uint8_t *fb, int fb_w, int fb_h,
                   int cx, int baseline_y,
                   uint8_t face_id, uint16_t px_size,
                   const char *utf8_str,
                   uint8_t cr, uint8_t cg, uint8_t cb,
                   bool shadow, uint8_t sr, uint8_t sg, uint8_t sb);

/* Return the total pixel advance of utf8_str rendered at face_id/px_size,
 * applying the same norm_ratio and width-fit as fr_draw_text.
 * Pass the framebuffer width as fb_w (used for the width-fit cap).
 * Returns 0 on error.  Does not render anything. */
int   fr_measure_text(int fb_w, uint8_t face_id, uint16_t px_size, const char *utf8_str);
