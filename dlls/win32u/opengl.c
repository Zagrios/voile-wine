/*
 * Copyright 2012 Alexandre Julliard
 * Copyright 2025 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "win32u_private.h"
#include "ntuser_private.h"

#include "wine/opengl_driver.h"

#include "dibdrv/dibdrv.h"
#include "../wineobelisk.drv/wsd_protocol.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

struct wgl_pbuffer
{
    struct opengl_drawable *drawable;

    HDC hdc;
    GLsizei width;
    GLsizei height;
    GLenum texture_format;
    GLenum texture_target;
    GLint mipmap_level;
    GLenum cube_face;
};

static const struct opengl_driver_funcs nulldrv_funcs, *driver_funcs = &nulldrv_funcs;
static struct list devices_egl = LIST_INIT( devices_egl );
static struct egl_platform display_egl;
static struct opengl_funcs display_funcs;

static struct wgl_pixel_format *pixel_formats;
static UINT formats_count, onscreen_count;
static char wgl_extensions[4096];

static BOOL has_extension( const char *list, const char *ext )
{
    size_t len = strlen( ext );
    const char *cur = list;

    while (cur && (cur = strstr( cur, ext )))
    {
        if ((!cur[len] || cur[len] == ' ') && (cur == list || cur[-1] == ' ')) return TRUE;
        cur = strchr( cur, ' ' );
    }

    return FALSE;
}

static void dump_extensions( const char *list )
{
    const char *start, *end, *ptr;

    for (start = end = ptr = list; ptr; ptr = strchr( ptr + 1, ' ' ))
    {
        if (ptr - start <= 128) end = ptr;
        else
        {
            TRACE( "%.*s\n", (int)(end - start), start );
            start = end + 1;
        }
    }

    TRACE( "%s\n", start );
}

static void register_extension( char *list, size_t size, const char *name )
{
    if (!has_extension( list, name ))
    {
        size_t len = strlen( list );
        assert( size - len >= strlen( name ) + 1 );
        if (*list) strcat( list + len, " " );
        strcat( list + len, name );
    }
}

void *opengl_drawable_create( UINT size, const struct opengl_drawable_funcs *funcs, int format, struct client_surface *client )
{
    struct opengl_drawable *drawable;

    if (!(drawable = calloc( 1, size ))) return NULL;
    drawable->funcs = funcs;
    drawable->ref = 1;

    drawable->format = format;
    drawable->interval = INT_MIN;
    drawable->doublebuffer = !!(pixel_formats[format - 1].pfd.dwFlags & PFD_DOUBLEBUFFER);
    drawable->stereo = !!(pixel_formats[format - 1].pfd.dwFlags & PFD_STEREO);
    if ((drawable->client = client)) client_surface_add_ref( client );
    for (UINT i = 0; i < ARRAY_SIZE(drawable->buffer_map); i++) drawable->buffer_map[i] = GL_FRONT_LEFT + i;

    TRACE( "created %s\n", debugstr_opengl_drawable( drawable ) );
    return drawable;
}

void opengl_drawable_add_ref( struct opengl_drawable *drawable )
{
    ULONG ref = InterlockedIncrement( &drawable->ref );
    TRACE( "%s increasing refcount to %u\n", debugstr_opengl_drawable( drawable ), ref );
}

void opengl_drawable_release( struct opengl_drawable *drawable )
{
    ULONG ref = InterlockedDecrement( &drawable->ref );
    TRACE( "%s decreasing refcount to %u\n", debugstr_opengl_drawable( drawable ), ref );

    if (!ref)
    {
        const struct opengl_funcs *funcs = &display_funcs;
        const struct egl_platform *egl = &display_egl;

        drawable->funcs->destroy( drawable );
        if (drawable->surface) funcs->p_eglDestroySurface( egl->display, drawable->surface );
        if (drawable->client) client_surface_release( drawable->client );
        free( drawable );
    }
}

static void opengl_drawable_set_context( struct opengl_drawable *drawable, struct wgl_context *context )
{
    if (!drawable->funcs->set_context) return;
    drawable->funcs->set_context( drawable, context ? context->driver_private : NULL );
}

static void opengl_drawable_flush( struct opengl_drawable *drawable, int interval, UINT flags )
{
    if (!is_client_surface_window( drawable->client, 0 )) return;

    if (InterlockedCompareExchange( &drawable->client->updated, 0, 1 )) flags |= GL_FLUSH_UPDATED;
    if (interval != drawable->interval)
    {
        drawable->interval = interval;
        flags = GL_FLUSH_INTERVAL;
    }

    if (flags) drawable->funcs->flush( drawable, flags );
}

static BOOL opengl_drawable_swap( struct opengl_drawable *drawable )
{
    if (!is_client_surface_window( drawable->client, 0 )) return FALSE;
    client_surface_update( drawable->client );
    return drawable->funcs->swap( drawable );
}

#ifdef SONAME_LIBEGL

struct gl_present;

struct framebuffer_surface
{
    struct opengl_drawable base;
    void                  *bits;            /* pixels of the last swapped frame, for the window */
    SIZE_T                 bits_size;
    struct gl_present     *present;         /* frames shown without a readback (gl_present_swap) */
};

static void gl_present_destroy( struct gl_present *present );
static void gl_present_forget_context( struct gl_present *present );

static GLenum color_format_from_pfd( const struct wgl_pixel_format *desc )
{
    TRACE( "format type %u bits %u/%u/%u/%u\n", desc->pixel_type, desc->pfd.cRedBits,
           desc->pfd.cGreenBits, desc->pfd.cBlueBits, desc->pfd.cAlphaBits );

    if (desc->pixel_type == WGL_TYPE_RGBA_FLOAT_ARB)
    {
        if (desc->pfd.cAlphaBits == 32) return GL_RGBA32F;
        if (desc->pfd.cAlphaBits == 16) return GL_RGBA16F;
        if (desc->pfd.cBlueBits == 32) return GL_RGB32F;
        if (desc->pfd.cBlueBits == 16) return GL_RGB16F;
        if (desc->pfd.cGreenBits == 32) return GL_RG32F;
        if (desc->pfd.cGreenBits == 16) return GL_RG16F;
        if (desc->pfd.cRedBits == 32) return GL_R32F;
        if (desc->pfd.cRedBits == 16) return GL_R16F;
    }
    else
    {
        if (desc->pfd.cBlueBits == 10 && desc->pfd.cGreenBits == 10 &&
            desc->pfd.cRedBits == 10 && desc->pfd.cAlphaBits == 2)
            return GL_RGB10_A2;
        if (desc->pfd.cAlphaBits == 32) return GL_RGBA32UI;
        if (desc->pfd.cAlphaBits == 16) return GL_RGBA16;
        if (desc->pfd.cAlphaBits == 8) return GL_RGBA8;
        if (desc->pfd.cAlphaBits == 4) return GL_RGBA4;
        if (desc->pfd.cBlueBits == 32) return GL_RGB32UI;
        if (desc->pfd.cBlueBits == 16) return GL_RGB16;
        if (desc->pfd.cBlueBits == 8) return GL_RGB8;
        if (desc->pfd.cBlueBits == 4) return GL_RGB4;
        if (desc->pfd.cGreenBits == 32) return GL_RG32UI;
        if (desc->pfd.cGreenBits == 16) return GL_RG16;
        if (desc->pfd.cGreenBits == 8) return GL_RG8;
        if (desc->pfd.cRedBits == 32) return GL_R32UI;
        if (desc->pfd.cRedBits == 16) return GL_R16;
        if (desc->pfd.cRedBits == 8) return GL_R8;
    }

    FIXME( "Unsupported format type %u bits %u/%u/%u/%u\n", desc->pixel_type, desc->pfd.cRedBits,
           desc->pfd.cGreenBits, desc->pfd.cBlueBits, desc->pfd.cAlphaBits );
    return 0;
}

static GLenum depth_format_from_pfd( const struct wgl_pixel_format *desc )
{
    TRACE( "format bits %u/%u\n", desc->pfd.cStencilBits, desc->pfd.cDepthBits );

    if (desc->pfd.cStencilBits)
    {
        if (desc->pfd.cDepthBits == 32) return GL_DEPTH32F_STENCIL8;
        if (desc->pfd.cDepthBits == 24) return GL_DEPTH24_STENCIL8;
    }
    else
    {
        if (desc->pfd.cDepthBits == 32) return GL_DEPTH_COMPONENT32;
        if (desc->pfd.cDepthBits == 24) return GL_DEPTH_COMPONENT24;
        if (desc->pfd.cDepthBits == 16) return GL_DEPTH_COMPONENT16;
    }

    FIXME( "Unsupported format bits %u/%u\n", desc->pfd.cStencilBits, desc->pfd.cDepthBits );
    return 0;
}

static GLuint create_framebuffer( struct opengl_drawable *drawable, const struct wgl_pixel_format *desc )
{
    const struct opengl_funcs *funcs = &display_funcs;
    GLuint count = 1, fbo, name;

    if (drawable->doublebuffer) count *= 2;
    if (drawable->stereo) count *= 2;

    funcs->p_glCreateFramebuffers( 1, &fbo );

    for (GLuint i = 0; i < count; i++)
    {
        funcs->p_glCreateRenderbuffers( 1, &name );
        funcs->p_glNamedFramebufferRenderbuffer( fbo, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, name );
        TRACE( "drawable %p/%u created color buffer %#x/%u\n", drawable, fbo, GL_COLOR_ATTACHMENT0 + i, name );
    }

    if (desc->pfd.cDepthBits)
    {
        funcs->p_glCreateRenderbuffers( 1, &name );
        funcs->p_glNamedFramebufferRenderbuffer( fbo, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, name );
        if (desc->pfd.cStencilBits) funcs->p_glNamedFramebufferRenderbuffer( fbo, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, name );
        TRACE( "drawable %p/%u created depth buffer %u\n", drawable, fbo, name );
    }

    /* GL's defaults for a window: draw into and read from the back buffer if there is one, else the front one */
    funcs->p_glNamedFramebufferDrawBuffer( fbo, drawable->doublebuffer ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0 );
    funcs->p_glNamedFramebufferReadBuffer( fbo, drawable->doublebuffer ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0 );
    TRACE( "drawable %p created framebuffer %u\n", drawable, fbo );

    return fbo;
}

static void resize_framebuffer( struct opengl_drawable *drawable, const struct wgl_pixel_format *desc, GLuint fbo,
                                int width, int height )
{
    const struct opengl_funcs *funcs = &display_funcs;
    GLuint count = 1, name;
    GLenum ret;

    if (drawable->doublebuffer) count *= 2;
    if (drawable->stereo) count *= 2;

    for (GLuint i = 0; i < count; i++)
    {
        funcs->p_glGetNamedFramebufferAttachmentParameteriv( fbo, GL_COLOR_ATTACHMENT0 + i, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, (GLint *)&name );
        funcs->p_glNamedRenderbufferStorageMultisample( name, desc->samples, color_format_from_pfd( desc ), width, height );
        TRACE( "drawable %p/%u resized color buffer %#x/%u to %d,%d\n", drawable, fbo, GL_COLOR_ATTACHMENT0 + i, name, width, height );
    }

    if (desc->pfd.cDepthBits)
    {
        funcs->p_glGetNamedFramebufferAttachmentParameteriv( fbo, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, (GLint *)&name );
        funcs->p_glNamedRenderbufferStorageMultisample( name, desc->samples, depth_format_from_pfd( desc ), width, height );
        TRACE( "drawable %p/%u resized depth buffer %u to %d,%d\n", drawable, fbo, name, width, height );
    }

    ret = funcs->p_glCheckNamedFramebufferStatus( fbo, GL_FRAMEBUFFER );
    if (ret != GL_FRAMEBUFFER_COMPLETE) WARN( "glCheckNamedFramebufferStatus returned %#x\n", ret );
    TRACE( "drawable %p/%u resized buffers to %d,%d\n", drawable, fbo, width, height );
}

static void destroy_framebuffer( struct opengl_drawable *drawable, const struct wgl_pixel_format *desc, GLuint fbo )
{
    const struct opengl_funcs *funcs = &display_funcs;
    GLuint count = 1, name;

    if (drawable->doublebuffer) count *= 2;
    if (drawable->stereo) count *= 2;

    for (GLuint i = 0; i < count; i++)
    {
        funcs->p_glGetNamedFramebufferAttachmentParameteriv( fbo, GL_COLOR_ATTACHMENT0 + i, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, (GLint *)&name );
        funcs->p_glDeleteRenderbuffers( 1, &name );
        TRACE( "drawable %p/%u destroyed color buffer %#x/%u\n", drawable, fbo, GL_COLOR_ATTACHMENT0 + i, name );
    }

    if (desc->pfd.cDepthBits)
    {
        funcs->p_glGetNamedFramebufferAttachmentParameteriv( fbo, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, (GLint *)&name );
        funcs->p_glDeleteRenderbuffers( 1, &name );
        TRACE( "drawable %p/%u destroyed depth buffer %u\n", drawable, fbo, name );
    }

    funcs->p_glDeleteFramebuffers( 1, &fbo );
    TRACE( "drawable %p destroyed framebuffer %u\n", drawable, fbo );
}

static void framebuffer_surface_destroy( struct opengl_drawable *drawable )
{
    struct framebuffer_surface *surface = CONTAINING_RECORD( drawable, struct framebuffer_surface, base );
    TRACE( "%s\n", debugstr_opengl_drawable( drawable ) );
    free( surface->bits );
    gl_present_destroy( surface->present );
}

static void framebuffer_surface_resize( struct opengl_drawable *drawable )
{
    struct wgl_pixel_format draw_desc = pixel_formats[drawable->format - 1], read_desc = draw_desc;
    RECT rect;

    NtUserGetClientRect( drawable->client->hwnd, &rect, NtUserGetDpiForWindow( drawable->client->hwnd ) );
    if (!rect.right) rect.right = 1;
    if (!rect.bottom) rect.bottom = 1;

    read_desc.samples = read_desc.sample_buffers = 0;

    TRACE( "Resizing drawable %p/%u to %ux%u\n", drawable, drawable->read_fbo, rect.right, rect.bottom );
    resize_framebuffer( drawable, &read_desc, drawable->read_fbo, rect.right, rect.bottom );

    if (drawable->draw_fbo != drawable->read_fbo)
    {
        TRACE( "Resizing drawable %p/%u to %ux%u\n", drawable, drawable->draw_fbo, rect.right, rect.bottom );
        resize_framebuffer( drawable, &draw_desc, drawable->draw_fbo, rect.right, rect.bottom );
    }
}

static void framebuffer_surface_set_context( struct opengl_drawable *drawable, void *private )
{
    struct wgl_pixel_format draw_desc = pixel_formats[drawable->format - 1], read_desc = draw_desc;
    read_desc.samples = read_desc.sample_buffers = 0;

    TRACE( "%s, private %p\n", debugstr_opengl_drawable( drawable ), private );

    if (!private)
    {
        /* the context that lets go of the drawable is still current */
        gl_present_forget_context( CONTAINING_RECORD( drawable, struct framebuffer_surface, base )->present );
        if (drawable->draw_fbo != drawable->read_fbo)
        {
            destroy_framebuffer( drawable, &draw_desc, drawable->draw_fbo );
            drawable->draw_fbo = 0;
        }
        destroy_framebuffer( drawable, &read_desc, drawable->read_fbo );
        drawable->read_fbo = 0;
    }
    else
    {
        drawable->read_fbo = create_framebuffer( drawable, &read_desc );
        if (!drawable->read_fbo) ERR( "Failed to create read framebuffer object\n" );

        if (!draw_desc.sample_buffers) drawable->draw_fbo = drawable->read_fbo;
        else drawable->draw_fbo = create_framebuffer( drawable, &draw_desc );
        if (!drawable->draw_fbo) ERR( "Failed to create draw framebuffer object\n" );

        framebuffer_surface_resize( drawable );
    }
}

static void framebuffer_surface_flush( struct opengl_drawable *drawable, UINT flags )
{
    TRACE( "%s, flags %#x\n", debugstr_opengl_drawable( drawable ), flags );
    if (flags & GL_FLUSH_UPDATED && drawable->read_fbo) framebuffer_surface_resize( drawable );
}


/* Obelisk's Linux engine: frames shown without a readback. The render server (Neptune's vtest server with
 * --use-cgl) and the window server (wsd) share six memory buffers per window with this process: the buffers are
 * attached to wsd through the display driver's connection and to the render server through a connection of our
 * own. On swap, the back buffer is blitted into a texture, a debug marker names the buffer to fill
 * ("obelisk-present token=... buf=... serial=..."), and the swap returns. The render server copies the texture into
 * that buffer on the GPU and confirms it in a trailer after the pixels; a present thread of ours watches the trailers
 * and has wsd show each buffer on the window's overlay plane once it is filled. Without the display driver or the
 * render server's support (a frame not confirmed), swaps read the frame back as before.
 *
 * The present thread is a plain thread, not a Win32 one: it only reads memory and writes to wsd's socket, and uses no
 * Wine debug output.
 *
 * A frame lock (the FrameLock value of HKCU\Software\Wine\OpenGL or AppDefaults\app.exe\OpenGL, or WINE_GL_FRAME_LOCK:
 * n refreshes a frame) has wsd show the window's frames at the refresh of its screen divided by n, on its ticks, and
 * makes each swap wait until the frame before it has been shown, so the game draws exactly one frame per interval:
 * motion is even when the game cannot keep up with every refresh (75-90 FPS on a 120 or 144 Hz screen shows frames for
 * one or two refreshes in turn). */

