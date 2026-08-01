/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/egl-wayland-smoke/egl-wayland-smoke.c
 * Layer: Userland / graphics diagnostics
 *
 * Responsibilities:
 * - Validate the public Wayland, libwayland-egl, EGL and GLES2 contracts.
 * - Exercise configure-driven resizing and frame-callback pacing.
 * - Present GPU-rendered buffers through the compositor swapchain.
 *
 * Notes:
 * - The client contains no VirtIO, VirGL, VC4, V3D or platform-specific API.
 * - Driver and buffer-sharing selection belong to EGL, DRM and the compositor.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl-core.h>
#include <xdg-shell-client-protocol.h>

#define INITIAL_WIDTH 640
#define INITIAL_HEIGHT 480
#define TEST_FRAMES 180u

struct application {
   struct wl_display *wayland;
   struct wl_registry *registry;
   struct wl_compositor *compositor;
   struct xdg_wm_base *wm_base;
   struct wl_surface *surface;
   struct xdg_surface *xdg_surface;
   struct xdg_toplevel *toplevel;
   struct wl_callback *frame_callback;
   struct wl_egl_window *native_window;
   EGLDisplay egl_display;
   EGLSurface egl_surface;
   EGLContext egl_context;
   EGLConfig egl_config;
   GLuint program;
   int width;
   int height;
   int pending_width;
   int pending_height;
   bool configured;
   bool frame_pending;
   bool running;
};

static const char vertex_shader_source[] =
   "attribute vec2 position;\n"
   "void main(void)\n"
   "{\n"
   "   gl_Position = vec4(position, 0.0, 1.0);\n"
   "}\n";

static const char fragment_shader_source[] =
   "precision mediump float;\n"
   "void main(void)\n"
   "{\n"
   "   gl_FragColor = vec4(0.15, 0.72, 1.0, 1.0);\n"
   "}\n";

static void
report_egl_error(const char *operation)
{
   fprintf(stderr, "egl-wayland-smoke: %s failed: EGL error 0x%04x\n",
           operation, (unsigned)eglGetError());
}

static GLuint
compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   GLint compiled = GL_FALSE;

   if (!shader)
      return 0;
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   if (!compiled) {
      char log[512];
      GLsizei length = 0;

      glGetShaderInfoLog(shader, sizeof(log), &length, log);
      fprintf(stderr, "egl-wayland-smoke: shader compilation failed: %.*s\n",
              (int)length, log);
      glDeleteShader(shader);
      return 0;
   }
   return shader;
}

static GLuint
create_program(void)
{
   GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
   GLuint fragment = compile_shader(GL_FRAGMENT_SHADER,
                                    fragment_shader_source);
   GLuint program = 0;
   GLint linked = GL_FALSE;

   if (!vertex || !fragment)
      goto out;
   program = glCreateProgram();
   if (!program)
      goto out;
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   if (!linked) {
      char log[512];
      GLsizei length = 0;

      glGetProgramInfoLog(program, sizeof(log), &length, log);
      fprintf(stderr, "egl-wayland-smoke: program link failed: %.*s\n",
              (int)length, log);
      glDeleteProgram(program);
      program = 0;
   }

out:
   if (vertex)
      glDeleteShader(vertex);
   if (fragment)
      glDeleteShader(fragment);
   return program;
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
   struct application *application = data;

   if (!application->compositor &&
       strcmp(interface, "wl_compositor") == 0) {
      application->compositor = wl_registry_bind(
         registry, name, &wl_compositor_interface,
         version < 4u ? version : 4u);
   } else if (!application->wm_base &&
              strcmp(interface, "xdg_wm_base") == 0) {
      application->wm_base = wl_registry_bind(
         registry, name, &xdg_wm_base_interface, 1u);
   }
}

static void
registry_global_remove(void *data, struct wl_registry *registry,
                       uint32_t name)
{
   (void)data;
   (void)registry;
   (void)name;
}

static const struct wl_registry_listener registry_listener = {
   .global = registry_global,
   .global_remove = registry_global_remove,
};

static void
wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
   (void)data;
   xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
   .ping = wm_base_ping,
};

