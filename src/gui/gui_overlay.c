/*
 * The runtime overlay: performance panel, menu, title windows, notices. See
 * gui_overlay.h.
 *
 * Statistics are the ones the earlier GDI panel showed, from the same
 * sources: frame timing from our own calls (one per present), CPU from
 * GetProcessTimes, RAM = working set, VRAM from
 * IDXGIAdapter3::QueryVideoMemoryInfo, GPU % from the counter Task Manager
 * reads (\GPU Engine(pid_<ours>_*engtype_3D)\Utilization Percentage). They
 * refresh twice a second; the frame-time graph every frame.
 *
 * ponytail: no temperatures -- Windows has no vendor-neutral API for them.
 * ponytail: the font is baked once, at the first frame's back-buffer size;
 * re-bake on a large resize if that ever matters.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <xinput.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "gui_d3d11.h"
#include "gui_overlay.h"

/* xbox_input (xinput_xbox.h is not included: it brings its own Win32 types). */
void xbox_InputSetCaptured(BOOL captured);

#define MAX_PAGES   32
#define MAX_NOTICES 4
#define FT_COUNT    120

typedef struct {
    char      name[48];
    GuiPageFn fn;
    void     *user;
} Page;

static SRWLOCK       s_lock = SRWLOCK_INIT;     /* pages, title, notices */
static Page          s_pages[MAX_PAGES], s_windows[MAX_PAGES];
static int           s_npages, s_nwindows;
static char          s_title[64] = "Debug";
static struct { char text[128]; ULONGLONG until; } s_notes[MAX_NOTICES];
static int           s_note_next;

static int           s_enabled = -1, s_perf = -1, s_failed, s_focus_menu;
static volatile LONG s_menu_open, s_wheel, s_press;
static ID3D11Device *s_dev;
static float         s_scale = 1.0f;

/* performance */
static LARGE_INTEGER  s_freq, s_last_frame, s_last_sample;
static float          s_ft[FT_COUNT];
static int            s_ft_at;
static unsigned       s_frames;
static double         s_ms_sum;
static ULONGLONG      s_cpu_last;
static double         s_fps, s_ms, s_cpu, s_gpu = -1.0, s_ram, s_vram = -1.0;
static IDXGIAdapter3 *s_adapter;
static PDH_HQUERY     s_query;
static PDH_HCOUNTER   s_gpu_counter;