#define GLP_BUFFERS        6  /* wsd holds two (shown, next); one is copied, one drawn into */
#define GLP_STRIDE_ALIGN   256
#define GLP_SHM_ALIGN      0x4000
#define GLP_TRAILER_SIZE   16
#define GLP_TRAILER_MAGIC  0x50424c4f
#define GLP_ID_TAG         0xc0000000  /* bit 31: the driver passes the buffer's events on; bit 30: ours */
#define GLP_SLOTS          64
#define VTEST_HDR_SIZE     2
#define VCMD_CREATE_RENDERER 8
#define VCMD_PROTOCOL_VERSION 11
#define VCMD_OBELISK_PRESENT_ATTACH 39
#define VCMD_OBELISK_PRESENT_DETACH 40
#define GLP_JOBS           16
#define GLP_WAIT_NS        500000000  /* how long a frame may take to be confirmed */

struct gl_present_buffer
{
    void  *map;
    SIZE_T size;
    UINT   stride;
    UINT   wsd_id;
    int    slot;
};

struct gl_present
{
    UINT64 token;
    UINT   window_id;
    int    width, height;
    struct gl_present_buffer buf[GLP_BUFFERS];
    UINT   next;
    UINT64 serial;
    GLuint texture, fbo, src_fbo;
    int    texture_width, texture_height;
    void  *context;      /* the GL context the objects above belong to */
    UINT   presents;
    UINT64 queued, done;   /* frames given to the present thread; frames wsd has released or that were not shown */
    UINT   lock_window;    /* the window wsd was given the frame lock for */
};

/* a frame for the present thread: the buffer it is shown in once its trailer has the serial */
struct glp_job
{
    const UINT8 *trailer;
    UINT64 serial;
    UINT   window_id, buffer_id;
    int    slot;
    int    width, height;
};

/* glp_lock: the process's state, the buffer slots (the release listener runs on the driver's event thread) and the
 * present thread's queue; glp_server_lock: the render server connection. A drawable's present state belongs to its
 * swapping thread. */
static pthread_mutex_t glp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t glp_server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t glp_job_cond = PTHREAD_COND_INITIALIZER;   /* a job was queued */
static pthread_cond_t glp_free_cond = PTHREAD_COND_INITIALIZER;  /* a buffer was released or a job finished */
static struct glp_job glp_jobs[GLP_JOBS];
static UINT glp_job_head, glp_job_count;   /* the job at glp_job_head stays queued until it is done */
static UINT64 glp_failed_serial;           /* the frame the present thread saw fail, for the swapping thread's log */
static BOOL glp_tried, glp_disabled, glp_failure_logged;
static int glp_vtest_fd = -1;
static UINT glp_generation;
static struct { UINT id; BOOL busy; struct gl_present *present; } glp_slots[GLP_SLOTS];
static UINT glp_frame_lock;               /* refreshes a frame (0: no lock), read once per process */
static BOOL (*p_obelisk_wsd_send)( UINT type, const void *payload, UINT len, int fd );
static void (*p_obelisk_wsd_set_release_listener)( void (*listener)( UINT, UINT, UINT64 ) );
static void *(*p_obelisk_wsd_get_release_listener)(void);
static BOOL (*p_obelisk_wsd_client_rect)( UINT window_id, int *x, int *y, UINT *width, UINT *height );
static UINT64 (*p_obelisk_wsd_next_serial)(void);
static void (*glp_chained_listener)( UINT, UINT, UINT64 );

static void glp_release_listener( UINT buffer_id, UINT shown, UINT64 serial )
{
    int i;
    if ((buffer_id & GLP_ID_TAG) != GLP_ID_TAG)
    {
        if (glp_chained_listener) glp_chained_listener( buffer_id, shown, serial );
        return;
    }
    pthread_mutex_lock( &glp_lock );
    for (i = 0; i < GLP_SLOTS; i++)
    {
        if (glp_slots[i].id != buffer_id) continue;
        glp_slots[i].busy = FALSE;
        if (glp_slots[i].present) glp_slots[i].present->done++;
    }
    pthread_cond_broadcast( &glp_free_cond );
    pthread_mutex_unlock( &glp_lock );
}

static INT64 glp_elapsed_ns( const struct timespec *start )
{
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (INT64)(now.tv_sec - start->tv_sec) * 1000000000 + now.tv_nsec - start->tv_nsec;
}

enum glp_frame { GLP_FRAME_COPIED, GLP_FRAME_FAILED, GLP_FRAME_LATE };

/* waits for the render server to confirm the frame, or fail it */
static enum glp_frame glp_wait_trailer( const UINT8 *trailer, UINT64 serial )
{
    struct timespec start;
    UINT spins = 0;

    clock_gettime( CLOCK_MONOTONIC, &start );
    for (;;)
    {
        if (__atomic_load_n( (const UINT64 *)(trailer + 8), __ATOMIC_ACQUIRE ) == serial)
            return *(volatile const UINT32 *)trailer == GLP_TRAILER_MAGIC && !*(volatile const UINT32 *)(trailer + 4)
                   ? GLP_FRAME_COPIED : GLP_FRAME_FAILED;
        if (++spins < 64) continue;
        if (glp_elapsed_ns( &start ) > GLP_WAIT_NS) return GLP_FRAME_LATE;
        usleep( 50 );
    }
}

/* A frame the render server failed is not shown (a context's first frames can fail while its objects are new); a
 * run of them, or one it does not answer, sends the process back to readbacks. */
#define GLP_MAX_FAILED 8

static void *glp_present_thread( void *arg )
{
    UINT failed_run = 0;

    pthread_mutex_lock( &glp_lock );
    for (;;)
    {
        struct wsd_present_region region;
        struct glp_job job;
        enum glp_frame frame;
        BOOL shown = FALSE, failed = FALSE;
        int x, y;
        UINT w, h;

        while (!glp_job_count) pthread_cond_wait( &glp_job_cond, &glp_lock );
        job = glp_jobs[glp_job_head];
        pthread_mutex_unlock( &glp_lock );

        if (!glp_disabled)
        {
            frame = glp_wait_trailer( job.trailer, job.serial );
            if (frame == GLP_FRAME_FAILED) failed_run++;
            else if (frame == GLP_FRAME_COPIED) failed_run = 0;
            if (frame == GLP_FRAME_COPIED)
            {
                memset( &region, 0, sizeof(region) );
                region.present.window_id = job.window_id;
                region.present.buffer_id = job.buffer_id;
                region.present.serial = p_obelisk_wsd_next_serial();
                if (!p_obelisk_wsd_client_rect( job.window_id, &x, &y, &w, &h ))
                {
                    x = y = 0;
                    w = job.width;
                    h = job.height;
                }
                region.x = x;
                region.y = y;
                region.width = w;
                region.height = h;
                if (!(shown = p_obelisk_wsd_send( WSD_PRESENT_REGION, &region, sizeof(region), -1 ))) frame = GLP_FRAME_LATE;
            }
            if (frame == GLP_FRAME_LATE || failed_run >= GLP_MAX_FAILED)
            {
                /* the window's frames are read back from now on, so its overlay plane goes */
                memset( &region, 0, sizeof(region) );
                region.present.window_id = job.window_id;
                p_obelisk_wsd_send( WSD_PRESENT_REGION, &region, sizeof(region), -1 );
                failed = TRUE;
            }
        }

        pthread_mutex_lock( &glp_lock );
        if (!shown)  /* wsd releases the ones it was given */
        {
            glp_slots[job.slot].busy = FALSE;
            if (glp_slots[job.slot].present) glp_slots[job.slot].present->done++;
        }
        if (failed && !glp_disabled)
        {
            glp_disabled = TRUE;
            glp_failed_serial = job.serial;
        }
        glp_job_head = (glp_job_head + 1) % GLP_JOBS;
        glp_job_count--;
        pthread_cond_broadcast( &glp_free_cond );
    }
    return NULL;
}

/* waits for the present thread to finish the frames it has, before their buffers go */
static void glp_drain(void)
{
    struct timespec deadline;

    clock_gettime( CLOCK_REALTIME, &deadline );
    deadline.tv_sec += 1;
    pthread_mutex_lock( &glp_lock );
    while (glp_job_count && pthread_cond_timedwait( &glp_free_cond, &glp_lock, &deadline ) != ETIMEDOUT) ;
    pthread_mutex_unlock( &glp_lock );
}

