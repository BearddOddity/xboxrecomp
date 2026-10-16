/*
 * gui -- immediate-mode GUI core. See gui.h for the model and the API.
 *
 * Layout of this file:
 *   draw lists      vertices, indices and clip-rectangle commands; paths,
 *                   anti-aliased convex fills and lines, text
 *   font            one TrueType face baked once (stb_truetype) into a
 *                   coverage atlas, glyphs 32..255
 *   context         input edges, navigation repeat, windows and their order
 *   windows         begin/end: background, title, close button, scrollbar,
 *                   resize grip, moving
 *   layout, widgets
 *   render          every window's list merged back to front
 *
 * Anti-aliasing works the way Dear ImGui's does: a filled shape gets a
 * one-pixel fringe that fades to transparent, so rounded corners and the
 * check mark stay smooth without MSAA.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define GUI_PI          3.14159265358979f
#define GUI_MAX_WINDOWS 32
#define GUI_MAX_NAV     512
#define GUI_MAX_IDS     64
#define GUI_MAX_WIDTHS  16
#define GUI_STORAGE     1024
#define GUI_MAX_TABS    32
#define GUI_PATH_MAX    512
#define GUI_GLYPH_FIRST 32
#define GUI_GLYPH_COUNT 224         /* 32..255: ASCII and Latin-1 */

#define GUI_WINDOW_POPUP_ (1 << 16) /* internal: a combo's drop-down list */

enum { CORNER_TL = 1, CORNER_TR = 2, CORNER_BR = 4, CORNER_BL = 8,
       CORNERS_TOP = 3, CORNERS_RIGHT = 6, CORNERS_BOTTOM = 12, CORNERS_LEFT = 9, CORNERS_ALL = 15 };