static void
surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
   struct application *application = data;

   xdg_surface_ack_configure(surface, serial);
   if (application->pending_width > 0 && application->pending_height > 0) {
      application->width = application->pending_width;
      application->height = application->pending_height;
      if (application->native_window)
         wl_egl_window_resize(application->native_window,
                              application->width, application->height, 0, 0);
      xdg_surface_set_window_geometry(surface, 0, 0,
                                      application->width,
                                      application->height);
   }
   application->configured = true;
}

static const struct xdg_surface_listener surface_listener = {
   .configure = surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                   int32_t width, int32_t height, struct wl_array *states)
{
   struct application *application = data;

   (void)toplevel;
   (void)states;
   if (width > 0 && height > 0) {
      application->pending_width = width;
      application->pending_height = height;
   }
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
   struct application *application = data;

   (void)toplevel;
   application->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
   .configure = toplevel_configure,
   .close = toplevel_close,
};

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
   struct application *application = data;

   (void)time;
   if (application->frame_callback == callback)
      application->frame_callback = NULL;
   application->frame_pending = false;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   .done = frame_done,
};

static bool
initialize_wayland(struct application *application)
{
   application->wayland = wl_display_connect(NULL);
   if (!application->wayland)
      return false;
   application->registry = wl_display_get_registry(application->wayland);
   if (!application->registry ||
       wl_registry_add_listener(application->registry,
                                &registry_listener, application) < 0 ||
       wl_display_roundtrip(application->wayland) < 0 ||
       !application->compositor || !application->wm_base ||
       xdg_wm_base_add_listener(application->wm_base,
                                &wm_base_listener, application) < 0)
      return false;

   application->surface =
      wl_compositor_create_surface(application->compositor);
   application->xdg_surface = application->surface ?
      xdg_wm_base_get_xdg_surface(application->wm_base,
                                  application->surface) : NULL;
   if (!application->xdg_surface ||
       xdg_surface_add_listener(application->xdg_surface,
                                &surface_listener, application) < 0)
      return false;
   application->toplevel =
      xdg_surface_get_toplevel(application->xdg_surface);
   if (!application->toplevel ||
       xdg_toplevel_add_listener(application->toplevel,
                                 &toplevel_listener, application) < 0)
      return false;

   xdg_toplevel_set_title(application->toplevel,
                          "ArmOS EGL/GLES2 swapchain");
   xdg_toplevel_set_app_id(application->toplevel,
                           "org.armos.egl-wayland-smoke");
   xdg_toplevel_set_min_size(application->toplevel, 160, 120);
   xdg_surface_set_window_geometry(application->xdg_surface, 0, 0,
                                   application->width, application->height);
   wl_surface_commit(application->surface);
   while (!application->configured) {
      if (wl_display_dispatch(application->wayland) < 0)
         return false;
   }
   return true;
}

static bool
initialize_egl(struct application *application)
{
   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   EGLint count = 0;

   application->native_window = wl_egl_window_create(
      application->surface, application->width, application->height);
   if (!application->native_window)
      return false;
   application->egl_display = eglGetPlatformDisplay(
      EGL_PLATFORM_WAYLAND_KHR, application->wayland, NULL);
   if (application->egl_display == EGL_NO_DISPLAY) {
      report_egl_error("eglGetPlatformDisplay");
      return false;
   }
   if (!eglInitialize(application->egl_display, NULL, NULL) ||
       !eglChooseConfig(application->egl_display, config_attributes,
                        &application->egl_config, 1, &count) || count != 1 ||
       !eglBindAPI(EGL_OPENGL_ES_API)) {
      report_egl_error("EGL display initialization");
      return false;
   }
   application->egl_surface = eglCreateWindowSurface(
      application->egl_display, application->egl_config,
      (EGLNativeWindowType)(uintptr_t)application->native_window, NULL);
   application->egl_context = eglCreateContext(
      application->egl_display, application->egl_config, EGL_NO_CONTEXT,
      context_attributes);
   if (application->egl_surface == EGL_NO_SURFACE ||
       application->egl_context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(application->egl_display, application->egl_surface,
                       application->egl_surface, application->egl_context)) {
      report_egl_error("EGL window surface");
      return false;
   }
   application->program = create_program();
   return application->program != 0;
}

