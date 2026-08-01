/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/raylib/rcore_armos.c
 * Layer: Userland / Raylib platform backend
 *
 * Responsibilities:
 * - Bind Raylib to the common ArmOS Wayland client contract.
 * - Create an OpenGL ES 2 context through the platform-neutral EGL ABI.
 * - Translate configure, pointer and keyboard events into Raylib core state.
 * - Pace presentation through the EGL/Wayland buffer lifecycle.
 *
 * Architecture:
 * - This file contains no VirtIO, VirGL, VC4, V3D, Raspberry Pi or QEMU API.
 * - EGL and the compositor select the active DRM driver and buffer path.
 * - Window geometry is changed only after an xdg_surface configure event.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl-core.h>
#include <xkbcommon/xkbcommon.h>
#include <xdg-shell-client-protocol.h>

#define ARMOS_WAYLAND_EVENTS_PER_FRAME 64u

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wmBase;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdgSurface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *nativeWindow;
    struct xkb_context *xkbContext;
    struct xkb_keymap *xkbKeymap;
    struct xkb_state *xkbState;
    EGLDisplay eglDisplay;
    EGLSurface eglSurface;
    EGLContext eglContext;
    EGLConfig eglConfig;
    int pendingWidth;
    int pendingHeight;
    bool configured;
    bool fullscreen;
} PlatformData;

extern CoreData CORE;
static PlatformData platform = { 0 };

static int ArmOSMapKey(uint32_t key)
{
    static const int numberRow[] = {
        KEY_ZERO, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR,
        KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE
    };

    if ((key >= 2u) && (key <= 10u)) return numberRow[key - 1u];
    if (key == 11u) return KEY_ZERO;
    if ((key >= 59u) && (key <= 68u)) return KEY_F1 + (int)(key - 59u);
    if (key == 87u) return KEY_F11;
    if (key == 88u) return KEY_F12;

    switch (key)
    {
        case 1: return KEY_ESCAPE;
        case 14: return KEY_BACKSPACE;
        case 15: return KEY_TAB;
        case 16: return KEY_Q;
        case 17: return KEY_W;
        case 18: return KEY_E;
        case 19: return KEY_R;
        case 20: return KEY_T;
        case 21: return KEY_Y;
        case 22: return KEY_U;
        case 23: return KEY_I;
        case 24: return KEY_O;
        case 25: return KEY_P;
        case 28: return KEY_ENTER;
        case 29: return KEY_LEFT_CONTROL;
        case 30: return KEY_A;
        case 31: return KEY_S;
        case 32: return KEY_D;
        case 33: return KEY_F;
        case 34: return KEY_G;
        case 35: return KEY_H;
        case 36: return KEY_J;
        case 37: return KEY_K;
        case 38: return KEY_L;
        case 42: return KEY_LEFT_SHIFT;
        case 44: return KEY_Z;
        case 45: return KEY_X;
        case 46: return KEY_C;
        case 47: return KEY_V;
        case 48: return KEY_B;
        case 49: return KEY_N;
        case 50: return KEY_M;
        case 54: return KEY_RIGHT_SHIFT;
        case 56: return KEY_LEFT_ALT;
        case 57: return KEY_SPACE;
        case 58: return KEY_CAPS_LOCK;
        case 97: return KEY_RIGHT_CONTROL;
        case 100: return KEY_RIGHT_ALT;
        case 102: return KEY_HOME;
        case 103: return KEY_UP;
        case 104: return KEY_PAGE_UP;
        case 105: return KEY_LEFT;
        case 106: return KEY_RIGHT;
        case 107: return KEY_END;
        case 108: return KEY_DOWN;
        case 109: return KEY_PAGE_DOWN;
        case 110: return KEY_INSERT;
        case 111: return KEY_DELETE;
        default: return KEY_NULL;
    }
}

static int ArmOSMapButton(uint32_t button)
{
    if (button == 272u) return MOUSE_BUTTON_LEFT;
    if (button == 273u) return MOUSE_BUTTON_RIGHT;
    if (button == 274u) return MOUSE_BUTTON_MIDDLE;
    return -1;
}