static float fminf_(float a, float b) { return a < b ? a : b; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ── Draw lists ───────────────────────────────────────────────── */

typedef struct {
    GuiVertex  *vtx;
    uint32_t   *idx;
    GuiDrawCmd *cmd;
    int vn, vcap, in, icap, cn, ccap;
    float clip[4];
} DrawList;

static void *grow(void *p, int *cap, int need, size_t elem)
{
    int n;
    if (need <= *cap)
        return p;
    n = *cap ? *cap * 2 : 256;
    while (n < need)
        n *= 2;
    p = realloc(p, (size_t)n * elem);
    if (!p)
        abort();                    /* out of memory drawing a debug overlay */
    *cap = n;
    return p;
}

static void dl_free(DrawList *d)
{
    free(d->vtx); free(d->idx); free(d->cmd);
    memset(d, 0, sizeof *d);
}

static void dl_reset(DrawList *d) { d->vn = d->in = d->cn = 0; }

static void dl_set_clip(DrawList *d, float x0, float y0, float x1, float y1)
{
    d->clip[0] = x0; d->clip[1] = y0; d->clip[2] = x1; d->clip[3] = y1;
}

/* Room for nv vertices and ni indices, in a command with the current clip. */
static uint32_t dl_reserve(DrawList *d, int nv, int ni)
{
    GuiDrawCmd *c = d->cn ? &d->cmd[d->cn - 1] : NULL;
    d->vtx = (GuiVertex *)grow(d->vtx, &d->vcap, d->vn + nv, sizeof(GuiVertex));
    d->idx = (uint32_t *)grow(d->idx, &d->icap, d->in + ni, sizeof(uint32_t));
    if (!c || memcmp(c->clip, d->clip, sizeof d->clip)) {
        if (c && c->idx_count == 0) {
            memcpy(c->clip, d->clip, sizeof d->clip);
        } else {
            d->cmd = (GuiDrawCmd *)grow(d->cmd, &d->ccap, d->cn + 1, sizeof(GuiDrawCmd));
            c = &d->cmd[d->cn++];
            memcpy(c->clip, d->clip, sizeof d->clip);
            c->idx_offset = (uint32_t)d->in;
            c->idx_count = 0;
        }
    }
    d->cmd[d->cn - 1].idx_count += (uint32_t)ni;
    return (uint32_t)d->vn;
}

static void dl_v(DrawList *d, float x, float y, float u, float v, uint32_t col)
{
    GuiVertex *p = &d->vtx[d->vn++];
    p->x = x; p->y = y; p->u = u; p->v = v; p->col = col;
}

static void dl_i3(DrawList *d, uint32_t a, uint32_t b, uint32_t c)
{
    d->idx[d->in++] = a; d->idx[d->in++] = b; d->idx[d->in++] = c;
}

/* ── Context ──────────────────────────────────────────────────── */

typedef struct GuiWindow {
    uint32_t id;
    char     name[64];
    int      flags;
    int      active, was_active;    /* submitted this frame / last frame */
    int      created_frame;
    int      measured;              /* frames ended: content size is known after 1 */
    int      autofit;               /* size to content once it is measured */
    int      hidden;                /* not drawn this frame (being measured) */
    int      pos_pending;           /* apply set_pos once the size is known */
    float    pend_x, pend_y, pend_px, pend_py;
    float    x, y, w, h;
    float    min_w;
    float    content_w, content_h;  /* last frame's */
    float    scroll_y, scroll_max;
    int      has_scrollbar;
    float    title_h;
    float    inner_x0, inner_y0, inner_x1, inner_y1;   /* content region */
    float    clip[4];
    /* layout */
    float    cx, cy, line_x0, indent;
    float    line_h, line_text_off;
    float    prev_x, prev_y, prev_line_h, prev_text_off;
    float    max_x, max_y, content_x0, content_y0;
    float    widths[GUI_MAX_WIDTHS];
    int      nwidths;
    float    last[4];
    uint32_t last_id;
    int      last_hovered;
    uint32_t nav_last_id;
    int      id_depth;
    DrawList dl;
} GuiWindow;

typedef struct { uint32_t key; int val; } StorageSlot;

static struct {
    int      ready;
    GuiIO    io;
    GuiStyle style;
    int      frame;

    /* font */
    unsigned char   *ttf;
    uint8_t         *atlas;
    int              atlas_w, atlas_h;
    stbtt_packedchar chars[GUI_GLYPH_COUNT];
    float            font_size, ascent, white_u, white_v;

    /* windows */
    GuiWindow  windows[GUI_MAX_WINDOWS];
    int        nwin;
    GuiWindow *order[GUI_MAX_WINDOWS];   /* back to front */
    GuiWindow *stack[8];
    int        nstack;
    GuiWindow *cur, *hovered;
    uint32_t   ids[GUI_MAX_IDS];
    int        nids;
    struct {
        int   pos_set, pos_cond, size_set, size_cond, alpha_set, focus;
        float x, y, px, py, w, h, alpha, min_w;
    } next;

    /* mouse */
    int      mprev[2], mclicked[2], mreleased[2];
    float    mprev_x, mprev_y;
    uint32_t active_id;
    int      active_seen;
    float    active_off_x, active_off_y;

    /* navigation */
    GuiWindow *nav_win;
    uint32_t   nav_id;
    struct { uint32_t id; GuiWindow *win; } nav_items[GUI_MAX_NAV];
    int        nav_count, nav_move, nav_scroll_to, nav_visible;
    int        nav_pressed[GUI_NAV_COUNT];
    float      nav_held[GUI_NAV_COUNT];

    /* the one open popup (a combo's list) */
    uint32_t   popup_id, popup_return_id, popup_closed_by_click;
    GuiWindow *popup_owner;
    int        popup_seen, popup_nav_init;

    /* tab bar being built */
    struct {
        uint32_t id, sel, ids[GUI_MAX_TABS];
        int      n;
    } tab;

    StorageSlot storage[GUI_STORAGE];
    char        tbuf[1024];

    DrawList out;
    GuiDrawData dd;
} G;

static GuiVec2 s_path[GUI_PATH_MAX];
static GuiVec2 s_nrm[GUI_PATH_MAX];
static int     s_pn;

GuiIO *gui_io(void) { return &G.io; }
GuiStyle *gui_style(void) { return &G.style; }
int gui_ready(void) { return G.ready; }
float gui_font_size(void) { return G.font_size; }

static uint32_t col32c(GuiColor c)
{
    float a = clampf(c.a * G.style.alpha, 0.0f, 1.0f);
    uint32_t r = (uint32_t)(clampf(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(clampf(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(clampf(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | (b << 16) | (g << 8) | r;
}

static uint32_t col32(int idx) { return col32c(G.style.colors[idx]); }

static uint32_t hash_str(const char *s, const char *end, uint32_t seed)
{
    uint32_t h = seed ^ 2166136261u;
    while (end ? s < end : *s != 0) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static uint32_t hash_int(int n, uint32_t seed)
{
    char b[16];
    snprintf(b, sizeof b, "#%d", n);
    return hash_str(b, NULL, seed);
}

static uint32_t id_seed(void) { return G.nids ? G.ids[G.nids - 1] : 0; }
static uint32_t get_id(const char *label) { return hash_str(label, NULL, id_seed()); }

void gui_push_id(const char *str)
{
    if (G.nids < GUI_MAX_IDS) { uint32_t h = hash_str(str, NULL, id_seed()); G.ids[G.nids++] = h; }
}
void gui_push_id_int(int n)
{
    if (G.nids < GUI_MAX_IDS) { uint32_t h = hash_int(n, id_seed()); G.ids[G.nids++] = h; }
}
void gui_pop_id(void)
{
    if (G.nids > (G.cur ? G.cur->id_depth + 1 : 0)) G.nids--;
}

static int *storage(uint32_t key, int def)
{
    uint32_t i = key % GUI_STORAGE, n;
    for (n = 0; n < GUI_STORAGE; n++, i = (i + 1) % GUI_STORAGE) {
        if (G.storage[i].key == key)
            return &G.storage[i].val;
        if (G.storage[i].key == 0) {
            G.storage[i].key = key;
            G.storage[i].val = def;
            return &G.storage[i].val;
        }
    }
    /* ponytail: full table -- no eviction; 1024 headers and tab bars is plenty */
    {
        static int spare;
        spare = def;
        return &spare;
    }
}

/* "Label##id" shows "Label". */
static const char *label_end(const char *s)
{
    const char *p = strstr(s, "##");
    return p ? p : s + strlen(s);
}

static const char *vfmt(const char *fmt, va_list ap)
{
    vsnprintf(G.tbuf, sizeof G.tbuf, fmt, ap);
    return G.tbuf;
}

/* ── Paths and shapes ─────────────────────────────────────────── */

static void path_clear(void) { s_pn = 0; }

static void path_line_to(float x, float y)
{
    if (s_pn < GUI_PATH_MAX) { s_path[s_pn].x = x; s_path[s_pn].y = y; s_pn++; }
}

/* Segments for an arc of radius r, at most 0.25 px from the true circle. */
static int arc_segments(float r, float span)
{
    float e = fminf_(0.25f, r);
    int full = (int)ceilf(GUI_PI / acosf(1.0f - e / r));
    int n;
    if (full < 8) full = 8;
    if (full > 96) full = 96;
    n = (int)ceilf((float)full * fabsf(span) / (2.0f * GUI_PI));
    return n < 1 ? 1 : n;
}

/* Angles with y pointing down: 0 right, PI/2 down, PI left, 3PI/2 up. */
static void path_arc_to(float cx, float cy, float r, float a0, float a1)
{
    int i, n;
    if (r < 0.5f) { path_line_to(cx, cy); return; }
    n = arc_segments(r, a1 - a0);
    for (i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        path_line_to(cx + cosf(a) * r, cy + sinf(a) * r);
    }
}

static void path_rect(float x0, float y0, float x1, float y1, float r, int corners)
{
    float tl, tr, br, bl;
    r = fminf_(r, fminf_(fabsf(x1 - x0), fabsf(y1 - y0)) * 0.5f);
    if (r < 0.5f) corners = 0;
    tl = (corners & CORNER_TL) ? r : 0.0f;
    tr = (corners & CORNER_TR) ? r : 0.0f;
    br = (corners & CORNER_BR) ? r : 0.0f;
    bl = (corners & CORNER_BL) ? r : 0.0f;
    path_arc_to(x0 + tl, y0 + tl, tl, GUI_PI, 1.5f * GUI_PI);
    path_arc_to(x1 - tr, y0 + tr, tr, 1.5f * GUI_PI, 2.0f * GUI_PI);
    path_arc_to(x1 - br, y1 - br, br, 0.0f, 0.5f * GUI_PI);
    path_arc_to(x0 + bl, y1 - bl, bl, 0.5f * GUI_PI, GUI_PI);
}

/* A convex polygon with a one-pixel fringe fading out (Dear ImGui's method). */
static void dl_fill_convex(DrawList *d, const GuiVec2 *p, int n, uint32_t col)
{
    const float aa = 1.0f;
    uint32_t trans = col & 0x00FFFFFFu, base;
    float area = 0.0f, sgn;
    int i, i0, i1;
    if (n < 3 || !(col >> 24))
        return;
    for (i = 0; i < n; i++) {
        const GuiVec2 *a = &p[i], *b = &p[(i + 1) % n];
        area += a->x * b->y - b->x * a->y;
    }
    sgn = area < 0.0f ? -1.0f : 1.0f;
    for (i0 = n - 1, i1 = 0; i1 < n; i0 = i1++) {
        float dx = p[i1].x - p[i0].x, dy = p[i1].y - p[i0].y, l = dx * dx + dy * dy;
        if (l > 0.0f) { l = 1.0f / sqrtf(l); dx *= l; dy *= l; }
        s_nrm[i0].x = dy * sgn;
        s_nrm[i0].y = -dx * sgn;
    }
    base = dl_reserve(d, n * 2, (n - 2) * 3 + n * 6);
    for (i0 = n - 1, i1 = 0; i1 < n; i0 = i1++) {
        float mx = (s_nrm[i0].x + s_nrm[i1].x) * 0.5f, my = (s_nrm[i0].y + s_nrm[i1].y) * 0.5f;
        float l2 = mx * mx + my * my;
        if (l2 > 0.000001f) { float inv = 1.0f / l2; if (inv > 100.0f) inv = 100.0f; mx *= inv; my *= inv; }
        mx *= aa * 0.5f; my *= aa * 0.5f;
        dl_v(d, p[i1].x - mx, p[i1].y - my, G.white_u, G.white_v, col);
        dl_v(d, p[i1].x + mx, p[i1].y + my, G.white_u, G.white_v, trans);
    }
    for (i = 2; i < n; i++)
        dl_i3(d, base, base + (uint32_t)(i - 1) * 2, base + (uint32_t)i * 2);
    for (i0 = n - 1, i1 = 0; i1 < n; i0 = i1++) {
        uint32_t a = base + (uint32_t)i1 * 2, b = base + (uint32_t)i0 * 2;
        dl_i3(d, a, b, b + 1);
        dl_i3(d, b + 1, a + 1, a);
    }
}

/* Each segment a quad of the given thickness with a fringe on both sides.
 * ponytail: no joins -- fine for 1-3 px lines; miter joins if thick lines appear. */
static void dl_polyline(DrawList *d, const GuiVec2 *p, int n, uint32_t col, int closed, float thick)
{
    uint32_t trans = col & 0x00FFFFFFu;
    float h = fmaxf_(thick, 1.0f) * 0.5f - 0.5f, f = h + 1.0f;
    int s, segs = closed ? n : n - 1;
    if (n < 2 || !(col >> 24))
        return;
    for (s = 0; s < segs; s++) {
        const GuiVec2 *a = &p[s], *b = &p[(s + 1) % n];
        float dx = b->x - a->x, dy = b->y - a->y, l = dx * dx + dy * dy, nx, ny;
        uint32_t v;
        if (l <= 0.0f) continue;
        l = 1.0f / sqrtf(l); dx *= l; dy *= l;
        nx = -dy; ny = dx;
        v = dl_reserve(d, 8, 18);
        dl_v(d, a->x + nx * f, a->y + ny * f, G.white_u, G.white_v, trans);
        dl_v(d, a->x + nx * h, a->y + ny * h, G.white_u, G.white_v, col);
        dl_v(d, a->x - nx * h, a->y - ny * h, G.white_u, G.white_v, col);
        dl_v(d, a->x - nx * f, a->y - ny * f, G.white_u, G.white_v, trans);
        dl_v(d, b->x + nx * f, b->y + ny * f, G.white_u, G.white_v, trans);
        dl_v(d, b->x + nx * h, b->y + ny * h, G.white_u, G.white_v, col);
        dl_v(d, b->x - nx * h, b->y - ny * h, G.white_u, G.white_v, col);
        dl_v(d, b->x - nx * f, b->y - ny * f, G.white_u, G.white_v, trans);
        dl_i3(d, v + 0, v + 1, v + 5); dl_i3(d, v + 0, v + 5, v + 4);
        dl_i3(d, v + 1, v + 2, v + 6); dl_i3(d, v + 1, v + 6, v + 5);
        dl_i3(d, v + 2, v + 3, v + 7); dl_i3(d, v + 2, v + 7, v + 6);
    }
}

/* An axis-aligned quad, no fringe: crisp when on whole pixels. */
static void dl_quad_uv(DrawList *d, float x0, float y0, float x1, float y1,
                       float u0, float v0, float u1, float v1, uint32_t col)
{
    uint32_t b = dl_reserve(d, 4, 6);
    dl_v(d, x0, y0, u0, v0, col);
    dl_v(d, x1, y0, u1, v0, col);
    dl_v(d, x1, y1, u1, v1, col);
    dl_v(d, x0, y1, u0, v1, col);
    dl_i3(d, b, b + 1, b + 2);
    dl_i3(d, b, b + 2, b + 3);
}

static void dl_rect_filled(DrawList *d, float x0, float y0, float x1, float y1, uint32_t col, float r, int corners)
{
    if (!(col >> 24) || x1 <= x0 || y1 <= y0)
        return;
    if (r < 0.5f || !corners) {
        dl_quad_uv(d, x0, y0, x1, y1, G.white_u, G.white_v, G.white_u, G.white_v, col);
        return;
    }
    path_clear();
    path_rect(x0, y0, x1, y1, r, corners);
    dl_fill_convex(d, s_path, s_pn, col);
}

static void dl_rect(DrawList *d, float x0, float y0, float x1, float y1, uint32_t col, float r, float thick)
{
    path_clear();
    path_rect(x0 + 0.5f, y0 + 0.5f, x1 - 0.5f, y1 - 0.5f, r, CORNERS_ALL);
    dl_polyline(d, s_path, s_pn, col, 1, thick);
}

static void dl_line(DrawList *d, float x0, float y0, float x1, float y1, uint32_t col, float thick)
{
    GuiVec2 p[2];
    p[0].x = x0; p[0].y = y0; p[1].x = x1; p[1].y = y1;
    dl_polyline(d, p, 2, col, 0, thick);
}

static void dl_triangle_filled(DrawList *d, float ax, float ay, float bx, float by, float cx, float cy, uint32_t col)
{
    GuiVec2 p[3];
    p[0].x = ax; p[0].y = ay; p[1].x = bx; p[1].y = by; p[2].x = cx; p[2].y = cy;
    dl_fill_convex(d, p, 3, col);
}

static void dl_circle_filled(DrawList *d, float cx, float cy, float r, uint32_t col)
{
    path_clear();
    path_arc_to(cx, cy, r, 0.0f, 2.0f * GUI_PI);
    if (s_pn > 1) s_pn--;               /* the last point repeats the first */
    dl_fill_convex(d, s_path, s_pn, col);
}

/* ── Font ─────────────────────────────────────────────────────── */

static unsigned utf8_next(const char **ps, const char *end)
{
    const unsigned char *s = (const unsigned char *)*ps, *e = (const unsigned char *)end;
    unsigned c = *s++;
    if (c >= 0x80) {
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i;
        if (!more) {
            c = '?';
        } else {
            c &= 0x3Fu >> more;
            for (i = 0; i < more && s < e && (*s & 0xC0) == 0x80; i++)
                c = (c << 6) | (*s++ & 0x3Fu);
            if (i < more) c = '?';
        }
    }
    *ps = (const char *)s;
    return c;
}

static int glyph_of(unsigned c)
{
    return c >= GUI_GLYPH_FIRST && c < GUI_GLYPH_FIRST + GUI_GLYPH_COUNT
         ? (int)(c - GUI_GLYPH_FIRST) : '?' - GUI_GLYPH_FIRST;
}

static GuiVec2 text_size(const char *s, const char *end)
{
    GuiVec2 r;
    float line = 0.0f;
    r.x = 0.0f; r.y = G.font_size;
    if (!end) end = s + strlen(s);
    while (s < end) {
        unsigned c = utf8_next(&s, end);
        if (c == '\n') { r.x = fmaxf_(r.x, line); line = 0.0f; r.y += G.font_size; continue; }
        line += G.chars[glyph_of(c)].xadvance;
    }
    r.x = fmaxf_(r.x, line);
    return r;
}

static void dl_text(DrawList *d, float x, float y, uint32_t col, const char *s, const char *end)
{
    float x0 = floorf(x + 0.5f), px = x0, base = floorf(y + 0.5f) + G.ascent;
    if (!end) end = s + strlen(s);
    if (!(col >> 24))
        return;
    while (s < end) {
        unsigned c = utf8_next(&s, end);
        stbtt_aligned_quad q;
        float qx = px, qy = base;
        if (c == '\n') { px = x0; base += G.font_size; continue; }
        stbtt_GetPackedQuad(G.chars, G.atlas_w, G.atlas_h, glyph_of(c), &qx, &qy, &q, 0);
        if (c != ' ' && q.y1 >= d->clip[1] && q.y0 <= d->clip[3] && q.x1 >= d->clip[0] && q.x0 <= d->clip[2])
            dl_quad_uv(d, q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1, col);
        px = qx;
    }
}

void gui_font_atlas(const uint8_t **alpha8, int *w, int *h)
{
    *alpha8 = G.atlas; *w = G.atlas_w; *h = G.atlas_h;
}

/* ── Style ────────────────────────────────────────────────────── */

static GuiColor rgba(float r, float g, float b, float a) { GuiColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c; }
static GuiVec2 v2(float x, float y) { GuiVec2 v; v.x = x; v.y = y; return v; }

/* Moonlight, by Madam-Herta / deathsu: https://github.com/Madam-Herta/Moonlight */
void gui_style_moonlight(GuiStyle *s)
{
    GuiColor *c = s->colors;
    memset(s, 0, sizeof *s);
    s->alpha = 1.0f;
    s->window_padding = v2(12.0f, 12.0f);
    s->window_rounding = 11.5f;
    s->window_border_size = 0.0f;
    s->window_min_size = v2(20.0f, 20.0f);
    s->window_title_align = v2(0.5f, 0.5f);
    s->popup_rounding = 0.0f;
    s->frame_padding = v2(20.0f, 3.4f);
    s->frame_rounding = 11.9f;
    s->frame_border_size = 0.0f;
    s->item_spacing = v2(4.3f, 5.5f);
    s->item_inner_spacing = v2(7.1f, 1.8f);
    s->indent_spacing = 0.0f;
    s->scrollbar_size = 11.6f;
    s->scrollbar_rounding = 15.9f;
    s->grab_min_size = 3.7f;
    s->grab_rounding = 20.0f;
    s->tab_rounding = 0.0f;
    s->button_text_align = v2(0.5f, 0.5f);

    c[GUI_COL_TEXT]                   = rgba(1.0f, 1.0f, 1.0f, 1.0f);
    c[GUI_COL_TEXT_DISABLED]          = rgba(0.2745f, 0.3176f, 0.4510f, 1.0f);
    c[GUI_COL_WINDOW_BG]              = rgba(0.0784f, 0.0863f, 0.1020f, 1.0f);
    c[GUI_COL_POPUP_BG]               = rgba(0.0784f, 0.0863f, 0.1020f, 1.0f);
    c[GUI_COL_BORDER]                 = rgba(0.1569f, 0.1686f, 0.1922f, 1.0f);
    c[GUI_COL_FRAME_BG]               = rgba(0.1121f, 0.1262f, 0.1545f, 1.0f);
    c[GUI_COL_FRAME_BG_HOVERED]       = rgba(0.1569f, 0.1686f, 0.1922f, 1.0f);
    c[GUI_COL_FRAME_BG_ACTIVE]        = rgba(0.1569f, 0.1686f, 0.1922f, 1.0f);
    c[GUI_COL_TITLE_BG]               = rgba(0.0471f, 0.0549f, 0.0706f, 1.0f);
    c[GUI_COL_TITLE_BG_ACTIVE]        = rgba(0.0471f, 0.0549f, 0.0706f, 1.0f);
    c[GUI_COL_SCROLLBAR_BG]           = rgba(0.0471f, 0.0549f, 0.0706f, 1.0f);
    c[GUI_COL_SCROLLBAR_GRAB]         = rgba(0.1176f, 0.1333f, 0.1490f, 1.0f);
    c[GUI_COL_SCROLLBAR_GRAB_HOVERED] = rgba(0.1569f, 0.1686f, 0.1922f, 1.0f);
    c[GUI_COL_SCROLLBAR_GRAB_ACTIVE]  = rgba(0.1176f, 0.1333f, 0.1490f, 1.0f);
    c[GUI_COL_CHECK_MARK]             = rgba(0.9725f, 1.0f, 0.4980f, 1.0f);
    c[GUI_COL_SLIDER_GRAB]            = rgba(0.9720f, 1.0f, 0.4980f, 1.0f);
    c[GUI_COL_SLIDER_GRAB_ACTIVE]     = rgba(1.0f, 0.7953f, 0.4980f, 1.0f);
    c[GUI_COL_BUTTON]                 = rgba(0.1176f, 0.1333f, 0.1490f, 1.0f);
    c[GUI_COL_BUTTON_HOVERED]         = rgba(0.1822f, 0.1898f, 0.1974f, 1.0f);
    c[GUI_COL_BUTTON_ACTIVE]          = rgba(0.1545f, 0.1545f, 0.1545f, 1.0f);
    c[GUI_COL_HEADER]                 = rgba(0.1415f, 0.1630f, 0.2060f, 1.0f);
    c[GUI_COL_HEADER_HOVERED]         = rgba(0.1073f, 0.1073f, 0.1073f, 1.0f);
    c[GUI_COL_HEADER_ACTIVE]          = rgba(0.0784f, 0.0863f, 0.1020f, 1.0f);
    c[GUI_COL_SEPARATOR]              = rgba(0.1293f, 0.1479f, 0.1931f, 1.0f);
    c[GUI_COL_RESIZE_GRIP]            = rgba(0.1459f, 0.1459f, 0.1459f, 1.0f);
    c[GUI_COL_RESIZE_GRIP_HOVERED]    = rgba(0.9725f, 1.0f, 0.4980f, 1.0f);
    c[GUI_COL_RESIZE_GRIP_ACTIVE]     = rgba(1.0f, 1.0f, 1.0f, 1.0f);
    c[GUI_COL_TAB]                    = rgba(0.0784f, 0.0863f, 0.1020f, 1.0f);
    c[GUI_COL_TAB_HOVERED]            = rgba(0.1176f, 0.1333f, 0.1490f, 1.0f);
    c[GUI_COL_TAB_ACTIVE]             = rgba(0.1176f, 0.1333f, 0.1490f, 1.0f);
    c[GUI_COL_PLOT_LINES]             = rgba(0.5216f, 0.6000f, 0.7020f, 1.0f);
    c[GUI_COL_PLOT_LINES_HOVERED]     = rgba(0.0392f, 0.9804f, 0.9804f, 1.0f);
    c[GUI_COL_PLOT_HISTOGRAM]         = rgba(0.8841f, 0.7941f, 0.5616f, 1.0f);
    c[GUI_COL_PLOT_HISTOGRAM_HOVERED] = rgba(0.9571f, 0.9571f, 0.9571f, 1.0f);
    c[GUI_COL_NAV_HIGHLIGHT]          = rgba(0.2661f, 0.2890f, 1.0f, 1.0f);
}

void gui_style_scale(GuiStyle *s, float k)
{
    GuiVec2 *v[] = { &s->window_padding, &s->window_min_size, &s->frame_padding,
                     &s->item_spacing, &s->item_inner_spacing };
    float *f[] = { &s->window_rounding, &s->popup_rounding, &s->frame_rounding, &s->indent_spacing,
                   &s->scrollbar_size, &s->scrollbar_rounding, &s->grab_min_size, &s->grab_rounding,
                   &s->tab_rounding };
    size_t i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) { v[i]->x = floorf(v[i]->x * k); v[i]->y = floorf(v[i]->y * k); }
    for (i = 0; i < sizeof f / sizeof f[0]; i++) *f[i] = floorf(*f[i] * k);
}

/* ── Setup ────────────────────────────────────────────────────── */

int gui_init(const void *ttf, size_t ttf_size, float font_px)
{
    stbtt_fontinfo info;
    int side, asc, desc, gap, offset;

    gui_shutdown();
    if (!ttf || ttf_size < 12 || font_px < 4.0f)
        return 0;
    G.ttf = (unsigned char *)malloc(ttf_size);
    if (!G.ttf)
        return 0;
    memcpy(G.ttf, ttf, ttf_size);
    offset = stbtt_GetFontOffsetForIndex(G.ttf, 0);
    if (offset < 0 || !stbtt_InitFont(&info, G.ttf, offset)) {
        gui_shutdown();
        return 0;
    }
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    G.font_size = floorf(font_px + 0.5f);
    G.ascent = floorf((float)asc * stbtt_ScaleForPixelHeight(&info, G.font_size) + 0.5f);

    /* Pack into the smallest square that holds every glyph, plus two rows
     * under it for the solid block. */
    for (side = 256; side <= 4096; side *= 2) {
        stbtt_pack_context pc;
        int ok;
        G.atlas = (uint8_t *)calloc((size_t)side * (size_t)(side + 2), 1);
        if (!G.atlas || !stbtt_PackBegin(&pc, G.atlas, side, side, side, 1, NULL)) {
            gui_shutdown();
            return 0;
        }
        stbtt_PackSetOversampling(&pc, 2, 1);
        ok = stbtt_PackFontRange(&pc, G.ttf, 0, G.font_size, GUI_GLYPH_FIRST, GUI_GLYPH_COUNT, G.chars);
        stbtt_PackEnd(&pc);
        if (ok) break;
        free(G.atlas);
        G.atlas = NULL;
    }
    if (!G.atlas) {
        gui_shutdown();
        return 0;
    }
    G.atlas_w = side;
    G.atlas_h = side + 2;
    G.atlas[(size_t)side * side] = G.atlas[(size_t)side * side + 1] = 255;
    G.atlas[(size_t)side * (side + 1)] = G.atlas[(size_t)side * (side + 1) + 1] = 255;
    G.white_u = 1.0f / (float)G.atlas_w;
    G.white_v = ((float)side + 1.0f) / (float)G.atlas_h;

    gui_style_moonlight(&G.style);
    gui_style_scale(&G.style, G.font_size / 16.0f);
    G.io.mouse_x = G.io.mouse_y = -1.0f;
    {
        int i;
        for (i = 0; i < GUI_NAV_COUNT; i++) G.nav_held[i] = -1.0f;
    }
    G.ready = 1;
    return 1;
}

void gui_shutdown(void)
{
    int i;
    for (i = 0; i < G.nwin; i++)
        dl_free(&G.windows[i].dl);
    dl_free(&G.out);
    free(G.ttf);
    free(G.atlas);
    memset(&G, 0, sizeof G);
}

/* ── Frame ────────────────────────────────────────────────────── */

static int in_rect(float x, float y, float x0, float y0, float x1, float y1)
{
    return x >= x0 && y >= y0 && x < x1 && y < y1;
}

static GuiWindow *popup_window(void)
{
    int i;
    for (i = 0; i < G.nwin; i++)
        if (G.windows[i].flags & GUI_WINDOW_POPUP_) return &G.windows[i];
    return NULL;
}

static void focus_window(GuiWindow *w)
{
    int i, j;
    for (i = 0; i < G.nwin && G.order[i] != w; i++) ;
    for (j = i; j + 1 < G.nwin; j++) G.order[j] = G.order[j + 1];
    G.order[G.nwin - 1] = w;
    if (!(w->flags & (GUI_WINDOW_NO_NAV | GUI_WINDOW_NO_INPUTS)) && G.nav_win != w) {
        G.nav_win = w;
        G.nav_id = w->nav_last_id;
    }
}

static void close_popup(void)
{
    if (!G.popup_id) return;
    if (G.popup_owner && G.popup_owner->active) {
        G.nav_win = G.popup_owner;
        G.nav_id = G.popup_return_id;
    }
    G.popup_id = 0;
    G.popup_owner = NULL;
}

void gui_new_frame(void)
{
    GuiIO *io = &G.io;
    float dt = io->delta_time > 0.0f ? io->delta_time : 1.0f / 60.0f;
    int b, k;

    if (!G.ready)
        return;
    G.frame++;
    for (b = 0; b < 2; b++) {
        G.mclicked[b] = io->mouse_down[b] && !G.mprev[b];
        G.mreleased[b] = !io->mouse_down[b] && G.mprev[b];
        G.mprev[b] = io->mouse_down[b];
    }
    if (G.mclicked[0] || G.mclicked[1] || fabsf(io->mouse_x - G.mprev_x) + fabsf(io->mouse_y - G.mprev_y) > 2.0f)
        G.nav_visible = 0;
    G.mprev_x = io->mouse_x; G.mprev_y = io->mouse_y;

    /* Presses from held inputs; directions and tab steps repeat. */
    for (k = 0; k < GUI_NAV_COUNT; k++) {
        int repeat = k != GUI_NAV_ACTIVATE && k != GUI_NAV_CANCEL;
        G.nav_pressed[k] = 0;
        if (!io->nav[k]) { G.nav_held[k] = -1.0f; continue; }
        if (G.nav_held[k] < 0.0f) {
            G.nav_pressed[k] = 1;
            G.nav_held[k] = 0.0f;
        } else {
            const float delay = 0.30f, rate = 0.07f;
            float prev = G.nav_held[k];
            G.nav_held[k] += dt;
            if (repeat && G.nav_held[k] >= delay
                    && (prev < delay || floorf((G.nav_held[k] - delay) / rate) > floorf((prev - delay) / rate)))
                G.nav_pressed[k] = 1;
        }
        if (G.nav_pressed[k]) G.nav_visible = 1;
    }

    /* The window under the mouse, by last frame's rectangles. */
    G.hovered = NULL;
    if (io->mouse_x >= 0.0f && io->mouse_y >= 0.0f) {
        GuiWindow *p = G.popup_id ? popup_window() : NULL;
        int i;
        if (p && p->was_active && in_rect(io->mouse_x, io->mouse_y, p->x, p->y, p->x + p->w, p->y + p->h))
            G.hovered = p;
        for (i = G.nwin - 1; i >= 0 && !G.hovered; i--) {
            GuiWindow *w = G.order[i];
            if (w->was_active && !w->hidden && !(w->flags & (GUI_WINDOW_NO_INPUTS | GUI_WINDOW_POPUP_))
                    && in_rect(io->mouse_x, io->mouse_y, w->x, w->y, w->x + w->w, w->y + w->h))
                G.hovered = w;
        }
    }

    G.popup_closed_by_click = 0;
    if (G.mclicked[0] || G.mclicked[1]) {
        if (G.popup_id && (!G.hovered || !(G.hovered->flags & GUI_WINDOW_POPUP_))) {
            G.popup_closed_by_click = G.popup_id;
            close_popup();
        }
        if (G.hovered && !(G.hovered->flags & GUI_WINDOW_POPUP_))
            focus_window(G.hovered);
    }
    if (G.popup_id && G.nav_pressed[GUI_NAV_CANCEL]) {
        close_popup();
        G.nav_pressed[GUI_NAV_CANCEL] = 0;
    }
    G.nav_move = G.nav_pressed[GUI_NAV_DOWN] - G.nav_pressed[GUI_NAV_UP];

    {
        int i;
        for (i = 0; i < G.nwin; i++) {
            G.windows[i].was_active = G.windows[i].active;
            G.windows[i].active = 0;
        }
    }
    G.nav_count = 0;
    G.popup_seen = 0;
    G.nstack = 0;
    G.cur = NULL;
    G.nids = 0;
}

int gui_nav_pressed(int nav)
{
    return nav >= 0 && nav < GUI_NAV_COUNT && G.nav_pressed[nav];
}

/* ── Windows ──────────────────────────────────────────────────── */

void gui_set_next_window_pos(float x, float y, int cond, float pivot_x, float pivot_y)
{
    G.next.pos_set = 1; G.next.pos_cond = cond;
    G.next.x = x; G.next.y = y; G.next.px = pivot_x; G.next.py = pivot_y;
}
void gui_set_next_window_size(float w, float h, int cond)
{
    G.next.size_set = 1; G.next.size_cond = cond; G.next.w = w; G.next.h = h;
}
void gui_set_next_window_bg_alpha(float a) { G.next.alpha_set = 1; G.next.alpha = a; }
void gui_set_next_window_focus(void) { G.next.focus = 1; }

static GuiWindow *find_window(uint32_t id, const char *name)
{
    GuiWindow *w;
    int i;
    for (i = 0; i < G.nwin; i++)
        if (G.windows[i].id == id) return &G.windows[i];
    if (G.nwin >= GUI_MAX_WINDOWS)
        return NULL;
    w = &G.windows[G.nwin];
    memset(w, 0, sizeof *w);
    w->id = id;
    snprintf(w->name, sizeof w->name, "%s", name);
    w->created_frame = G.frame;
    w->x = w->y = 60.0f + 24.0f * (float)G.nwin;
    G.order[G.nwin++] = w;
    return w;
}

static void window_close_button(GuiWindow *w, int *p_open);

int gui_begin(const char *name, int *p_open, int flags)
{
    GuiStyle *st = &G.style;
    uint32_t id = hash_str(name, NULL, 0);
    GuiWindow *w = G.ready ? find_window(id, name) : NULL;
    int first = w && w->created_frame == G.frame;
    float pad_x = st->window_padding.x, pad_y = st->window_padding.y;
    uint32_t bg;

    if (!w) {
        /* No font, or too many windows: draw into a throwaway window. */
        static GuiWindow dummy;
        DrawList keep = dummy.dl;
        w = &dummy;
        memset(w, 0, sizeof *w);
        w->dl = keep;
        w->hidden = 1;
    }
    if (G.nstack < (int)(sizeof G.stack / sizeof G.stack[0]))
        G.stack[G.nstack++] = w;
    G.cur = w;
    w->flags = flags;
    w->active = 1;
    w->id_depth = G.nids;
    if (G.nids < GUI_MAX_IDS) G.ids[G.nids++] = id;

    if (G.next.size_set && (G.next.size_cond == GUI_COND_ALWAYS || first)) {
        w->w = G.next.w; w->h = G.next.h;
    } else if (first) {
        w->autofit = 1;
    }
    if (G.next.pos_set && (G.next.pos_cond == GUI_COND_ALWAYS || first)) {
        w->pos_pending = 1;
        w->pend_x = G.next.x; w->pend_y = G.next.y; w->pend_px = G.next.px; w->pend_py = G.next.py;
    }
    if (G.next.focus || first)
        focus_window(w);
    w->min_w = G.next.min_w;
    bg = G.next.alpha_set ? col32c(rgba(st->colors[GUI_COL_WINDOW_BG].r, st->colors[GUI_COL_WINDOW_BG].g,
                                        st->colors[GUI_COL_WINDOW_BG].b, G.next.alpha))
                          : col32((flags & GUI_WINDOW_POPUP_) ? GUI_COL_POPUP_BG : GUI_COL_WINDOW_BG);
    memset(&G.next, 0, sizeof G.next);

    if ((flags & GUI_WINDOW_POPUP_) && G.popup_nav_init)
        G.nav_win = w;

    w->title_h = (flags & GUI_WINDOW_NO_TITLE_BAR) ? 0.0f : floorf(G.font_size + st->frame_padding.y * 2.0f);

    /* Size: the content's, for auto-sized windows, once measured. */
    if (w->measured && ((flags & GUI_WINDOW_AUTO_RESIZE) || w->autofit)) {
        w->w = w->content_w + pad_x * 2.0f;
        w->h = w->content_h + pad_y * 2.0f + w->title_h;
        if (w->autofit && !(flags & GUI_WINDOW_AUTO_RESIZE)) w->autofit = 0;
    }
    if (!(flags & GUI_WINDOW_NO_TITLE_BAR)) {
        GuiVec2 t = text_size(name, label_end(name));
        w->w = fmaxf_(w->w, t.x + pad_x * 2.0f + (p_open ? w->title_h : 0.0f));
    }
    w->w = floorf(fmaxf_(fmaxf_(w->w, st->window_min_size.x), w->min_w));
    w->h = floorf(fmaxf_(w->h, st->window_min_size.y));
    if (G.io.display_h > 0.0f && w->h > G.io.display_h - 8.0f)
        w->h = fmaxf_(st->window_min_size.y, G.io.display_h - 8.0f);
    w->hidden = !w->measured && ((flags & GUI_WINDOW_AUTO_RESIZE) || w->autofit);

    if (w->pos_pending) {
        w->x = w->pend_x - w->pend_px * w->w;
        w->y = w->pend_y - w->pend_py * w->h;
        if (w->measured || !w->hidden) w->pos_pending = 0;
    }
    /* Keep enough of it on the screen to grab. */
    if (G.io.display_w > 0.0f) {
        w->x = clampf(w->x, 40.0f - w->w, fmaxf_(0.0f, G.io.display_w - 40.0f));
        w->y = clampf(w->y, 0.0f, fmaxf_(0.0f, G.io.display_h - fmaxf_(w->title_h, 20.0f)));
    }
    w->x = floorf(w->x); w->y = floorf(w->y);

    /* Content region and scrolling, from last frame's content size. */
    w->has_scrollbar = !(flags & GUI_WINDOW_NO_SCROLLBAR) && w->measured
                     && w->content_h > w->h - w->title_h - pad_y * 2.0f + 0.5f;
    w->inner_x0 = w->x + pad_x;
    w->inner_y0 = w->y + w->title_h + pad_y;
    w->inner_x1 = w->x + w->w - pad_x - (w->has_scrollbar ? st->scrollbar_size : 0.0f);
    w->inner_y1 = w->y + w->h - pad_y;
    w->scroll_max = fmaxf_(0.0f, w->content_h - (w->inner_y1 - w->inner_y0));
    w->scroll_y = floorf(clampf(w->scroll_y, 0.0f, w->scroll_max));
    w->clip[0] = w->x + floorf(pad_x * 0.5f);
    w->clip[1] = w->title_h > 0.0f ? w->y + w->title_h : w->y + floorf(pad_y * 0.5f);
    w->clip[2] = w->inner_x1 + floorf(pad_x * 0.5f);
    w->clip[3] = w->y + w->h - floorf(pad_y * 0.5f);

    w->indent = 0.0f;
    w->line_x0 = w->inner_x0;
    w->cx = w->inner_x0;
    w->cy = w->inner_y0 - w->scroll_y;
    w->content_x0 = w->cx;
    w->content_y0 = w->cy;
    w->max_x = w->cx;
    w->max_y = w->cy;
    w->line_h = w->line_text_off = 0.0f;
    w->prev_x = w->cx; w->prev_y = w->cy; w->prev_line_h = w->prev_text_off = 0.0f;
    w->nwidths = 0;
    w->last_id = 0;
    w->last_hovered = 0;

    /* Background and title bar. */
    dl_reset(&w->dl);
    dl_set_clip(&w->dl, 0.0f, 0.0f, G.io.display_w, G.io.display_h);
    if (!(flags & GUI_WINDOW_NO_BACKGROUND)) {
        float r = (flags & GUI_WINDOW_POPUP_) ? st->popup_rounding : st->window_rounding;
        dl_rect_filled(&w->dl, w->x, w->y, w->x + w->w, w->y + w->h, bg, r, CORNERS_ALL);
        if (st->window_border_size > 0.0f)
            dl_rect(&w->dl, w->x, w->y, w->x + w->w, w->y + w->h, col32(GUI_COL_BORDER), r, st->window_border_size);
    }
    if (w->title_h > 0.0f) {
        const char *te = label_end(name);
        GuiVec2 t = text_size(name, te);
        float avail = w->w - pad_x * 2.0f - (p_open ? w->title_h : 0.0f);
        float tx = w->x + pad_x + fmaxf_(0.0f, (avail - t.x) * st->window_title_align.x);
        dl_rect_filled(&w->dl, w->x, w->y, w->x + w->w, w->y + w->title_h,
                       col32(G.nav_win == w ? GUI_COL_TITLE_BG_ACTIVE : GUI_COL_TITLE_BG),
                       st->window_rounding, CORNERS_TOP);
        dl_text(&w->dl, tx, w->y + (w->title_h - G.font_size) * 0.5f, col32(GUI_COL_TEXT), name, te);
        if (p_open)
            window_close_button(w, p_open);
    }
    dl_set_clip(&w->dl, w->clip[0], w->clip[1], w->clip[2], w->clip[3]);
    return 1;
}

static int item_hoverable(float x0, float y0, float x1, float y1, uint32_t id);

static void window_close_button(GuiWindow *w, int *p_open)
{
    uint32_t id = hash_str("#CLOSE", NULL, w->id);
    float s = G.font_size, cx = w->x + w->w - w->title_h * 0.5f - G.style.window_padding.x * 0.25f;
    float cy = w->y + w->title_h * 0.5f, e = s * 0.5f * 0.7071f - 1.0f;
    int hovered = item_hoverable(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f, id);
    uint32_t col = col32(GUI_COL_TEXT);
    if (hovered && G.mclicked[0]) G.active_id = id;
    if (G.active_id == id) {
        G.active_seen = 1;
        if (!G.io.mouse_down[0]) {
            if (hovered) *p_open = 0;
            G.active_id = 0;
        }
    }
    if (hovered)
        dl_circle_filled(&w->dl, cx, cy, s * 0.5f + 1.0f,
                         col32(G.active_id == id ? GUI_COL_BUTTON_ACTIVE : GUI_COL_BUTTON_HOVERED));
    dl_line(&w->dl, cx - e, cy - e, cx + e, cy + e, col, 1.0f);
    dl_line(&w->dl, cx + e, cy - e, cx - e, cy + e, col, 1.0f);
}

void gui_end(void)
{
    GuiStyle *st = &G.style;
    GuiWindow *w = G.cur;
    GuiIO *io = &G.io;
    if (!w)
        return;

    w->content_w = fmaxf_(0.0f, w->max_x - w->content_x0);
    w->content_h = fmaxf_(0.0f, w->max_y - w->content_y0);
    w->measured++;
    dl_set_clip(&w->dl, 0.0f, 0.0f, io->display_w, io->display_h);

    if (!w->hidden && !(w->flags & GUI_WINDOW_NO_INPUTS)) {
        /* Wheel. */
        if (G.hovered == w && io->mouse_wheel != 0.0f && w->scroll_max > 0.0f) {
            w->scroll_y = clampf(w->scroll_y - io->mouse_wheel * G.font_size * 3.0f, 0.0f, w->scroll_max);
            io->mouse_wheel = 0.0f;
        }
        /* Scrollbar. */
        if (w->has_scrollbar) {
            uint32_t id = hash_str("#SCROLLY", NULL, w->id);
            float x1 = w->x + w->w, x0 = x1 - st->scrollbar_size;
            float y0 = w->y + w->title_h, y1 = w->y + w->h;
            float track = y1 - y0 - st->window_rounding;
            float visible = w->inner_y1 - w->inner_y0;
            float gh = fmaxf_(track * visible / fmaxf_(w->content_h, 1.0f), fmaxf_(st->grab_min_size, G.font_size));
            float gy = y0 + st->window_rounding * 0.5f
                     + (track - gh) * (w->scroll_max > 0.0f ? w->scroll_y / w->scroll_max : 0.0f);
            int hovered = item_hoverable(x0, y0, x1, y1, id);
            if (hovered && G.mclicked[0]) {
                G.active_id = id;
                /* a click off the grab jumps there */
                G.active_off_y = (io->mouse_y >= gy && io->mouse_y < gy + gh) ? io->mouse_y - gy : gh * 0.5f;
            }
            if (G.active_id == id) {
                G.active_seen = 1;
                if (track > gh)
                    w->scroll_y = clampf((io->mouse_y - G.active_off_y - y0 - st->window_rounding * 0.5f)
                                         / (track - gh) * w->scroll_max, 0.0f, w->scroll_max);
            }
            dl_rect_filled(&w->dl, x0, y0, x1, y1, col32(GUI_COL_SCROLLBAR_BG), st->window_rounding,
                           w->title_h > 0.0f ? CORNER_BR : CORNERS_RIGHT);
            dl_rect_filled(&w->dl, x0 + 2.0f, gy, x1 - 2.0f, gy + gh,
                           col32(G.active_id == id ? GUI_COL_SCROLLBAR_GRAB_ACTIVE
                                 : hovered ? GUI_COL_SCROLLBAR_GRAB_HOVERED : GUI_COL_SCROLLBAR_GRAB),
                           st->scrollbar_rounding, CORNERS_ALL);
        }
        /* Resize grip, bottom right. */
        if (!(w->flags & (GUI_WINDOW_NO_RESIZE | GUI_WINDOW_AUTO_RESIZE | GUI_WINDOW_POPUP_))) {
            uint32_t id = hash_str("#RESIZE", NULL, w->id);
            float g = floorf(G.font_size * 1.1f), in = st->window_rounding * 0.29f;
            float cx = w->x + w->w - in, cy = w->y + w->h - in;
            int hovered = item_hoverable(cx - g, cy - g, cx + in, cy + in, id);
            if (hovered && G.mclicked[0]) {
                G.active_id = id;
                G.active_off_x = w->x + w->w - io->mouse_x;
                G.active_off_y = w->y + w->h - io->mouse_y;
            }
            if (G.active_id == id) {
                G.active_seen = 1;
                w->w = fmaxf_(st->window_min_size.x, io->mouse_x + G.active_off_x - w->x);
                w->h = fmaxf_(st->window_min_size.y, io->mouse_y + G.active_off_y - w->y);
                w->autofit = 0;
            }
            dl_triangle_filled(&w->dl, cx, cy - g, cx, cy, cx - g, cy,
                               col32(G.active_id == id ? GUI_COL_RESIZE_GRIP_ACTIVE
                                     : hovered ? GUI_COL_RESIZE_GRIP_HOVERED : GUI_COL_RESIZE_GRIP));
        }
        /* Moving: a press that no widget took. */
        if (!(w->flags & (GUI_WINDOW_NO_MOVE | GUI_WINDOW_POPUP_))) {
            uint32_t id = hash_str("#MOVE", NULL, w->id);
            if (G.mclicked[0] && G.hovered == w && G.active_id == 0) {
                G.active_id = id;
                G.active_off_x = io->mouse_x - w->x;
                G.active_off_y = io->mouse_y - w->y;
            }
            if (G.active_id == id) {
                G.active_seen = 1;
                w->x = io->mouse_x - G.active_off_x;
                w->y = io->mouse_y - G.active_off_y;
            }
        }
    }

    G.nids = w->id_depth;
    G.nstack--;
    G.cur = G.nstack > 0 ? G.stack[G.nstack - 1] : NULL;
}

/* ── Layout ───────────────────────────────────────────────────── */

/* Advance the cursor past an item w x h. text_off: how far below the line
 * top this item's text sits (frame padding for framed widgets). */
static void item_size(float w, float h, float text_off)
{
    GuiWindow *win = G.cur;
    float line_h = fmaxf_(win->line_h, h);
    win->prev_x = win->cx + w;
    win->prev_y = win->cy;
    win->prev_line_h = line_h;
    win->prev_text_off = fmaxf_(win->line_text_off, text_off);
    win->max_x = fmaxf_(win->max_x, win->cx + w);
    win->max_y = fmaxf_(win->max_y, win->cy + line_h);
    win->cx = win->line_x0;
    win->cy = floorf(win->cy + line_h + G.style.item_spacing.y);
    win->line_h = 0.0f;
    win->line_text_off = 0.0f;
}

void gui_same_line(float spacing)
{
    GuiWindow *w = G.cur;
    if (!w) return;
    w->cx = floorf(w->prev_x + (spacing < 0.0f ? G.style.item_spacing.x : spacing));
    w->cy = w->prev_y;
    w->line_h = w->prev_line_h;
    w->line_text_off = w->prev_text_off;
}

void gui_new_line(void) { if (G.cur) item_size(0.0f, G.font_size, 0.0f); }
void gui_spacing(void) { if (G.cur) item_size(0.0f, 0.0f, 0.0f); }

void gui_align_text_to_frame_padding(void)
{
    if (!G.cur) return;
    G.cur->line_h = fmaxf_(G.cur->line_h, floorf(G.font_size + G.style.frame_padding.y * 2.0f));
    G.cur->line_text_off = fmaxf_(G.cur->line_text_off, G.style.frame_padding.y);
}

static float indent_step(void) { return G.style.indent_spacing > 0.0f ? G.style.indent_spacing : G.font_size; }

void gui_indent(void)
{
    if (!G.cur) return;
    G.cur->indent += indent_step();
    G.cur->line_x0 = G.cur->inner_x0 + G.cur->indent;
    G.cur->cx = G.cur->line_x0;
}

void gui_unindent(void)
{
    if (!G.cur) return;
    G.cur->indent = fmaxf_(0.0f, G.cur->indent - indent_step());
    G.cur->line_x0 = G.cur->inner_x0 + G.cur->indent;
    G.cur->cx = G.cur->line_x0;
}

void gui_push_item_width(float w)
{
    if (G.cur && G.cur->nwidths < GUI_MAX_WIDTHS) G.cur->widths[G.cur->nwidths++] = w;
}
void gui_pop_item_width(void) { if (G.cur && G.cur->nwidths) G.cur->nwidths--; }

float gui_content_width(void) { return G.cur ? fmaxf_(0.0f, G.cur->inner_x1 - G.cur->cx) : 0.0f; }

static float item_width(void)
{
    GuiWindow *w = G.cur;
    float v = w->nwidths ? w->widths[w->nwidths - 1] : 0.0f;
    if (v > 0.0f) return floorf(v);
    if (v < 0.0f) return fmaxf_(1.0f, floorf(w->inner_x1 - w->cx + v));
    if (w->flags & GUI_WINDOW_AUTO_RESIZE) return floorf(G.font_size * 12.0f);
    return fmaxf_(1.0f, floorf(w->w * 0.65f - G.style.window_padding.x));
}

static float frame_h(void) { return floorf(G.font_size + G.style.frame_padding.y * 2.0f); }

/* ── Items: hover, press, navigation ──────────────────────────── */

static int item_hoverable(float x0, float y0, float x1, float y1, uint32_t id)
{
    GuiWindow *w = G.cur;
    float mx = G.io.mouse_x, my = G.io.mouse_y;
    if (!w || G.hovered != w || w->hidden)
        return 0;
    if (G.active_id && G.active_id != id)
        return 0;
    return in_rect(mx, my, x0, y0, x1, y1) && in_rect(mx, my, w->dl.clip[0], w->dl.clip[1], w->dl.clip[2], w->dl.clip[3]);
}

static int nav_here(uint32_t id) { return id && id == G.nav_id && G.cur == G.nav_win; }

/* Register an item: for navigation, and to scroll the selected one into view.
 * Every window's items are kept, tagged, because which window has navigation
 * can change in the middle of a frame (a combo's list opening or closing). */
static void item_add(float x0, float y0, float x1, float y1, uint32_t id, int navable)
{
    GuiWindow *w = G.cur;
    w->last[0] = x0; w->last[1] = y0; w->last[2] = x1; w->last[3] = y1;
    w->last_id = id;
    w->last_hovered = 0;
    if (!id || !navable || w->hidden || (w->flags & (GUI_WINDOW_NO_NAV | GUI_WINDOW_NO_INPUTS)))
        return;
    if (G.nav_count < GUI_MAX_NAV) {
        G.nav_items[G.nav_count].id = id;
        G.nav_items[G.nav_count].win = w;
        G.nav_count++;
    }
    if (w == G.nav_win && id == G.nav_id && G.nav_scroll_to) {
        float m = G.style.item_spacing.y;
        if (y0 - m < w->inner_y0) w->scroll_y -= w->inner_y0 - (y0 - m);
        else if (y1 + m > w->inner_y1) w->scroll_y += (y1 + m) - w->inner_y1;
        w->scroll_y = clampf(w->scroll_y, 0.0f, fmaxf_(w->scroll_max, w->content_h));
    }
}

static int button_behavior(float x0, float y0, float x1, float y1, uint32_t id, int *out_hovered, int *out_held)
{
    int hovered = item_hoverable(x0, y0, x1, y1, id), held = 0, pressed = 0;
    if (hovered && G.mclicked[0]) {
        G.active_id = id;
        if (G.cur == G.nav_win) G.nav_id = id;
    }
    if (G.active_id == id) {
        G.active_seen = 1;
        if (G.io.mouse_down[0]) held = 1;
        else { pressed = hovered; G.active_id = 0; }
    }
    if (nav_here(id) && G.nav_pressed[GUI_NAV_ACTIVATE]) {
        pressed = 1;
        held = 1;
    }
    G.cur->last_hovered = hovered;
    if (out_hovered) *out_hovered = hovered;
    if (out_held) *out_held = held;
    return pressed;
}

static void nav_highlight(float x0, float y0, float x1, float y1, uint32_t id, float rounding)
{
    if (G.nav_visible && nav_here(id))
        dl_rect(&G.cur->dl, x0 - 2.0f, y0 - 2.0f, x1 + 2.0f, y1 + 2.0f, col32(GUI_COL_NAV_HIGHLIGHT),
                rounding + 2.0f, 2.0f);
}

static int nav_left_right(uint32_t id)
{
    if (!nav_here(id)) return 0;
    return G.nav_pressed[GUI_NAV_RIGHT] - G.nav_pressed[GUI_NAV_LEFT];
}

int gui_is_item_hovered(void) { return G.cur && G.cur->last_hovered; }

/* ── Widgets ──────────────────────────────────────────────────── */

static void text_ex(const char *s, const char *end, uint32_t col)
{
    GuiWindow *w = G.cur;
    GuiVec2 sz;
    float y;
    if (!w) return;
    if (!end) end = s + strlen(s);
    sz = text_size(s, end);
    y = w->cy + w->line_text_off;
    if (!w->hidden) dl_text(&w->dl, w->cx, y, col, s, end);
    item_add(w->cx, y, w->cx + sz.x, y + sz.y, 0, 0);
    item_size(sz.x, sz.y + w->line_text_off, 0.0f);
}

void gui_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    text_ex(vfmt(fmt, ap), NULL, col32(GUI_COL_TEXT));
    va_end(ap);
}

void gui_text_colored(GuiColor col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    text_ex(vfmt(fmt, ap), NULL, col32c(col));
    va_end(ap);
}

void gui_text_disabled(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    text_ex(vfmt(fmt, ap), NULL, col32(GUI_COL_TEXT_DISABLED));
    va_end(ap);
}

void gui_bullet_text(const char *fmt, ...)
{
    GuiWindow *w = G.cur;
    const char *s;
    GuiVec2 sz;
    va_list ap;
    float y, fs = G.font_size;
    if (!w) return;
    va_start(ap, fmt);
    s = vfmt(fmt, ap);
    va_end(ap);
    sz = text_size(s, NULL);
    y = w->cy + w->line_text_off;
    dl_circle_filled(&w->dl, w->cx + fs * 0.5f, y + fs * 0.5f, fs * 0.18f, col32(GUI_COL_TEXT));
    dl_text(&w->dl, w->cx + fs + G.style.item_inner_spacing.x, y, col32(GUI_COL_TEXT), s, NULL);
    item_add(w->cx, y, w->cx + fs + G.style.item_inner_spacing.x + sz.x, y + sz.y, 0, 0);
    item_size(fs + G.style.item_inner_spacing.x + sz.x, sz.y + w->line_text_off, 0.0f);
}

/* The label of a framed widget, right of the frame. Returns its width. */
static float side_label(const char *label, float x, float y)
{
    const char *le = label_end(label);
    if (le == label) return 0.0f;
    dl_text(&G.cur->dl, x + G.style.item_inner_spacing.x, y + G.style.frame_padding.y, col32(GUI_COL_TEXT), label, le);
    return G.style.item_inner_spacing.x + text_size(label, le).x;
}

void gui_label_text(const char *label, const char *fmt, ...)
{
    GuiWindow *w = G.cur;
    const char *s;
    va_list ap;
    float iw, lw;
    if (!w) return;
    va_start(ap, fmt);
    s = vfmt(fmt, ap);
    va_end(ap);
    iw = item_width();
    dl_text(&w->dl, w->cx, w->cy + G.style.frame_padding.y, col32(GUI_COL_TEXT), s, NULL);
    lw = side_label(label, w->cx + iw, w->cy);
    item_add(w->cx, w->cy, w->cx + iw + lw, w->cy + frame_h(), 0, 0);
    item_size(iw + lw, frame_h(), G.style.frame_padding.y);
}

int gui_button(const char *label)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    const char *le;
    GuiVec2 ts;
    uint32_t id;
    float x0, y0, x1, y1;
    int hovered, held, pressed;
    if (!w) return 0;
    le = label_end(label);
    ts = text_size(label, le);
    id = get_id(label);
    x0 = w->cx; y0 = w->cy;
    x1 = floorf(x0 + ts.x + st->frame_padding.x * 2.0f);
    y1 = y0 + frame_h();
    item_add(x0, y0, x1, y1, id, 1);
    pressed = button_behavior(x0, y0, x1, y1, id, &hovered, &held);
    dl_rect_filled(&w->dl, x0, y0, x1, y1,
                   col32(held && hovered ? GUI_COL_BUTTON_ACTIVE : hovered ? GUI_COL_BUTTON_HOVERED : GUI_COL_BUTTON),
                   st->frame_rounding, CORNERS_ALL);
    dl_text(&w->dl, x0 + (x1 - x0 - ts.x) * st->button_text_align.x, y0 + (y1 - y0 - ts.y) * st->button_text_align.y,
            col32(GUI_COL_TEXT), label, le);
    nav_highlight(x0, y0, x1, y1, id, st->frame_rounding);
    item_size(x1 - x0, y1 - y0, st->frame_padding.y);
    return pressed;
}

static void check_mark(DrawList *d, float x, float y, float sz, uint32_t col)
{
    float thick = fmaxf_(sz / 5.0f, 1.0f), third, bx, by;
    GuiVec2 p[3];
    sz -= thick * 0.5f;
    x += thick * 0.25f; y += thick * 0.25f;
    third = sz / 3.0f;
    bx = x + third;
    by = y + sz - third * 0.5f;
    p[0].x = bx - third; p[0].y = by - third;
    p[1].x = bx;         p[1].y = by;
    p[2].x = bx + third * 2.0f; p[2].y = by - third * 2.0f;
    dl_polyline(d, p, 3, col, 0, thick);
}

int gui_checkbox(const char *label, int *v)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    uint32_t id;
    float sq = frame_h(), x0, y0, lw;
    int hovered, held, pressed;
    if (!w || !v) return 0;
    id = get_id(label);
    x0 = w->cx; y0 = w->cy;
    lw = text_size(label, label_end(label)).x;
    lw = lw > 0.0f ? st->item_inner_spacing.x + lw : 0.0f;
    item_add(x0, y0, x0 + sq + lw, y0 + sq, id, 1);
    pressed = button_behavior(x0, y0, x0 + sq + lw, y0 + sq, id, &hovered, &held);
    if (pressed) *v = !*v;
    dl_rect_filled(&w->dl, x0, y0, x0 + sq, y0 + sq,
                   col32(held && hovered ? GUI_COL_FRAME_BG_ACTIVE : hovered ? GUI_COL_FRAME_BG_HOVERED : GUI_COL_FRAME_BG),
                   st->frame_rounding, CORNERS_ALL);
    if (*v) {
        float pad = fmaxf_(1.0f, floorf(sq / 6.0f));
        check_mark(&w->dl, x0 + pad, y0 + pad, sq - pad * 2.0f, col32(GUI_COL_CHECK_MARK));
    }
    side_label(label, x0 + sq, y0);
    nav_highlight(x0, y0, x0 + sq + lw, y0 + sq, id, st->frame_rounding);
    item_size(sq + lw, sq, st->frame_padding.y);
    return pressed;
}

int gui_radio_button(const char *label, int active)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    uint32_t id;
    float sq = frame_h(), x0, y0, lw, r;
    int hovered, held, pressed;
    if (!w) return 0;
    id = get_id(label);
    x0 = w->cx; y0 = w->cy;
    lw = text_size(label, label_end(label)).x;
    lw = lw > 0.0f ? st->item_inner_spacing.x + lw : 0.0f;
    item_add(x0, y0, x0 + sq + lw, y0 + sq, id, 1);
    pressed = button_behavior(x0, y0, x0 + sq + lw, y0 + sq, id, &hovered, &held);
    r = sq * 0.5f;
    dl_circle_filled(&w->dl, x0 + r, y0 + r, r,
                     col32(held && hovered ? GUI_COL_FRAME_BG_ACTIVE : hovered ? GUI_COL_FRAME_BG_HOVERED : GUI_COL_FRAME_BG));
    if (active)
        dl_circle_filled(&w->dl, x0 + r, y0 + r, r - fmaxf_(1.0f, floorf(sq / 6.0f)) - 1.0f, col32(GUI_COL_CHECK_MARK));
    side_label(label, x0 + sq, y0);
    nav_highlight(x0, y0, x0 + sq + lw, y0 + sq, id, r);
    item_size(sq + lw, sq, st->frame_padding.y);
    return pressed;
}

/* Slider over a float; is_int rounds to whole steps. */
static int slider_ex(const char *label, float *v, float min, float max, const char *fmt, int is_int)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    uint32_t id;
    float iw, x0, y0, x1, y1, grab, gpad = 2.0f, range = max - min, t, gx, old = *v;
    int hovered, steps;
    char buf[64];
    GuiVec2 ts;
    if (!w) return 0;
    id = get_id(label);
    iw = item_width();
    x0 = w->cx; y0 = w->cy; x1 = x0 + iw; y1 = y0 + frame_h();
    item_add(x0, y0, x1, y1, id, 1);
    hovered = item_hoverable(x0, y0, x1, y1, id);
    w->last_hovered = hovered;
    grab = st->grab_min_size;
    if (is_int && range >= 0.0f)
        grab = fmaxf_(grab, (iw - gpad * 2.0f) / (range + 1.0f));
    grab = fminf_(grab, iw - gpad * 2.0f);
    if (hovered && G.mclicked[0]) {
        G.active_id = id;
        if (G.cur == G.nav_win) G.nav_id = id;
    }
    if (G.active_id == id) {
        G.active_seen = 1;
        if (G.io.mouse_down[0] && iw - gpad * 2.0f - grab > 0.0f) {
            t = clampf((G.io.mouse_x - x0 - gpad - grab * 0.5f) / (iw - gpad * 2.0f - grab), 0.0f, 1.0f);
            *v = min + range * t;
        } else if (!G.io.mouse_down[0]) {
            G.active_id = 0;
        }
    }
    steps = nav_left_right(id);
    if (steps)
        *v = clampf(*v + (float)steps * (is_int ? 1.0f : range / 100.0f), fminf_(min, max), fmaxf_(min, max));
    if (is_int) *v = floorf(*v + 0.5f);
    t = range != 0.0f ? clampf((*v - min) / range, 0.0f, 1.0f) : 0.0f;
    gx = x0 + gpad + grab * 0.5f + t * (iw - gpad * 2.0f - grab);

    dl_rect_filled(&w->dl, x0, y0, x1, y1,
                   col32(G.active_id == id ? GUI_COL_FRAME_BG_ACTIVE : hovered ? GUI_COL_FRAME_BG_HOVERED : GUI_COL_FRAME_BG),
                   st->frame_rounding, CORNERS_ALL);
    dl_rect_filled(&w->dl, gx - grab * 0.5f, y0 + gpad, gx + grab * 0.5f, y1 - gpad,
                   col32(G.active_id == id ? GUI_COL_SLIDER_GRAB_ACTIVE : GUI_COL_SLIDER_GRAB),
                   st->grab_rounding, CORNERS_ALL);
    if (is_int) snprintf(buf, sizeof buf, fmt ? fmt : "%d", (int)*v);
    else snprintf(buf, sizeof buf, fmt ? fmt : "%.3f", *v);
    ts = text_size(buf, NULL);
    dl_text(&w->dl, x0 + (iw - ts.x) * 0.5f, y0 + st->frame_padding.y, col32(GUI_COL_TEXT), buf, NULL);
    nav_highlight(x0, y0, x1, y1, id, st->frame_rounding);
    item_size(iw + side_label(label, x1, y0), y1 - y0, st->frame_padding.y);
    return *v != old;
}

int gui_slider_float(const char *label, float *v, float min, float max, const char *fmt)
{
    return v ? slider_ex(label, v, min, max, fmt, 0) : 0;
}

int gui_slider_int(const char *label, int *v, int min, int max, const char *fmt)
{
    float f;
    int changed;
    if (!v) return 0;
    f = (float)*v;
    changed = slider_ex(label, &f, (float)min, (float)max, fmt, 1);
    *v = (int)f;
    return changed;
}

/* A small square button that is not a navigation stop. */
static int step_button(const char *txt, uint32_t id, float x0, float y0, float sz)
{
    GuiWindow *w = G.cur;
    int hovered, held, pressed;
    GuiVec2 ts = text_size(txt, NULL);
    item_add(x0, y0, x0 + sz, y0 + sz, id, 0);
    pressed = button_behavior(x0, y0, x0 + sz, y0 + sz, id, &hovered, &held);
    dl_rect_filled(&w->dl, x0, y0, x0 + sz, y0 + sz,
                   col32(held && hovered ? GUI_COL_BUTTON_ACTIVE : hovered ? GUI_COL_BUTTON_HOVERED : GUI_COL_BUTTON),
                   G.style.frame_rounding, CORNERS_ALL);
    dl_text(&w->dl, x0 + (sz - ts.x) * 0.5f, y0 + (sz - ts.y) * 0.5f, col32(GUI_COL_TEXT), txt, NULL);
    return pressed;
}

int gui_input_int(const char *label, int *v, int step)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    uint32_t id;
    float iw, sz = frame_h(), x0, y0, fw, sp = st->item_inner_spacing.x;
    int old, hovered, d;
    char buf[32];
    GuiVec2 ts;
    if (!w || !v) return 0;
    old = *v;
    if (step == 0) step = 1;
    id = get_id(label);
    iw = item_width();
    x0 = w->cx; y0 = w->cy;
    fw = fmaxf_(sz, iw - (sz + sp) * 2.0f);
    item_add(x0, y0, x0 + fw, y0 + sz, id, 1);
    hovered = item_hoverable(x0, y0, x0 + fw, y0 + sz, id);
    w->last_hovered = hovered;
    d = nav_left_right(id);
    if (d) *v += d * step;
    dl_rect_filled(&w->dl, x0, y0, x0 + fw, y0 + sz,
                   col32(hovered ? GUI_COL_FRAME_BG_HOVERED : GUI_COL_FRAME_BG), st->frame_rounding, CORNERS_ALL);
    snprintf(buf, sizeof buf, "%d", *v);
    ts = text_size(buf, NULL);
    dl_text(&w->dl, x0 + (fw - ts.x) * 0.5f, y0 + st->frame_padding.y, col32(GUI_COL_TEXT), buf, NULL);
    nav_highlight(x0, y0, x0 + fw, y0 + sz, id, st->frame_rounding);
    if (step_button("-", hash_str("-", NULL, id), x0 + fw + sp, y0, sz)) *v -= step;
    if (step_button("+", hash_str("+", NULL, id), x0 + fw + sp * 2.0f + sz, y0, sz)) *v += step;
    w->last_id = id;
    item_size(fw + (sz + sp) * 2.0f + side_label(label, x0 + fw + (sz + sp) * 2.0f, y0), sz, st->frame_padding.y);
    return *v != old;
}

static void arrow(DrawList *d, float x, float y, int down, uint32_t col)
{
    float h = G.font_size, r = h * 0.40f, cx = x + h * 0.5f, cy = y + h * 0.5f;
    if (down)
        dl_triangle_filled(d, cx, cy + 0.75f * r, cx - 0.866f * r, cy - 0.75f * r, cx + 0.866f * r, cy - 0.75f * r, col);
    else
        dl_triangle_filled(d, cx + 0.75f * r, cy, cx - 0.75f * r, cy + 0.866f * r, cx - 0.75f * r, cy - 0.866f * r, col);
}

int gui_selectable(const char *label, int selected)
{
    GuiWindow *w = G.cur;
    const char *le;
    uint32_t id;
    GuiVec2 ts;
    float x0, y0, x1, y1, sp;
    int hovered, held, pressed;
    if (!w) return 0;
    le = label_end(label);
    ts = text_size(label, le);
    id = get_id(label);
    sp = floorf(G.style.item_spacing.y * 0.5f);
    x0 = w->cx; y0 = w->cy - sp;
    x1 = fmaxf_(w->inner_x1, x0 + ts.x);
    y1 = w->cy + ts.y + sp;
    item_add(x0, y0, x1, y1, id, 1);
    pressed = button_behavior(x0, y0, x1, y1, id, &hovered, &held);
    if (selected || hovered)
        dl_rect_filled(&w->dl, x0, y0, x1, y1,
                       col32(held && hovered ? GUI_COL_HEADER_ACTIVE : hovered ? GUI_COL_HEADER_HOVERED : GUI_COL_HEADER),
                       0.0f, 0);
    dl_text(&w->dl, x0, w->cy, col32(GUI_COL_TEXT), label, le);
    nav_highlight(x0, y0, x1, y1, id, 0.0f);
    item_size(ts.x, ts.y, 0.0f);
    return pressed;
}

int gui_combo(const char *label, int *current, const char *const *items, int count)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    uint32_t id;
    float iw, fh = frame_h(), x0, y0, x1, y1;
    int hovered, held, pressed, changed = 0, d;
    const char *preview;
    if (!w || !current || !items || count <= 0) return 0;
    id = get_id(label);
    iw = item_width();
    x0 = w->cx; y0 = w->cy; x1 = x0 + iw; y1 = y0 + fh;
    item_add(x0, y0, x1, y1, id, 1);
    pressed = button_behavior(x0, y0, x1, y1, id, &hovered, &held);
    if (pressed) {
        if (G.popup_id == id) {
            close_popup();
        } else if (G.popup_closed_by_click != id) {
            G.popup_id = id;
            G.popup_return_id = id;
            G.popup_owner = w;
            G.popup_nav_init = 1;
        }
    }
    d = G.popup_id == id ? 0 : nav_left_right(id);
    if (d) {
        int n = *current + d;
        if (n >= 0 && n < count) { *current = n; changed = 1; }
    }
    preview = *current >= 0 && *current < count ? items[*current] : "";
    dl_rect_filled(&w->dl, x0, y0, x1 - fh, y1, col32(hovered ? GUI_COL_FRAME_BG_HOVERED : GUI_COL_FRAME_BG),
                   st->frame_rounding, CORNERS_LEFT);
    dl_rect_filled(&w->dl, x1 - fh, y0, x1, y1, col32(hovered || G.popup_id == id ? GUI_COL_BUTTON_HOVERED : GUI_COL_BUTTON),
                   st->frame_rounding, CORNERS_RIGHT);
    arrow(&w->dl, x1 - fh + (fh - G.font_size) * 0.5f, y0 + st->frame_padding.y, 1, col32(GUI_COL_TEXT));
    {
        float c[4];
        memcpy(c, w->dl.clip, sizeof c);
        dl_set_clip(&w->dl, fmaxf_(c[0], x0), fmaxf_(c[1], y0), fminf_(c[2], x1 - fh), fminf_(c[3], y1));
        dl_text(&w->dl, x0 + st->frame_padding.x, y0 + st->frame_padding.y, col32(GUI_COL_TEXT), preview, NULL);
        dl_set_clip(&w->dl, c[0], c[1], c[2], c[3]);
    }
    nav_highlight(x0, y0, x1, y1, id, st->frame_rounding);
    item_size(iw + side_label(label, x1, y0), fh, st->frame_padding.y);

    if (G.popup_id == id) {
        int i, init = G.popup_nav_init;
        G.popup_seen = 1;
        gui_set_next_window_pos(x0, y1 + 2.0f, GUI_COND_ALWAYS, 0.0f, 0.0f);
        G.next.min_w = iw;
        gui_begin("##ComboPopup", NULL, GUI_WINDOW_NO_TITLE_BAR | GUI_WINDOW_NO_RESIZE | GUI_WINDOW_NO_MOVE
                  | GUI_WINDOW_AUTO_RESIZE | GUI_WINDOW_POPUP_);
        G.popup_nav_init = 0;
        for (i = 0; i < count; i++) {
            gui_push_id_int(i);
            if (init && i == *current) G.nav_id = get_id(items[i]);
            if (gui_selectable(items[i], i == *current)) {
                *current = i;
                changed = 1;
                G.popup_id = 0;             /* closed after gui_end below */
            }
            gui_pop_id();
        }
        gui_end();
        if (!G.popup_id) {
            G.popup_id = id;
            close_popup();
        }
    }
    return changed;
}