static BOOL glp_write_all( int fd, const void *data, size_t size )
{
    const char *ptr = data;
    while (size)
    {
        ssize_t ret = write( fd, ptr, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static BOOL glp_read_all( int fd, void *data, size_t size )
{
    char *ptr = data;
    while (size)
    {
        ssize_t ret = read( fd, ptr, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static UINT glp_reg_uint( HKEY key, const char *name, UINT def )
{
    char buffer[256];
    KEY_VALUE_PARTIAL_INFORMATION *info = (void *)buffer;
    UINT value = 0, size = query_reg_ascii_value( key, name, info, sizeof(buffer) );
    const WCHAR *str = (const WCHAR *)info->Data;

    if (!size) return def;
    if (info->Type == REG_DWORD && size == sizeof(DWORD)) return *(const DWORD *)info->Data;
    if (info->Type != REG_SZ) return def;
    for (; size >= sizeof(WCHAR) && *str >= '0' && *str <= '9'; str++, size -= sizeof(WCHAR))
        value = value * 10 + *str - '0';
    return value;
}

/* FrameLock: 0 for none; 1 to 8, every frame stays for that many refreshes of the window's screen; 20 and above, the
 * highest rate in frames a second: wsd picks the steady rate for the screen, and a slower one if the game can't keep up */
#define GLP_LOCK_MAX_REFRESHES 8
#define GLP_LOCK_MIN_FPS 20
#define GLP_LOCK_MAX_FPS 240
static UINT glp_lock_value( UINT lock )
{
    if (lock <= GLP_LOCK_MAX_REFRESHES) return lock;
    if (lock < GLP_LOCK_MIN_FPS) return GLP_LOCK_MAX_REFRESHES;
    return min( lock, GLP_LOCK_MAX_FPS );
}

static UINT glp_read_frame_lock(void)
{
    static const WCHAR openglW[] = {'\\','O','p','e','n','G','L',0};
    const char *env = getenv( "WINE_GL_FRAME_LOCK" );
    const WCHAR *appname, *p;
    WCHAR appkey[MAX_PATH + ARRAY_SIZE(openglW)];
    HKEY key, apps;
    UINT lock = 0, len;

    if (env) return glp_lock_value( (UINT)atoi( env ) );
    /* @@ Wine registry key: HKCU\Software\Wine\OpenGL */
    if ((key = reg_open_hkcu_key( "Software\\Wine\\OpenGL" )))
    {
        lock = glp_reg_uint( key, "FrameLock", lock );
        NtClose( key );
    }
    /* @@ Wine registry key: HKCU\Software\Wine\AppDefaults\app.exe\OpenGL */
    appname = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    if (appname && (p = wcsrchr( appname, '/' ))) appname = p + 1;
    if (appname && (p = wcsrchr( appname, '\\' ))) appname = p + 1;
    if (appname && (len = lstrlenW( appname )) && len < MAX_PATH &&
        (apps = reg_open_hkcu_key( "Software\\Wine\\AppDefaults" )))
    {
        memcpy( appkey, appname, len * sizeof(WCHAR) );
        memcpy( appkey + len, openglW, sizeof(openglW) );
        if ((key = reg_open_key( apps, appkey, lstrlenW( appkey ) * sizeof(WCHAR) )))
        {
            lock = glp_reg_uint( key, "FrameLock", lock );
            NtClose( key );
        }
        NtClose( apps );
    }
    return glp_lock_value( lock );
}

/* the display driver's wsd connection and a connection of our own to the render server, once per process */
static BOOL glp_init(void)
{
    static const char name[] = "wine-gl-present";
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *path = getenv( "VTEST_SOCKET_NAME" );
    UINT32 hdr[VTEST_HDR_SIZE], version[VTEST_HDR_SIZE + 1];
    sigset_t signals, old_signals;
    pthread_t thread;
    BOOL started;
    void *driver;
    int fd;

    if (glp_tried) return !glp_disabled;
    glp_tried = TRUE;
    glp_disabled = TRUE;

    if (!(driver = dlopen( "wineobelisk.so", RTLD_NOW | RTLD_NOLOAD ))) return FALSE;
    p_obelisk_wsd_send = dlsym( driver, "obelisk_wsd_send" );
    p_obelisk_wsd_set_release_listener = dlsym( driver, "obelisk_wsd_set_release_listener" );
    p_obelisk_wsd_get_release_listener = dlsym( driver, "obelisk_wsd_get_release_listener" );
    p_obelisk_wsd_client_rect = dlsym( driver, "obelisk_wsd_client_rect" );
    p_obelisk_wsd_next_serial = dlsym( driver, "obelisk_wsd_next_serial" );
    if (!p_obelisk_wsd_send || !p_obelisk_wsd_set_release_listener || !p_obelisk_wsd_get_release_listener ||
        !p_obelisk_wsd_client_rect || !p_obelisk_wsd_next_serial || !path || strlen( path ) >= sizeof(addr.sun_path))
        return FALSE;

    strcpy( addr.sun_path, path );
    if ((fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 )) < 0) return FALSE;
    hdr[0] = sizeof(name);
    hdr[1] = VCMD_CREATE_RENDERER;
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ) < 0 || !glp_write_all( fd, hdr, sizeof(hdr) ) ||
        !glp_write_all( fd, name, sizeof(name) ))
    {
        WARN( "no connection to the render server at %s: %s\n", path, strerror( errno ) );
        close( fd );
        return FALSE;
    }
    /* a server with several clients makes contexts only for protocol version 3 and later */
    version[0] = 1;
    version[1] = VCMD_PROTOCOL_VERSION;
    version[2] = 3;
    if (!glp_write_all( fd, version, sizeof(version) ) || !glp_read_all( fd, version, sizeof(version) ) ||
        version[1] != VCMD_PROTOCOL_VERSION || version[2] < 3)
    {
        WARN( "the render server's protocol is too old for presents\n" );
        close( fd );
        return FALSE;
    }
    /* the present thread takes no signals: Wine's handlers expect a Win32 thread */
    sigfillset( &signals );
    pthread_sigmask( SIG_SETMASK, &signals, &old_signals );
    started = !pthread_create( &thread, NULL, glp_present_thread, NULL );
    pthread_sigmask( SIG_SETMASK, &old_signals, NULL );
    if (!started)
    {
        WARN( "no present thread\n" );
        close( fd );
        return FALSE;
    }
    pthread_detach( thread );

    glp_vtest_fd = fd;
    if ((glp_frame_lock = glp_read_frame_lock()))
        TRACE( "frame lock value %u\n", glp_frame_lock );
    glp_chained_listener = p_obelisk_wsd_get_release_listener();
    p_obelisk_wsd_set_release_listener( glp_release_listener );
    glp_disabled = FALSE;
    return TRUE;
}

/* attaches a buffer to the render server's present chain; the server answers 0 or a negative errno */
static BOOL glp_attach_to_server( UINT64 token, UINT index, int width, int height, UINT stride, SIZE_T size, int fd )
{
    UINT32 cmd[VTEST_HDR_SIZE + 9], reply[VTEST_HDR_SIZE + 1];
    char byte = 0;
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR( &msg );

    cmd[0] = 9;
    cmd[1] = VCMD_OBELISK_PRESENT_ATTACH;
    cmd[2] = (UINT32)token;
    cmd[3] = (UINT32)(token >> 32);
    cmd[4] = index;
    cmd[5] = width;
    cmd[6] = height;
    cmd[7] = stride;
    cmd[8] = WSD_FMT_BGRA8;
    cmd[9] = (UINT32)size;
    cmd[10] = (UINT32)((UINT64)size >> 32);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy( CMSG_DATA(cmsg), &fd, sizeof(int) );
    pthread_mutex_lock( &glp_server_lock );
    if (glp_vtest_fd < 0 || !glp_write_all( glp_vtest_fd, cmd, sizeof(cmd) ) || sendmsg( glp_vtest_fd, &msg, 0 ) != 1 ||
        !glp_read_all( glp_vtest_fd, reply, sizeof(reply) ) || reply[1] != VCMD_OBELISK_PRESENT_ATTACH)
    {
        ERR( "the render server connection failed: %s\n", strerror( errno ) );
        if (glp_vtest_fd >= 0) close( glp_vtest_fd );
        glp_vtest_fd = -1;
        pthread_mutex_unlock( &glp_server_lock );
        glp_disabled = TRUE;
        return FALSE;
    }
    pthread_mutex_unlock( &glp_server_lock );
    if (reply[2])
    {
        WARN( "the render server refused buffer %u: %d\n", index, (int)reply[2] );
        return FALSE;
    }
    return TRUE;
}

/* the end of the drawable's chain on the render server, whose buffers go there too */
static void glp_detach_from_server( UINT64 token )
{
    UINT32 cmd[VTEST_HDR_SIZE + 2], reply[VTEST_HDR_SIZE + 1];

    cmd[0] = 2;
    cmd[1] = VCMD_OBELISK_PRESENT_DETACH;
    cmd[2] = (UINT32)token;
    cmd[3] = (UINT32)(token >> 32);
    pthread_mutex_lock( &glp_server_lock );
    if (glp_vtest_fd >= 0 && (!glp_write_all( glp_vtest_fd, cmd, sizeof(cmd) ) ||
        !glp_read_all( glp_vtest_fd, reply, sizeof(reply) ) || reply[1] != VCMD_OBELISK_PRESENT_DETACH))
    {
        ERR( "the render server connection failed: %s\n", strerror( errno ) );
        close( glp_vtest_fd );
        glp_vtest_fd = -1;
        glp_disabled = TRUE;
    }
    pthread_mutex_unlock( &glp_server_lock );
}

static void glp_free_buffers( struct gl_present *present )
{
    UINT i;
    glp_drain();
    for (i = 0; i < GLP_BUFFERS; i++)
    {
        struct gl_present_buffer *buf = &present->buf[i];
        if (!buf->map) continue;
        if (p_obelisk_wsd_send)
        {
            struct wsd_buffer detach = { buf->wsd_id };
            p_obelisk_wsd_send( WSD_BUFFER_DETACH, &detach, sizeof(detach), -1 );
        }
        munmap( buf->map, buf->size );
        pthread_mutex_lock( &glp_lock );
        glp_slots[buf->slot].id = 0;
        glp_slots[buf->slot].busy = FALSE;
        glp_slots[buf->slot].present = NULL;
        pthread_mutex_unlock( &glp_lock );
        memset( buf, 0, sizeof(*buf) );
    }
}

static BOOL glp_alloc_buffers( struct gl_present *present, int width, int height )
{
    UINT stride = (width * 4 + GLP_STRIDE_ALIGN - 1) & ~(GLP_STRIDE_ALIGN - 1);
    SIZE_T size = ((SIZE_T)stride * height + GLP_TRAILER_SIZE + GLP_SHM_ALIGN - 1) & ~(SIZE_T)(GLP_SHM_ALIGN - 1);
    UINT i, gen;

    glp_free_buffers( present );
    pthread_mutex_lock( &glp_lock );
    gen = ++glp_generation;
    pthread_mutex_unlock( &glp_lock );
    /* one chain per drawable: a new size replaces its buffers on the render server */
    if (!present->token) present->token = ((UINT64)getpid() << 32) | gen;

    for (i = 0; i < GLP_BUFFERS; i++)
    {
        struct gl_present_buffer *buf = &present->buf[i];
        struct wsd_buffer_attach attach;
        int fd, slot;
        void *map;

        pthread_mutex_lock( &glp_lock );
        for (slot = 0; slot < GLP_SLOTS && glp_slots[slot].id; slot++) ;
        if (slot < GLP_SLOTS)
        {
            glp_slots[slot].id = GLP_ID_TAG | (gen & 0xfffff) << 4 | i;
            glp_slots[slot].present = present;
        }
        pthread_mutex_unlock( &glp_lock );
        if (slot == GLP_SLOTS) goto failed;

        if ((fd = syscall( __NR_memfd_create, "wine-gl-present", 1 /* MFD_CLOEXEC */ )) < 0) goto failed_slot;
        if (ftruncate( fd, size ) < 0 ||
            (map = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
        {
            close( fd );
            goto failed_slot;
        }
        buf->map = map;
        buf->size = size;
        buf->stride = stride;
        buf->slot = slot;
        buf->wsd_id = glp_slots[slot].id;

        memset( &attach, 0, sizeof(attach) );
        attach.buffer_id = buf->wsd_id;
        attach.width = width;
        attach.height = height;
        attach.stride = stride;
        attach.format = WSD_FMT_BGRA8;
        attach.size = (UINT64)stride * height;
        if (!p_obelisk_wsd_send( WSD_BUFFER_ATTACH, &attach, sizeof(attach), fd ) ||
            !glp_attach_to_server( present->token, i, width, height, stride, size, fd ))
        {
            close( fd );
            goto failed;
        }
        close( fd );
        continue;

    failed_slot:
        pthread_mutex_lock( &glp_lock );
        glp_slots[slot].id = 0;
        glp_slots[slot].present = NULL;
        pthread_mutex_unlock( &glp_lock );
        goto failed;
    }
    present->width = width;
    present->height = height;
    present->next = 0;
    pthread_mutex_lock( &glp_lock );
    present->queued = present->done = 0; /* the old buffers' frames are gone with them */
    pthread_mutex_unlock( &glp_lock );
    return TRUE;

failed:
    glp_free_buffers( present );
    present->width = present->height = 0;
    return FALSE;
}

/* The present's GL objects belong to the context that draws: framebuffer objects are not shared between contexts,
 * and a new context may have the address of the one it replaced. They go when the context lets go of the drawable,
 * with that context current, and the next swap makes them again. */
static void gl_present_forget_context( struct gl_present *present )
{
    const struct opengl_funcs *funcs = &display_funcs;

    if (!present) return;
    if (present->texture) funcs->p_glDeleteTextures( 1, &present->texture );
    if (present->fbo) funcs->p_glDeleteFramebuffers( 1, &present->fbo );
    if (present->src_fbo) funcs->p_glDeleteFramebuffers( 1, &present->src_fbo );
    present->texture = present->fbo = present->src_fbo = 0;
    present->texture_width = present->texture_height = 0;
    present->context = NULL;
}

static void gl_present_destroy( struct gl_present *present )
{
    if (!present) return;
    glp_drain();
    if (present->window_id && p_obelisk_wsd_send)
    {
        struct wsd_present_region region = { .present.window_id = present->window_id };
        p_obelisk_wsd_send( WSD_PRESENT_REGION, &region, sizeof(region), -1 ); /* 0x0: no overlay plane */
    }
    glp_free_buffers( present );
    if (present->token) glp_detach_from_server( present->token );
    /* the GL objects go with their context */
    free( present );
}

/* returns TRUE when the frame was shown (or dropped on purpose), FALSE to read it back as before */
static BOOL gl_present_swap( struct framebuffer_surface *surface, HWND hwnd, int width, int height )
{
    static const WCHAR obelisk_window_propW[] = {'_','_','w','i','n','e','_','o','b','e','l','i','s','k','_','w','i','n','d','o','w',0};
    const struct opengl_funcs *funcs = &display_funcs;
    struct opengl_drawable *drawable = &surface->base;
    struct gl_present *present = surface->present;
    struct gl_present_buffer *buf = NULL;
    struct timespec deadline;
    struct glp_job job;
    GLboolean scissor, discard;
    GLint renderbuffer = 0;
    UINT window_id, i;
    char marker[96];
    int len;
    void *context = NtCurrentTeb()->glContext;

    pthread_mutex_lock( &glp_lock );
    if (!glp_init())
    {
        pthread_mutex_unlock( &glp_lock );
        return FALSE;
    }
    pthread_mutex_unlock( &glp_lock );

    if (!(window_id = HandleToULong( NtUserGetProp( NtUserGetAncestor( hwnd, GA_ROOT ), obelisk_window_propW ) )))
        return FALSE;
    if (!present)
    {
        if (!funcs->p_glDebugMessageInsert || !funcs->p_glBlitNamedFramebuffer || !funcs->p_glCreateTextures ||
            !funcs->p_glTextureStorage2D || !funcs->p_glNamedFramebufferTexture)
        {
            WARN( "frames read back: framebuffer functions missing\n" );
            pthread_mutex_lock( &glp_lock );
            glp_disabled = TRUE;
            pthread_mutex_unlock( &glp_lock );
            return FALSE;
        }
        if (!(present = surface->present = calloc( 1, sizeof(*present) ))) return FALSE;
    }
    present->window_id = window_id;
    if (glp_frame_lock && present->lock_window != window_id)
    {
        struct wsd_window_flag rate = { window_id, (glp_frame_lock > GLP_LOCK_MAX_REFRESHES ? WSD_FRAME_RATE_AUTO : WSD_FRAME_RATE_DIVISOR) | glp_frame_lock };
        if (p_obelisk_wsd_send( WSD_SET_FRAME_RATE, &rate, sizeof(rate), -1 )) present->lock_window = window_id;
    }

    if ((present->width != width || present->height != height) && !glp_alloc_buffers( present, width, height ))
        return FALSE;

    /* the texture the back buffer is blitted into, in the context that draws now (framebuffer objects are not
     * shared between contexts; those of a previous context stay with it) */
    if (present->context != context)
    {
        present->texture = present->fbo = present->src_fbo = 0;
        present->texture_width = present->texture_height = 0;
        present->context = context;
    }
    if (present->texture_width != width || present->texture_height != height)
    {
        if (present->texture) funcs->p_glDeleteTextures( 1, &present->texture );
        funcs->p_glCreateTextures( GL_TEXTURE_2D, 1, &present->texture );
        funcs->p_glTextureStorage2D( present->texture, 1, GL_RGBA8, width, height );
        if (!present->fbo) funcs->p_glCreateFramebuffers( 1, &present->fbo );
        funcs->p_glNamedFramebufferTexture( present->fbo, GL_COLOR_ATTACHMENT0, present->texture, 0 );
        if (!present->src_fbo) funcs->p_glCreateFramebuffers( 1, &present->src_fbo );
        present->texture_width = width;
        present->texture_height = height;
    }

    /* a free buffer: wsd keeps the one it shows and may hold the next until its display tick, and the present thread
     * one it waits for; up to 20 ms */
    clock_gettime( CLOCK_REALTIME, &deadline );
    if ((deadline.tv_nsec += 20000000) >= 1000000000)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock( &glp_lock );
    for (;;)
    {
        for (i = 0; i < GLP_BUFFERS && !buf; i++)
        {
            struct gl_present_buffer *candidate = &present->buf[(present->next + i) % GLP_BUFFERS];
            if (!glp_slots[candidate->slot].busy) buf = candidate;
        }
        if (buf || glp_disabled || pthread_cond_timedwait( &glp_free_cond, &glp_lock, &deadline ) == ETIMEDOUT) break;
    }
    if (buf) glp_slots[buf->slot].busy = TRUE;
    pthread_mutex_unlock( &glp_lock );
    if (!buf) return !glp_disabled; /* wsd holds them all (the window is hidden): the frame is dropped */

    funcs->p_glGetNamedFramebufferAttachmentParameteriv( drawable->read_fbo,
                                                         drawable->doublebuffer ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0,
                                                         GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &renderbuffer );
    funcs->p_glNamedFramebufferRenderbuffer( present->src_fbo, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer );
    funcs->p_glNamedFramebufferReadBuffer( present->src_fbo, GL_COLOR_ATTACHMENT0 );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    discard = funcs->p_glIsEnabled( GL_RASTERIZER_DISCARD );
    if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
    if (discard) funcs->p_glDisable( GL_RASTERIZER_DISCARD );
    funcs->p_glBlitNamedFramebuffer( present->src_fbo, present->fbo, 0, 0, width, height, 0, 0, width, height,
                                     GL_COLOR_BUFFER_BIT, GL_NEAREST );
    if (discard) funcs->p_glEnable( GL_RASTERIZER_DISCARD );
    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );

    i = buf - present->buf;
    len = snprintf( marker, sizeof(marker), "obelisk-present token=%llx buf=%u serial=%llu",
                    (unsigned long long)present->token, i, (unsigned long long)++present->serial );
    /* a debug marker reaches the driver in command order (Mesa: emit_string_marker), not only the debug log */
    funcs->p_glDebugMessageInsert( GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0, GL_DEBUG_SEVERITY_NOTIFICATION,
                                   len, marker );
    funcs->p_glFlush();

    /* the present thread shows the buffer once the render server has stamped the serial into its trailer */
    job.trailer = (const UINT8 *)buf->map + (SIZE_T)buf->stride * height;
    job.serial = present->serial;
    job.window_id = window_id;
    job.buffer_id = buf->wsd_id;
    job.slot = buf->slot;
    job.width = width;
    job.height = height;
    pthread_mutex_lock( &glp_lock );
    if (glp_frame_lock)
    {
        /* One frame waits for the screen at a time: this one goes to wsd once a display tick has taken the one before
         * it (wsd releases a buffer as soon as a tick has drawn it), up to 100 ms (a hidden window shows nothing). Sent
         * earlier, it would replace that one unshown, since wsd keeps only the newest, and the game would run ahead of
         * the lock. The render server copies this frame meanwhile. */
        clock_gettime( CLOCK_REALTIME, &deadline );
        if ((deadline.tv_nsec += 100000000) >= 1000000000)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        while (present->queued != present->done && !glp_disabled &&
               pthread_cond_timedwait( &glp_free_cond, &glp_lock, &deadline ) != ETIMEDOUT) ;
    }
    while (glp_job_count == GLP_JOBS) pthread_cond_wait( &glp_free_cond, &glp_lock );
    glp_jobs[(glp_job_head + glp_job_count) % GLP_JOBS] = job;
    glp_job_count++;
    present->queued++;
    pthread_cond_signal( &glp_job_cond );
    pthread_mutex_unlock( &glp_lock );

    present->next = (i + 1) % GLP_BUFFERS;
    if (!present->presents++) TRACE( "first frame sent without a readback, %dx%d\n", width, height );
    return TRUE;
}

/* A window drawable of a driver that gives windows no GL surface of their own (the surfaceless EGL path) is this
 * framebuffer object, which nothing shows. On swap, copy its back buffer into the window through GDI, as the
 * window surface's pixels, and flush the window surfaces so that the driver presents the frame now. */
static BOOL framebuffer_surface_swap( struct opengl_drawable *drawable )
{
    struct framebuffer_surface *surface = CONTAINING_RECORD( drawable, struct framebuffer_surface, base );
    const struct opengl_funcs *funcs = &display_funcs;
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    GLint read_binding = 0, read_buffer = GL_NONE, pack_buffer = 0;
    GLint pack_alignment = 4, pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
    HWND hwnd = drawable->client->hwnd;
    int width, height;
    RECT rect;
    void *bits;
    HDC hdc;

    TRACE( "%s\n", debugstr_opengl_drawable( drawable ) );

    /* opengl32 has already resolved a multisampled draw framebuffer into this one */
    if (!drawable->read_fbo) return TRUE;
    NtUserGetClientRect( hwnd, &rect, NtUserGetDpiForWindow( hwnd ) );
    if ((width = rect.right) <= 0 || (height = rect.bottom) <= 0) return TRUE;
    if (glp_failed_serial && !glp_failure_logged)
    {
        glp_failure_logged = TRUE;
        ERR( "the render server did not confirm frame %llu; frames are read back from now on\n",
             (unsigned long long)glp_failed_serial );
    }
    if (!glp_disabled && gl_present_swap( surface, hwnd, width, height )) return TRUE;
    if (surface->bits_size < (SIZE_T)width * height * 4)
    {
        free( surface->bits );
        surface->bits_size = 0;
        if (!(surface->bits = malloc( (SIZE_T)width * height * 4 ))) return FALSE;
        surface->bits_size = (SIZE_T)width * height * 4;
    }
    bits = surface->bits;

    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_binding );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, drawable->read_fbo );
    funcs->p_glGetIntegerv( GL_READ_BUFFER, &read_buffer );
    funcs->p_glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer );
    funcs->p_glGetIntegerv( GL_PACK_ALIGNMENT, &pack_alignment );
    funcs->p_glGetIntegerv( GL_PACK_ROW_LENGTH, &pack_row_length );
    funcs->p_glGetIntegerv( GL_PACK_SKIP_ROWS, &pack_skip_rows );
    funcs->p_glGetIntegerv( GL_PACK_SKIP_PIXELS, &pack_skip_pixels );
    if (pack_buffer) funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
    funcs->p_glPixelStorei( GL_PACK_ALIGNMENT, 4 );
    funcs->p_glPixelStorei( GL_PACK_ROW_LENGTH, 0 );
    funcs->p_glPixelStorei( GL_PACK_SKIP_ROWS, 0 );
    funcs->p_glPixelStorei( GL_PACK_SKIP_PIXELS, 0 );
    /* the back buffer, which the frame was drawn into (the front one if the format has none) */
    funcs->p_glReadBuffer( drawable->doublebuffer ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0 );
    funcs->p_glReadPixels( 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, bits );
    funcs->p_glReadBuffer( read_buffer );
    funcs->p_glPixelStorei( GL_PACK_ALIGNMENT, pack_alignment );
    funcs->p_glPixelStorei( GL_PACK_ROW_LENGTH, pack_row_length );
    funcs->p_glPixelStorei( GL_PACK_SKIP_ROWS, pack_skip_rows );
    funcs->p_glPixelStorei( GL_PACK_SKIP_PIXELS, pack_skip_pixels );
    if (pack_buffer) funcs->p_glBindBuffer( GL_PIXEL_PACK_BUFFER, pack_buffer );
    funcs->p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_binding );

    memset( info, 0, sizeof(info->bmiHeader) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = height; /* bottom-up, as GL returns the rows */
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    /* a cache DC: the window's own DC (CS_OWNDC) may carry a mapping mode or an origin the application set */
    if ((hdc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE )))
    {
        NtGdiSetDIBitsToDeviceInternal( hdc, 0, 0, width, height, 0, 0, 0, height, bits, info, DIB_RGB_COLORS,
                                        0, 0, FALSE, NULL );
        NtUserReleaseDC( hwnd, hdc );
    }
    flush_window_surfaces( TRUE );
    return TRUE;
}

static const struct opengl_drawable_funcs framebuffer_surface_funcs =
{
    .destroy = framebuffer_surface_destroy,
    .flush = framebuffer_surface_flush,
    .swap = framebuffer_surface_swap,
    .set_context = framebuffer_surface_set_context,
};

static struct opengl_drawable *framebuffer_surface_create( int format, struct client_surface *client )
{
    struct framebuffer_surface *surface;
    if (!(surface = opengl_drawable_create( sizeof(*surface), &framebuffer_surface_funcs, format, client ))) return NULL;