static int env_on(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return !(!_stricmp(v, "off") || !strcmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no"));
}

/* ── Public switches ──────────────────────────────────────────── */

static void add_entry(Page *list, int *n, const char *name, GuiPageFn draw, void *user)
{
    int i;
    if (!name || !draw) return;
    AcquireSRWLockExclusive(&s_lock);
    for (i = 0; i < *n && strcmp(list[i].name, name); i++) ;
    if (i < MAX_PAGES) {
        snprintf(list[i].name, sizeof list[i].name, "%s", name);
        list[i].fn = draw;
        list[i].user = user;
        if (i == *n) (*n)++;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void gui_overlay_add_page(const char *name, GuiPageFn draw, void *user) { add_entry(s_pages, &s_npages, name, draw, user); }
void gui_overlay_add_window(const char *name, GuiPageFn draw, void *user) { add_entry(s_windows, &s_nwindows, name, draw, user); }

void gui_overlay_set_title(const char *title)
{
    AcquireSRWLockExclusive(&s_lock);
    snprintf(s_title, sizeof s_title, "%s", title && *title ? title : "Debug");
    ReleaseSRWLockExclusive(&s_lock);
}

int gui_overlay_enabled(void)
{
    if (s_enabled < 0) s_enabled = env_on("RECOMP_GUI", 0);
    return s_enabled && !s_failed;
}

void gui_overlay_set_enabled(int on)
{
    s_enabled = on ? 1 : 0;
    if (!on) gui_overlay_set_menu_open(0);
}

void gui_overlay_set_menu_open(int open)
{
    open = open && gui_overlay_enabled();
    if (open && !s_menu_open) s_focus_menu = 1;
    if (!open != !s_menu_open) fprintf(stderr, "[GUI] menu %s\n", open ? "open" : "closed");
    InterlockedExchange(&s_menu_open, open);
    xbox_InputSetCaptured(open);
}

int gui_overlay_menu_open(void) { return s_menu_open != 0; }
int gui_overlay_wants_input(void) { return s_menu_open != 0 && gui_overlay_enabled(); }
int gui_overlay_wants_mouse(void) { return gui_overlay_enabled() && gui_ready() && (s_menu_open || gui_io()->want_mouse); }

int gui_overlay_perf_shown(void)
{
    if (s_perf < 0) s_perf = env_on("RECOMP_OSD", 1);
    return s_perf;
}
void gui_overlay_show_perf(int on) { s_perf = on ? 1 : 0; }

void gui_overlay_notice(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[GUI] %s\n", buf);
    AcquireSRWLockExclusive(&s_lock);
    snprintf(s_notes[s_note_next].text, sizeof s_notes[0].text, "%s", buf);
    s_notes[s_note_next].until = GetTickCount64() + 4000;
    s_note_next = (s_note_next + 1) % MAX_NOTICES;
    ReleaseSRWLockExclusive(&s_lock);
}

void gui_overlay_press(int nav)
{
    if (nav >= 0 && nav < GUI_NAV_COUNT) InterlockedOr(&s_press, 1L << nav);
}

void gui_overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam)
{
    (void)hwnd; (void)lparam;
    if (msg == WM_MOUSEWHEEL)
        InterlockedExchangeAdd(&s_wheel, GET_WHEEL_DELTA_WPARAM((WPARAM)wparam));
}

/* ── Start-up ─────────────────────────────────────────────────── */

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *p = NULL;
    long n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0
            && (p = malloc((size_t)n)) != NULL && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    if (p) *size = (size_t)n;
    return p;
}

static int font_init(unsigned target_h)
{
    static const char *faces[] = { "segoeuisb.ttf", "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
    const char *env = getenv("RECOMP_GUI_FONT");
    char path[MAX_PATH], dir[MAX_PATH];
    void *ttf = NULL;
    size_t size = 0, i;
    int ok;

    if (env && *env) {
        snprintf(path, sizeof path, "%s", env);
        ttf = read_file(path, &size);
    }
    if (!GetWindowsDirectoryA(dir, sizeof dir))
        snprintf(dir, sizeof dir, "C:\\Windows");
    for (i = 0; !ttf && i < sizeof faces / sizeof faces[0]; i++) {
        snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, faces[i]);
        ttf = read_file(path, &size);
    }
    if (!ttf) {
        fprintf(stderr, "[GUI] no font found (set RECOMP_GUI_FONT to a .ttf); overlay off\n");
        return 0;
    }
    {
        const char *v = getenv("RECOMP_GUI_SCALE");
        s_scale = v && atof(v) > 0.0 ? (float)atof(v) : (float)target_h / 720.0f;
        if (s_scale < 1.0f && !(v && atof(v) > 0.0)) s_scale = 1.0f;
        if (s_scale > 4.0f) s_scale = 4.0f;
    }
    ok = gui_init(ttf, size, 16.0f * s_scale);
    free(ttf);
    if (!ok) fprintf(stderr, "[GUI] cannot read font %s; overlay off\n", path);
    else fprintf(stderr, "[GUI] overlay ready: %s, %.0f px\n", path, gui_font_size());
    return ok;
}

static void perf_init(ID3D11Device *dev)
{
    IDXGIDevice *dx = NULL;
    IDXGIAdapter *ad = NULL;
    if (s_adapter) { IDXGIAdapter3_Release(s_adapter); s_adapter = NULL; }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad))) {
            IDXGIAdapter_QueryInterface(ad, &IID_IDXGIAdapter3, (void **)&s_adapter);
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dx);
    }
    if (!s_query && PdhOpenQueryW(NULL, 0, &s_query) == ERROR_SUCCESS) {
        wchar_t path[128];
        swprintf(path, 128, L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
        if (PdhAddEnglishCounterW(s_query, path, 0, &s_gpu_counter) != ERROR_SUCCESS)
            s_gpu_counter = NULL;
        PdhCollectQueryData(s_query);
    }
    if (!s_freq.QuadPart) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_last_sample);
        s_last_frame = s_last_sample;
    }
}