int gui_collapsing_header(const char *label, int default_open)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    const char *le;
    uint32_t id;
    int *open, hovered, held, d;
    float x0, y0, x1, y1;
    GuiVec2 ts;
    if (!w) return 0;
    le = label_end(label);
    ts = text_size(label, le);
    id = get_id(label);
    open = storage(id, default_open ? 1 : 0);
    x0 = w->cx; y0 = w->cy;
    x1 = fmaxf_(w->inner_x1, x0 + ts.x);
    y1 = y0 + frame_h();
    item_add(x0, y0, x1, y1, id, 1);
    if (button_behavior(x0, y0, x1, y1, id, &hovered, &held)) *open = !*open;
    d = nav_left_right(id);
    if (d) *open = d > 0;
    dl_rect_filled(&w->dl, x0, y0, x1, y1,
                   col32(held && hovered ? GUI_COL_HEADER_ACTIVE : hovered ? GUI_COL_HEADER_HOVERED : GUI_COL_HEADER),
                   st->frame_rounding, CORNERS_ALL);
    arrow(&w->dl, x0 + st->frame_padding.x, y0 + st->frame_padding.y, *open, col32(GUI_COL_TEXT));
    dl_text(&w->dl, x0 + G.font_size + st->frame_padding.x * 2.0f, y0 + st->frame_padding.y, col32(GUI_COL_TEXT), label, le);
    nav_highlight(x0, y0, x1, y1, id, st->frame_rounding);
    item_size(G.font_size + st->frame_padding.x * 3.0f + ts.x, y1 - y0, st->frame_padding.y);
    return *open;
}

