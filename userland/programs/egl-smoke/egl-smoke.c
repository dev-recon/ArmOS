/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/egl-smoke/egl-smoke.c
 * Layer: Userland / graphics diagnostics
 *
 * Responsibilities:
 * - Validate the public EGL and OpenGL ES 2 contracts on ArmOS.
 * - Render through Mesa/Gallium into an off-screen pbuffer.
 * - Verify shader execution and readback without relying on a window system.
 *
 * Notes:
 * - The program contains no VirtIO, VirGL, VC4, V3D or platform-specific API.
 * - Driver selection belongs to Mesa's ArmOS pipe loader and DRM negotiation.
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SMOKE_WIDTH 64
#define SMOKE_HEIGHT 64

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
   "   gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
   "}\n";

static const GLfloat triangle_vertices[] = {
   -0.8f, -0.8f,
    0.8f, -0.8f,
    0.0f,  0.8f,
};

static void
report_egl_error(const char *operation)
{
   fprintf(stderr, "egl-smoke: %s failed: EGL error 0x%04x\n",
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
      fprintf(stderr, "egl-smoke: shader compilation failed: %.*s\n",
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
   GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
   GLuint program;
   GLint linked = GL_FALSE;

   if (!vertex || !fragment) {
      glDeleteShader(vertex);
      glDeleteShader(fragment);
      return 0;
   }

   program = glCreateProgram();
   if (program) {
      glAttachShader(program, vertex);
      glAttachShader(program, fragment);
      glBindAttribLocation(program, 0, "position");
      glLinkProgram(program);
      glGetProgramiv(program, GL_LINK_STATUS, &linked);
   }
   glDeleteShader(vertex);
   glDeleteShader(fragment);

   if (!linked) {
      char log[512];
      GLsizei length = 0;

      if (program)
         glGetProgramInfoLog(program, sizeof(log), &length, log);
      fprintf(stderr, "egl-smoke: program link failed: %.*s\n",
              (int)length, log);
      glDeleteProgram(program);
      return 0;
   }
   return program;
}

int
main(void)
{
   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, SMOKE_WIDTH,
      EGL_HEIGHT, SMOKE_HEIGHT,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   EGLDisplay display = EGL_NO_DISPLAY;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLContext context = EGL_NO_CONTEXT;
   EGLConfig config;
   EGLint config_count = 0;
   EGLint major = 0;
   EGLint minor = 0;
   GLuint program = 0;
   uint8_t center[4] = {0, 0, 0, 0};
   int result = EXIT_FAILURE;

   display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (display == EGL_NO_DISPLAY) {
      report_egl_error("eglGetDisplay");
      goto out;
   }
   if (!eglInitialize(display, &major, &minor)) {
      report_egl_error("eglInitialize");
      goto out;
   }
   if (!eglChooseConfig(display, config_attributes, &config, 1,
                        &config_count) || config_count != 1) {
      report_egl_error("eglChooseConfig");
      goto out;
   }
   if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      report_egl_error("eglBindAPI");
      goto out;
   }

   surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
   if (surface == EGL_NO_SURFACE) {
      report_egl_error("eglCreatePbufferSurface");
      goto out;
   }
   context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                              context_attributes);
   if (context == EGL_NO_CONTEXT) {
      report_egl_error("eglCreateContext");
      goto out;
   }
   if (!eglMakeCurrent(display, surface, surface, context)) {
      report_egl_error("eglMakeCurrent");
      goto out;
   }

   program = create_program();
   if (!program)
      goto out;

   glViewport(0, 0, SMOKE_WIDTH, SMOKE_HEIGHT);
   glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(program);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, triangle_vertices);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   glReadPixels(SMOKE_WIDTH / 2, SMOKE_HEIGHT / 2,
                1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);

   if (glGetError() != GL_NO_ERROR) {
      fprintf(stderr, "egl-smoke: OpenGL ES operation failed\n");
      goto out;
   }
   if (center[0] < 240 || center[1] > 15 ||
       center[2] > 15 || center[3] < 240) {
      fprintf(stderr,
              "egl-smoke: unexpected center pixel %02x%02x%02x%02x\n",
              center[0], center[1], center[2], center[3]);
      goto out;
   }

   printf("egl-smoke: EGL %d.%d, vendor=%s\n",
          major, minor, glGetString(GL_VENDOR));
   printf("egl-smoke: renderer=%s, GLES=%s\n",
          glGetString(GL_RENDERER), glGetString(GL_VERSION));
   printf("egl-smoke: off-screen GLES2 triangle verified\n");
   result = EXIT_SUCCESS;

out:
   if (program)
      glDeleteProgram(program);
   if (display != EGL_NO_DISPLAY)
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT)
      eglDestroyContext(display, context);
   if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE)
      eglDestroySurface(display, surface);
   if (display != EGL_NO_DISPLAY)
      eglTerminate(display);
   return result;
}
