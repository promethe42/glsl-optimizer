/*
 * Mesa 3-D graphics library
 * Version:  7.9
 *
 * Copyright (C) 2010 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "util/u_debug.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_dl.h"
#include "egldriver.h"
#include "egllog.h"

#include "state_tracker/st_api.h"
#include "state_tracker/drm_driver.h"
#include "common/egl_g3d_loader.h"

struct egl_g3d_loader egl_g3d_loader;

static struct st_module {
   boolean initialized;
   char *name;
   struct util_dl_library *lib;
   struct st_api *stapi;
} st_modules[ST_API_COUNT];

static struct pipe_module {
   boolean initialized;
   char *name;
   struct util_dl_library *lib;
   const struct drm_driver_descriptor *drmdd;
   struct pipe_screen *(*swrast_create_screen)(struct sw_winsys *);
} pipe_modules[16];

static char *
loader_strdup(const char *s)
{
   size_t len = (s) ? strlen(s) : 0;
   char *t = MALLOC(len + 1);
   if (t) {
      memcpy(t, s, len);
      t[len] = '\0';
   }
   return t;
}

static EGLBoolean
dlopen_st_module_cb(const char *dir, size_t len, void *callback_data)
{
   struct st_module *stmod =
      (struct st_module *) callback_data;
   char path[1024];
   int ret;

   if (len) {
      ret = util_snprintf(path, sizeof(path),
            "%.*s/" ST_PREFIX "%s" UTIL_DL_EXT, len, dir, stmod->name);
   }
   else {
      ret = util_snprintf(path, sizeof(path),
            ST_PREFIX "%s" UTIL_DL_EXT, stmod->name);
   }

   if (ret > 0 && ret < sizeof(path)) {
      stmod->lib = util_dl_open(path);
      if (stmod->lib)
         _eglLog(_EGL_DEBUG, "loaded %s", path);
   }

   return !(stmod->lib);
}

static boolean
load_st_module(struct st_module *stmod,
                       const char *name, const char *procname)
{
   struct st_api *(*create_api)(void);

   stmod->name = loader_strdup(name);
   if (stmod->name)
      _eglSearchPathForEach(dlopen_st_module_cb, (void *) stmod);
   else
      stmod->lib = util_dl_open(NULL);

   if (stmod->lib) {
      create_api = (struct st_api *(*)(void))
         util_dl_get_proc_address(stmod->lib, procname);
      if (create_api)
         stmod->stapi = create_api();

      if (!stmod->stapi) {
         util_dl_close(stmod->lib);
         stmod->lib = NULL;
      }
   }

   if (!stmod->stapi) {
      FREE(stmod->name);
      stmod->name = NULL;
   }

   return (stmod->stapi != NULL);
}

static EGLBoolean
dlopen_pipe_module_cb(const char *dir, size_t len, void *callback_data)
{
   struct pipe_module *pmod = (struct pipe_module *) callback_data;
   char path[1024];
   int ret;

   if (len) {
      ret = util_snprintf(path, sizeof(path),
            "%.*s/" PIPE_PREFIX "%s" UTIL_DL_EXT, len, dir, pmod->name);
   }
   else {
      ret = util_snprintf(path, sizeof(path),
            PIPE_PREFIX "%s" UTIL_DL_EXT, pmod->name);
   }
   if (ret > 0 && ret < sizeof(path)) {
      pmod->lib = util_dl_open(path);
      if (pmod->lib)
         _eglLog(_EGL_DEBUG, "loaded %s", path);
   }

   return !(pmod->lib);
}

static boolean
load_pipe_module(struct pipe_module *pmod, const char *name)
{
   pmod->name = loader_strdup(name);
   if (!pmod->name)
      return FALSE;

   _eglSearchPathForEach(dlopen_pipe_module_cb, (void *) pmod);
   if (pmod->lib) {
      pmod->drmdd = (const struct drm_driver_descriptor *)
         util_dl_get_proc_address(pmod->lib, "driver_descriptor");
      if (pmod->drmdd) {
         if (pmod->drmdd->driver_name) {
            /* driver name mismatch */
            if (strcmp(pmod->drmdd->driver_name, pmod->name) != 0)
               pmod->drmdd = NULL;
         }
         else {
            /* swrast */
            pmod->swrast_create_screen =
               (struct pipe_screen *(*)(struct sw_winsys *))
               util_dl_get_proc_address(pmod->lib, "swrast_create_screen");
            if (!pmod->swrast_create_screen)
               pmod->drmdd = NULL;
         }
      }

      if (!pmod->drmdd) {
         util_dl_close(pmod->lib);
         pmod->lib = NULL;
      }
   }

   if (!pmod->drmdd)
      pmod->name = NULL;

   return (pmod->drmdd != NULL);
}

static struct st_api *
get_st_api(enum st_api_type api)
{
   struct st_module *stmod = &st_modules[api];
   const char *names[8], *symbol;
   int i, count = 0;

   if (stmod->initialized)
      return stmod->stapi;

   switch (api) {
   case ST_API_OPENGL:
      symbol = ST_CREATE_OPENGL_SYMBOL;
      names[count++] = "GL";
      break;
   case ST_API_OPENGL_ES1:
      symbol = ST_CREATE_OPENGL_ES1_SYMBOL;
      names[count++] = "GLESv1_CM";
      names[count++] = "GL";
      break;
   case ST_API_OPENGL_ES2:
      symbol = ST_CREATE_OPENGL_ES2_SYMBOL;
      names[count++] = "GLESv2";
      names[count++] = "GL";
      break;
   case ST_API_OPENVG:
      symbol = ST_CREATE_OPENVG_SYMBOL;
      names[count++] = "OpenVG";
      break;
   default:
      symbol = NULL;
      assert(!"Unknown API Type\n");
      break;
   }

   /* NULL means the process itself */
   names[count++] = NULL;

   for (i = 0; i < count; i++) {
      if (load_st_module(stmod, names[i], symbol))
         break;
   }

   if (!stmod->stapi) {
      EGLint level = (egl_g3d_loader.api_mask & (1 << api)) ?
         _EGL_WARNING : _EGL_DEBUG;
      _eglLog(level, "unable to load " ST_PREFIX "%s" UTIL_DL_EXT, names[0]);
   }

   stmod->initialized = TRUE;

   return stmod->stapi;
}