void gui_progress_bar(float fraction, float bw, float bh, const char *overlay)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    float x0, y0, x1, y1;
    char buf[32];
    GuiVec2 ts;
    if (!w) return;
    fraction = clampf(fraction, 0.0f, 1.0f);
    x0 = w->cx; y0 = w->cy;
    x1 = x0 + (bw > 0.0f ? bw : fmaxf_(1.0f, w->inner_x1 - x0));
    y1 = y0 + (bh > 0.0f ? bh : frame_h());
    item_add(x0, y0, x1, y1, 0, 0);
    dl_rect_filled(&w->dl, x0, y0, x1, y1, col32(GUI_COL_FRAME_BG), st->frame_rounding, CORNERS_ALL);
    if (fraction > 0.0f)
        dl_rect_filled(&w->dl, x0, y0, x0 + (x1 - x0) * fraction, y1, col32(GUI_COL_PLOT_HISTOGRAM),
                       st->frame_rounding, CORNERS_ALL);
    if (!overlay) {
        snprintf(buf, sizeof buf, "%.0f%%", fraction * 100.0f);
        overlay = buf;
    }
    ts = text_size(overlay, NULL);
    dl_text(&w->dl, x0 + (x1 - x0 - ts.x) * 0.5f, y0 + (y1 - y0 - ts.y) * 0.5f, col32(GUI_COL_TEXT), overlay, NULL);
    item_size(bw > 0.0f ? bw : 0.0f, y1 - y0, st->frame_padding.y);
}

