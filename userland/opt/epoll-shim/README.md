# ArmOS epoll compatibility

This directory contains an original ArmOS userland implementation of the
level-triggered `epoll` API required by event-driven ports such as Foot.

The implementation translates an epoll watch set to the architecture-neutral
`ppoll(2)` interface. It supports `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`,
`EPOLL_CTL_DEL`, `epoll_wait(2)` and `epoll_pwait(2)`, including preservation
of the application-owned event data. The bundle includes `epoll-shim.pc`, so
Meson can resolve Foot's optional `epoll-shim` dependency directly.

Current limits:

- eight epoll instances per process;
- 256 watched descriptors per instance;
- level-triggered `EPOLLIN`, `EPOLLPRI` and `EPOLLOUT`;
- `EPOLLERR` and `EPOLLHUP` readiness reporting;
- no edge-triggered, one-shot, exclusive-wakeup or half-close mode yet.

The compatibility descriptor is backed by an ArmOS `eventfd`. Closing it
releases the kernel descriptor, but the small userland registry slot remains
reserved until process exit. This is sufficient for Foot's single long-lived
event loop and can later be replaced by a native kernel event queue without
changing application sources.