static void ArmOSRegistryGlobal(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (!platform.compositor && strcmp(interface, "wl_compositor") == 0)
        platform.compositor = wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4u? version : 4u);
    else if (!platform.wmBase && strcmp(interface, "xdg_wm_base") == 0)
        platform.wmBase = wl_registry_bind(registry, name,
            &xdg_wm_base_interface, 1u);
    else if (!platform.seat && strcmp(interface, "wl_seat") == 0)
        platform.seat = wl_registry_bind(registry, name,
            &wl_seat_interface, version < 5u? version : 5u);
}

static void ArmOSRegistryRemove(void *data, struct wl_registry *registry,
    uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registryListener = {
    .global = ArmOSRegistryGlobal,
    .global_remove = ArmOSRegistryRemove
};

static void ArmOSWmPing(void *data, struct xdg_wm_base *wmBase,
    uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = {
    .ping = ArmOSWmPing
};

static void ArmOSSurfaceConfigure(void *data, struct xdg_surface *surface,
    uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    if ((platform.pendingWidth > 0) && (platform.pendingHeight > 0))
    {
        CORE.Window.resizedLastFrame =
            ((unsigned int)platform.pendingWidth != CORE.Window.screen.width) ||
            ((unsigned int)platform.pendingHeight != CORE.Window.screen.height);
        CORE.Window.screen.width = (unsigned int)platform.pendingWidth;
        CORE.Window.screen.height = (unsigned int)platform.pendingHeight;
        CORE.Window.render = CORE.Window.screen;
        CORE.Window.currentFbo = CORE.Window.screen;
        if (platform.nativeWindow)
            wl_egl_window_resize(platform.nativeWindow,
                platform.pendingWidth, platform.pendingHeight, 0, 0);
        xdg_surface_set_window_geometry(surface, 0, 0,
            platform.pendingWidth, platform.pendingHeight);
    }
    platform.configured = true;
}

static const struct xdg_surface_listener surfaceListener = {
    .configure = ArmOSSurfaceConfigure
};

static void ArmOSToplevelConfigure(void *data, struct xdg_toplevel *toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)states;
    if ((width > 0) && (height > 0))
    {
        platform.pendingWidth = width;
        platform.pendingHeight = height;
    }
}

static void ArmOSToplevelClose(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
    CORE.Window.shouldClose = true;
}

static const struct xdg_toplevel_listener toplevelListener = {
    .configure = ArmOSToplevelConfigure,
    .close = ArmOSToplevelClose
};

static void ArmOSPointerEnter(void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    CORE.Input.Mouse.cursorOnScreen = true;
    CORE.Input.Mouse.currentPosition = (Vector2){
        (float)wl_fixed_to_int(x), (float)wl_fixed_to_int(y) };
}

static void ArmOSPointerLeave(void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    CORE.Input.Mouse.cursorOnScreen = false;
}

static void ArmOSPointerMotion(void *data, struct wl_pointer *pointer,
    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)time;
    CORE.Input.Mouse.currentPosition = (Vector2){
        (float)wl_fixed_to_int(x), (float)wl_fixed_to_int(y) };
}

static void ArmOSPointerButton(void *data, struct wl_pointer *pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    int mapped = ArmOSMapButton(button);
    (void)data; (void)pointer; (void)serial; (void)time;
    if (mapped >= 0)
        CORE.Input.Mouse.currentButtonState[mapped] =
            state == WL_POINTER_BUTTON_STATE_PRESSED;
}