static void plot_ex(int histogram, const char *label, const float *values, int count, int offset,
                    const char *overlay, float min, float max, float pw, float ph)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    float x0, y0, x1, y1, ix0, iy0, ix1, iy1;
    int i;
    if (!w) return;
    x0 = w->cx; y0 = w->cy;
    x1 = x0 + (pw > 0.0f ? pw : item_width());
    y1 = y0 + (ph > 0.0f ? ph : frame_h());
    item_add(x0, y0, x1, y1, 0, 0);
    dl_rect_filled(&w->dl, x0, y0, x1, y1, col32(GUI_COL_FRAME_BG), st->frame_rounding, CORNERS_ALL);
    ix0 = x0 + st->frame_padding.x; iy0 = y0 + st->frame_padding.y;
    ix1 = x1 - st->frame_padding.x; iy1 = y1 - st->frame_padding.y;
    if (values && count > 0 && ix1 > ix0 && iy1 > iy0) {
        if (min >= max) {
            min = max = values[0];
            for (i = 1; i < count; i++) { min = fminf_(min, values[i]); max = fmaxf_(max, values[i]); }
            if (max - min < 1e-6f) max = min + 1.0f;
        }
        if (histogram) {
            float bw = (ix1 - ix0) / (float)count;
            for (i = 0; i < count; i++) {
                float v = values[((i + offset) % count + count) % count];
                float t = clampf((v - min) / (max - min), 0.0f, 1.0f);
                dl_rect_filled(&w->dl, ix0 + bw * (float)i, iy1 - (iy1 - iy0) * t, ix0 + bw * (float)(i + 1) - 1.0f, iy1,
                               col32(GUI_COL_PLOT_HISTOGRAM), 0.0f, 0);
            }
        } else if (count > 1) {
            static GuiVec2 pts[GUI_PATH_MAX];
            int n = count < GUI_PATH_MAX ? count : GUI_PATH_MAX;
            for (i = 0; i < n; i++) {
                float v = values[((count - n + i + offset) % count + count) % count];
                pts[i].x = ix0 + (ix1 - ix0) * (float)i / (float)(n - 1);
                pts[i].y = iy1 - (iy1 - iy0) * clampf((v - min) / (max - min), 0.0f, 1.0f);
            }
            dl_polyline(&w->dl, pts, n, col32(GUI_COL_PLOT_LINES), 0, 1.0f);
        }
    }
    if (overlay) {
        GuiVec2 ts = text_size(overlay, NULL);
        dl_text(&w->dl, x0 + (x1 - x0 - ts.x) * 0.5f, y0 + st->frame_padding.y, col32(GUI_COL_TEXT), overlay, NULL);
    }
    item_size((x1 - x0) + (label ? side_label(label, x1, y0) : 0.0f), y1 - y0, st->frame_padding.y);
}