/* ── Performance ──────────────────────────────────────────────── */

static ULONGLONG cpu_time(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)
         + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static double gpu_percent(void)
{
    DWORD size = 0, count = 0, i;
    PDH_FMT_COUNTERVALUE_ITEM_W *items;
    double sum = 0.0;
    if (!s_gpu_counter || PdhCollectQueryData(s_query) != ERROR_SUCCESS)
        return -1.0;
    if (PdhGetFormattedCounterArrayW(s_gpu_counter, PDH_FMT_DOUBLE, &size, &count, NULL) != (PDH_STATUS)PDH_MORE_DATA || !size)
        return -1.0;
    items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size);
    if (!items)
        return -1.0;
    if (PdhGetFormattedCounterArrayW(s_gpu_counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS)
        for (i = 0; i < count; i++)
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                sum += items[i].FmtValue.doubleValue;
    free(items);
    return sum > 100.0 ? 100.0 : sum;
}

/* Count this frame; every half second, fresh numbers. Returns the frame time in s. */
static float perf_tick(void)
{
    LARGE_INTEGER now;
    double dt, since;
    QueryPerformanceCounter(&now);
    dt = (double)(now.QuadPart - s_last_frame.QuadPart) / (double)s_freq.QuadPart;
    s_last_frame = now;
    s_frames++;
    s_ms_sum += dt * 1000.0;
    s_ft[s_ft_at] = (float)(dt * 1000.0);
    s_ft_at = (s_ft_at + 1) % FT_COUNT;
    since = (double)(now.QuadPart - s_last_sample.QuadPart) / (double)s_freq.QuadPart;
    if (since >= 0.5 && (gui_overlay_perf_shown() || s_menu_open)) {
        ULONGLONG cpu = cpu_time();
        SYSTEM_INFO si;
        PROCESS_MEMORY_COUNTERS pmc;
        GetSystemInfo(&si);
        s_fps = s_frames / since;
        s_ms = s_frames ? s_ms_sum / s_frames : 0.0;
        s_cpu = s_cpu_last ? (double)(cpu - s_cpu_last) / (since * 1e7) * 100.0 / si.dwNumberOfProcessors : 0.0;
        s_cpu_last = cpu;
        s_gpu = gpu_percent();
        memset(&pmc, 0, sizeof pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        s_ram = pmc.WorkingSetSize / (1024.0 * 1024.0);
        s_vram = -1.0;
        if (s_adapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO vi;
            if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(s_adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi)))
                s_vram = vi.CurrentUsage / (1024.0 * 1024.0);
        }
        {   /* the same numbers in the log every 5 s, for unattended runs */
            static unsigned logged;
            if (gui_overlay_perf_shown() && logged++ % 10 == 0) {
                fprintf(stderr, "[OSD] FPS %5.1f   %5.1f ms  CPU %.0f%%  GPU %.0f%%  RAM %.0f MB  VRAM %.0f MB\n",
                        s_fps, s_ms, s_cpu, s_gpu, s_ram, s_vram);
                fflush(stderr);
            }
        }
        s_frames = 0;
        s_ms_sum = 0.0;
        s_last_sample = now;
    }
    return (float)dt;
}

static void perf_lines(void)
{
    gui_text("FPS %5.1f   %5.1f ms", s_fps, s_ms);
}

static void perf_resources(void)
{
    gui_text("CPU %4.0f%%   RAM %5.0f MB", s_cpu, s_ram);
    if (s_gpu >= 0.0 && s_vram >= 0.0) gui_text("GPU %4.0f%%   VRAM %4.0f MB", s_gpu, s_vram);
    else if (s_vram >= 0.0) gui_text("GPU   --    VRAM %4.0f MB", s_vram);
    else gui_text("GPU   --");
}

static void frame_graph(float w, float h)
{
    float top = 33.4f;
    int i;
    for (i = 0; i < FT_COUNT; i++) if (s_ft[i] * 1.1f > top) top = s_ft[i] * 1.1f;
    gui_plot_lines("##frametime", s_ft, FT_COUNT, s_ft_at, NULL, 0.0f, top, w, h);
}

