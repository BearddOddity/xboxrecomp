/*
 * gui -- an immediate-mode GUI in C, after Dear ImGui.
 *
 * The same model as Dear ImGui: no retained widget tree. Each frame the code
 * that owns a window calls gui_begin(), then one function per widget, then
 * gui_end(); a widget returns what the user did to it this frame. State that
 * must outlive a frame (window position, which header is open) lives inside
 * the library, keyed by a hash of the label.
 *
 *     gui_new_frame();
 *     if (gui_begin("Debug", &open, 0)) {
 *         gui_text("frame %d", n);
 *         if (gui_button("Reload")) reload();
 *         gui_checkbox("Wireframe", &wire);
 *         gui_slider_float("Gamma", &gamma, 0.5f, 3.0f, "%.2f");
 *     }
 *     gui_end();
 *     draw = gui_render();          (hand it to a renderer: gui_d3d11.h)
 *
 * Labels: "Name##id" shows "Name" and hashes the whole string, so two widgets
 * with the same caption stay apart. gui_push_id() scopes ids in loops.
 *
 * The core is plain C11 with no platform calls: input arrives in GuiIO, output
 * leaves as one vertex/index list (GuiDrawData). Navigation without a mouse
 * (keyboard or pad: move, activate, cancel, adjust, switch tab) is built in,
 * because the overlay this exists for runs over a game held with a pad.
 *
 * Not a port: no tables, docking, text input, child windows or multiple fonts.
 * ponytail: add each when a page needs it, not before.
 *
 * Threading: one thread drives a frame from gui_new_frame() to gui_render().
 */
#ifndef RECOMP_GUI_H
#define RECOMP_GUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float x, y; } GuiVec2;
typedef struct { float r, g, b, a; } GuiColor;

/* ── Style ─────────────────────────────────────────────────────── */

enum {
    GUI_COL_TEXT, GUI_COL_TEXT_DISABLED,
    GUI_COL_WINDOW_BG, GUI_COL_POPUP_BG, GUI_COL_BORDER,
    GUI_COL_FRAME_BG, GUI_COL_FRAME_BG_HOVERED, GUI_COL_FRAME_BG_ACTIVE,
    GUI_COL_TITLE_BG, GUI_COL_TITLE_BG_ACTIVE,
    GUI_COL_SCROLLBAR_BG, GUI_COL_SCROLLBAR_GRAB, GUI_COL_SCROLLBAR_GRAB_HOVERED, GUI_COL_SCROLLBAR_GRAB_ACTIVE,
    GUI_COL_CHECK_MARK, GUI_COL_SLIDER_GRAB, GUI_COL_SLIDER_GRAB_ACTIVE,
    GUI_COL_BUTTON, GUI_COL_BUTTON_HOVERED, GUI_COL_BUTTON_ACTIVE,
    GUI_COL_HEADER, GUI_COL_HEADER_HOVERED, GUI_COL_HEADER_ACTIVE,
    GUI_COL_SEPARATOR,
    GUI_COL_RESIZE_GRIP, GUI_COL_RESIZE_GRIP_HOVERED, GUI_COL_RESIZE_GRIP_ACTIVE,
    GUI_COL_TAB, GUI_COL_TAB_HOVERED, GUI_COL_TAB_ACTIVE,
    GUI_COL_PLOT_LINES, GUI_COL_PLOT_LINES_HOVERED, GUI_COL_PLOT_HISTOGRAM, GUI_COL_PLOT_HISTOGRAM_HOVERED,
    GUI_COL_NAV_HIGHLIGHT,
    GUI_COL_COUNT
};

typedef struct {
    float   alpha;
    GuiVec2 window_padding;
    float   window_rounding, window_border_size;
    GuiVec2 window_min_size, window_title_align;
    float   popup_rounding;
    GuiVec2 frame_padding;
    float   frame_rounding, frame_border_size;
    GuiVec2 item_spacing, item_inner_spacing;
    float   indent_spacing;
    float   scrollbar_size, scrollbar_rounding;
    float   grab_min_size, grab_rounding;
    float   tab_rounding;
    GuiVec2 button_text_align;
    GuiColor colors[GUI_COL_COUNT];
} GuiStyle;