void gui_plot_lines(const char *label, const float *values, int count, int offset,
                    const char *overlay, float min, float max, float w, float h)
{
    plot_ex(0, label, values, count, offset, overlay, min, max, w, h);
}

void gui_plot_histogram(const char *label, const float *values, int count, int offset,
                        const char *overlay, float min, float max, float w, float h)
{
    plot_ex(1, label, values, count, offset, overlay, min, max, w, h);
}

void gui_separator(void)
{
    GuiWindow *w = G.cur;
    if (!w) return;
    dl_rect_filled(&w->dl, w->inner_x0, floorf(w->cy), w->inner_x1, floorf(w->cy) + 1.0f, col32(GUI_COL_SEPARATOR), 0.0f, 0);
    item_size(0.0f, 1.0f, 0.0f);
}

/* ── Tabs ─────────────────────────────────────────────────────── */

void gui_begin_tab_bar(const char *str_id)
{
    if (!G.cur) return;
    G.tab.id = get_id(str_id);
    G.tab.sel = (uint32_t)*storage(G.tab.id, 0);
    G.tab.n = 0;
}

int gui_tab_item(const char *label)
{
    GuiWindow *w = G.cur;
    GuiStyle *st = &G.style;
    const char *le;
    GuiVec2 ts;
    uint32_t id;
    float tw, x0, y0, x1, y1;
    int hovered, held;
    if (!w || !G.tab.id) return 0;
    le = label_end(label);
    ts = text_size(label, le);
    id = hash_str(label, NULL, G.tab.id);
    if (!G.tab.sel) G.tab.sel = id;
    if (G.tab.n < GUI_MAX_TABS) G.tab.ids[G.tab.n++] = id;
    tw = floorf(ts.x + st->frame_padding.x * 2.0f);
    if (G.tab.n > 1 && w->prev_x + st->item_inner_spacing.x + tw <= w->inner_x1)
        gui_same_line(st->item_inner_spacing.x);
    x0 = w->cx; y0 = w->cy; x1 = x0 + tw; y1 = y0 + frame_h();
    item_add(x0, y0, x1, y1, id, 1);
    if (button_behavior(x0, y0, x1, y1, id, &hovered, &held))
        *storage(G.tab.id, 0) = (int)id;    /* shown from the next frame */
    dl_rect_filled(&w->dl, x0, y0, x1, y1,
                   col32(id == G.tab.sel ? GUI_COL_TAB_ACTIVE : hovered ? GUI_COL_TAB_HOVERED : GUI_COL_TAB),
                   st->tab_rounding, CORNERS_TOP);
    if (id == G.tab.sel)
        dl_rect_filled(&w->dl, x0, y1 - 2.0f, x1, y1, col32(GUI_COL_SLIDER_GRAB), 0.0f, 0);
    dl_text(&w->dl, x0 + st->frame_padding.x, y0 + st->frame_padding.y,
            col32(id == G.tab.sel ? GUI_COL_TEXT : GUI_COL_TEXT_DISABLED), label, le);
    nav_highlight(x0, y0, x1, y1, id, st->tab_rounding);
    item_size(tw, y1 - y0, st->frame_padding.y);
    return id == G.tab.sel;
}