static void ArmOSPointerAxis(void *data, struct wl_pointer *pointer,
    uint32_t time, uint32_t axis, wl_fixed_t value)
{
    float amount = -(float)wl_fixed_to_int(value)/10.0f;
    (void)data; (void)pointer; (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        CORE.Input.Mouse.currentWheelMove.y += amount;
    else CORE.Input.Mouse.currentWheelMove.x += amount;
}

static void ArmOSPointerFrame(void *data, struct wl_pointer *pointer)
{ (void)data; (void)pointer; }
static void ArmOSPointerAxisSource(void *data, struct wl_pointer *pointer,
    uint32_t source) { (void)data; (void)pointer; (void)source; }
static void ArmOSPointerAxisStop(void *data, struct wl_pointer *pointer,
    uint32_t time, uint32_t axis)
{ (void)data; (void)pointer; (void)time; (void)axis; }
static void ArmOSPointerAxisDiscrete(void *data, struct wl_pointer *pointer,
    uint32_t axis, int32_t discrete)
{ (void)data; (void)pointer; (void)axis; (void)discrete; }

static const struct wl_pointer_listener pointerListener = {
    ArmOSPointerEnter, ArmOSPointerLeave, ArmOSPointerMotion,
    ArmOSPointerButton, ArmOSPointerAxis, ArmOSPointerFrame,
    ArmOSPointerAxisSource, ArmOSPointerAxisStop, ArmOSPointerAxisDiscrete
};

static void ArmOSKeyboardKeymap(void *data, struct wl_keyboard *keyboard,
    uint32_t format, int32_t fd, uint32_t size)
{
    char *mapping = MAP_FAILED;
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    (void)data; (void)keyboard;

    if ((format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) && (size > 0u))
    {
        mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if ((mapping != MAP_FAILED) && platform.xkbContext)
        {
            keymap = xkb_keymap_new_from_buffer(platform.xkbContext,
                mapping, size, XKB_KEYMAP_FORMAT_TEXT_V1,
                XKB_KEYMAP_COMPILE_NO_FLAGS);
            if (keymap) state = xkb_state_new(keymap);
        }
    }
    if (mapping != MAP_FAILED) munmap(mapping, size);
    close(fd);
    if (!keymap || !state)
    {
        if (state) xkb_state_unref(state);
        if (keymap) xkb_keymap_unref(keymap);
        return;
    }
    if (platform.xkbState) xkb_state_unref(platform.xkbState);
    if (platform.xkbKeymap) xkb_keymap_unref(platform.xkbKeymap);
    platform.xkbKeymap = keymap;
    platform.xkbState = state;
}

static void ArmOSKeyboardEnter(void *data, struct wl_keyboard *keyboard,
    uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{ (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys; }
static void ArmOSKeyboardLeave(void *data, struct wl_keyboard *keyboard,
    uint32_t serial, struct wl_surface *surface)
{ (void)data; (void)keyboard; (void)serial; (void)surface; }

static void ArmOSKeyboardKey(void *data, struct wl_keyboard *keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    int mapped = ArmOSMapKey(key);
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t codepoint = 0;
    (void)data; (void)keyboard; (void)serial; (void)time;

    if ((mapped > KEY_NULL) && (mapped < MAX_KEYBOARD_KEYS))
    {
        bool wasDown = CORE.Input.Keyboard.currentKeyState[mapped] != 0;
        CORE.Input.Keyboard.currentKeyState[mapped] = pressed;
        if (pressed && !wasDown &&
            (CORE.Input.Keyboard.keyPressedQueueCount < MAX_KEY_PRESSED_QUEUE))
            CORE.Input.Keyboard.keyPressedQueue[
                CORE.Input.Keyboard.keyPressedQueueCount++] = mapped;
        else if (pressed && wasDown)
            CORE.Input.Keyboard.keyRepeatInFrame[mapped] = 1;
    }
    if (pressed && (mapped != KEY_NULL) &&
        (mapped == CORE.Input.Keyboard.exitKey))
        CORE.Window.shouldClose = true;
    if (pressed && platform.xkbState)
        codepoint = xkb_state_key_get_utf32(platform.xkbState, key + 8u);
    if ((codepoint >= 0x20u) && (codepoint != 0x7fu) &&
        (CORE.Input.Keyboard.charPressedQueueCount < MAX_CHAR_PRESSED_QUEUE))
        CORE.Input.Keyboard.charPressedQueue[
            CORE.Input.Keyboard.charPressedQueueCount++] = (int)codepoint;
}

static void ArmOSKeyboardModifiers(void *data, struct wl_keyboard *keyboard,
    uint32_t serial, uint32_t depressed, uint32_t latched,
    uint32_t locked, uint32_t group)
{
    (void)data; (void)keyboard; (void)serial;
    if (platform.xkbState)
        (void)xkb_state_update_mask(platform.xkbState, depressed, latched,
            locked, 0u, 0u, group);
}

static void ArmOSKeyboardRepeat(void *data, struct wl_keyboard *keyboard,
    int32_t rate, int32_t delay)
{ (void)data; (void)keyboard; (void)rate; (void)delay; }

static const struct wl_keyboard_listener keyboardListener = {
    ArmOSKeyboardKeymap, ArmOSKeyboardEnter, ArmOSKeyboardLeave,
    ArmOSKeyboardKey, ArmOSKeyboardModifiers, ArmOSKeyboardRepeat
};

static void ArmOSSeatCapabilities(void *data, struct wl_seat *seat,
    uint32_t capabilities)
{
    (void)data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !platform.pointer)
    {
        platform.pointer = wl_seat_get_pointer(seat);
        if (platform.pointer)
            wl_pointer_add_listener(platform.pointer, &pointerListener, NULL);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !platform.keyboard)
    {
        platform.keyboard = wl_seat_get_keyboard(seat);
        if (platform.keyboard)
            wl_keyboard_add_listener(platform.keyboard, &keyboardListener, NULL);
    }
}

static void ArmOSSeatName(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }

static const struct wl_seat_listener seatListener = {
    ArmOSSeatCapabilities, ArmOSSeatName
};

bool WindowShouldClose(void)
{
    return !CORE.Window.ready || CORE.Window.shouldClose;
}

void ToggleFullscreen(void)
{
    if (!platform.toplevel) return;
    if (platform.fullscreen) xdg_toplevel_unset_fullscreen(platform.toplevel);
    else xdg_toplevel_set_fullscreen(platform.toplevel, NULL);
    platform.fullscreen = !platform.fullscreen;
}

void ToggleBorderlessWindowed(void) { ToggleFullscreen(); }
void MaximizeWindow(void)
{ if (platform.toplevel) xdg_toplevel_set_maximized(platform.toplevel); }
void MinimizeWindow(void)
{ if (platform.toplevel) xdg_toplevel_set_minimized(platform.toplevel); }
void RestoreWindow(void)
{ if (platform.toplevel) { xdg_toplevel_unset_maximized(platform.toplevel); xdg_toplevel_unset_fullscreen(platform.toplevel); } }
void SetWindowState(unsigned int flags)
{
    CORE.Window.flags |= flags;
    if (flags & FLAG_FULLSCREEN_MODE) ToggleFullscreen();
    if (flags & FLAG_WINDOW_MAXIMIZED) MaximizeWindow();
    if (flags & FLAG_WINDOW_MINIMIZED) MinimizeWindow();
}
void ClearWindowState(unsigned int flags)
{
    CORE.Window.flags &= ~flags;
    if (flags & (FLAG_FULLSCREEN_MODE | FLAG_WINDOW_MAXIMIZED)) RestoreWindow();
}
void SetWindowIcon(Image image) { (void)image; }
void SetWindowIcons(Image *images, int count) { (void)images; (void)count; }
void SetWindowTitle(const char *title)
{
    CORE.Window.title = title;
    if (platform.toplevel) xdg_toplevel_set_title(platform.toplevel, title);
}
void SetWindowPosition(int x, int y) { (void)x; (void)y; }
void SetWindowMonitor(int monitor) { (void)monitor; }
void SetWindowMinSize(int width, int height)
{
    CORE.Window.screenMin = (Size){ (unsigned int)width, (unsigned int)height };
    if (platform.toplevel) xdg_toplevel_set_min_size(platform.toplevel, width, height);
}
void SetWindowMaxSize(int width, int height)
{
    CORE.Window.screenMax = (Size){ (unsigned int)width, (unsigned int)height };
    if (platform.toplevel) xdg_toplevel_set_max_size(platform.toplevel, width, height);
}
void SetWindowSize(int width, int height)
{
    platform.pendingWidth = width;
    platform.pendingHeight = height;
    if (platform.xdgSurface) xdg_surface_set_window_geometry(platform.xdgSurface, 0, 0, width, height);
}
void SetWindowOpacity(float opacity) { (void)opacity; }
void SetWindowFocused(void) { }
void *GetWindowHandle(void) { return platform.surface; }
int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { (void)monitor; return (Vector2){ 0, 0 }; }
int GetMonitorWidth(int monitor) { (void)monitor; return (int)CORE.Window.display.width; }
int GetMonitorHeight(int monitor) { (void)monitor; return (int)CORE.Window.display.height; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "ArmOS Wayland output"; }
Vector2 GetWindowPosition(void) { return (Vector2){ 0, 0 }; }
Vector2 GetWindowScaleDPI(void) { return (Vector2){ 1.0f, 1.0f }; }
void SetClipboardText(const char *text) { (void)text; }
const char *GetClipboardText(void) { return ""; }
Image GetClipboardImage(void) { return (Image){ 0 }; }
void ShowCursor(void) { CORE.Input.Mouse.cursorHidden = false; }
void HideCursor(void) { CORE.Input.Mouse.cursorHidden = true; }
void EnableCursor(void) { CORE.Input.Mouse.cursorLocked = false; ShowCursor(); }
void DisableCursor(void) { CORE.Input.Mouse.cursorLocked = true; HideCursor(); }
void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
}
void SetMouseCursor(int cursor) { CORE.Input.Mouse.cursor = cursor; }
const char *GetKeyName(int key) { (void)key; return ""; }
int SetGamepadMappings(const char *mappings) { (void)mappings; return 0; }
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{ (void)gamepad; (void)leftMotor; (void)rightMotor; (void)duration; }
void OpenURL(const char *url) { (void)url; }

void SwapScreenBuffer(void)
{
    if (!eglSwapBuffers(platform.eglDisplay, platform.eglSurface))
        CORE.Window.shouldClose = true;
}

double GetTime(void)
{
    struct timespec ts = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((double)((uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec - CORE.Time.base))*1e-9;
}

void PollInputEvents(void)
{
    struct pollfd descriptor = { 0 };
    unsigned int dispatched = 0u;
    int pending;

#if SUPPORT_GESTURES_SYSTEM
    UpdateGestures();
#endif

    CORE.Window.resizedLastFrame = false;
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)
    {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++)
        CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
    CORE.Input.Mouse.currentWheelMove = (Vector2){ 0.0f, 0.0f };

    if (!platform.display) return;
    pending = armos_wl_display_dispatch_pending_bounded(
        platform.display, ARMOS_WAYLAND_EVENTS_PER_FRAME);
    if (pending < 0)
    {
        CORE.Window.shouldClose = true;
        return;
    }
    dispatched = (unsigned int)pending;
    if (dispatched >= ARMOS_WAYLAND_EVENTS_PER_FRAME)
        return;
    wl_display_flush(platform.display);
    descriptor.fd = wl_display_get_fd(platform.display);
    descriptor.events = POLLIN;
    /*
     * Keep input latency low without allowing a continuously readable
     * Wayland socket to starve the render loop.  Pointer motion can arrive
     * faster than frames are produced; draining until EAGAIN made an
     * animated Raylib client stop rendering for as long as the mouse moved.
     */
    while (dispatched < ARMOS_WAYLAND_EVENTS_PER_FRAME)
    {
        int count;

        descriptor.revents = 0;
        if (poll(&descriptor, 1, 0) <= 0)
            break;
        count = armos_wl_display_dispatch_bounded(
            platform.display, ARMOS_WAYLAND_EVENTS_PER_FRAME - dispatched);
        if (count < 0)
        {
            CORE.Window.shouldClose = true;
            break;
        }
        dispatched += (unsigned int)count;
    }
}

int InitPlatform(void)
{
    static const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    static const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    EGLConfig configs[256];
    EGLint configCount = 0;
    EGLint bestScore = 1000;

    platform.pendingWidth = (int)CORE.Window.screen.width;
    platform.pendingHeight = (int)CORE.Window.screen.height;
    platform.display = wl_display_connect(NULL);
    if (!platform.display) return -1;
    platform.registry = wl_display_get_registry(platform.display);
    wl_registry_add_listener(platform.registry, &registryListener, NULL);
    if (wl_display_roundtrip(platform.display) < 0 || !platform.compositor || !platform.wmBase) return -1;
    xdg_wm_base_add_listener(platform.wmBase, &wmBaseListener, NULL);
    if (platform.seat) wl_seat_add_listener(platform.seat, &seatListener, NULL);
    platform.xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    platform.surface = wl_compositor_create_surface(platform.compositor);
    platform.xdgSurface = xdg_wm_base_get_xdg_surface(platform.wmBase, platform.surface);
    platform.toplevel = xdg_surface_get_toplevel(platform.xdgSurface);
    xdg_surface_add_listener(platform.xdgSurface, &surfaceListener, NULL);
    xdg_toplevel_add_listener(platform.toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_title(platform.toplevel, CORE.Window.title? CORE.Window.title : "raylib");
    xdg_toplevel_set_app_id(platform.toplevel, "org.armos.raylib");
    wl_surface_commit(platform.surface);
    while (!platform.configured && !CORE.Window.shouldClose)
        if (wl_display_dispatch(platform.display) < 0) return -1;

    platform.nativeWindow = wl_egl_window_create(platform.surface,
        (int)CORE.Window.screen.width, (int)CORE.Window.screen.height);
    platform.eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
        platform.display, NULL);
    if (platform.eglDisplay == EGL_NO_DISPLAY ||
        !eglInitialize(platform.eglDisplay, NULL, NULL) ||
        !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(platform.eglDisplay, configAttributes,
            configs, (EGLint)(sizeof(configs)/sizeof(configs[0])),
            &configCount) || configCount < 1) return -1;

    /* EGL size attributes are minima.  Taking the first matching config can
     * silently select a packed Z24S8 buffer even though the client never
     * requested stencil.  Rank the returned configs explicitly: GLES2's
     * portable 16-bit depth format first, then an unpacked 24-bit depth
     * format, and finally a depthless 2D surface.  Packed depth/stencil is
     * deliberately excluded because it is not portable across VirGL hosts. */
    platform.eglConfig = NULL;
    for (EGLint index = 0; index < configCount; index++)
    {
        EGLint depth = 0;
        EGLint stencil = 0;
        EGLint score;

        if (!eglGetConfigAttrib(platform.eglDisplay, configs[index],
                EGL_DEPTH_SIZE, &depth) ||
            !eglGetConfigAttrib(platform.eglDisplay, configs[index],
                EGL_STENCIL_SIZE, &stencil)) continue;
        if (depth == 16 && stencil == 0) score = 0;
        else if (depth == 24 && stencil == 0) score = 1;
        else if (depth == 0 && stencil == 0) score = 2;
        else if (depth == 0 && stencil == 8) score = 3;
        else continue;
        if (score < bestScore)
        {
            platform.eglConfig = configs[index];
            bestScore = score;
        }
    }
    if (!platform.eglConfig) return -1;
    platform.eglContext = eglCreateContext(platform.eglDisplay,
        platform.eglConfig, EGL_NO_CONTEXT, contextAttributes);
    platform.eglSurface = eglCreateWindowSurface(platform.eglDisplay,
        platform.eglConfig, (EGLNativeWindowType)platform.nativeWindow, NULL);
    if (platform.eglContext == EGL_NO_CONTEXT || platform.eglSurface == EGL_NO_SURFACE ||
        !eglMakeCurrent(platform.eglDisplay, platform.eglSurface,
            platform.eglSurface, platform.eglContext)) return -1;

    CORE.Window.display = CORE.Window.screen;
    CORE.Window.render = CORE.Window.screen;
    CORE.Window.currentFbo = CORE.Window.screen;
    CORE.Window.ready = true;
    CORE.Input.Mouse.cursorOnScreen = true;
    rlLoadExtensions((void *)eglGetProcAddress);
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();
    TRACELOG(LOG_INFO, "PLATFORM: ArmOS Wayland/EGL initialized");
    return 0;
}

void ClosePlatform(void)
{
    CORE.Window.ready = false;
    if (platform.eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(platform.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (platform.eglSurface != EGL_NO_SURFACE) eglDestroySurface(platform.eglDisplay, platform.eglSurface);
        if (platform.eglContext != EGL_NO_CONTEXT) eglDestroyContext(platform.eglDisplay, platform.eglContext);
        eglTerminate(platform.eglDisplay);
    }
    if (platform.nativeWindow) wl_egl_window_destroy(platform.nativeWindow);
    if (platform.keyboard) wl_keyboard_destroy(platform.keyboard);
    if (platform.pointer) wl_pointer_destroy(platform.pointer);
    if (platform.toplevel) xdg_toplevel_destroy(platform.toplevel);
    if (platform.xdgSurface) xdg_surface_destroy(platform.xdgSurface);
    if (platform.surface) wl_surface_destroy(platform.surface);
    if (platform.seat) wl_seat_destroy(platform.seat);
    if (platform.wmBase) xdg_wm_base_destroy(platform.wmBase);
    if (platform.compositor) wl_compositor_destroy(platform.compositor);
    if (platform.registry) wl_registry_destroy(platform.registry);
    if (platform.display) wl_display_disconnect(platform.display);
    if (platform.xkbState) xkb_state_unref(platform.xkbState);
    if (platform.xkbKeymap) xkb_keymap_unref(platform.xkbKeymap);
    if (platform.xkbContext) xkb_context_unref(platform.xkbContext);
    platform = (PlatformData){ 0 };
}