/* ── Windows ──────────────────────────────────────────────────── */

static void draw_perf_panel(void)
{
    float fs = gui_font_size();
    gui_set_next_window_pos(10.0f * s_scale, 10.0f * s_scale, GUI_COND_ALWAYS, 0.0f, 0.0f);
    gui_set_next_window_bg_alpha(0.82f);
    gui_begin("##perf", NULL, GUI_WINDOW_NO_TITLE_BAR | GUI_WINDOW_NO_INPUTS | GUI_WINDOW_AUTO_RESIZE
              | GUI_WINDOW_NO_NAV | GUI_WINDOW_NO_SCROLLBAR);
    perf_lines();
    frame_graph(fs * 12.0f, fs * 2.2f);
    perf_resources();
    gui_end();
}

static void draw_notices(float bw, float bh)
{
    char text[MAX_NOTICES][128];
    ULONGLONG now = GetTickCount64();
    int i, n = 0;
    AcquireSRWLockShared(&s_lock);
    for (i = 0; i < MAX_NOTICES; i++) {
        int k = (s_note_next + i) % MAX_NOTICES;     /* oldest first */
        if (s_notes[k].until > now) memcpy(text[n++], s_notes[k].text, sizeof text[0]);
    }
    ReleaseSRWLockShared(&s_lock);
    if (!n) return;
    gui_set_next_window_pos(bw * 0.5f, bh - 40.0f * s_scale, GUI_COND_ALWAYS, 0.5f, 1.0f);
    gui_set_next_window_bg_alpha(0.88f);
    gui_begin("##notices", NULL, GUI_WINDOW_NO_TITLE_BAR | GUI_WINDOW_NO_INPUTS | GUI_WINDOW_AUTO_RESIZE
              | GUI_WINDOW_NO_NAV | GUI_WINDOW_NO_SCROLLBAR);
    for (i = 0; i < n; i++) gui_text("%s", text[i]);
    gui_end();
}

static void performance_page(void)
{
    int on = gui_overlay_perf_shown();
    if (gui_checkbox("Show the performance panel", &on)) gui_overlay_show_perf(on);
    gui_spacing();
    perf_lines();
    perf_resources();
    gui_spacing();
    gui_text_disabled("Frame time, last %d frames (ms)", FT_COUNT);
    frame_graph(gui_content_width(), gui_font_size() * 4.0f);
    gui_spacing();
    gui_separator();
    gui_spacing();
    gui_text_disabled("F1 or L3+R3: open / close this menu");
    gui_text_disabled("Arrows or D-pad: move     Enter or A: select");
    gui_text_disabled("Left / right: adjust     PgUp / PgDn or LB / RB: tab");
    gui_text_disabled("Esc or B: close");
}

/* The SDK's own page for the host pads: what XInput reports on each port,
 * before the title's mapping. */