void gui_end_tab_bar(void)
{
    GuiWindow *w = G.cur;
    int *stored, i, cur = -1, dir = 0, nav_on_tab = 0;
    if (!w || !G.tab.id) return;
    stored = storage(G.tab.id, 0);
    for (i = 0; i < G.tab.n; i++) {
        if (G.tab.ids[i] == (uint32_t)*stored) cur = i;
        if (nav_here(G.tab.ids[i])) nav_on_tab = 1;
    }
    if (cur < 0 && G.tab.n) { cur = 0; *stored = (int)G.tab.ids[0]; }
    if (w == G.nav_win)
        dir = G.nav_pressed[GUI_NAV_TAB_NEXT] - G.nav_pressed[GUI_NAV_TAB_PREV];
    if (nav_on_tab)
        dir += G.nav_pressed[GUI_NAV_RIGHT] - G.nav_pressed[GUI_NAV_LEFT];
    if (dir && G.tab.n) {
        cur = ((cur + dir) % G.tab.n + G.tab.n) % G.tab.n;
        *stored = (int)G.tab.ids[cur];
        if (nav_on_tab) G.nav_id = G.tab.ids[cur];
    }
    dl_rect_filled(&w->dl, w->inner_x0, w->prev_y + frame_h(), w->inner_x1, w->prev_y + frame_h() + 1.0f,
                   col32(GUI_COL_TAB_ACTIVE), 0.0f, 0);
    G.tab.id = 0;
}