/* The default look: the Moonlight theme by Madam-Herta / deathsu
 * (https://github.com/Madam-Herta/Moonlight), at scale 1. */
void gui_style_moonlight(GuiStyle *s);
/* Multiply every size in the style by k (for high-resolution output). */
void gui_style_scale(GuiStyle *s, float k);
GuiStyle *gui_style(void);

/* ── Input and output of a frame ───────────────────────────────── */

/* Navigation without a mouse. Fill GuiIO.nav[] with what is held now; the
 * library makes presses and key repeat out of it. */
enum {
    GUI_NAV_UP, GUI_NAV_DOWN, GUI_NAV_LEFT, GUI_NAV_RIGHT,
    GUI_NAV_ACTIVATE,           /* Enter, Space, pad A */
    GUI_NAV_CANCEL,             /* Escape, Backspace, pad B */
    GUI_NAV_TAB_PREV, GUI_NAV_TAB_NEXT,     /* pad LB / RB, PgUp / PgDn */
    GUI_NAV_COUNT
};

typedef struct {
    /* in, every frame */
    float display_w, display_h;     /* pixels of the target the GUI draws into */
    float delta_time;               /* seconds since the last frame */
    float mouse_x, mouse_y;         /* < 0: no mouse */
    int   mouse_down[2];            /* left, right */
    float mouse_wheel;              /* notches this frame, + is away from the user */
    int   nav[GUI_NAV_COUNT];
    /* out, after gui_render() */
    int   want_mouse;               /* the mouse is over a window or dragging one */
} GuiIO;

GuiIO *gui_io(void);

typedef struct {
    float    x, y, u, v;
    uint32_t col;                   /* 0xAABBGGRR: R8G8B8A8_UNORM byte order */
} GuiVertex;

typedef struct {
    float    clip[4];               /* x0 y0 x1 y1, pixels: scissor rectangle */
    uint32_t idx_offset, idx_count;
} GuiDrawCmd;

/* The whole frame, back to front. Indices are absolute into vtx. */
typedef struct {
    const GuiVertex  *vtx;
    const uint32_t   *idx;
    const GuiDrawCmd *cmd;
    int   vtx_count, idx_count, cmd_count;
    float display_w, display_h;
} GuiDrawData;

/* ── Setup ─────────────────────────────────────────────────────── */

/* ttf: a TrueType font file in memory (copied). font_px: line height in
 * pixels. The style starts as Moonlight scaled by font_px / 16. Returns 0 if
 * the font cannot be read. */
int  gui_init(const void *ttf, size_t ttf_size, float font_px);
void gui_shutdown(void);
int  gui_ready(void);

/* The glyph atlas the renderer uploads once: one byte of coverage per pixel.
 * A 2x2 fully covered block is included for solid fills. */
void gui_font_atlas(const uint8_t **alpha8, int *w, int *h);
float gui_font_size(void);

/* ── Frame ─────────────────────────────────────────────────────── */

void gui_new_frame(void);
const GuiDrawData *gui_render(void);   /* ends the frame */

/* Was this navigation input pressed this frame (with repeat)? Cancel is not
 * consumed by the library unless a popup is open, so the owner of a window
 * can close it on cancel. */
int  gui_nav_pressed(int nav);

/* ── Windows ───────────────────────────────────────────────────── */

enum {
    GUI_WINDOW_NO_TITLE_BAR  = 1 << 0,
    GUI_WINDOW_NO_RESIZE     = 1 << 1,
    GUI_WINDOW_NO_MOVE       = 1 << 2,
    GUI_WINDOW_AUTO_RESIZE   = 1 << 3,  /* size follows the content */
    GUI_WINDOW_NO_INPUTS     = 1 << 4,  /* the mouse passes through; no navigation */
    GUI_WINDOW_NO_BACKGROUND = 1 << 5,
    GUI_WINDOW_NO_SCROLLBAR  = 1 << 6,
    GUI_WINDOW_NO_NAV        = 1 << 7,
};
enum { GUI_COND_ALWAYS, GUI_COND_FIRST_USE };

/* For the next gui_begin(). pivot (0..1) says which point of the window sits
 * at (x, y): 0,0 top-left, 1,0 top-right, 0.5,0.5 centre. */