static struct st_api *
guess_gl_api(void)
{
   struct st_api *stapi;
   int gl_apis[] = {
      ST_API_OPENGL,
      ST_API_OPENGL_ES1,
      ST_API_OPENGL_ES2,
      -1
   };
   int i, api = -1;

   /* determine the api from the loaded libraries */
   for (i = 0; gl_apis[i] != -1; i++) {
      if (st_modules[gl_apis[i]].stapi) {
         api = gl_apis[i];
         break;
      }
   }
   /* determine the api from the linked libraries */
   if (api == -1) {
      struct util_dl_library *self = util_dl_open(NULL);

      if (self) {
         if (util_dl_get_proc_address(self, "glColor4d"))
            api = ST_API_OPENGL;
         else if (util_dl_get_proc_address(self, "glColor4x"))
            api = ST_API_OPENGL_ES1;
         else if (util_dl_get_proc_address(self, "glShaderBinary"))
            api = ST_API_OPENGL_ES2;
         util_dl_close(self);
      }
   }

   stapi = (api != -1) ? get_st_api(api) : NULL;
   if (!stapi) {
      for (i = 0; gl_apis[i] != -1; i++) {
         api = gl_apis[i];
         stapi = get_st_api(api);
         if (stapi)
            break;
      }
   }

   return stapi;
}

static struct pipe_module *
get_pipe_module(const char *name)
{
   struct pipe_module *pmod = NULL;
   int i;

   if (!name)
      return NULL;

   for (i = 0; i < Elements(pipe_modules); i++) {
      if (!pipe_modules[i].initialized ||
          strcmp(pipe_modules[i].name, name) == 0) {
         pmod = &pipe_modules[i];
         break;
      }
   }
   if (!pmod)
      return NULL;

   if (!pmod->initialized) {
      load_pipe_module(pmod, name);
      pmod->initialized = TRUE;
   }

   return pmod;
}

static struct pipe_screen *
create_drm_screen(const char *name, int fd)
{
   struct pipe_module *pmod = get_pipe_module(name);
   return (pmod && pmod->drmdd->create_screen) ?
      pmod->drmdd->create_screen(fd) : NULL;
}

static struct pipe_screen *
create_sw_screen(struct sw_winsys *ws)
{
   struct pipe_module *pmod = get_pipe_module("swrast");
   return (pmod && pmod->swrast_create_screen) ?
      pmod->swrast_create_screen(ws) : NULL;
}

static const struct egl_g3d_loader *
loader_init(void)
{
   uint api_mask = 0x0;

   /* TODO detect at runtime? */
#if FEATURE_GL
   api_mask |= 1 << ST_API_OPENGL;
#endif
#if FEATURE_ES1
   api_mask |= 1 << ST_API_OPENGL_ES1;
#endif
#if FEATURE_ES2
   api_mask |= 1 << ST_API_OPENGL_ES2;
#endif
#if FEATURE_VG
   api_mask |= 1 << ST_API_OPENVG;
#endif

   egl_g3d_loader.api_mask = api_mask;
   egl_g3d_loader.get_st_api = get_st_api;
   egl_g3d_loader.guess_gl_api = guess_gl_api;
   egl_g3d_loader.create_drm_screen = create_drm_screen;
   egl_g3d_loader.create_sw_screen = create_sw_screen;

   return &egl_g3d_loader;
}

static void
loader_fini(void)
{
   int i;

   for (i = 0; i < ST_API_COUNT; i++) {
      struct st_module *stmod = &st_modules[i];

      if (stmod->stapi) {
         stmod->stapi->destroy(stmod->stapi);
         stmod->stapi = NULL;
      }
      if (stmod->lib) {
         util_dl_close(stmod->lib);
         stmod->lib = NULL;
      }
      if (stmod->name) {
         FREE(stmod->name);
         stmod->name = NULL;
      }
      stmod->initialized = FALSE;
   }
   for (i = 0; i < Elements(pipe_modules); i++) {
      struct pipe_module *pmod = &pipe_modules[i];

      if (!pmod->initialized)
         break;

      pmod->drmdd = NULL;
      pmod->swrast_create_screen = NULL;
      if (pmod->lib) {
         util_dl_close(pmod->lib);
         pmod->lib = NULL;
      }
      if (pmod->name) {
         FREE(pmod->name);
         pmod->name = NULL;
      }
      pmod->initialized = FALSE;
   }
}

static void
egl_g3d_unload(_EGLDriver *drv)
{
   egl_g3d_destroy_driver(drv);
   loader_fini();
}

_EGLDriver *
_eglMain(const char *args)
{
   const struct egl_g3d_loader *loader;
   _EGLDriver *drv;

   loader = loader_init();
   drv = egl_g3d_create_driver(loader);
   if (!drv) {
      loader_fini();
      return NULL;
   }

   drv->Name = "Gallium";
   drv->Unload = egl_g3d_unload;

   return drv;
}