    surface->base.buffer_map[0] = GL_COLOR_ATTACHMENT0;
    surface->base.buffer_map[2] = surface->base.doublebuffer ? GL_COLOR_ATTACHMENT1 : GL_NONE;
    if (surface->base.stereo)
    {
        surface->base.buffer_map[1] = surface->base.doublebuffer ? GL_COLOR_ATTACHMENT2 : GL_COLOR_ATTACHMENT1;
        surface->base.buffer_map[3] = surface->base.doublebuffer ? GL_COLOR_ATTACHMENT3 : GL_NONE;
    }
    else
    {
        surface->base.buffer_map[1] = GL_NONE;
        surface->base.buffer_map[3] = GL_NONE;
    }
    surface->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = surface->base.buffer_map[0]; /* only front left */
    surface->base.buffer_map[GL_BACK - GL_FRONT_LEFT] = surface->base.buffer_map[2]; /* only back left */
    surface->base.buffer_map[GL_LEFT - GL_FRONT_LEFT] = surface->base.buffer_map[0]; /* only front left */
    surface->base.buffer_map[GL_RIGHT - GL_FRONT_LEFT] = surface->base.buffer_map[1]; /* only front right */
    surface->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = surface->base.buffer_map[0]; /* only front left */

    return &surface->base;
}

static const struct opengl_drawable_funcs egldrv_pbuffer_funcs;

static EGLConfig egl_config_for_format( const struct egl_platform *egl, int format )
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void egldrv_init_egl_platform( struct egl_platform *platform )
{
    platform->type = EGL_PLATFORM_SURFACELESS_MESA;
    platform->native_display = 0;
}

static void *egldrv_get_proc_address( const char *name )
{
    return display_funcs.p_eglGetProcAddress( name );
}

/* list of flags with which each EGL config will be combined */
static const UINT pixel_format_flags[] =
{
    PFD_SUPPORT_GDI,
    PFD_DOUBLEBUFFER,
    0, /* offscreen */
};

static UINT egldrv_init_pixel_formats( UINT *onscreen_count )
{
    const struct opengl_funcs *funcs = &display_funcs;
    struct egl_platform *egl = &display_egl;
    EGLConfig *configs, *deep;
    EGLint i, j, render, count;

    funcs->p_eglGetConfigs( egl->display, NULL, 0, &count );
    if (!(configs = malloc( count * sizeof(*configs) ))) return 0;
    if (!funcs->p_eglGetConfigs( egl->display, configs, count, &count ) || !count)
    {
        ERR( "Failed to get any configs from eglChooseConfig\n" );
        free( configs );
        return 0;
    }

    for (i = 0, j = 0; i < count; i++)
    {
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_RENDERABLE_TYPE, &render );
        if (render & EGL_OPENGL_BIT) configs[j++] = configs[i];
    }
    count = j;

    /* Put the configs of at most 8 bits per channel first, keeping their order. Windows drivers list those
     * formats first, and ChoosePixelFormat keeps the first of the formats that match a request equally well:
     * Mesa lists RGB10_A2 first, which would answer a request for 32 color bits with 2 alpha bits (and make
     * every presented frame need a conversion). */
    if ((deep = malloc( count * sizeof(*deep) )))
    {
        EGLint red, n = 0;
        for (i = 0, j = 0; i < count; i++)
        {
            funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_RED_SIZE, &red );
            if (red > 8) deep[n++] = configs[i];
            else configs[j++] = configs[i];
        }
        memcpy( configs + j, deep, n * sizeof(*deep) );
        free( deep );
    }

    if (TRACE_ON(wgl)) for (i = 0; i < count; i++)
    {
        EGLint id, type, visual_id, native, color, r, g, b, a, d, s;
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_NATIVE_VISUAL_ID, &visual_id );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_SURFACE_TYPE, &type );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_RENDERABLE_TYPE, &render );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_CONFIG_ID, &id );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_NATIVE_RENDERABLE, &native );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_COLOR_BUFFER_TYPE, &color );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_RED_SIZE, &r );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_GREEN_SIZE, &g );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_BLUE_SIZE, &b );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_ALPHA_SIZE, &a );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_DEPTH_SIZE, &d );
        funcs->p_eglGetConfigAttrib( egl->display, configs[i], EGL_STENCIL_SIZE, &s );
        TRACE( "%u: config %d id %d type %x visual %d native %d render %x colortype %d rgba %d,%d,%d,%d depth %u stencil %d\n",
               count, i, id, type, visual_id, native, render, color, r, g, b, a, d, s );
    }

    egl->configs = configs;
    egl->config_count = count;

    *onscreen_count = (ARRAY_SIZE(pixel_format_flags) - 1) * count;
    return ARRAY_SIZE(pixel_format_flags) * count;
}

static BOOL describe_egl_config( EGLConfig config, struct wgl_pixel_format *fmt, UINT flags )
{
    const struct opengl_funcs *funcs = &display_funcs;
    struct egl_platform *egl = &display_egl;
    EGLint value, surface_type;
    PIXELFORMATDESCRIPTOR *pfd = &fmt->pfd;

    /* If we can't get basic information, there is no point continuing */
    if (!funcs->p_eglGetConfigAttrib( egl->display, config, EGL_SURFACE_TYPE, &surface_type )) return FALSE;
    if (egl->type == EGL_PLATFORM_SURFACELESS_MESA) surface_type |= EGL_WINDOW_BIT | EGL_PBUFFER_BIT;

    memset( fmt, 0, sizeof(*fmt) );
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION | flags;
    if (flags && surface_type & EGL_WINDOW_BIT) pfd->dwFlags |= PFD_DRAW_TO_WINDOW;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->iLayerType = PFD_MAIN_PLANE;

#define SET_ATTRIB( field, attrib )                                                                \
    value = 0;                                                                                     \
    funcs->p_eglGetConfigAttrib( egl->display, config, attrib, &value );                           \
    pfd->field = value;
#define SET_ATTRIB_ARB( field, attrib )                                                            \
    if (!funcs->p_eglGetConfigAttrib( egl->display, config, attrib, &value )) value = -1;          \
    fmt->field = value;

    /* Although the documentation describes cColorBits as excluding alpha, real
     * drivers tend to return the full pixel size, so do the same. */
    SET_ATTRIB( cColorBits, EGL_BUFFER_SIZE );
    SET_ATTRIB( cRedBits, EGL_RED_SIZE );
    SET_ATTRIB( cGreenBits, EGL_GREEN_SIZE );
    SET_ATTRIB( cBlueBits, EGL_BLUE_SIZE );
    SET_ATTRIB( cAlphaBits, EGL_ALPHA_SIZE );
    /* Although we don't get information from EGL about the component shifts
     * or the native format, the 0xARGB order is the most common. */
    pfd->cBlueShift = 0;
    pfd->cGreenShift = pfd->cBlueBits;
    pfd->cRedShift = pfd->cGreenBits + pfd->cBlueBits;
    if (!pfd->cAlphaBits) pfd->cAlphaShift = 0;
    else pfd->cAlphaShift = pfd->cRedBits + pfd->cGreenBits + pfd->cBlueBits;

    SET_ATTRIB( cDepthBits, EGL_DEPTH_SIZE );
    SET_ATTRIB( cStencilBits, EGL_STENCIL_SIZE );

    fmt->swap_method = WGL_SWAP_UNDEFINED_ARB;

    if (funcs->p_eglGetConfigAttrib( egl->display, config, EGL_TRANSPARENT_TYPE, &value ))
    {
        switch (value)
        {
        case EGL_TRANSPARENT_RGB:
            fmt->transparent = GL_TRUE;
            break;
        case EGL_NONE:
            fmt->transparent = GL_FALSE;
            break;
        default:
            ERR( "unexpected transparency type 0x%x\n", value );
            fmt->transparent = -1;
            break;
        }
    }
    else fmt->transparent = -1;

    if (!egl->has_EGL_EXT_pixel_format_float) fmt->pixel_type = WGL_TYPE_RGBA_ARB;
    else if (funcs->p_eglGetConfigAttrib( egl->display, config, EGL_COLOR_COMPONENT_TYPE_EXT, &value ))
    {
        switch (value)
        {
        case EGL_COLOR_COMPONENT_TYPE_FIXED_EXT:
            fmt->pixel_type = WGL_TYPE_RGBA_ARB;
            break;
        case EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT:
            fmt->pixel_type = WGL_TYPE_RGBA_FLOAT_ARB;
            break;
        default:
            ERR( "unexpected color component type 0x%x\n", value );
            fmt->pixel_type = -1;
            break;
        }
    }
    else fmt->pixel_type = -1;

    if (egl->force_pbuffer_formats) fmt->draw_to_pbuffer = TRUE;
    else if (surface_type & EGL_PBUFFER_BIT) fmt->draw_to_pbuffer = TRUE;
    if (fmt->draw_to_pbuffer) pfd->dwFlags |= PFD_DRAW_TO_BITMAP;

    /* Use some arbitrary but reasonable limits (4096 is also Mesa's default) */
    fmt->max_pbuffer_width = 4096;
    fmt->max_pbuffer_height = 4096;
    fmt->max_pbuffer_pixels = fmt->max_pbuffer_width * fmt->max_pbuffer_height;

    if (funcs->p_eglGetConfigAttrib( egl->display, config, EGL_TRANSPARENT_RED_VALUE, &value ))
    {
        fmt->transparent_red_value_valid = GL_TRUE;
        fmt->transparent_red_value = value;
    }
    if (funcs->p_eglGetConfigAttrib( egl->display, config, EGL_TRANSPARENT_GREEN_VALUE, &value ))
    {
        fmt->transparent_green_value_valid = GL_TRUE;
        fmt->transparent_green_value = value;
    }
    if (funcs->p_eglGetConfigAttrib( egl->display, config, EGL_TRANSPARENT_BLUE_VALUE, &value ))
    {
        fmt->transparent_blue_value_valid = GL_TRUE;
        fmt->transparent_blue_value = value;
    }
    fmt->transparent_alpha_value_valid = GL_TRUE;
    fmt->transparent_alpha_value = 0;
    fmt->transparent_index_value_valid = GL_TRUE;
    fmt->transparent_index_value = 0;

    SET_ATTRIB_ARB( sample_buffers, EGL_SAMPLE_BUFFERS );
    SET_ATTRIB_ARB( samples, EGL_SAMPLES );

    fmt->bind_to_texture_rgb = GL_TRUE;
    fmt->bind_to_texture_rgba = GL_TRUE;
    fmt->bind_to_texture_rectangle_rgb = GL_TRUE;
    fmt->bind_to_texture_rectangle_rgba = GL_TRUE;

    /* TODO: Support SRGB surfaces and enable the attribute */
    fmt->framebuffer_srgb_capable = GL_FALSE;

    fmt->float_components = GL_FALSE;

#undef SET_ATTRIB
#undef SET_ATTRIB_ARB
    return TRUE;
}

static BOOL egldrv_describe_pixel_format( int format, struct wgl_pixel_format *desc )
{
    struct egl_platform *egl = &display_egl;
    int count = egl->config_count;

    if (--format < 0 || format > ARRAY_SIZE(pixel_format_flags) * count) return FALSE;
    return describe_egl_config( egl->configs[format % count], desc, pixel_format_flags[format / count] );
}

static const char *egldrv_init_wgl_extensions( struct opengl_funcs *funcs )
{
    return "";
}

static BOOL egldrv_surface_create( HWND hwnd, int format, struct opengl_drawable **drawable )
{
    struct client_surface *client;

    if (!(client = nulldrv_client_surface_create( hwnd ))) return FALSE;
    *drawable = framebuffer_surface_create( format, client );
    client_surface_release( client );

    return !!*drawable;
}

static BOOL egldrv_pbuffer_create( HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **drawable )
{
    const struct opengl_funcs *funcs = &display_funcs;
    const struct egl_platform *egl = &display_egl;
    EGLint attribs[13], *attrib = attribs;
    struct opengl_drawable *gl;

    TRACE( "hdc %p, format %d, largest %u, texture_format %#x, texture_target %#x, max_level %#x, width %d, height %d, drawable %p\n",
           hdc, format, largest, texture_format, texture_target, max_level, *width, *height, drawable );

    *attrib++ = EGL_WIDTH;
    *attrib++ = *width;
    *attrib++ = EGL_HEIGHT;
    *attrib++ = *height;
    if (largest)
    {
        *attrib++ = EGL_LARGEST_PBUFFER;
        *attrib++ = 1;
    }
    switch (texture_format)
    {
    case 0: break;
    case GL_RGB:
        *attrib++ = EGL_TEXTURE_FORMAT;
        *attrib++ = EGL_TEXTURE_RGB;
        break;
    case GL_RGBA:
        *attrib++ = EGL_TEXTURE_FORMAT;
        *attrib++ = EGL_TEXTURE_RGBA;
        break;
    default:
        FIXME( "Unsupported format %#x\n", texture_format );
        *attrib++ = EGL_TEXTURE_FORMAT;
        *attrib++ = EGL_TEXTURE_RGBA;
        break;
    }
    switch (texture_target)
    {
    case 0: break;
    case GL_TEXTURE_2D:
        *attrib++ = EGL_TEXTURE_TARGET;
        *attrib++ = EGL_TEXTURE_2D;
        break;
    default:
        FIXME( "Unsupported target %#x\n", texture_target );
        *attrib++ = EGL_TEXTURE_TARGET;
        *attrib++ = EGL_TEXTURE_2D;
        break;
    }
    if (max_level)
    {
        *attrib++ = EGL_MIPMAP_TEXTURE;
        *attrib++ = GL_TRUE;
    }
    *attrib++ = EGL_NONE;

    if (!(gl = opengl_drawable_create( sizeof(*gl), &egldrv_pbuffer_funcs, format, NULL ))) return FALSE;
    if (!(gl->surface = funcs->p_eglCreatePbufferSurface( egl->display, egl_config_for_format( egl, gl->format ), attribs )))
    {
        opengl_drawable_release( gl );
        return FALSE;
    }

    funcs->p_eglQuerySurface( egl->display, gl->surface, EGL_WIDTH, width );
    funcs->p_eglQuerySurface( egl->display, gl->surface, EGL_HEIGHT, height );

    *drawable = gl;
    return TRUE;
}

static BOOL egldrv_pbuffer_updated( HDC hdc, struct opengl_drawable *drawable, GLenum cube_face, GLint mipmap_level )
{
    return GL_TRUE;
}

static UINT egldrv_pbuffer_bind( HDC hdc, struct opengl_drawable *drawable, GLenum buffer )
{
    return -1; /* use default implementation */
}

static BOOL egldrv_context_create( int format, void *share, const int *attribs, void **context )
{
    const struct opengl_funcs *funcs = &display_funcs;
    const struct egl_platform *egl = &display_egl;
    EGLint err, egl_attribs[16], *attribs_end = egl_attribs;

    TRACE( "format %d, share %p, attribs %p\n", format, share, attribs );

    for (; attribs && attribs[0] != 0; attribs += 2)
    {
        EGLint name;

        TRACE( "%#x %#x\n", attribs[0], attribs[1] );

        /* Find the EGL attribute names corresponding to the WGL names.
         * For all of the attributes below, the values match between the two
         * systems, so we can use them directly. */
        switch (attribs[0])
        {
        case WGL_CONTEXT_MAJOR_VERSION_ARB:
            name = EGL_CONTEXT_MAJOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_MINOR_VERSION_ARB:
            name = EGL_CONTEXT_MINOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_FLAGS_ARB:
            name = EGL_CONTEXT_FLAGS_KHR;
            break;
        case WGL_CONTEXT_OPENGL_NO_ERROR_ARB:
            name = EGL_CONTEXT_OPENGL_NO_ERROR_KHR;
            break;
        case WGL_CONTEXT_PROFILE_MASK_ARB:
            if (attribs[1] & WGL_CONTEXT_ES2_PROFILE_BIT_EXT)
            {
                ERR( "OpenGL ES contexts are not supported\n" );
                return FALSE;
            }
            name = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
            break;
        default:
            name = EGL_NONE;
            FIXME( "Unhandled attributes: %#x %#x\n", attribs[0], attribs[1] );
        }

        if (name != EGL_NONE)
        {
            EGLint *dst = egl_attribs;
            /* Check if we have already set the same attribute and replace it. */
            for (; dst != attribs_end && *dst != name; dst += 2) continue;
            /* Our context attribute array should have enough space for all the
             * attributes we support (we merge repetitions), plus EGL_NONE. */
            assert( dst - egl_attribs <= ARRAY_SIZE(egl_attribs) - 3 );
            dst[0] = name;
            dst[1] = attribs[1];
            if (dst == attribs_end) attribs_end += 2;
        }
    }
    *attribs_end = EGL_NONE;

    /* For now only OpenGL is supported. It's enough to set the API only for
     * context creation, since:
     * 1. the default API is EGL_OPENGL_ES_API
     * 2. the EGL specification says in section 3.7:
     *    > EGL_OPENGL_API and EGL_OPENGL_ES_API are interchangeable for all
     *    > purposes except eglCreateContext.
     */
    funcs->p_eglBindAPI( EGL_OPENGL_API );
    *context = funcs->p_eglCreateContext( egl->display, EGL_NO_CONFIG_KHR, share, attribs ? egl_attribs : NULL );

    if ((err = funcs->p_eglGetError()) != EGL_SUCCESS || !*context)
    {
        WARN( "Context creation failed (error %#x).\n", err );
        return FALSE;
    }

    TRACE( "Created context %p\n", *context );
    return TRUE;
}

static BOOL egldrv_context_destroy( void *context )
{
    const struct opengl_funcs *funcs = &display_funcs;
    const struct egl_platform *egl = &display_egl;

    funcs->p_eglDestroyContext( egl->display, context );
    return TRUE;
}

static BOOL egldrv_make_current( struct opengl_drawable *draw, struct opengl_drawable *read, void *context )
{
    const struct opengl_funcs *funcs = &display_funcs;
    const struct egl_platform *egl = &display_egl;

    TRACE( "draw %s, read %s, context %p\n", debugstr_opengl_drawable( draw ), debugstr_opengl_drawable( read ), context );

    return funcs->p_eglMakeCurrent( egl->display, context ? draw->surface : EGL_NO_SURFACE, context ? read->surface : EGL_NO_SURFACE, context );
}