void gui_set_next_window_pos(float x, float y, int cond, float pivot_x, float pivot_y);
void gui_set_next_window_size(float w, float h, int cond);
void gui_set_next_window_bg_alpha(float a);
void gui_set_next_window_focus(void);   /* bring to front, take navigation */

/* p_open: non-NULL shows a close button that clears *p_open. Always pair
 * with gui_end(), whatever this returns (it returns 1). */
int  gui_begin(const char *name, int *p_open, int flags);
void gui_end(void);

/* ── Layout ────────────────────────────────────────────────────── */

void  gui_same_line(float spacing);     /* spacing < 0: the style's */
void  gui_new_line(void);
void  gui_spacing(void);
void  gui_separator(void);
void  gui_indent(void);
void  gui_unindent(void);
void  gui_align_text_to_frame_padding(void);   /* text lines up with a framed widget beside it */
void  gui_push_item_width(float w);     /* > 0 pixels; < 0 leaves that much on the right */
void  gui_pop_item_width(void);
float gui_content_width(void);          /* from the cursor to the right edge */

void gui_push_id(const char *str);
void gui_push_id_int(int n);
void gui_pop_id(void);

/* ── Widgets ───────────────────────────────────────────────────── */

void gui_text(const char *fmt, ...);
void gui_text_colored(GuiColor col, const char *fmt, ...);
void gui_text_disabled(const char *fmt, ...);
void gui_label_text(const char *label, const char *fmt, ...);   /* value, then label on the right */
void gui_bullet_text(const char *fmt, ...);

int  gui_button(const char *label);
int  gui_checkbox(const char *label, int *v);
int  gui_radio_button(const char *label, int active);
int  gui_slider_float(const char *label, float *v, float min, float max, const char *fmt);
int  gui_slider_int(const char *label, int *v, int min, int max, const char *fmt);
int  gui_input_int(const char *label, int *v, int step);         /* - and + buttons */
int  gui_combo(const char *label, int *current, const char *const *items, int count);
int  gui_selectable(const char *label, int selected);
int  gui_collapsing_header(const char *label, int default_open);
void gui_progress_bar(float fraction, float w, float h, const char *overlay);   /* w,h <= 0: default */
/* min >= max: scale to the values. offset: index of the oldest value (ring
 * buffer). w,h <= 0: default. */
void gui_plot_lines(const char *label, const float *values, int count, int offset,
                    const char *overlay, float min, float max, float w, float h);
void gui_plot_histogram(const char *label, const float *values, int count, int offset,
                        const char *overlay, float min, float max, float w, float h);

/* Tabs: a row of buttons, one selected. Draw the selected tab's content after
 * gui_end_tab_bar(). Pad LB/RB step through the tabs from anywhere in the window. */
void gui_begin_tab_bar(const char *id);
int  gui_tab_item(const char *label);   /* 1 for the selected tab */
void gui_end_tab_bar(void);

int  gui_is_item_hovered(void);         /* the widget just submitted */

/* The size the current font gives text (up to "##", as labels show it), for
 * lining columns up with gui_same_line(). */
GuiVec2 gui_calc_text_size(const char *text);

/* ── Free drawing ──────────────────────────────────────────────── */

/* Shapes and text at absolute display pixels, into the current window (after
 * gui_begin), clipped to it: for a screen that draws its own look, such as a
 * game's menu style. Colours: gui_color_u32(). */
uint32_t gui_color_u32(GuiColor c);
void gui_draw_rect_filled(float x0, float y0, float x1, float y1, uint32_t col, float rounding);
void gui_draw_rect(float x0, float y0, float x1, float y1, uint32_t col, float rounding, float thick);
void gui_draw_line(float x0, float y0, float x1, float y1, uint32_t col, float thick);
void gui_draw_triangle_filled(float ax, float ay, float bx, float by, float cx, float cy, uint32_t col);
void gui_draw_circle_filled(float cx, float cy, float r, uint32_t col);
/* Text with its top-left at (x, y); scale 1 is the font's size.
 * ponytail: one baked font, so large scales are soft; a second size is the upgrade. */
void gui_draw_text(float x, float y, uint32_t col, float scale, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_GUI_H */
