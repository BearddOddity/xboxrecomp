/*
 * The runtime overlay: the SDK's GUI layer over any title's image, drawn
 * with gui.h (an immediate-mode GUI in C, after Dear ImGui) at each present.
 * Off unless the title (or RECOMP_GUI=on) turns it on; templates/new-game
 * turns it on.
 *
 * What is in it:
 *   performance panel   FPS, frame time and a frame-time graph, process CPU,
 *                       GPU 3D load, RAM and VRAM. Top left. RECOMP_OSD=off
 *                       hides it at start; the menu toggles it.
 *   menu                F1, or both sticks clicked (L3+R3) on pad 1. A window
 *                       of tabs. The SDK's own come first: "Performance",
 *                       "Input" (the host pads), and pages SDK subsystems add
 *                       ("Files", from the kernel). Then the title's pages.
 *                       Arrows / D-pad move, Enter / A select, left/right
 *                       adjust, PgUp/PgDn or LB/RB switch tab, Esc / B close.
 *   windows             a title's own windows, drawn every frame whether the
 *                       menu is open or not: settings screens, tools,
 *                       anything built from gui_* widgets.
 *   notices             one-line messages at the bottom, four seconds each.
 *
 * The SDK holds no title's UI. A title (and any SDK subsystem) adds its own:
 *
 *     static void cheats(void *user) {
 *         gui_checkbox("God mode", &god);
 *         if (gui_button("Reload level")) want_reload = 1;
 *     }
 *     static void tracker(void *user) {
 *         gui_begin("Tracker", NULL, GUI_WINDOW_AUTO_RESIZE);
 *         gui_text("lap %d", lap);
 *         gui_end();
 *     }
 *     gui_overlay_add_page("Cheats", cheats, NULL);       (a tab in the menu)
 *     gui_overlay_add_window("Tracker", tracker, NULL);   (always drawn)
 *     gui_overlay_set_enabled(1);
 *
 * Callbacks run on the thread that presents. A page runs between gui_begin()
 * and gui_end() of the menu window while its tab is selected; a window
 * callback runs every frame and makes its own gui_begin()/gui_end(). They may
 * call any gui_* function, and should only set flags or request work that the
 * title's own thread picks up; they must not block.
 *
 * While the menu is open the pad belongs to it: xbox_InputGetState() reports
 * a pad with nothing pressed, and a title reading the host pad another way
 * should check gui_overlay_wants_input(). gui_overlay_wants_mouse() says the
 * mouse is over the GUI (keep it from the title's own mouse handling).
 *
 * A host with its own Direct3D 11 presenter (an NV2A back end) calls
 * gui_overlay_frame_d3d11() just before its Present; the SDK's D3D8 layer
 * already does. Forward WM_MOUSEWHEEL to gui_overlay_wndproc() for the wheel.
 *
 * Environment: RECOMP_GUI=on|off, RECOMP_OSD=on|off, RECOMP_GUI_SCALE=<k>
 * (default: back-buffer height / 720, at least 1), RECOMP_GUI_FONT=<.ttf>
 * (default: Segoe UI Semibold from the Windows font folder).
 */
#ifndef RECOMP_GUI_OVERLAY_H
#define RECOMP_GUI_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

typedef void (*GuiPageFn)(void *user);

/* Menu tabs and always-drawn windows, in the order added; adding a name that
 * exists replaces its callback. Callable from any thread, before or after
 * the overlay starts. */
void gui_overlay_add_page(const char *name, GuiPageFn draw, void *user);
void gui_overlay_add_window(const char *name, GuiPageFn draw, void *user);
void gui_overlay_set_title(const char *title);    /* the menu window's title */

void gui_overlay_set_enabled(int on);
int  gui_overlay_enabled(void);
void gui_overlay_set_menu_open(int open);
int  gui_overlay_menu_open(void);
int  gui_overlay_wants_input(void);               /* the menu has the pad and keys */
int  gui_overlay_wants_mouse(void);               /* the mouse is on the GUI */
void gui_overlay_show_perf(int on);
int  gui_overlay_perf_shown(void);

/* One press of a GUI_NAV_* input (gui.h) at the next frame, as if from a
 * key: lets scripted test runs drive the menu without the host keyboard.
 * Callable from any thread. */
void gui_overlay_press(int nav);

/* A message at the bottom of the screen for four seconds, and in the log.
 * Callable from any thread. */
void gui_overlay_notice(const char *fmt, ...);

/* Once per presented frame, after the frame is drawn and just before
 * Present: reads input, runs the GUI, draws it into rtv. hwnd: the game
 * window (an HWND), for the mouse and for knowing when it has focus. Does
 * nothing while the overlay is off. */
void gui_overlay_frame_d3d11(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx,
                             struct ID3D11RenderTargetView *rtv, void *hwnd);

/* Optional: forward the window's messages here for the mouse wheel. */
void gui_overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_GUI_OVERLAY_H */