static void egldrv_pbuffer_destroy( struct opengl_drawable *drawable )
{
    TRACE( "%s\n", debugstr_opengl_drawable( drawable ) );
}

static const struct opengl_drawable_funcs egldrv_pbuffer_funcs =
{
    .destroy = egldrv_pbuffer_destroy,
};

static const struct opengl_driver_funcs egldrv_funcs =
{
    .p_init_egl_platform = egldrv_init_egl_platform,
    .p_get_proc_address = egldrv_get_proc_address,
    .p_init_pixel_formats = egldrv_init_pixel_formats,
    .p_describe_pixel_format = egldrv_describe_pixel_format,
    .p_init_wgl_extensions = egldrv_init_wgl_extensions,
    .p_surface_create = egldrv_surface_create,
    .p_pbuffer_create = egldrv_pbuffer_create,
    .p_pbuffer_updated = egldrv_pbuffer_updated,
    .p_pbuffer_bind = egldrv_pbuffer_bind,
    .p_context_create = egldrv_context_create,
    .p_context_destroy = egldrv_context_destroy,
    .p_make_current = egldrv_make_current,
};

static BOOL egl_init( const struct opengl_driver_funcs **driver_funcs )
{
    struct opengl_funcs *funcs = &display_funcs;
    const char *extensions;

    if (!(funcs->egl_handle = dlopen( SONAME_LIBEGL, RTLD_NOW | RTLD_GLOBAL )))
    {
        ERR( "Failed to load %s: %s\n", SONAME_LIBEGL, dlerror() );
        return FALSE;
    }

#define LOAD_FUNCPTR( name )                                    \
    if (!(funcs->p_##name = dlsym( funcs->egl_handle, #name ))) \
    {                                                           \
        ERR( "Failed to find EGL function %s\n", #name );       \
        goto failed;                                            \
    }
    LOAD_FUNCPTR( eglGetProcAddress );
    LOAD_FUNCPTR( eglQueryString );
#undef LOAD_FUNCPTR

    if (!(extensions = funcs->p_eglQueryString( EGL_NO_DISPLAY, EGL_EXTENSIONS )))
    {
        ERR( "Failed to find client extensions\n" );
        goto failed;
    }
    TRACE( "EGL client extensions:\n" );
    dump_extensions( extensions );

#define CHECK_EXTENSION( ext )                                  \
    if (!has_extension( extensions, #ext ))                     \
    {                                                           \
        ERR( "Failed to find required extension %s\n", #ext );  \
        goto failed;                                            \
    }
    CHECK_EXTENSION( EGL_KHR_client_get_all_proc_addresses );
    CHECK_EXTENSION( EGL_EXT_platform_base );
#undef CHECK_EXTENSION

#define USE_GL_FUNC( func )                                                                     \
    if (!funcs->p_##func && !(funcs->p_##func = (void *)funcs->p_eglGetProcAddress( #func )))   \
    {                                                                                           \
        ERR( "Failed to load symbol %s\n", #func );                                             \
        goto failed;                                                                            \
    }
    ALL_EGL_FUNCS
#undef USE_GL_FUNC

    *driver_funcs = &egldrv_funcs;
    return TRUE;

failed:
    dlclose( funcs->egl_handle );
    funcs->egl_handle = NULL;
    return FALSE;
}

static BOOL init_egl_platform( struct egl_platform *egl, const struct opengl_funcs *funcs )
{
    const char *extensions;
    EGLint major, minor;

    if (!egl->display) egl->display = funcs->p_eglGetPlatformDisplay( egl->type, egl->native_display, NULL );
    if (!egl->display)
    {
        WARN( "Failed to open EGL display for type %#x, native display %p\n", egl->type, egl->native_display );
        return FALSE;
    }

    if (!funcs->p_eglInitialize( egl->display, &major, &minor )) return FALSE;
    TRACE( "Initialized EGL display %p, version %d.%d\n", egl->display, major, minor );

    if (!(extensions = funcs->p_eglQueryString( egl->display, EGL_EXTENSIONS ))) return FALSE;
    TRACE( "EGL display extensions:\n" );
    dump_extensions( extensions );

#define CHECK_EXTENSION( ext )                                                                     \
    if (!has_extension( extensions, #ext ))                                                        \
    {                                                                                              \
        WARN( "Failed to find required extension %s\n", #ext );                                    \
        return FALSE;                                                                              \
    }
    CHECK_EXTENSION( EGL_KHR_create_context );
    CHECK_EXTENSION( EGL_KHR_create_context_no_error );
    CHECK_EXTENSION( EGL_KHR_no_config_context );
#undef CHECK_EXTENSION

    egl->has_EGL_EXT_present_opaque = has_extension( extensions, "EGL_EXT_present_opaque" );
    egl->has_EGL_EXT_pixel_format_float = has_extension( extensions, "EGL_EXT_pixel_format_float" );
    return TRUE;
}

static void init_egl_devices( struct opengl_funcs *funcs )
{
    EGLDeviceEXT *devices = NULL;
    struct egl_platform *egl;
    const char *extensions;
    EGLint count;

    if (!(extensions = funcs->p_eglQueryString( EGL_NO_DISPLAY, EGL_EXTENSIONS ))) return;
    if (!has_extension( extensions, "EGL_EXT_device_base" ) || !has_extension( extensions, "EGL_EXT_platform_device" )) return;

#define LOAD_FUNCPTR( func )                                                                    \
    if (!funcs->p_##func && !(funcs->p_##func = (void *)funcs->p_eglGetProcAddress( #func ))) return
    LOAD_FUNCPTR( eglQueryDevicesEXT );
    LOAD_FUNCPTR( eglQueryDeviceStringEXT );
    LOAD_FUNCPTR( eglQueryDisplayAttribEXT );
#undef LOAD_FUNCPTR

#define LOAD_FUNCPTR( func )                                                                    \
    if (!(funcs->p_##func = (void *)funcs->p_eglGetProcAddress( #func ))) WARN( #func " not found\n" );
    LOAD_FUNCPTR( eglQueryDeviceBinaryEXT );
#undef LOAD_FUNCPTR

    if (!funcs->p_eglQueryDisplayAttribEXT( display_egl.display, EGL_DEVICE_EXT, (EGLAttrib *)&display_egl.device ))
    {
        WARN( "Failed to query EGL display device (error %#x).\n", funcs->p_eglGetError() );
        return;
    }
    TRACE( "Found display platform device %p\n", display_egl.device );
    list_add_tail( &devices_egl, &display_egl.entry );

    funcs->p_eglQueryDevicesEXT( 0, NULL, &count );
    if (!count || !(devices = calloc( count, sizeof(EGLDeviceEXT *) ))) goto done;
    funcs->p_eglQueryDevicesEXT( count, devices, &count );

    for (int i = 0; i < count; i++)
    {
        if (devices[i] == display_egl.device) continue;

        extensions = funcs->p_eglQueryDeviceStringEXT( devices[i], EGL_EXTENSIONS );
        /* Assume that all devices without EGL_MESA_device_software are accelerated. */
        if (has_extension( extensions, "EGL_MESA_device_software" )) continue;

        if (!(egl = calloc( 1, sizeof(*egl) ))) break;

        TRACE( "Initializing EGL device %p\n", devices[i] );
        egl->type = EGL_PLATFORM_DEVICE_EXT;
        egl->native_display = devices[i];
        egl->device = devices[i];
        egl->index = list_count( &devices_egl );

        if (!init_egl_platform( egl, funcs )) free( egl );
        else list_add_tail( &devices_egl, &egl->entry );
    }

done:
    TRACE( "Initialized %u EGL devices\n", list_count( &devices_egl ) );
    free( devices );
}

static void init_egl_platforms( struct opengl_funcs *funcs, const struct opengl_driver_funcs *driver_funcs )
{
    if (!funcs->egl_handle || !driver_funcs->p_init_egl_platform) return;

    driver_funcs->p_init_egl_platform( &display_egl );
    if (!display_egl.type) display_egl.display = funcs->p_eglGetDisplay( EGL_DEFAULT_DISPLAY );

    init_egl_platform( &display_egl, funcs );
    init_egl_devices( funcs );
}

#else /* SONAME_LIBEGL */

static BOOL egl_init( const struct opengl_driver_funcs **driver_funcs )
{
    WARN( "EGL support not compiled in!\n" );
    return FALSE;
}

static void init_egl_platforms( struct opengl_funcs *funcs, const struct opengl_driver_funcs *driver_funcs )
{
}

#endif /* SONAME_LIBEGL */

static UINT read_drm_device_prop( const char *name, const char *prop )
{
    UINT value = -1;
    char *path;
    FILE *file;

    if (asprintf( &path, "/sys/class/drm%s/device/%s", name, prop ) == -1) return value;
    if ((file = fopen( path, "r" )))
    {
        fscanf( file, "%x", &value );
        fclose( file );
    }

    free( path );
    return value;
}

static const char *opengl_vendor_to_name( UINT16 vendor, const char *default_name )
{
    switch (vendor)
    {
    case 0x8086: return "Intel";
    case 0x1002: return "ATI Technologies Inc.";
    case 0x10de: return "NVIDIA Corporation";
    default:     return default_name;
    }
}

static void init_device_info( struct egl_platform *egl, const struct opengl_funcs *funcs )
{
    static const UINT versions[] = {46, 45, 44, 43, 42, 41, 40, 33, 32, 31, 30, 21, 20, 15, 14, 13, 12, 11, 10, 0};
    EGLContext core_context = EGL_NO_CONTEXT, compat_context = EGL_NO_CONTEXT, context = EGL_NO_CONTEXT;
    int i, count, values[3] = {0};
    const char *extensions, *str;
    EGLConfig config;

    TRACE( "Initializing device %u (%p)\n", egl->index, egl->device);

    extensions = funcs->p_eglQueryDeviceStringEXT( egl->device, EGL_EXTENSIONS );
    /* Assume that all devices without EGL_MESA_device_software are accelerated. */
    egl->accelerated = !has_extension( extensions, "EGL_MESA_device_software" );
    TRACE( "  - accelerated: %u\n", egl->accelerated );

    /* EGL does not provide a convenient way to get device / vendor ID, so we have to do it
     * manually through DRM. Otherwise fallback to value as for SoC devices in GLX */
    if (!has_extension( extensions, "EGL_EXT_device_drm" )) egl->vendor_id = egl->device_id = 0xffffffff;
    else if ((str = funcs->p_eglQueryDeviceStringEXT( egl->device, EGL_DRM_DEVICE_FILE_EXT )) && (str = strrchr( str, '/' )))
    {
        egl->vendor_id = read_drm_device_prop( str, "vendor" );
        egl->device_id = read_drm_device_prop( str, "device" );
    }
    TRACE( "  - device_id: %#x\n", egl->device_id );
    TRACE( "  - vendor_id: %#x\n", egl->vendor_id );

    if (has_extension( extensions, "EGL_EXT_device_persistent_id" ))
    {
        funcs->p_eglQueryDeviceBinaryEXT( egl->device, EGL_DEVICE_UUID_EXT, sizeof(egl->device_uuid), &egl->device_uuid, &count );
        funcs->p_eglQueryDeviceBinaryEXT( egl->device, EGL_DRIVER_UUID_EXT, sizeof(egl->driver_uuid), &egl->driver_uuid, &count );
    }
    TRACE( "  - device_uuid: %s\n", debugstr_guid(&egl->device_uuid) );
    TRACE( "  - driver_uuid: %s\n", debugstr_guid(&egl->driver_uuid) );

    funcs->p_eglBindAPI( EGL_OPENGL_API );
    funcs->p_eglGetConfigs( egl->display, &config, 1, &count );
    if (!count) config = EGL_NO_CONFIG_KHR;

    for (i = 0; i < ARRAY_SIZE(versions) && (!egl->core_version || !egl->compat_version); i++)
    {
        int context_attribs[] =
        {
           EGL_CONTEXT_MAJOR_VERSION, versions[i] / 10,
           EGL_CONTEXT_MINOR_VERSION, versions[i] % 10,
           EGL_CONTEXT_OPENGL_PROFILE_MASK, 0,
           EGL_NONE,
        };

        if (!egl->compat_version && versions[i] >= 30)
        {
            context_attribs[5] = EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
            compat_context = funcs->p_eglCreateContext( egl->display, config, EGL_NO_CONTEXT, context_attribs );
            if (compat_context) egl->compat_version = versions[i];
            if (!context) context = compat_context;
        }
        if (!egl->core_version)
        {
            context_attribs[5] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
            core_context = funcs->p_eglCreateContext( egl->display, config, EGL_NO_CONTEXT, context_attribs );
            if (core_context) egl->core_version = versions[i];
            if (!context) context = core_context;
        }
    }
    TRACE( "  - core_version: %u\n", egl->core_version );
    TRACE( "  - compat_version: %u\n", egl->compat_version );

    if (context)
    {
        char *renderer, *vendor;

        funcs->p_eglMakeCurrent( egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, context );

        renderer = strdup( (const char *)funcs->p_glGetString( GL_RENDERER ) );
        egl->device_name = gpu_device_name( egl->vendor_id, egl->device_id, renderer );
        if (egl->device_name != renderer) free( renderer );

        vendor = strdup( (const char *)funcs->p_glGetString( GL_VENDOR ) );
        egl->vendor_name = opengl_vendor_to_name( egl->vendor_id, vendor );
        if (egl->vendor_name != vendor) free( vendor );

        TRACE( "  - device_name: %s\n", debugstr_a( egl->device_name ) );
        TRACE( "  - vendor_name: %s\n", debugstr_a( egl->vendor_name ) );

        if ((str = (const char *)funcs->p_glGetString( GL_VERSION )) && (str = strrchr( str, ' ' )) &&
            (count = sscanf( str, "%u.%u.%u", &values[0], &values[1], &values[2] )) >= 2)
            memcpy( egl->version, values, sizeof(egl->version) );
        TRACE( "  - version: %u.%u.%u\n", egl->version[0], egl->version[1], egl->version[2] );

        extensions = (const char *)funcs->p_glGetString( GL_EXTENSIONS );
        if (has_extension( extensions, "GL_NVX_gpu_memory_info" ))
        {
            funcs->p_glGetIntegerv( GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, values );
            egl->video_memory = values[0] / 1024;
        }
        TRACE( "  - video_memory: %u MiB\n", egl->video_memory );

        funcs->p_eglMakeCurrent( egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
    }

    if (compat_context) funcs->p_eglDestroyContext( egl->display, compat_context );
    if (core_context) funcs->p_eglDestroyContext( egl->display, core_context );

    if (egl != &display_egl) funcs->p_eglTerminate( egl->display );
}

static const struct
{
    BYTE color_bits;
    BYTE red_bits, red_shift;
    BYTE green_bits, green_shift;
    BYTE blue_bits, blue_shift;
    BYTE alpha_bits, alpha_shift;
    BYTE accum_bits;
    BYTE depth_bits;
    BYTE stencil_bits;
} nulldrv_pixel_formats[] =
{
    { 32,  8, 16, 8, 8,  8, 0,  8, 24,  16, 32, 8 },
    { 32,  8, 16, 8, 8,  8, 0,  8, 24,  16, 16, 8 },
    { 32,  8, 0,  8, 8,  8, 16, 8, 24,  16, 32, 8 },
    { 32,  8, 0,  8, 8,  8, 16, 8, 24,  16, 16, 8 },
    { 32,  8, 8,  8, 16, 8, 24, 8, 0,   16, 32, 8 },
    { 32,  8, 8,  8, 16, 8, 24, 8, 0,   16, 16, 8 },
    { 24,  8, 0,  8, 8,  8, 16, 0, 0,   16, 32, 8 },
    { 24,  8, 0,  8, 8,  8, 16, 0, 0,   16, 16, 8 },
    { 24,  8, 16, 8, 8,  8, 0,  0, 0,   16, 32, 8 },
    { 24,  8, 16, 8, 8,  8, 0,  0, 0,   16, 16, 8 },
    { 16,  5, 0,  6, 5,  5, 11, 0, 0,   16, 32, 8 },
    { 16,  5, 0,  6, 5,  5, 11, 0, 0,   16, 16, 8 },
};

static void *nulldrv_get_proc_address( const char *name )
{
    return NULL;
}

static UINT nulldrv_init_pixel_formats( UINT *onscreen_count )
{
    *onscreen_count = ARRAY_SIZE(nulldrv_pixel_formats);
    return ARRAY_SIZE(nulldrv_pixel_formats);
}

static BOOL nulldrv_describe_pixel_format( int format, struct wgl_pixel_format *descr )
{
    if (format <= 0 || format > ARRAY_SIZE(nulldrv_pixel_formats)) return FALSE;

    memset( descr, 0, sizeof(*descr) );
    descr->pfd.nSize            = sizeof(*descr);
    descr->pfd.nVersion         = 1;
    descr->pfd.dwFlags          = PFD_SUPPORT_GDI | PFD_SUPPORT_OPENGL | PFD_DRAW_TO_BITMAP | PFD_GENERIC_FORMAT;
    descr->pfd.iPixelType       = PFD_TYPE_RGBA;
    descr->pfd.cColorBits       = nulldrv_pixel_formats[format - 1].color_bits;
    descr->pfd.cRedBits         = nulldrv_pixel_formats[format - 1].red_bits;
    descr->pfd.cRedShift        = nulldrv_pixel_formats[format - 1].red_shift;
    descr->pfd.cGreenBits       = nulldrv_pixel_formats[format - 1].green_bits;
    descr->pfd.cGreenShift      = nulldrv_pixel_formats[format - 1].green_shift;
    descr->pfd.cBlueBits        = nulldrv_pixel_formats[format - 1].blue_bits;
    descr->pfd.cBlueShift       = nulldrv_pixel_formats[format - 1].blue_shift;
    descr->pfd.cAlphaBits       = nulldrv_pixel_formats[format - 1].alpha_bits;
    descr->pfd.cAlphaShift      = nulldrv_pixel_formats[format - 1].alpha_shift;
    descr->pfd.cAccumBits       = nulldrv_pixel_formats[format - 1].accum_bits;
    descr->pfd.cAccumRedBits    = nulldrv_pixel_formats[format - 1].accum_bits / 4;
    descr->pfd.cAccumGreenBits  = nulldrv_pixel_formats[format - 1].accum_bits / 4;
    descr->pfd.cAccumBlueBits   = nulldrv_pixel_formats[format - 1].accum_bits / 4;
    descr->pfd.cAccumAlphaBits  = nulldrv_pixel_formats[format - 1].accum_bits / 4;
    descr->pfd.cDepthBits       = nulldrv_pixel_formats[format - 1].depth_bits;
    descr->pfd.cStencilBits     = nulldrv_pixel_formats[format - 1].stencil_bits;
    descr->pfd.cAuxBuffers      = 0;
    descr->pfd.iLayerType       = PFD_MAIN_PLANE;

    return TRUE;
}

static const char *nulldrv_init_wgl_extensions( struct opengl_funcs *funcs )
{
    return "";
}

static BOOL nulldrv_surface_create( HWND hwnd, int format, struct opengl_drawable **drawable )
{
    return TRUE;
}

static BOOL nulldrv_pbuffer_create( HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                    GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **drawable )
{
    return FALSE;
}

static BOOL nulldrv_pbuffer_updated( HDC hdc, struct opengl_drawable *drawable, GLenum cube_face, GLint mipmap_level )
{
    return GL_TRUE;
}

static UINT nulldrv_pbuffer_bind( HDC hdc, struct opengl_drawable *drawable, GLenum buffer )
{
    return -1; /* use default implementation */
}

static BOOL nulldrv_context_create( int format, void *share, const int *attribs, void **private )
{
    return FALSE;
}

static BOOL nulldrv_context_destroy( void *private )
{
    return FALSE;
}

static BOOL nulldrv_make_current( struct opengl_drawable *draw_base, struct opengl_drawable *read_base, void *private )
{
    return FALSE;
}

static const struct opengl_driver_funcs nulldrv_funcs =
{
    .p_get_proc_address = nulldrv_get_proc_address,
    .p_init_pixel_formats = nulldrv_init_pixel_formats,
    .p_describe_pixel_format = nulldrv_describe_pixel_format,
    .p_init_wgl_extensions = nulldrv_init_wgl_extensions,
    .p_surface_create = nulldrv_surface_create,
    .p_pbuffer_create = nulldrv_pbuffer_create,
    .p_pbuffer_updated = nulldrv_pbuffer_updated,
    .p_pbuffer_bind = nulldrv_pbuffer_bind,
    .p_context_create = nulldrv_context_create,
    .p_context_destroy = nulldrv_context_destroy,
    .p_make_current = nulldrv_make_current,
};

static const char *win32u_wglGetExtensionsStringARB( HDC hdc )
{
    TRACE( "hdc %p\n", hdc );
    if (TRACE_ON(wgl)) dump_extensions( wgl_extensions );
    return wgl_extensions;
}

static const char *win32u_wglGetExtensionsStringEXT(void)
{
    TRACE( "\n" );
    if (TRACE_ON(wgl)) dump_extensions( wgl_extensions );
    return wgl_extensions;
}

static int win32u_wglGetPixelFormat( HDC hdc )
{
    BOOL is_display = is_dc_display( hdc );
    int format;
    HWND hwnd;

    if ((hwnd = NtUserWindowFromDC( hdc )))
        format = get_window_pixel_format( hwnd );
    else if ((format = get_dc_pixel_format( hdc )) < 0)
    {
        WARN( "Invalid DC handle %p\n", hdc );
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return -1;
    }

    /* Offscreen formats can't be used with traditional WGL calls. As has been
     * verified on Windows GetPixelFormat doesn't fail but returns 1.
     */
    if (is_display && format > onscreen_count) format = 1;

    TRACE( "%p/%p -> %d\n", hdc, hwnd, format );
    return format > 0 ? format : 0;
}

static void set_window_opengl_drawable( HWND hwnd, struct opengl_drawable *new_drawable, BOOL current )
{
    struct opengl_drawable *old_drawable = NULL;
    WND *win;

    if ((win = get_win_ptr( hwnd )) && win != WND_DESKTOP && win != WND_OTHER_PROCESS)
    {
        struct opengl_drawable **ptr = current ? &win->current_drawable : &win->unused_drawable;
        old_drawable = *ptr;
        if ((*ptr = new_drawable)) opengl_drawable_add_ref( new_drawable );
        release_win_ptr( win );
    }

    if (old_drawable) opengl_drawable_release( old_drawable );
}

static struct opengl_drawable *get_window_current_drawable( HWND hwnd )
{
    struct opengl_drawable *drawable = NULL;
    WND *win;

    if ((win = get_win_ptr( hwnd )) && win != WND_DESKTOP && win != WND_OTHER_PROCESS)
    {
        if ((drawable = win->current_drawable)) opengl_drawable_add_ref( drawable );
        release_win_ptr( win );
    }

    TRACE( "hwnd %p, drawable %s\n", hwnd, debugstr_opengl_drawable( drawable ) );
    return drawable;
}

static struct opengl_drawable *get_window_unused_drawable( HWND hwnd, int format )
{
    struct opengl_drawable *drawable = NULL;
    WND *win;

    if ((win = get_win_ptr( hwnd )) && win != WND_DESKTOP && win != WND_OTHER_PROCESS)
    {
        drawable = win->unused_drawable;
        win->unused_drawable = NULL;
        release_win_ptr( win );
    }

    if (drawable && drawable->format != format)
    {
        opengl_drawable_release( drawable );
        drawable = NULL;
    }

    /* No compatible window drawable found, try creating a new one. This is not what native
     * is doing, it allows multiple contexts to be current on separate threads on the same
     * window, each drawing to the same back/front buffers. We cannot do that because host
     * OpenGL usually doesn't allow multiple contexts to use the same surface at the same time.
     */
    if (!drawable) driver_funcs->p_surface_create( hwnd, format, &drawable );

    TRACE( "hwnd %p, drawable %s\n", hwnd, debugstr_opengl_drawable( drawable ) );
    return drawable;
}

static void set_dc_opengl_drawable( HDC hdc, struct opengl_drawable *new_drawable )
{
    void *old_drawable = NULL;
    DC *dc;

    TRACE( "hdc %p, new_drawable %s\n", hdc, debugstr_opengl_drawable( new_drawable ) );

    if ((dc = get_dc_ptr( hdc )))
    {
        old_drawable = dc->opengl_drawable;
        if ((dc->opengl_drawable = new_drawable)) opengl_drawable_add_ref( new_drawable );
        release_dc_ptr( dc );
    }

    if (old_drawable) opengl_drawable_release( old_drawable );
}

static struct opengl_drawable *get_dc_opengl_drawable( HDC hdc )
{
    void *drawable = NULL;
    DC *dc;

    if ((dc = get_dc_ptr( hdc )))
    {
        if ((drawable = dc->opengl_drawable)) opengl_drawable_add_ref( drawable );
        release_dc_ptr( dc );
    }

    TRACE( "hdc %p, drawable %s\n", hdc, debugstr_opengl_drawable( drawable ) );
    return drawable;
}

static BOOL create_memory_pbuffer( HDC hdc )
{
    const struct opengl_funcs *funcs = &display_funcs;
    dib_info dib = {.rect = {0, 0, 1, 1}};
    BOOL ret = TRUE;
    BITMAPOBJ *bmp;
    int format = 0;
    DC *dc;

    if (!(dc = get_dc_ptr( hdc ))) return FALSE;
    else if (dc->opengl_drawable) ret = FALSE;
    else if (get_gdi_object_type( hdc ) != NTGDI_OBJ_MEMDC) ret = FALSE;
    else if ((bmp = GDI_GetObjPtr( dc->hBitmap, NTGDI_OBJ_BITMAP )))
    {
        if (!(format = dc->pixel_format)) ret = FALSE;
        init_dib_info_from_bitmapobj( &dib, bmp );
        GDI_ReleaseObj( dc->hBitmap );
    }
    release_dc_ptr( dc );

    if (ret)
    {
        int width = dib.rect.right - dib.rect.left, height = dib.rect.bottom - dib.rect.top;
        struct wgl_pbuffer *pbuffer;

        if (!(pbuffer = funcs->p_wglCreatePbufferARB( hdc, format, width, height, NULL )))
            WARN( "Failed to create pbuffer for memory DC %p\n", hdc );
        else
        {
            TRACE( "Created pbuffer %p for memory DC %p\n", pbuffer, hdc );
            set_dc_opengl_drawable( hdc, pbuffer->drawable );
            funcs->p_wglDestroyPbufferARB( pbuffer );
        }
    }

    return ret;
}

static BOOL flush_memory_dc( struct wgl_context *context, HDC hdc, BOOL write, void (*flush)(void) )
{
    const struct opengl_funcs *funcs = &display_funcs;
    BOOL ret = TRUE;
    BITMAPOBJ *bmp;
    DC *dc;

    if (!(dc = get_dc_ptr( hdc ))) return FALSE;
    if (get_gdi_object_type( hdc ) != NTGDI_OBJ_MEMDC) ret = FALSE;
    else if (context && (bmp = GDI_GetObjPtr( dc->hBitmap, NTGDI_OBJ_BITMAP )))
    {
        char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
        BITMAPINFO *info = (BITMAPINFO *)buffer;
        struct bitblt_coords src = {0};
        struct gdi_image_bits bits;

        if (flush) flush();

        if (!get_image_from_bitmap( bmp, info, &bits, &src ))
        {
            int width = info->bmiHeader.biWidth, height = info->bmiHeader.biSizeImage / 4 / width;
            if (write) funcs->p_glDrawPixels( width, height, GL_BGRA, GL_UNSIGNED_BYTE, bits.ptr );
            else funcs->p_glReadPixels( 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, bits.ptr );
        }
        GDI_ReleaseObj( dc->hBitmap );
    }
    release_dc_ptr( dc );

    return ret;
}

static BOOL win32u_wglSetPixelFormat( HDC hdc, int new_format, const PIXELFORMATDESCRIPTOR *pfd )
{
    const struct opengl_funcs *funcs = &display_funcs;
    UINT total, onscreen;
    HWND hwnd;

    funcs->p_get_pixel_formats( NULL, 0, &total, &onscreen );
    if (new_format <= 0 || new_format > total) return FALSE;

    if ((hwnd = NtUserWindowFromDC( hdc )))
    {
        struct opengl_drawable *drawable;
        int old_format;

        if (new_format > onscreen)
        {
            WARN( "Invalid format %d for %p/%p\n", new_format, hdc, hwnd );
            return FALSE;
        }

        TRACE( "%p/%p format %d\n", hdc, hwnd, new_format );

        if ((old_format = get_window_pixel_format( hwnd ))) return old_format == new_format;

        if (!(drawable = get_window_unused_drawable( hwnd, new_format ))) return FALSE;
        set_window_opengl_drawable( hwnd, drawable, TRUE );
        set_window_opengl_drawable( hwnd, drawable, FALSE );
        opengl_drawable_release( drawable );

        return set_window_pixel_format( hwnd, new_format, FALSE );
    }

    TRACE( "%p/%p format %d\n", hdc, hwnd, new_format );
    return NtGdiSetPixelFormat( hdc, new_format );
}

BOOL set_dc_pixel_format_internal( HDC hdc, int format, struct list *drawables )
{
    DC *dc;

    if (!(dc = get_dc_ptr( hdc ))) return FALSE;
    dc->pixel_format = format;
    if (dc->opengl_drawable) list_add_tail( drawables, &dc->opengl_drawable->entry );
    dc->opengl_drawable = NULL;
    release_dc_ptr( dc );

    return TRUE;
}

void release_opengl_drawables( struct list *drawables )
{
    struct opengl_drawable *drawable, *next;

    LIST_FOR_EACH_ENTRY_SAFE( drawable, next, drawables, struct opengl_drawable, entry )
    {
        list_remove( &drawable->entry );
        opengl_drawable_release( drawable );
    }
}

static BOOL win32u_wglSetPixelFormatWINE( HDC hdc, int format )
{
    struct list *ptr, drawables = LIST_INIT( drawables );
    const struct opengl_funcs *funcs = &display_funcs;
    struct opengl_drawable *drawable;
    UINT total, onscreen;
    HWND hwnd;

    if (!(hwnd = NtUserWindowFromDC( hdc )) || !is_cache_dc( hdc )) return FALSE;
    if (format && get_window_pixel_format( hwnd ) == format) return TRUE;

    TRACE( "%p/%p format %d\n", hdc, hwnd, format );

    funcs->p_get_pixel_formats( NULL, 0, &total, &onscreen );
    if (format < 0 || format > total) return FALSE;
    if (format > onscreen) return FALSE;

    if (!set_dc_pixel_format_internal( hdc, format, &drawables )) return FALSE;
    if (!(ptr = list_head( &drawables ))) drawable = NULL;
    else drawable = LIST_ENTRY( ptr, struct opengl_drawable, entry );

    if (drawable && drawable->format != format)
    {
        opengl_drawable_release( drawable );
        drawable = NULL;
    }
    if (format && (drawable || (drawable = get_window_unused_drawable( hwnd, format ))))
    {
        set_window_opengl_drawable( hwnd, drawable, TRUE );
        set_dc_opengl_drawable( hdc, drawable );
        opengl_drawable_release( drawable );

        set_window_pixel_format( hwnd, format, TRUE );
    }

    return TRUE;
}

static PROC win32u_wglGetProcAddress( const char *name )
{
    PROC ret;
    if (!strncmp( name, "wgl", 3 )) return NULL;
    ret = driver_funcs->p_get_proc_address( name );
    TRACE( "%s -> %p\n", debugstr_a(name), ret );
    return ret;
}

static void win32u_get_pixel_formats( struct wgl_pixel_format *formats, UINT max_formats,
                                      UINT *num_formats, UINT *num_onscreen_formats )
{
    memcpy( formats, pixel_formats, min( max_formats, formats_count ) * sizeof(*pixel_formats) );
    *num_formats = formats_count;
    *num_onscreen_formats = onscreen_count;
}

static void context_exchange_drawables( struct wgl_context *context, struct opengl_drawable **draw, struct opengl_drawable **read )
{
    struct opengl_drawable *old_draw = context->draw, *old_read = context->read;
    context->draw = *draw;
    context->read = *read;
    *draw = old_draw;
    *read = old_read;
}

static BOOL context_unset_current( struct wgl_context *context )
{
    struct opengl_drawable *old_draw = context->draw, *old_read = context->read;

    TRACE( "context %p\n", context );

    opengl_drawable_set_context( old_read, NULL );
    if (old_read != old_draw) opengl_drawable_set_context( old_draw, NULL );
    if (driver_funcs->p_make_current( NULL, NULL, NULL )) return TRUE;
    opengl_drawable_set_context( old_read, context );
    if (old_read != old_draw) opengl_drawable_set_context( old_draw, context );

    return FALSE;
}

/* return an updated drawable, recreating one if the window drawables have been invalidated (mostly wineandroid) */
static struct opengl_drawable *get_updated_drawable( HDC hdc, int format, struct opengl_drawable *drawable )
{
    HWND hwnd = NULL;

    /* return the memory DCs drawables directly */
    if (hdc && !(hwnd = NtUserWindowFromDC( hdc ))) return get_dc_opengl_drawable( hdc );
    if (!hdc && drawable && drawable->client) hwnd = drawable->client->hwnd;
    if (!hwnd) return NULL;

    /* if the drawable we were using is for the same window, keep using it */
    if (drawable && is_client_surface_window( drawable->client, hwnd ))
    {
        opengl_drawable_add_ref( drawable );
        return drawable;
    }

    /* retrieve D3D internal drawables from the DCs if they have any */
    if (hdc && (drawable = get_dc_opengl_drawable( hdc ))) return drawable;

    /* get an updated drawable with the desired format */
    return get_window_unused_drawable( hwnd, format );
}

static BOOL context_sync_drawables( struct wgl_context *context, HDC draw_hdc, HDC read_hdc )
{
    struct opengl_drawable *new_draw, *new_read, *old_draw = NULL, *old_read = NULL;
    struct wgl_context *previous = NtCurrentTeb()->glContext;
    BOOL ret = FALSE;

    if (!(new_draw = get_updated_drawable( draw_hdc, context->format, context->draw ))) return FALSE;
    if (!draw_hdc && context->draw == context->read) opengl_drawable_add_ref( (new_read = new_draw) );
    else if (draw_hdc && draw_hdc == read_hdc) opengl_drawable_add_ref( (new_read = new_draw) );
    else new_read = get_updated_drawable( read_hdc, context->format, context->read );

    TRACE( "context %p, new_draw %s, new_read %s\n", context, debugstr_opengl_drawable( new_draw ), debugstr_opengl_drawable( new_read ) );

    if (!new_draw || !new_read)
    {
        WARN( "One of the drawable has been lost, ignoring\n" );
        return FALSE;
    }

    if (previous == context && new_draw == context->draw && new_read == context->read) ret = TRUE;
    else if (previous)
    {
        context_exchange_drawables( previous, &old_draw, &old_read ); /* take ownership of the previous context drawables */
        opengl_drawable_set_context( old_read, NULL );
        if (old_read != old_draw) opengl_drawable_set_context( old_draw, NULL );
    }

    if (!ret && (ret = driver_funcs->p_make_current( new_draw, new_read, context->driver_private )))
    {
        NtCurrentTeb()->glContext = context;

        if (old_draw && old_draw != new_draw && old_draw != new_read && old_draw->client)
            set_window_opengl_drawable( old_draw->client->hwnd, old_draw, FALSE );
        if (old_read && old_read != new_draw && old_read != new_read && old_read->client)
            set_window_opengl_drawable( old_read->client->hwnd, old_read, FALSE );

        /* all good, release previous context drawables if any */
        if (old_draw) opengl_drawable_release( old_draw );
        if (old_read) opengl_drawable_release( old_read );

        opengl_drawable_set_context( new_read, context );
        if (new_read != new_draw) opengl_drawable_set_context( new_draw, context );

        opengl_drawable_flush( new_read, new_read->interval, 0 );
        opengl_drawable_flush( new_draw, new_draw->interval, GL_FLUSH_PRESENT );
    }

    if (ret)
    {
        /* update the current window drawable to the last used draw surface */
        if (new_draw->client) set_window_opengl_drawable( new_draw->client->hwnd, new_draw, TRUE );
        context_exchange_drawables( context, &new_draw, &new_read );
    }
    else if (previous)
    {
        opengl_drawable_set_context( old_read, context );
        if (old_read != old_draw) opengl_drawable_set_context( old_draw, context );
        context_exchange_drawables( previous, &old_draw, &old_read ); /* give back ownership of the previous drawables */
        assert( !old_draw && !old_read );
    }

    if (new_draw) opengl_drawable_release( new_draw );
    if (new_read) opengl_drawable_release( new_read );
    return ret;
}

static void push_internal_context( struct wgl_context *context, HDC hdc, int format )
{
    TRACE( "context %p, hdc %p\n", context, hdc );

    if (!context->internal_context)
    {
        driver_funcs->p_context_create( format, context->driver_private, NULL, &context->internal_context );
        if (!context->internal_context) ERR( "Failed to create internal context\n" );
    }

    driver_funcs->p_make_current( context->draw, context->read, context->internal_context );
}

static void pop_internal_context( struct wgl_context *context )
{
    TRACE( "context %p\n", context );
    driver_funcs->p_make_current( context->draw, context->read, context->driver_private );
}

static BOOL win32u_wglMakeContextCurrentARB( HDC draw_hdc, HDC read_hdc, struct wgl_context *context )
{
    struct wgl_context *prev_context = NtCurrentTeb()->glContext;
    BOOL created;
    int format;

    TRACE( "draw_hdc %p, read_hdc %p, context %p\n", draw_hdc, read_hdc, context );

    if (!context)
    {
        struct opengl_drawable *draw = NULL, *read = NULL;

        if (!(context = prev_context)) return TRUE;
        if (!context_unset_current( context )) return FALSE;
        NtCurrentTeb()->glContext = NULL;

        context_exchange_drawables( context, &draw, &read );
        if (draw->client) set_window_opengl_drawable( draw->client->hwnd, draw, FALSE );
        opengl_drawable_release( draw );
        if (read->client) set_window_opengl_drawable( read->client->hwnd, read, FALSE );
        opengl_drawable_release( read );

        return TRUE;
    }

    if ((format = get_dc_pixel_format( draw_hdc )) <= 0 &&
        (format = get_window_pixel_format( NtUserWindowFromDC( draw_hdc ) )) <= 0)
    {
        WARN( "Invalid draw_hdc %p format %u\n", draw_hdc, format );
        if (!format) RtlSetLastWin32Error( ERROR_INVALID_PIXEL_FORMAT );
        else RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    if (context->format != format)
    {
        WARN( "Mismatched draw_hdc %p format %u, context %p format %u\n", draw_hdc, format, context, context->format );
        RtlSetLastWin32Error( ERROR_INVALID_PIXEL_FORMAT );
        return FALSE;
    }

    created = create_memory_pbuffer( draw_hdc );
    if (!context_sync_drawables( context, draw_hdc, read_hdc )) return FALSE;
    NtCurrentTeb()->glContext = context;
    if (created) flush_memory_dc( context, draw_hdc, TRUE, NULL );

    return TRUE;
}

static BOOL win32u_wglMakeCurrent( HDC hdc, struct wgl_context *context )
{
    return win32u_wglMakeContextCurrentARB( hdc, hdc, context );
}

static struct wgl_pbuffer *win32u_wglCreatePbufferARB( HDC hdc, int format, int width, int height,
                                                       const int *attribs )
{
    const struct opengl_funcs *funcs = &display_funcs;
    UINT total, onscreen, size, max_level = 0;
    struct wgl_pbuffer *pbuffer;
    BOOL largest = FALSE;

    TRACE( "(%p, %d, %d, %d, %p)\n", hdc, format, width, height, attribs );

    funcs->p_get_pixel_formats( NULL, 0, &total, &onscreen );
    if (format <= 0 || format > total)
    {
        RtlSetLastWin32Error( ERROR_INVALID_PIXEL_FORMAT );
        return NULL;
    }
    if (width <= 0 || height <= 0)
    {
        RtlSetLastWin32Error( ERROR_INVALID_DATA );
        return NULL;
    }

    if (!(pbuffer = calloc( 1, sizeof(*pbuffer) )) || !(pbuffer->hdc = NtGdiOpenDCW( NULL, NULL, NULL, 0, TRUE, NULL, NULL, NULL )))
    {
        RtlSetLastWin32Error( ERROR_NO_SYSTEM_RESOURCES );
        free( pbuffer );
        return NULL;
    }
    NtGdiSetPixelFormat( pbuffer->hdc, format );
    pbuffer->width = width;
    pbuffer->height = height;
    pbuffer->mipmap_level = -1;

    for (; attribs && attribs[0]; attribs += 2)
    {
        switch (attribs[0])
        {
        case WGL_PBUFFER_LARGEST_ARB:
            TRACE( "WGL_PBUFFER_LARGEST_ARB %#x\n", attribs[1] );
            largest = !!attribs[1];
            break;

        case WGL_TEXTURE_FORMAT_ARB:
            TRACE( "WGL_TEXTURE_FORMAT_ARB %#x\n", attribs[1] );
            switch (attribs[1])
            {
            case WGL_NO_TEXTURE_ARB:
                pbuffer->texture_format = 0;
                break;
            case WGL_TEXTURE_RGB_ARB:
                pbuffer->texture_format = GL_RGB;
                break;
            case WGL_TEXTURE_RGBA_ARB:
                pbuffer->texture_format = GL_RGBA;
                break;
            /* WGL_FLOAT_COMPONENTS_NV */
            case WGL_TEXTURE_FLOAT_R_NV:
                pbuffer->texture_format = GL_FLOAT_R_NV;
                break;
            case WGL_TEXTURE_FLOAT_RG_NV:
                pbuffer->texture_format = GL_FLOAT_RG_NV;
                break;
            case WGL_TEXTURE_FLOAT_RGB_NV:
                pbuffer->texture_format = GL_FLOAT_RGB_NV;
                break;
            case WGL_TEXTURE_FLOAT_RGBA_NV:
                pbuffer->texture_format = GL_FLOAT_RGBA_NV;
                break;
            default:
                FIXME( "Unknown texture format: %x\n", attribs[1] );
                goto failed;
            }
            break;

        case WGL_TEXTURE_TARGET_ARB:
            TRACE( "WGL_TEXTURE_TARGET_ARB %#x\n", attribs[1] );
            switch (attribs[1])
            {
            case WGL_NO_TEXTURE_ARB:
                pbuffer->texture_target = 0;
                break;
            case WGL_TEXTURE_CUBE_MAP_ARB:
                if (width != height) goto failed;
                pbuffer->texture_target = GL_TEXTURE_CUBE_MAP;
                break;
            case WGL_TEXTURE_1D_ARB:
                if (height != 1) goto failed;
                pbuffer->texture_target = GL_TEXTURE_1D;
                break;
            case WGL_TEXTURE_2D_ARB:
                pbuffer->texture_target = GL_TEXTURE_2D;
                break;
            case WGL_TEXTURE_RECTANGLE_NV:
                pbuffer->texture_target = GL_TEXTURE_RECTANGLE_NV;
                break;
            default:
                FIXME( "Unknown texture target: %x\n", attribs[1] );
                goto failed;
            }
            break;

        case WGL_MIPMAP_TEXTURE_ARB:
            TRACE( "WGL_MIPMAP_TEXTURE_ARB %#x\n", attribs[1] );
            if (attribs[1])
            {
                pbuffer->mipmap_level = max_level = 0;
                for (size = min( width, height ) / 2; size; size /= 2) max_level++;
            }
            break;

        default:
            WARN( "attribute %#x %#x not handled\n", attribs[0], attribs[1] );
            break;
        }
    }

    if (driver_funcs->p_pbuffer_create( pbuffer->hdc, format, largest, pbuffer->texture_format,
                                        pbuffer->texture_target, max_level, &pbuffer->width,
                                        &pbuffer->height, &pbuffer->drawable ))
    {
        set_dc_opengl_drawable( pbuffer->hdc, pbuffer->drawable );
        return pbuffer;
    }

failed:
    RtlSetLastWin32Error( ERROR_INVALID_DATA );
    NtGdiDeleteObjectApp( pbuffer->hdc );
    free( pbuffer );
    return NULL;
}

static BOOL win32u_wglDestroyPbufferARB( struct wgl_pbuffer *pbuffer )
{
    TRACE( "pbuffer %p\n", pbuffer );

    opengl_drawable_release( pbuffer->drawable );
    NtGdiDeleteObjectApp( pbuffer->hdc );
    free( pbuffer );

    return GL_TRUE;
}

static HDC win32u_wglGetPbufferDCARB( struct wgl_pbuffer *pbuffer )
{
    TRACE( "pbuffer %p\n", pbuffer );
    return pbuffer->hdc;
}

static int win32u_wglReleasePbufferDCARB( struct wgl_pbuffer *pbuffer, HDC hdc )
{
    TRACE( "pbuffer %p, hdc %p\n", pbuffer, hdc );

    if (hdc != pbuffer->hdc)
    {
        RtlSetLastWin32Error( ERROR_DC_NOT_FOUND );
        return FALSE;
    }

    return TRUE;
}

static BOOL win32u_wglQueryPbufferARB( struct wgl_pbuffer *pbuffer, int attrib, int *value )
{
    TRACE( "pbuffer %p, attrib %#x, value %p\n", pbuffer, attrib, value );

    switch (attrib)
    {
    case WGL_PBUFFER_WIDTH_ARB:
        *value = pbuffer->width;
        break;
    case WGL_PBUFFER_HEIGHT_ARB:
        *value = pbuffer->height;
        break;
    case WGL_PBUFFER_LOST_ARB:
        *value = GL_FALSE;
        break;

    case WGL_TEXTURE_FORMAT_ARB:
        switch (pbuffer->texture_format)
        {
        case 0: *value = WGL_NO_TEXTURE_ARB; break;
        case GL_RGB: *value = WGL_TEXTURE_RGB_ARB; break;
        case GL_RGBA: *value = WGL_TEXTURE_RGBA_ARB; break;
        /* WGL_FLOAT_COMPONENTS_NV */
        case GL_FLOAT_R_NV: *value = WGL_TEXTURE_FLOAT_R_NV; break;
        case GL_FLOAT_RG_NV: *value = WGL_TEXTURE_FLOAT_RG_NV; break;
        case GL_FLOAT_RGB_NV: *value = WGL_TEXTURE_FLOAT_RGB_NV; break;
        case GL_FLOAT_RGBA_NV: *value = WGL_TEXTURE_FLOAT_RGBA_NV; break;
        default: ERR( "Unknown texture format: %x\n", pbuffer->texture_format );
        }
        break;

    case WGL_TEXTURE_TARGET_ARB:
        switch (pbuffer->texture_target)
        {
        case 0: *value = WGL_NO_TEXTURE_ARB; break;
        case GL_TEXTURE_1D: *value = WGL_TEXTURE_1D_ARB; break;
        case GL_TEXTURE_2D: *value = WGL_TEXTURE_2D_ARB; break;
        case GL_TEXTURE_CUBE_MAP: *value = WGL_TEXTURE_CUBE_MAP_ARB; break;
        case GL_TEXTURE_RECTANGLE_NV: *value = WGL_TEXTURE_RECTANGLE_NV; break;
        }
        break;

    case WGL_MIPMAP_TEXTURE_ARB:
        *value = pbuffer->mipmap_level >= 0;
        break;
    case WGL_MIPMAP_LEVEL_ARB:
        *value = max( pbuffer->mipmap_level, 0 );
        break;
    case WGL_CUBE_MAP_FACE_ARB:
        switch (pbuffer->cube_face)
        {
        case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
        default:
            *value = WGL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB;
            break;
        case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
            *value = WGL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB;
            break;
        case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
            *value = WGL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB;
            break;
        case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
            *value = WGL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB;
            break;
        case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
            *value = WGL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB;
            break;
        case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
            *value = WGL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB;
            break;
        }
        break;

    default:
        FIXME( "unexpected attribute %x\n", attrib );
        break;
    }

    return GL_TRUE;
}

static GLenum binding_from_target( GLenum target )
{
    switch (target)
    {
    case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_TEXTURE_1D: return GL_TEXTURE_BINDING_1D;
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_RECTANGLE_NV: return GL_TEXTURE_BINDING_RECTANGLE_NV;
    }
    FIXME( "Unsupported target %#x\n", target );
    return 0;
}

static BOOL win32u_wglBindTexImageARB( struct wgl_pbuffer *pbuffer, int buffer )
{
    const struct opengl_funcs *funcs = &display_funcs;
    int prev_texture = 0, format = win32u_wglGetPixelFormat( pbuffer->hdc );
    struct wgl_pixel_format desc;
    GLenum source;
    UINT ret;

    TRACE( "pbuffer %p, buffer %d\n", pbuffer, buffer );

    if (!pbuffer->texture_format)
    {
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return GL_FALSE;
    }

    if (!driver_funcs->p_describe_pixel_format( format, &desc ))
    {
        RtlSetLastWin32Error( ERROR_INVALID_PIXEL_FORMAT );
        return FALSE;
    }

    switch (buffer)
    {
    case WGL_FRONT_LEFT_ARB:
        if (desc.pfd.dwFlags & PFD_STEREO) source = GL_FRONT_LEFT;
        else source = GL_FRONT;
        break;
    case WGL_FRONT_RIGHT_ARB:
        source = GL_FRONT_RIGHT;
        break;
    case WGL_BACK_LEFT_ARB:
        if (desc.pfd.dwFlags & PFD_STEREO) source = GL_BACK_LEFT;
        else source = GL_BACK;
        break;
    case WGL_BACK_RIGHT_ARB:
        source = GL_BACK_RIGHT;
        break;
    case WGL_AUX0_ARB: source = GL_AUX0; break;
    case WGL_AUX1_ARB: source = GL_AUX1; break;
    case WGL_AUX2_ARB: source = GL_AUX2; break;
    case WGL_AUX3_ARB: source = GL_AUX3; break;

    case WGL_AUX4_ARB:
    case WGL_AUX5_ARB:
    case WGL_AUX6_ARB:
    case WGL_AUX7_ARB:
    case WGL_AUX8_ARB:
    case WGL_AUX9_ARB:
        FIXME( "Unsupported source buffer %#x\n", buffer );
        RtlSetLastWin32Error( ERROR_INVALID_DATA );
        return GL_FALSE;

    default:
        WARN( "Unknown source buffer %#x\n", buffer );
        RtlSetLastWin32Error( ERROR_INVALID_DATA );
        return GL_FALSE;
    }

    if ((ret = driver_funcs->p_pbuffer_bind( pbuffer->hdc, pbuffer->drawable, source )) != -1)
        return ret;

    funcs->p_glGetIntegerv( binding_from_target( pbuffer->texture_target ), &prev_texture );
    push_internal_context( NtCurrentTeb()->glContext, pbuffer->hdc, format );

    /* Make sure that the prev_texture is set as the current texture state isn't shared
     * between contexts. After that copy the pbuffer texture data. */
    funcs->p_glBindTexture( pbuffer->texture_target, prev_texture );
    funcs->p_glCopyTexImage2D( pbuffer->texture_target, 0, pbuffer->texture_format, 0, 0,
                                        pbuffer->width, pbuffer->height, 0 );

    pop_internal_context( NtCurrentTeb()->glContext );
    return GL_TRUE;
}

static BOOL win32u_wglReleaseTexImageARB( struct wgl_pbuffer *pbuffer, int buffer )
{
    TRACE( "pbuffer %p, buffer %d\n", pbuffer, buffer );

    if (!pbuffer->texture_format)
    {
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return GL_FALSE;
    }

    return !!driver_funcs->p_pbuffer_bind( pbuffer->hdc, pbuffer->drawable, GL_NONE );
}

static BOOL win32u_wglSetPbufferAttribARB( struct wgl_pbuffer *pbuffer, const int *attribs )
{
    TRACE( "pbuffer %p, attribs %p\n", pbuffer, attribs );

    if (!pbuffer->texture_format)
    {
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return GL_FALSE;
    }

    for (; attribs && attribs[0]; attribs += 2)
    {
        switch (attribs[0])
        {
        case WGL_MIPMAP_LEVEL_ARB:
            TRACE( "WGL_MIPMAP_LEVEL_ARB %#x\n", attribs[1] );
            pbuffer->mipmap_level = attribs[1];
            break;

        case WGL_CUBE_MAP_FACE_ARB:
            TRACE( "WGL_CUBE_MAP_FACE_ARB %#x\n", attribs[1] );
            switch (attribs[1])
            {
            case WGL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
                break;
            case WGL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_NEGATIVE_X;
                break;
            case WGL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_POSITIVE_Y;
                break;
            case WGL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_NEGATIVE_Y;
                break;
            case WGL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_POSITIVE_Z;
                break;
            case WGL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB:
                pbuffer->cube_face = GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
                break;
            default:
                FIXME( "Unknown texture face: %x\n", attribs[1] );
                RtlSetLastWin32Error( ERROR_INVALID_DATA );
                return GL_FALSE;
            }
            break;

        default:
            FIXME( "Invalid attribute 0x%x\n", attribs[0] );
            RtlSetLastWin32Error( ERROR_INVALID_DATA );
            return GL_FALSE;
        }
    }

    return driver_funcs->p_pbuffer_updated( pbuffer->hdc, pbuffer->drawable, pbuffer->cube_face,
                                            max( pbuffer->mipmap_level, 0 ) );
}

static int get_window_swap_interval( HWND hwnd )
{
    int interval;
    WND *win;

    if (!(win = get_win_ptr( hwnd )) || win == WND_DESKTOP || win == WND_OTHER_PROCESS) return 0;
    interval = win->swap_interval;
    release_win_ptr( win );

    return interval;
}

static BOOL win32u_wgl_context_reset( struct wgl_context *context, HDC hdc, struct wgl_context *share, const int *attribs )
{
    void *share_private = share ? share->driver_private : NULL;
    int format;

    TRACE( "context %p, hdc %p, share %p, attribs %p\n", context, hdc, share, attribs );

    if (context->internal_context)
    {
        driver_funcs->p_context_destroy( context->internal_context );
        context->internal_context = NULL;
    }
    if (context->driver_private && !driver_funcs->p_context_destroy( context->driver_private ))
    {
        WARN( "Failed to destroy driver context %p\n", context->driver_private );
        return FALSE;
    }
    context->driver_private = NULL;
    if (!hdc) return TRUE;

    if ((format = get_dc_pixel_format( hdc )) <= 0 &&
        (format = get_window_pixel_format( NtUserWindowFromDC( hdc ) )) <= 0)
    {
        if (!format) RtlSetLastWin32Error( ERROR_INVALID_PIXEL_FORMAT );
        else RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    if (!driver_funcs->p_context_create( format, share_private, attribs, &context->driver_private ))
    {
        WARN( "Failed to create driver context for context %p\n", context );
        return FALSE;
    }
    context->format = format;

    TRACE( "reset context %p, format %u for driver context %p\n", context, format, context->driver_private );
    return TRUE;
}

static BOOL flush_memory_pbuffer( void (*flush)(void) )
{
    HDC draw_hdc = NtCurrentTeb()->glReserved1[0], read_hdc = NtCurrentTeb()->glReserved1[1];
    struct wgl_context *context = NtCurrentTeb()->glContext;
    BOOL created;

    TRACE( "context %p, draw_hdc %p, read_hdc %p, flush %p\n", context, draw_hdc, read_hdc, flush );

    created = create_memory_pbuffer( draw_hdc );
    if (context) context_sync_drawables( context, draw_hdc, read_hdc );
    if (created) flush_memory_dc( context, draw_hdc, TRUE, NULL );
    return flush_memory_dc( context, draw_hdc, FALSE, flush );
}

static BOOL win32u_wgl_context_flush( struct wgl_context *context, void (*flush)(void), UINT flags )
{
    const struct opengl_funcs *funcs = &display_funcs;
    struct opengl_drawable *draw = context->draw;
    int interval;

    if (!draw->client) return flush_memory_pbuffer( flush );
    interval = get_window_swap_interval( draw->client->hwnd );
    if (flags & GL_FLUSH_FORCE_SWAP) interval = 0;

    TRACE( "context %p, hwnd %p, interval %d, flush %p, flags %#x\n", context, draw->client->hwnd, interval, flush, flags );

    context_sync_drawables( context, 0, 0 );
    if (!(draw = context->draw)) return FALSE; /* should never happen */

    if (flush) flush();
    if (flush == funcs->p_glFinish) flags |= GL_FLUSH_FINISHED;
    opengl_drawable_flush( draw, interval, flags );
    if (flags & GL_FLUSH_FORCE_SWAP) opengl_drawable_swap( draw );

    return TRUE;
}

static BOOL win32u_wglSwapBuffers( HDC hdc )
{
    struct wgl_context *context = NtCurrentTeb()->glContext;
    const struct opengl_funcs *funcs = &display_funcs;
    struct opengl_drawable *draw;
    int interval;
    HWND hwnd;
    BOOL ret;

    if (!(hwnd = NtUserWindowFromDC( hdc ))) return flush_memory_pbuffer( funcs->p_glFlush );

    interval = get_window_swap_interval( hwnd );

    TRACE( "context %p, hwnd %p, hdc %p, interval %d\n", context, hwnd, hdc, interval );

    if (context) context_sync_drawables( context, 0, 0 );

    if (context) opengl_drawable_add_ref( (draw = context->draw) );
    else if (!(draw = get_window_current_drawable( hwnd ))) return FALSE;

    opengl_drawable_flush( draw, interval, 0 );
    ret = opengl_drawable_swap( draw );
    opengl_drawable_release( draw );

    return ret;
}

static BOOL win32u_wglSwapIntervalEXT( int interval )
{
    HDC hdc = NtCurrentTeb()->glReserved1[0];
    HWND hwnd;
    WND *win;

    if (!(hwnd = NtUserWindowFromDC( hdc )) || !(win = get_win_ptr( hwnd )))
    {
        RtlSetLastWin32Error( ERROR_DC_NOT_FOUND );
        return FALSE;
    }
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS)
    {
        WARN( "setting swap interval on win %p not supported\n", hwnd );
        return TRUE;
    }

    TRACE( "setting window %p swap interval %d\n", hwnd, interval );
    win->swap_interval = interval;
    release_win_ptr( win );
    return TRUE;
}

static int win32u_wglGetSwapIntervalEXT(void)
{
    HDC hdc = NtCurrentTeb()->glReserved1[0];
    int interval;
    HWND hwnd;
    WND *win;

    if (!(hwnd = NtUserWindowFromDC( hdc )) || !(win = get_win_ptr( hwnd )))
    {
        RtlSetLastWin32Error( ERROR_DC_NOT_FOUND );
        return 0;
    }
    if (win == WND_DESKTOP || win == WND_OTHER_PROCESS)
    {
        WARN( "setting swap interval on win %p not supported\n", hwnd );
        return TRUE;
    }
    interval = win->swap_interval;
    release_win_ptr( win );

    return interval;
}

static void set_gl_error( GLenum error )
{
    struct wgl_context *ctx = NtCurrentTeb()->glContext;
    const struct opengl_funcs *funcs = &display_funcs;

    if (!ctx || ctx->error) return;
    if ((ctx->error = funcs->p_glGetError())) return;
    ctx->error = error;
}

static struct egl_platform *egl_platform_from_index( GLint index )
{
    struct egl_platform *egl;

    LIST_FOR_EACH_ENTRY( egl, &devices_egl, struct egl_platform, entry )
        if (egl->index == index) return egl;

    WARN( "Cannot find renderer at index %d\n", index );
    return NULL;
}

static BOOL query_renderer_integer( struct egl_platform *egl, GLenum attribute, GLuint *value )
{
    switch (attribute)
    {
    case WGL_RENDERER_ACCELERATED_WINE: *value = egl->accelerated; return TRUE;
    case WGL_RENDERER_DEVICE_ID_WINE: *value = egl->device_id; return TRUE;
    case WGL_RENDERER_VENDOR_ID_WINE: *value = egl->vendor_id; return TRUE;
    case WGL_RENDERER_UNIFIED_MEMORY_ARCHITECTURE_WINE: *value = 0; return TRUE;
    case WGL_RENDERER_VERSION_WINE: memcpy( value, egl->version, 3 ); return TRUE;
    case WGL_RENDERER_OPENGL_COMPATIBILITY_PROFILE_VERSION_WINE:
        value[0] = egl->compat_version / 10;
        value[1] = egl->compat_version % 10;
        return TRUE;
    case WGL_RENDERER_OPENGL_CORE_PROFILE_VERSION_WINE:
        value[0] = egl->core_version / 10;
        value[1] = egl->core_version % 10;
        return TRUE;
    case WGL_RENDERER_PREFERRED_PROFILE_WINE:
        *value = egl->core_version ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        return TRUE;
    case WGL_RENDERER_VIDEO_MEMORY_WINE: *value = egl->video_memory; return TRUE;
    default:
        FIXME( "Unsupported attribute %#x\n", attribute );
        set_gl_error( GL_INVALID_ENUM );
        return FALSE;
    }
}

static BOOL win32u_wglQueryRendererIntegerWINE( HDC hdc, GLint renderer, GLenum attribute, GLuint *value )
{
    struct egl_platform *egl;

    TRACE( "hdc %p, renderer %u, attribute %#x, value %p\n", hdc, renderer, attribute, value );

    if (!(egl = egl_platform_from_index( renderer )))
    {
        set_gl_error( GL_INVALID_INDEX );
        return FALSE;
    }
    return query_renderer_integer( egl, attribute, value );
}

static const char *query_renderer_string( struct egl_platform *egl, GLenum attribute )
{
    switch (attribute)
    {
    case WGL_RENDERER_DEVICE_ID_WINE: return egl->device_name;
    case WGL_RENDERER_VENDOR_ID_WINE: return egl->vendor_name;
    default:
        FIXME( "Unsupported attribute %#x\n", attribute );
        set_gl_error( GL_INVALID_ENUM );
        return NULL;
    }
}

static const char *win32u_wglQueryRendererStringWINE( HDC hdc, GLint renderer, GLenum attribute )
{
    struct egl_platform *egl;

    TRACE( "hdc %p, renderer %u, attribute %#x\n", hdc, renderer, attribute );

    if (!(egl = egl_platform_from_index( renderer )))
    {
        set_gl_error( GL_INVALID_INDEX );
        return NULL;
    }
    return query_renderer_string( egl, attribute );
}

static BOOL win32u_wglQueryCurrentRendererIntegerWINE( GLenum attribute, GLuint *value )
{
    struct list *ptr;

    TRACE( "attribute %#x, value %p\n", attribute, value );

    if (!(ptr = list_head( &devices_egl )))
    {
        set_gl_error( GL_INVALID_OPERATION );
        return FALSE;
    }
    return query_renderer_integer( LIST_ENTRY( ptr, struct egl_platform, entry ), attribute, value );
}

static const char *win32u_wglQueryCurrentRendererStringWINE( GLenum attribute )
{
    struct list *ptr;

    TRACE( "attribute %#x\n", attribute );

    if (!(ptr = list_head( &devices_egl )))
    {
        set_gl_error( GL_INVALID_OPERATION );
        return NULL;
    }
    return query_renderer_string( LIST_ENTRY( ptr, struct egl_platform, entry ), attribute );
}

static void display_funcs_init(void)
{
    struct egl_platform *egl;
    UINT status;

    if (egl_init( &driver_funcs )) TRACE( "Initialized EGL library\n" );

    if ((status = user_driver->pOpenGLInit( WINE_OPENGL_DRIVER_VERSION, &display_funcs, &driver_funcs )))
        WARN( "Failed to initialize the driver OpenGL functions, status %#x\n", status );
    init_egl_platforms( &display_funcs, driver_funcs );

    formats_count = driver_funcs->p_init_pixel_formats( &onscreen_count );
    if (!(pixel_formats = malloc( formats_count * sizeof(*pixel_formats) ))) ERR( "Failed to allocate memory for pixel formats\n" );
    else for (int i = 0; i < formats_count; i++) driver_funcs->p_describe_pixel_format( i + 1, pixel_formats + i );

#define USE_GL_FUNC(func) \
    if (!display_funcs.p_##func && !(display_funcs.p_##func = driver_funcs->p_get_proc_address( #func ))) \
        WARN( "%s not found.\n", #func );
    ALL_GL_FUNCS
    USE_GL_FUNC(glBindBuffer)
    USE_GL_FUNC(glBlitNamedFramebuffer)
    USE_GL_FUNC(glBindFramebuffer)
    USE_GL_FUNC(glCheckNamedFramebufferStatus)
    USE_GL_FUNC(glCreateFramebuffers)
    USE_GL_FUNC(glCreateRenderbuffers)
    USE_GL_FUNC(glCreateTextures)
    USE_GL_FUNC(glDebugMessageInsert)
    USE_GL_FUNC(glDeleteFramebuffers)
    USE_GL_FUNC(glDeleteRenderbuffers)
    USE_GL_FUNC(glGetNamedFramebufferAttachmentParameteriv)
    USE_GL_FUNC(glNamedFramebufferDrawBuffer)
    USE_GL_FUNC(glNamedFramebufferReadBuffer)
    USE_GL_FUNC(glNamedFramebufferRenderbuffer)
    USE_GL_FUNC(glNamedFramebufferTexture)
    USE_GL_FUNC(glNamedRenderbufferStorageMultisample)
    USE_GL_FUNC(glTextureStorage2D)
#undef USE_GL_FUNC

    display_funcs.p_wglGetProcAddress = win32u_wglGetProcAddress;
    display_funcs.p_get_pixel_formats = win32u_get_pixel_formats;

    strcpy( wgl_extensions, driver_funcs->p_init_wgl_extensions( &display_funcs ) );
    display_funcs.p_wglGetPixelFormat = win32u_wglGetPixelFormat;
    display_funcs.p_wglSetPixelFormat = win32u_wglSetPixelFormat;

    display_funcs.p_wglCreateContext = (void *)1; /* never called */
    display_funcs.p_wglDeleteContext = (void *)1; /* never called */
    display_funcs.p_wglCopyContext = (void *)1; /* never called */
    display_funcs.p_wglShareLists = (void *)1; /* never called */
    display_funcs.p_wglMakeCurrent = win32u_wglMakeCurrent;

    display_funcs.p_wglSwapBuffers = win32u_wglSwapBuffers;
    display_funcs.p_wgl_context_reset = win32u_wgl_context_reset;
    display_funcs.p_wgl_context_flush = win32u_wgl_context_flush;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_pixel_format" );
    display_funcs.p_wglChoosePixelFormatARB      = (void *)1; /* never called */
    display_funcs.p_wglGetPixelFormatAttribfvARB = (void *)1; /* never called */
    display_funcs.p_wglGetPixelFormatAttribivARB = (void *)1; /* never called */

    if (display_egl.has_EGL_EXT_pixel_format_float)
    {
        register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_pixel_format_float" );
        register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ATI_pixel_format_float" );
    }

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_extensions_string" );
    display_funcs.p_wglGetExtensionsStringARB = win32u_wglGetExtensionsStringARB;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_EXT_extensions_string" );
    display_funcs.p_wglGetExtensionsStringEXT = win32u_wglGetExtensionsStringEXT;

    /* In WineD3D we need the ability to set the pixel format more than once (e.g. after a device reset).
     * The default wglSetPixelFormat doesn't allow this, so add our own which allows it.
     */
    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_WINE_pixel_format_passthrough" );
    display_funcs.p_wglSetPixelFormatWINE = win32u_wglSetPixelFormatWINE;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_create_context" );
    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_create_context_no_error" );
    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_create_context_profile" );
    display_funcs.p_wglCreateContextAttribsARB = (void *)1; /* never called */

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_make_current_read" );
    display_funcs.p_wglGetCurrentReadDCARB   = (void *)1;  /* never called */
    display_funcs.p_wglMakeContextCurrentARB = win32u_wglMakeContextCurrentARB;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_pbuffer" );
    display_funcs.p_wglCreatePbufferARB    = win32u_wglCreatePbufferARB;
    display_funcs.p_wglDestroyPbufferARB   = win32u_wglDestroyPbufferARB;
    display_funcs.p_wglGetPbufferDCARB     = win32u_wglGetPbufferDCARB;
    display_funcs.p_wglReleasePbufferDCARB = win32u_wglReleasePbufferDCARB;
    display_funcs.p_wglQueryPbufferARB     = win32u_wglQueryPbufferARB;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_ARB_render_texture" );
    display_funcs.p_wglBindTexImageARB     = win32u_wglBindTexImageARB;
    display_funcs.p_wglReleaseTexImageARB  = win32u_wglReleaseTexImageARB;
    display_funcs.p_wglSetPbufferAttribARB = win32u_wglSetPbufferAttribARB;

    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_EXT_swap_control" );
    register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_EXT_swap_control_tear" );
    display_funcs.p_wglSwapIntervalEXT = win32u_wglSwapIntervalEXT;
    display_funcs.p_wglGetSwapIntervalEXT = win32u_wglGetSwapIntervalEXT;

    if (!list_empty( &devices_egl ))
    {
        register_extension( wgl_extensions, ARRAY_SIZE(wgl_extensions), "WGL_WINE_query_renderer" );
        display_funcs.p_wglQueryCurrentRendererIntegerWINE = win32u_wglQueryCurrentRendererIntegerWINE;
        display_funcs.p_wglQueryCurrentRendererStringWINE = win32u_wglQueryCurrentRendererStringWINE;
        display_funcs.p_wglQueryRendererIntegerWINE = win32u_wglQueryRendererIntegerWINE;
        display_funcs.p_wglQueryRendererStringWINE = win32u_wglQueryRendererStringWINE;
        LIST_FOR_EACH_ENTRY( egl, &devices_egl, struct egl_platform, entry )
            init_device_info( egl, &display_funcs );
    }
}

/***********************************************************************
 *      __wine_get_wgl_driver  (win32u.@)
 */
const struct opengl_funcs *__wine_get_opengl_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but dibdrv has %u\n",
             version, WINE_OPENGL_DRIVER_VERSION );
        return NULL;
    }

    pthread_once( &init_once, display_funcs_init );
    return &display_funcs;
}