static bool
render_frame(struct application *application, unsigned frame)
{
   GLfloat vertices[6];
   GLfloat offset;
   uint8_t center[4] = {0, 0, 0, 0};

   offset = frame == 0u ? 0.0f :
      ((GLfloat)(frame % 120u) / 120.0f - 0.5f) * 0.6f;
   vertices[0] = -0.55f + offset;
   vertices[1] = -0.55f;
   vertices[2] = 0.55f + offset;
   vertices[3] = -0.55f;
   vertices[4] = offset;
   vertices[5] = 0.55f;

   glViewport(0, 0, application->width, application->height);
   glClearColor(0.035f, 0.055f, 0.09f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(application->program);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   if (frame == 0u) {
      glReadPixels(application->width / 2, application->height / 2,
                   1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
      if (center[1] < 160u || center[2] < 220u)
         return false;
   }
   if (glGetError() != GL_NO_ERROR)
      return false;

   application->frame_callback = wl_surface_frame(application->surface);
   if (!application->frame_callback ||
       wl_callback_add_listener(application->frame_callback,
                                &frame_listener, application) < 0)
      return false;
   application->frame_pending = true;
   if (!eglSwapBuffers(application->egl_display,
                       application->egl_surface)) {
      report_egl_error("eglSwapBuffers");
      return false;
   }
   while (application->frame_pending && application->running) {
      if (wl_display_dispatch(application->wayland) < 0)
         return false;
   }
   return true;
}

static void
destroy_application(struct application *application)
{
   if (application->frame_callback)
      wl_callback_destroy(application->frame_callback);
   if (application->program)
      glDeleteProgram(application->program);
   if (application->egl_display != EGL_NO_DISPLAY)
      eglMakeCurrent(application->egl_display, EGL_NO_SURFACE,
                     EGL_NO_SURFACE, EGL_NO_CONTEXT);
   if (application->egl_display != EGL_NO_DISPLAY &&
       application->egl_context != EGL_NO_CONTEXT)
      eglDestroyContext(application->egl_display, application->egl_context);
   if (application->egl_display != EGL_NO_DISPLAY &&
       application->egl_surface != EGL_NO_SURFACE)
      eglDestroySurface(application->egl_display, application->egl_surface);
   if (application->egl_display != EGL_NO_DISPLAY)
      eglTerminate(application->egl_display);
   if (application->native_window)
      wl_egl_window_destroy(application->native_window);
   if (application->toplevel)
      xdg_toplevel_destroy(application->toplevel);
   if (application->xdg_surface)
      xdg_surface_destroy(application->xdg_surface);
   if (application->surface)
      wl_surface_destroy(application->surface);
   if (application->wm_base)
      xdg_wm_base_destroy(application->wm_base);
   if (application->compositor)
      wl_compositor_destroy(application->compositor);
   if (application->registry)
      wl_registry_destroy(application->registry);
   if (application->wayland)
      wl_display_disconnect(application->wayland);
}

int
main(void)
{
   struct application application;
   unsigned frame;
   int result = EXIT_FAILURE;

   memset(&application, 0, sizeof(application));
   application.width = INITIAL_WIDTH;
   application.height = INITIAL_HEIGHT;
   application.pending_width = INITIAL_WIDTH;
   application.pending_height = INITIAL_HEIGHT;
   application.running = true;
   application.egl_display = EGL_NO_DISPLAY;
   application.egl_surface = EGL_NO_SURFACE;
   application.egl_context = EGL_NO_CONTEXT;

   if (!initialize_wayland(&application)) {
      fprintf(stderr, "egl-wayland-smoke: Wayland initialization failed\n");
      goto out;
   }
   if (!initialize_egl(&application))
      goto out;

   for (frame = 0u; frame < TEST_FRAMES && application.running; ++frame) {
      if (!render_frame(&application, frame)) {
         fprintf(stderr, "egl-wayland-smoke: frame %u failed\n", frame);
         goto out;
      }
   }
   printf("egl-wayland-smoke: %u GPU frames presented with explicit reuse\n",
          frame);
   result = EXIT_SUCCESS;

out:
   destroy_application(&application);
   return result;
}
