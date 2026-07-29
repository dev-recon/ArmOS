# Graphical application contract

ArmOS exposes two application models. Applications must not implement a third
window-system path or depend on a display-controller implementation.

## Terminal applications

```text
application
    |
    v
PTY and termios
    |
    v
Foot
    |
    v
common Wayland client contract
```

Terminal programs own terminal semantics only. They receive sizing through the
PTY contract and `SIGWINCH`; they never access Wayland, SHM, framebuffer or
input devices directly.

Foot is a regular Wayland client. Any compositor, scheduling, clipboard,
buffer-lifetime or input correction must benefit every Wayland client and must
not be implemented as a Foot-specific policy.

## Graphical applications

```text
application
    |
    v
public libarmui API
    |
    +-- Nuklear UI engine
    +-- common ArmUI Wayland backend
            |
            +-- xdg-shell window lifecycle
            +-- input and UTF-8
            +-- SHM buffer generations
            +-- damage and frame pacing
            +-- resize and window states
            +-- clipboard
```

Applications express UI and application state through `libarmui`. Nuklear is
private to the library. Applications must not include Nuklear structures or
duplicate Wayland connection, SHM pool, configure, frame callback or resize
logic.

Custom canvases such as the Utah teapot provide pixels to an ArmUI canvas.
They do not own the window-system lifecycle.

Desktop applications obtain system snapshots through `libarmos-services`.
They must not parse procfs, open framebuffer/input devices or embed privileged
kernel calls. State-changing operations will use explicit service requests
with authorization at that boundary.

The system bar is also an ArmUI client, but requests a dedicated panel role
through `armos_shell_v1`. The compositor authenticates the session capability,
owns placement and reserves the work area. No application receives privileges
because of its executable name, title or application identifier.

## Wayland transaction

Every top-level window follows the same state machine:

1. create `wl_surface`, `xdg_surface` and `xdg_toplevel`;
2. commit the `wl_surface` without a buffer;
3. receive `xdg_toplevel.configure` followed by
   `xdg_surface.configure`;
4. acknowledge the configure serial;
5. create or select a buffer matching the accepted size;
6. attach, damage and commit the buffer;
7. reuse a buffer only after `wl_buffer.release`;
8. schedule continuous drawing only through frame callbacks.

Pointer messages are accumulated through the matching `wl_pointer.frame`
boundary and applied as one input transaction. A discrete release may request
one additional stabilization frame when an immediate-mode widget changes
theme or enabled state while building the current command list. Static
applications otherwise return to sleep immediately.

Transient menus use `xdg_positioner` and `xdg_popup`. They remain
parent-relative, participate in the parent's stacking group and follow the
same configure/ack and buffer-release rules.

Configure state is transactional. A resize, maximize, fullscreen or restore
request becomes visible only when the client acknowledges its configure and
commits matching content. Minimized or fully hidden surfaces do not receive
continuous frame pacing.

## Kernel contract

The common kernel provides:

- event-driven `poll` and `select` waits without millisecond polling;
- PTY, sockets, timers and input as independent wakeup sources;
- SHM objects with a global byte/page budget;
- descriptor passing and process isolation;
- architecture-neutral framebuffer and future display/GPU UAPIs.

The current readiness generation closes scan-to-sleep races but still wakes all
descriptor waiters. Per-object wait queues are the next scalability step: a
source will then wake only tasks registered on that source. Platform drivers
publish events but never contain Wayland, terminal-emulator or UI policy.

## Compositor contract

`armos-wlcomp` owns:

- Wayland object and transaction validation;
- scene graph, subsurface atomicity and stacking;
- focus, pointer routing, keyboard routing and clipboard;
- server-side decorations and window policy;
- damage tracking, occlusion, composition and presentation;
- non-blocking per-client input/output queues.

Framebuffer, VirtIO-GPU, VC4/V3D and HDMI details remain backend
implementations behind common compositor and kernel interfaces.

## Validation applications

`armui-demo` validates widgets, input, UTF-8, scrolling, resize and window
states. `teapot-demo` validates a continuously updated ArmUI canvas and frame
pacing. Foot validates the terminal contract.

These programs are conformance and stress clients. The current ArmUI,
compositor, transient-surface and privileged-panel contracts have been
validated together. A behavior needed by more than one client belongs in the
common contract, not in the client.
