/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_math.h"

#include "sp_screen.h"
#include "sp_buffer.h"


static void *
softpipe_buffer_map(struct pipe_screen *screen,
                    struct pipe_buffer *buf,
                    unsigned flags)
{
   struct softpipe_buffer *softpipe_buf = softpipe_buffer(buf);
   return softpipe_buf->data;
}


static void
softpipe_buffer_unmap(struct pipe_screen *screen,
                      struct pipe_buffer *buf)
{
}


static void
softpipe_buffer_destroy(struct pipe_buffer *buf)
{
   struct softpipe_buffer *sbuf = softpipe_buffer(buf);

   if (!sbuf->userBuffer)
      align_free(sbuf->data);
      
   FREE(sbuf);
}


static struct pipe_buffer *
softpipe_buffer_create(struct pipe_screen *screen,
                       unsigned alignment,
                       unsigned usage,
                       unsigned size)
{
   struct softpipe_buffer *buffer = CALLOC_STRUCT(softpipe_buffer);

   pipe_reference_init(&buffer->base.reference, 1);
   buffer->base.screen = screen;
   buffer->base.alignment = MAX2(alignment, 16);
   buffer->base.usage = usage;
   buffer->base.size = size;

   buffer->data = align_malloc(size, alignment);

   return &buffer->base;
}


/**
 * Create buffer which wraps user-space data.
 */
static struct pipe_buffer *
softpipe_user_buffer_create(struct pipe_screen *screen,
                            void *ptr,
                            unsigned bytes)
{
   struct softpipe_buffer *buffer;

   buffer = CALLOC_STRUCT(softpipe_buffer);
   if(!buffer)
      return NULL;

   pipe_reference_init(&buffer->base.reference, 1);
   buffer->base.screen = screen;
   buffer->base.size = bytes;
   buffer->userBuffer = TRUE;
   buffer->data = ptr;

   return &buffer->base;
}


void
softpipe_init_screen_buffer_funcs(struct pipe_screen *screen)
{
   screen->buffer_create = softpipe_buffer_create;
   screen->user_buffer_create = softpipe_user_buffer_create;
   screen->buffer_map = softpipe_buffer_map;
   screen->buffer_unmap = softpipe_buffer_unmap;
   screen->buffer_destroy = softpipe_buffer_destroy;
}