static void input_page(void)
{
    static const struct { WORD bit; const char *name; } btn[] = {
        { XINPUT_GAMEPAD_A, "A" }, { XINPUT_GAMEPAD_B, "B" }, { XINPUT_GAMEPAD_X, "X" }, { XINPUT_GAMEPAD_Y, "Y" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
        { XINPUT_GAMEPAD_BACK, "Back" }, { XINPUT_GAMEPAD_START, "Start" },
        { XINPUT_GAMEPAD_LEFT_THUMB, "L3" }, { XINPUT_GAMEPAD_RIGHT_THUMB, "R3" },
        { XINPUT_GAMEPAD_DPAD_UP, "Up" }, { XINPUT_GAMEPAD_DPAD_DOWN, "Down" },
        { XINPUT_GAMEPAD_DPAD_LEFT, "Left" }, { XINPUT_GAMEPAD_DPAD_RIGHT, "Right" },
    };
    DWORD port;
    gui_text_disabled("While this menu is open the title sees an idle pad.");
    for (port = 0; port < 4; port++) {
        XINPUT_STATE xs;
        char hdr[32], held[128];
        size_t i, n = 0;
        int on = XInputGetState(port, &xs) == ERROR_SUCCESS;
        snprintf(hdr, sizeof hdr, "Pad %lu: %s##pad%lu", port + 1, on ? "connected" : "not connected", port);
        if (!gui_collapsing_header(hdr, on && port == 0) || !on)
            continue;
        held[0] = 0;
        for (i = 0; i < sizeof btn / sizeof btn[0]; i++)
            if (xs.Gamepad.wButtons & btn[i].bit)
                n += (size_t)snprintf(held + n, n < sizeof held ? sizeof held - n : 0, "%s ", btn[i].name);
        gui_text("Buttons: %s", held[0] ? held : "none");
        gui_text("Left stick %6d %6d     Right stick %6d %6d", xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY,
                 xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY);
        gui_progress_bar(xs.Gamepad.bLeftTrigger / 255.0f, gui_content_width() * 0.5f - 4.0f, 0, "LT");
        gui_same_line(-1);
        gui_progress_bar(xs.Gamepad.bRightTrigger / 255.0f, gui_content_width(), 0, "RT");
    }
}

static void draw_menu(float bw, float bh)
{
    Page pages[MAX_PAGES];
    char title[64];
    float fs = gui_font_size();
    int open = 1, n, i, sel = -2;

    AcquireSRWLockShared(&s_lock);
    n = s_npages;
    memcpy(pages, s_pages, sizeof(Page) * (size_t)n);
    snprintf(title, sizeof title, "%s", s_title);
    ReleaseSRWLockShared(&s_lock);

    gui_set_next_window_pos(40.0f * s_scale, 150.0f * s_scale, GUI_COND_FIRST_USE, 0.0f, 0.0f);   /* below the panel */
    gui_set_next_window_size(fminf(bw - 80.0f * s_scale, fs * 32.0f), fminf(bh - 190.0f * s_scale, fs * 28.0f),
                             GUI_COND_FIRST_USE);
    if (s_focus_menu) {
        gui_set_next_window_focus();
        s_focus_menu = 0;
    }
    gui_begin(title, &open, 0);
    gui_begin_tab_bar("##pages");
    if (gui_tab_item("Performance")) sel = -1;
    if (gui_tab_item("Input")) sel = -3;
    for (i = 0; i < n; i++)
        if (gui_tab_item(pages[i].name)) sel = i;
    gui_end_tab_bar();
    gui_spacing();
    {   /* in the log, so unattended runs can follow the menu */
        static int logged = -100;
        if (sel != logged) {
            fprintf(stderr, "[GUI] page %s\n", sel == -1 ? "Performance" : sel == -3 ? "Input"
                    : sel >= 0 ? pages[sel].name : "(none)");
            logged = sel;
        }
    }
    if (sel == -1) {
        performance_page();
    } else if (sel == -3) {
        input_page();
    } else if (sel >= 0) {
        gui_push_id(pages[sel].name);
        pages[sel].fn(pages[sel].user);
        gui_pop_id();
    }
    if (gui_nav_pressed(GUI_NAV_CANCEL)) open = 0;
    gui_end();
    if (!open) gui_overlay_set_menu_open(0);
}

/* ── Input ────────────────────────────────────────────────────── */

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void poll_input(HWND hwnd, float bw, float bh, float dt)
{
    GuiIO *io = gui_io();
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : NULL;
    int fg = root && GetForegroundWindow() == root;
    XINPUT_STATE xs;
    WORD b = 0;
    short ly = 0, lx = 0;
    static int prev_f1, prev_combo;
    int f1, combo;

    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        b = xs.Gamepad.wButtons;
        lx = xs.Gamepad.sThumbLX;
        ly = xs.Gamepad.sThumbLY;
    }
    f1 = fg && key(VK_F1);
    combo = (b & (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB))
          == (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
    if ((f1 && !prev_f1) || (combo && !prev_combo))
        gui_overlay_set_menu_open(!s_menu_open);
    prev_f1 = f1;
    prev_combo = combo;

    io->display_w = bw;
    io->display_h = bh;
    io->delta_time = dt;
    io->mouse_x = io->mouse_y = -1.0f;
    io->mouse_down[0] = io->mouse_down[1] = 0;
    io->mouse_wheel = (float)InterlockedExchange(&s_wheel, 0) / (float)WHEEL_DELTA;
    memset(io->nav, 0, sizeof io->nav);
    if (fg) {
        POINT p;
        RECT rc;
        if (GetCursorPos(&p) && ScreenToClient(hwnd, &p) && GetClientRect(hwnd, &rc)
                && rc.right > 0 && rc.bottom > 0 && p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom) {
            io->mouse_x = (float)p.x * bw / (float)rc.right;
            io->mouse_y = (float)p.y * bh / (float)rc.bottom;
        }
        io->mouse_down[0] = key(VK_LBUTTON);
        io->mouse_down[1] = key(VK_RBUTTON);
    }
    if (!s_menu_open)
        return;
    if (fg) {
        io->nav[GUI_NAV_UP] = key(VK_UP);
        io->nav[GUI_NAV_DOWN] = key(VK_DOWN);
        io->nav[GUI_NAV_LEFT] = key(VK_LEFT);
        io->nav[GUI_NAV_RIGHT] = key(VK_RIGHT);
        io->nav[GUI_NAV_ACTIVATE] = key(VK_RETURN) || key(VK_SPACE);
        io->nav[GUI_NAV_CANCEL] = key(VK_ESCAPE) || key(VK_BACK);
        io->nav[GUI_NAV_TAB_PREV] = key(VK_PRIOR);
        io->nav[GUI_NAV_TAB_NEXT] = key(VK_NEXT);
    }
    io->nav[GUI_NAV_UP] |= (b & XINPUT_GAMEPAD_DPAD_UP) || ly > 16000;
    io->nav[GUI_NAV_DOWN] |= (b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -16000;
    io->nav[GUI_NAV_LEFT] |= (b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -16000;
    io->nav[GUI_NAV_RIGHT] |= (b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 16000;
    io->nav[GUI_NAV_ACTIVATE] |= (b & XINPUT_GAMEPAD_A) != 0;
    io->nav[GUI_NAV_CANCEL] |= (b & XINPUT_GAMEPAD_B) != 0;
    io->nav[GUI_NAV_TAB_PREV] |= (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    io->nav[GUI_NAV_TAB_NEXT] |= (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    {   /* scripted presses (gui_overlay_press): down this frame, up the next */
        LONG p = InterlockedExchange(&s_press, 0);
        int k;
        for (k = 0; k < GUI_NAV_COUNT; k++)
            if (p & (1L << k)) io->nav[k] = 1;
    }
}

/* ── Frame ────────────────────────────────────────────────────── */

void gui_overlay_frame_d3d11(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx,
                             struct ID3D11RenderTargetView *rtv, void *hwnd)
{
    ID3D11Resource *res = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC td;
    float dt, bw, bh;

    if (!gui_overlay_enabled() || !dev || !ctx || !rtv)
        return;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    if (!res) return;
    ID3D11Resource_GetType(res, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Resource_Release(res);
        return;
    }
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    ID3D11Resource_Release(res);
    bw = (float)td.Width;
    bh = (float)td.Height;

    if (!gui_ready() && !font_init(td.Height)) {
        s_failed = 1;
        return;
    }
    /* Every frame: returns at once unless the device is new, or the host
     * released the renderer's objects with gui_d3d11_shutdown() when its
     * device was lost. */
    if (!gui_d3d11_init(dev, ctx)) {
        fprintf(stderr, "[GUI] D3D11 renderer failed; overlay off\n");
        s_failed = 1;
        return;
    }
    if (s_dev != dev) {
        perf_init(dev);
        s_dev = dev;
    }

    dt = perf_tick();
    poll_input((HWND)hwnd, bw, bh, dt);
    gui_new_frame();
    if (gui_overlay_perf_shown())
        draw_perf_panel();
    {   /* the title's own windows */
        Page w[MAX_PAGES];
        int i, n;
        AcquireSRWLockShared(&s_lock);
        n = s_nwindows;
        memcpy(w, s_windows, sizeof(Page) * (size_t)n);
        ReleaseSRWLockShared(&s_lock);
        for (i = 0; i < n; i++)
            w[i].fn(w[i].user);
    }
    draw_notices(bw, bh);
    if (s_menu_open)
        draw_menu(bw, bh);
    gui_d3d11_render(gui_render(), rtv);
}