/* ── Render ───────────────────────────────────────────────────── */

static void append(const DrawList *d)
{
    uint32_t vbase = (uint32_t)G.out.vn;
    int i;
    if (!d->in) return;
    G.out.vtx = (GuiVertex *)grow(G.out.vtx, &G.out.vcap, G.out.vn + d->vn, sizeof(GuiVertex));
    G.out.idx = (uint32_t *)grow(G.out.idx, &G.out.icap, G.out.in + d->in, sizeof(uint32_t));
    G.out.cmd = (GuiDrawCmd *)grow(G.out.cmd, &G.out.ccap, G.out.cn + d->cn, sizeof(GuiDrawCmd));
    memcpy(G.out.vtx + G.out.vn, d->vtx, sizeof(GuiVertex) * (size_t)d->vn);
    for (i = 0; i < d->in; i++)
        G.out.idx[G.out.in + i] = d->idx[i] + vbase;
    for (i = 0; i < d->cn; i++) {
        GuiDrawCmd c = d->cmd[i];
        if (!c.idx_count) continue;
        c.idx_offset += (uint32_t)G.out.in;
        G.out.cmd[G.out.cn++] = c;
    }
    G.out.vn += d->vn;
    G.out.in += d->in;
}

const GuiDrawData *gui_render(void)
{
    int i;
    GuiWindow *popup = NULL;

    memset(&G.dd, 0, sizeof G.dd);
    if (!G.ready)
        return &G.dd;
    if (G.popup_id && !G.popup_seen)
        close_popup();

    /* Navigation: the window that has it, and the step asked for. */
    if (G.nav_win && !G.nav_win->active) {
        G.nav_win = NULL;
        for (i = G.nwin - 1; i >= 0; i--) {
            GuiWindow *w = G.order[i];
            if (w->active && !(w->flags & (GUI_WINDOW_NO_NAV | GUI_WINDOW_NO_INPUTS | GUI_WINDOW_POPUP_))) {
                G.nav_win = w;
                G.nav_id = w->nav_last_id;
                break;
            }
        }
    }
    G.nav_scroll_to = 0;
    if (G.nav_win) {
        static uint32_t ids[GUI_MAX_NAV];
        int n = 0, at = -1;
        for (i = 0; i < G.nav_count; i++)
            if (G.nav_items[i].win == G.nav_win) {
                if (G.nav_items[i].id == G.nav_id) at = n;
                ids[n++] = G.nav_items[i].id;
            }
        if (n && at < 0) {
            G.nav_id = ids[0];
            G.nav_scroll_to = 1;
        } else if (n && G.nav_move) {
            at = ((at + G.nav_move) % n + n) % n;
            G.nav_id = ids[at];
            G.nav_scroll_to = 1;
        }
        G.nav_win->nav_last_id = G.nav_id;
    }

    if (!G.active_seen || !G.io.mouse_down[0])
        G.active_id = 0;
    G.active_seen = 0;
    G.io.want_mouse = G.hovered != NULL || G.active_id != 0;
    G.io.mouse_wheel = 0.0f;

    dl_reset(&G.out);
    for (i = 0; i < G.nwin; i++) {
        GuiWindow *w = G.order[i];
        if (!w->active || w->hidden) continue;
        if (w->flags & GUI_WINDOW_POPUP_) { popup = w; continue; }
        append(&w->dl);
    }
    if (popup && G.popup_id)
        append(&popup->dl);

    G.dd.vtx = G.out.vtx; G.dd.idx = G.out.idx; G.dd.cmd = G.out.cmd;
    G.dd.vtx_count = G.out.vn; G.dd.idx_count = G.out.in; G.dd.cmd_count = G.out.cn;
    G.dd.display_w = G.io.display_w; G.dd.display_h = G.io.display_h;
    return &G.dd;
}
