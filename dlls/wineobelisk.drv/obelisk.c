/*
 * wineobelisk.drv unix side: user driver backed by the Obelisk window server wsd
 *
 * One AF_UNIX connection per process. Top-level visible windows get a wsd window: a macOS titled window
 * (title bar and buttons drawn by macOS, like winemac.drv) when the Win32 window has a caption, a borderless
 * one otherwise (Wine draws any frame itself). Their window surface is copied into a shared memory buffer
 * (memfd sent with SCM_RIGHTS) and presented. A Win32 thread created by the PE side reads the wsd events and
 * injects keyboard and mouse input with NtUserSendHardwareInput; moves, resizes and minimizes made by the user
 * on the macOS window are applied to the Win32 window by its own thread (a posted driver message).
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

#define OEMRESOURCE

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"
#include "wine/debug.h"
#include "unixlib.h"
#include "wsd_protocol.h"

WINE_DEFAULT_DEBUG_CHANNEL(obelisk);

#define SHM_ALIGN      0x4000  /* host page; wsd maps the whole fd */
#define STRIDE_ALIGN   256     /* multiple of every Metal linear texture alignment */
#define NB_BUFFERS     2

struct obelisk_buffer
{
    UINT  id;        /* wsd buffer id, 0 when unused */
    void *bits;
    size_t map_size;
    UINT  width, height, stride;
    BOOL  busy;      /* presented, no EV_RELEASE yet */
};

struct obelisk_win
{
    HWND  hwnd;      /* 0 when the slot is free */
    BOOL  created;   /* wsd window exists */
    BOOL  shown;
    BOOL  fullscreen;
    BOOL  minimized; /* in the Dock */
    BOOL  external;  /* a D3D swapchain (nptunix) presents into this window, GDI flushes are not shown */
    UINT  style;     /* WSD_STYLE_* frame of the wsd window */
    UINT  width, height;
    int   x, y;      /* last position sent to wsd */
    RECT  window;    /* window rect in screen coordinates */
    RECT  visible;   /* visible rect in screen coordinates */
    RECT  client;    /* client rect relative to the visible rect (window pixels) */
    BOOL  frame_pending;
    RECT  pending;   /* visible rect the user gave the macOS window, applied by the window's thread */
    struct obelisk_buffer buffers[NB_BUFFERS];
};

/* driver messages, handled by OBELISK_WindowMessage in the window's thread */
enum { WM_OBELISK_APPLY_FRAME = WM_WINE_FIRST_DRIVER_MSG, WM_OBELISK_TAKE_FOCUS };

static int wsd_fd = -1;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static struct obelisk_win windows[WSD_MAX_WINDOWS];
static UINT next_buffer_id = 1;
static UINT64 present_serial;
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wsd_hello_reply hello;  /* display_lock held: the screen, from EV_HELLO then EV_DISPLAY */
static BOOL cursor_visible = TRUE;
static const WCHAR obelisk_window_propW[] = {'_','_','w','i','n','e','_','o','b','e','l','i','s','k','_','w','i','n','d','o','w',0};
static void (*release_listener)( UINT buffer_id, UINT shown, UINT64 serial );
static void (*presented_listener)( UINT buffer_id, UINT64 serial, UINT64 presented_ns );
static BOOL debug_injected;
static BOOL wsd_send( UINT type, const void *payload, UINT len, const void *extra, UINT extra_len, int fd );
static UINT relative_window;  /* wsd window in relative mouse mode, 0 when none */
static BOOL clip_active;
static float relative_rest_x, relative_rest_y;  /* fractions of a pixel not sent yet */
static UINT64 last_warp_ns;  /* SetCursorPos: the mouse moves wsd sent before it carry the old place */

/* Relative (raw delta) mouse while the game hides the cursor and confines it to its window, like a captured mouse
 * on Windows; absolute otherwise. */
static void update_relative_mouse( UINT window_id )
{
    UINT want = (!cursor_visible && clip_active) ? window_id : 0;
    if (want == relative_window) return;
    if (relative_window)
        wsd_send( WSD_RELATIVE_MOUSE, &(struct wsd_window_flag){ relative_window, 0 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    if (want)
        wsd_send( WSD_RELATIVE_MOUSE, &(struct wsd_window_flag){ want, 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    TRACE( "relative mouse window %u -> %u\n", relative_window, want );
    relative_window = want;
}  /* WINE_OBELISK_DEBUG_INJECT already sent */

static BOOL latency_trace;

static UINT64 now_ns(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/***********************************************************************
 *           protocol helpers
 */
static BOOL write_all( const void *data, size_t size, int fd_to_send )
{
    const char *ptr = data;
    BOOL first = TRUE;

    while (size)
    {
        struct iovec iov = { (void *)ptr, size };
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t ret;

        if (first && fd_to_send != -1)
        {
            struct cmsghdr *cmsg;
            memset( control, 0, sizeof(control) );
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            cmsg = CMSG_FIRSTHDR( &msg );
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy( CMSG_DATA(cmsg), &fd_to_send, sizeof(int) );
        }
        ret = sendmsg( wsd_fd, &msg, MSG_NOSIGNAL );
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            WARN( "sendmsg failed: %s\n", strerror(errno) );
            return FALSE;
        }
        first = FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static BOOL wsd_send( UINT type, const void *payload, UINT len, const void *extra, UINT extra_len, int fd )
{
    char buf[sizeof(struct wsd_hdr) + WSD_MAX_PAYLOAD];
    struct wsd_hdr *hdr = (struct wsd_hdr *)buf;
    BOOL ret;

    if (wsd_fd == -1 || len + extra_len > WSD_MAX_PAYLOAD) return FALSE;
    hdr->type = type;
    hdr->len = len + extra_len;
    if (len) memcpy( hdr + 1, payload, len );
    if (extra_len) memcpy( (char *)(hdr + 1) + len, extra, extra_len );
    pthread_mutex_lock( &write_lock );
    ret = write_all( buf, sizeof(*hdr) + len + extra_len, fd );
    pthread_mutex_unlock( &write_lock );
    TRACE( "request %u len %u fd %d -> %d\n", type, len + extra_len, fd, ret );
    return ret;
}

static BOOL read_all( void *data, size_t size )
{
    char *ptr = data;
    while (size)
    {
        ssize_t ret = read( wsd_fd, ptr, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

/* reads one message; payload must hold WSD_MAX_PAYLOAD bytes */
static BOOL wsd_read( struct wsd_hdr *hdr, void *payload )
{
    if (!read_all( hdr, sizeof(*hdr) )) return FALSE;
    if (hdr->len > WSD_MAX_PAYLOAD) return FALSE;
    return read_all( payload, hdr->len );
}

/***********************************************************************
 *           window table (win_lock held)
 */
static struct obelisk_win *get_win( HWND hwnd )
{
    UINT i;
    for (i = 0; i < WSD_MAX_WINDOWS; i++) if (windows[i].hwnd == hwnd) return &windows[i];
    return NULL;
}

static struct obelisk_win *get_win_by_id( UINT id )
{
    if (!id || id > WSD_MAX_WINDOWS || !windows[id - 1].hwnd) return NULL;
    return &windows[id - 1];
}

static UINT win_id( struct obelisk_win *win ) { return win - windows + 1; }

static struct obelisk_win *alloc_win( HWND hwnd )
{
    UINT i;
    for (i = 0; i < WSD_MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd) continue;
        memset( &windows[i], 0, sizeof(windows[i]) );
        windows[i].hwnd = hwnd;
        return &windows[i];
    }
    WARN( "more than %u top-level windows, %p not shown\n", WSD_MAX_WINDOWS, hwnd );
    return NULL;
}

static void free_buffer( struct obelisk_buffer *buf )
{
    if (!buf->id) return;
    wsd_send( WSD_BUFFER_DETACH, &(struct wsd_buffer){ buf->id }, sizeof(struct wsd_buffer), NULL, 0, -1 );
    munmap( buf->bits, buf->map_size );
    memset( buf, 0, sizeof(*buf) );
}

static void destroy_win( struct obelisk_win *win )
{
    UINT i;
    for (i = 0; i < NB_BUFFERS; i++) free_buffer( &win->buffers[i] );
    if (win->created)
        wsd_send( WSD_WINDOW_DESTROY, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), NULL, 0, -1 );
    memset( win, 0, sizeof(*win) );
}

static BOOL is_toplevel( HWND hwnd )
{
    return hwnd != NtUserGetDesktopWindow() && NtUserGetAncestor( hwnd, GA_PARENT ) == NtUserGetDesktopWindow();
}

/* The wsd frame of a Win32 window: a window with a caption gets the macOS title bar and the buttons its system
 * menu, minimize box and sizing border ask for (the same rule as winemac.drv); any other one is borderless. A topmost
 * window stays above the others, a WS_EX_NOACTIVATE one never takes the keyboard. */
static UINT frame_style( UINT style, UINT ex_style )
{
    UINT ret = WSD_STYLE_TITLED;

    if ((style & WS_CAPTION) != WS_CAPTION || (ex_style & WS_EX_LAYERED)) ret = WSD_STYLE_BORDERLESS;
    else
    {
        if (style & WS_SYSMENU) ret |= WSD_STYLE_CLOSABLE;
        if (style & WS_MINIMIZEBOX) ret |= WSD_STYLE_MINIATURIZABLE;
        if (style & WS_THICKFRAME) ret |= WSD_STYLE_RESIZABLE;
    }
    if (ex_style & WS_EX_TOPMOST) ret |= WSD_STYLE_TOPMOST;
    if (ex_style & WS_EX_NOACTIVATE) ret |= WSD_STYLE_NOACTIVATE;
    return ret;
}

/* creates a shared memory buffer and attaches it in wsd */
static BOOL alloc_buffer( struct obelisk_buffer *buf, UINT width, UINT height )
{
    UINT stride = (width * 4 + STRIDE_ALIGN - 1) & ~(STRIDE_ALIGN - 1);
    size_t size = ((size_t)stride * height + SHM_ALIGN - 1) & ~(size_t)(SHM_ALIGN - 1);
    struct wsd_buffer_attach attach;
    void *bits;
    int fd;

    if ((fd = syscall( __NR_memfd_create, "wsd-buffer", 1 /* MFD_CLOEXEC */ )) < 0)
    {
        ERR( "memfd_create failed: %s\n", strerror(errno) );
        return FALSE;
    }
    if (ftruncate( fd, size ) < 0 ||
        (bits = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        ERR( "shared buffer of %zu bytes failed: %s\n", size, strerror(errno) );
        close( fd );
        return FALSE;
    }
    memset( &attach, 0, sizeof(attach) );
    attach.buffer_id = next_buffer_id++;
    attach.width = width;
    attach.height = height;
    attach.stride = stride;
    attach.format = WSD_FMT_BGRA8;
    attach.offset = 0;
    attach.size = (UINT64)stride * height;
    if (!wsd_send( WSD_BUFFER_ATTACH, &attach, sizeof(attach), NULL, 0, fd ))
    {
        close( fd );
        munmap( bits, size );
        return FALSE;
    }
    close( fd );
    buf->id = attach.buffer_id;
    buf->bits = bits;
    buf->map_size = size;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->busy = FALSE;
    TRACE( "buffer %u %ux%u stride %u\n", buf->id, width, height, stride );
    return TRUE;
}

/***********************************************************************
 *           window surface
 */
static void obelisk_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
}

static BOOL obelisk_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                const BITMAPINFO *shape_info, const void *shape_bits )
{
    UINT surf_width = color_info->bmiHeader.biWidth, surf_height = abs( color_info->bmiHeader.biHeight );
    UINT src_stride = surf_width * 4, width, height, y, i;
    int x0, y0;
    struct obelisk_buffer *buf = NULL;
    struct obelisk_win *win;
    struct wsd_present present;
    BOOL ret = FALSE;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( surface->hwnd )) || !win->created) goto done;  /* stays dirty until shown */
    if (win->external || win->minimized) { ret = TRUE; goto done; }  /* nothing to show */

    /* The surface is usually larger than the window (Wine rounds it up); present exactly the visible window:
     * rect is the surface position relative to the visible rect, so the window starts at (-left, -top). */
    x0 = max( 0, -rect->left );
    y0 = max( 0, -rect->top );
    width = min( win->width, surf_width - min( (UINT)x0, surf_width ));
    height = min( win->height, surf_height - min( (UINT)y0, surf_height ));
    if (!width || !height) { ret = TRUE; goto done; }

    for (i = 0; i < NB_BUFFERS; i++)
    {
        struct obelisk_buffer *b = &win->buffers[i];
        if (b->id && (b->width != width || b->height != height) && !b->busy) free_buffer( b );
        if (!b->id && !alloc_buffer( b, width, height )) goto done;
        if (!b->busy && b->width == width && b->height == height) { buf = b; break; }
    }
    if (!buf) goto done;  /* both buffers in flight, retried on the next flush */

    for (y = 0; y < height; y++)
        memcpy( (char *)buf->bits + y * buf->stride,
                (const char *)color_bits + (y + y0) * src_stride + x0 * 4, width * 4 );
    if (shape_bits)  /* opaque everywhere for now, wsd ignores alpha */
        TRACE( "window shape ignored\n" );

    present.window_id = win_id( win );
    present.buffer_id = buf->id;
    present.serial = __atomic_add_fetch( &present_serial, 1, __ATOMIC_SEQ_CST );
    present.client_time_ns = now_ns();
    buf->busy = TRUE;
    if (!(ret = wsd_send( WSD_PRESENT, &present, sizeof(present), NULL, 0, -1 ))) buf->busy = FALSE;
    if (ret && !debug_injected && getenv( "WINE_OBELISK_DEBUG_INJECT" ))
    {
        /* test only, once per process: wsd feeds synthetic key, button and motion events to this window */
        debug_injected = TRUE;
        wsd_send( WSD_DEBUG_INJECT, &(struct wsd_window_flag){ win_id( win ), 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    }
done:
    pthread_mutex_unlock( &win_lock );
    return ret;
}

static void obelisk_surface_destroy( struct window_surface *surface )
{
}

static const struct window_surface_funcs obelisk_surface_funcs =
{
    obelisk_surface_set_clip,
    obelisk_surface_flush,
    obelisk_surface_destroy
};

static BOOL OBELISK_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface )
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    int width = surface_rect->right - surface_rect->left, height = surface_rect->bottom - surface_rect->top;
    struct window_surface *previous;

    if ((previous = *surface) && previous->funcs == &obelisk_surface_funcs) return TRUE;
    if (!is_toplevel( hwnd )) return TRUE;  /* default surface */
    if (previous) window_surface_release( previous );

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height;  /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;
    *surface = window_surface_create( sizeof(struct window_surface), &obelisk_surface_funcs, hwnd, surface_rect, info, 0 );
    TRACE( "hwnd %p rect %s surface %p\n", hwnd, wine_dbgstr_rect( surface_rect ), *surface );
    return TRUE;
}

/***********************************************************************
 *           window management
 */
static BOOL OBELISK_WindowPosChanging( HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects )
{
    return is_toplevel( hwnd );  /* top-level windows get a driver surface */
}

/* The caption and borders macOS draws for a titled window: win32u leaves them out of the visible rect. */
static BOOL OBELISK_GetWindowStyleMasks( HWND hwnd, UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask )
{
    if (!(frame_style( style, ex_style ) & WSD_STYLE_TITLED)) return FALSE;
    *style_mask = WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
    *ex_style_mask = WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    return TRUE;
}

static void OBELISK_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                   const struct window_rects *rects, struct window_surface *surface )
{
    DWORD style = NtUserGetWindowLongW( hwnd, GWL_STYLE ), ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    BOOL visible = (style & WS_VISIBLE) && !(style & WS_MINIMIZE) && !IsRectEmpty( &rects->visible );
    BOOL minimized = (style & WS_VISIBLE) && (style & WS_MINIMIZE);
    BOOL fullscreen = !!(swp_flags & WINE_SWP_FULLSCREEN);
    UINT width = rects->visible.right - rects->visible.left, height = rects->visible.bottom - rects->visible.top;
    UINT frame = frame_style( style, ex_style );
    struct obelisk_win *win;

    if (!is_toplevel( hwnd )) return;
    TRACE( "hwnd %p rects %s flags %08x visible %u fullscreen %u\n", hwnd, debugstr_window_rects( rects ),
           swp_flags, visible, fullscreen );

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( hwnd )))
    {
        if (!visible || !(win = alloc_win( hwnd ))) goto done;
    }
    if (minimized && win->created && win->shown)
    {
        /* into the Dock, or out of sight for a window without a minimize button, which wsd brings back when the user
         * comes back to the game; Wine's minimized position (off screen) is not given to wsd */
        if (!win->minimized)
            wsd_send( WSD_WINDOW_MINIMIZE, &(struct wsd_window_flag){ win_id( win ), 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->minimized = TRUE;
        goto done;
    }
    if (win->minimized && !minimized)
    {
        wsd_send( WSD_WINDOW_MINIMIZE, &(struct wsd_window_flag){ win_id( win ), 0 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->minimized = FALSE;
    }
    if (win->created && frame != win->style)
    {
        wsd_send( WSD_WINDOW_SET_STYLE, &(struct wsd_window_flag){ win_id( win ), frame }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->style = frame;
    }
    win->window = rects->window;
    win->visible = rects->visible;
    win->client = rects->client;
    OffsetRect( &win->client, -rects->visible.left, -rects->visible.top );

    if (visible && win->created && !fullscreen && !win->fullscreen)
    {
        struct wsd_window_frame frame = { win_id( win ), 0, rects->visible.left, rects->visible.top, width, height };
        if (win->width != width || win->height != height) frame.flags |= WSD_FRAME_SIZE;
        if (win->x != rects->visible.left || win->y != rects->visible.top) frame.flags |= WSD_FRAME_MOVE;
        if (frame.flags) wsd_send( WSD_WINDOW_SET_FRAME, &frame, sizeof(frame), NULL, 0, -1 );
        win->width = width;
        win->height = height;
        win->x = rects->visible.left;
        win->y = rects->visible.top;
    }
    if (visible && (width > 16384 || height > 16384))
    {
        /* off-screen helper windows (Wine sizes some popups far beyond the screen): wsd limit is 16384 px */
        TRACE( "hwnd %p too large for wsd (%ux%u), not shown\n", hwnd, width, height );
        visible = FALSE;
    }
    if (visible && !win->created)
    {
        struct wsd_window_create create = { win_id( win ), width, height, frame };
        WCHAR textW[256];
        char title[256 * 3];
        DWORD len = 0;
        INT n = NtUserInternalGetWindowText( hwnd, textW, ARRAY_SIZE(textW) );

        if (n > 0) RtlUnicodeToUTF8N( title, sizeof(title), &len, textW, n * sizeof(WCHAR) );
        if (!wsd_send( WSD_WINDOW_CREATE, &create, sizeof(create), title, len, -1 )) goto done;
        win->created = TRUE;
        win->style = frame;
        /* nptunix (Neptune D3D presentation) finds the wsd window of a HWND through this property */
        NtUserSetProp( hwnd, obelisk_window_propW, ULongToHandle( win_id( win ) ));
        win->width = width;
        win->height = height;
        win->x = rects->visible.left;
        win->y = rects->visible.top;
        {
            struct wsd_window_frame frame = { win_id( win ), WSD_FRAME_MOVE, win->x, win->y, width, height };
            wsd_send( WSD_WINDOW_SET_FRAME, &frame, sizeof(frame), NULL, 0, -1 );
        }
    }
    if (win->created && visible != win->shown)
    {
        /* a window shown without activation (SW_SHOWNA, a menu, a tooltip) leaves the Mac's key window where it is */
        UINT show = !visible ? 0 : (swp_flags & SWP_NOACTIVATE) || (ex_style & WS_EX_NOACTIVATE) ? 2 : 1;
        wsd_send( WSD_WINDOW_SHOW, &(struct wsd_window_flag){ win_id( win ), show }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->shown = visible;
    }
    if (win->created && visible && fullscreen != win->fullscreen)
    {
        wsd_send( WSD_WINDOW_FULLSCREEN, &(struct wsd_window_flag){ win_id( win ), fullscreen }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->fullscreen = fullscreen;
    }
done:
    pthread_mutex_unlock( &win_lock );
}

/* Applies in the window's thread the frame the user gave the macOS window (WSD_EV_FRAME). */
static void apply_frame( HWND hwnd )
{
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    struct obelisk_win *win;
    RECT rect;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( hwnd )) || !win->frame_pending || win->minimized || win->fullscreen)
    {
        if (win) win->frame_pending = FALSE;
        pthread_mutex_unlock( &win_lock );
        return;
    }
    win->frame_pending = FALSE;
    /* the window rect around the new visible rect, with the same non-client margins */
    rect.left   = win->pending.left - (win->visible.left - win->window.left);
    rect.top    = win->pending.top - (win->visible.top - win->window.top);
    rect.right  = win->pending.right + (win->window.right - win->visible.right);
    rect.bottom = win->pending.bottom + (win->window.bottom - win->visible.bottom);
    if (rect.right - rect.left == win->window.right - win->window.left &&
        rect.bottom - rect.top == win->window.bottom - win->window.top) flags |= SWP_NOSIZE;
    /* wsd already shows this frame: WindowPosChanged must not send it back */
    win->x = win->pending.left;
    win->y = win->pending.top;
    win->width = win->pending.right - win->pending.left;
    win->height = win->pending.bottom - win->pending.top;
    pthread_mutex_unlock( &win_lock );

    TRACE( "hwnd %p moved by the user to %s\n", hwnd, wine_dbgstr_rect( &rect ));
    NtUserSetWindowPos( hwnd, 0, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags );
}

static LRESULT OBELISK_WindowMessage( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_OBELISK_APPLY_FRAME:
        apply_frame( hwnd );
        return 0;
    case WM_OBELISK_TAKE_FOCUS:
    {
        /* The user made it the Mac's key window: it takes the foreground whatever process has it (Wine's foreground
         * lock refused the event thread once the desktop had it), as winemac.drv's macdrv_window_got_focus gives it,
         * in the window's thread. A window still minimized waits for the restore posted before this message. */
        DWORD style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
        if ((style & WS_VISIBLE) && !(style & (WS_DISABLED | WS_MINIMIZE))) NtUserSetForegroundWindowInternal( hwnd );
        return 0;
    }
    }
    FIXME( "unknown driver message %#x hwnd %p\n", msg, hwnd );
    return 0;
}

static void OBELISK_DestroyWindow( HWND hwnd )
{
    struct obelisk_win *win;

    NtUserRemoveProp( hwnd, obelisk_window_propW );

    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd ))) destroy_win( win );
    pthread_mutex_unlock( &win_lock );
}

static void OBELISK_SetWindowText( HWND hwnd, LPCWSTR text )
{
    struct obelisk_win *win;
    char title[1024];
    DWORD len = 0;

    if (text) RtlUnicodeToUTF8N( title, sizeof(title), &len, text, wcslen( text ) * sizeof(WCHAR) );
    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd )) && win->created)
        wsd_send( WSD_SET_TITLE, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), title, len, -1 );
    pthread_mutex_unlock( &win_lock );
}

/* Wine activated the window: the Mac's key window follows, as winemac.drv gives it the focus in ActivateWindow. */
static void OBELISK_ActivateWindow( HWND hwnd, HWND previous )
{
    struct obelisk_win *win;

    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd )) && win->created && win->shown)
        wsd_send( WSD_WINDOW_FOCUS, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), NULL, 0, -1 );
    pthread_mutex_unlock( &win_lock );
}

/***********************************************************************
 *           cursor
 */

/* The Mac cursors that stand for Windows' own (winemac.drv's table in mouse.c): the module and resource of a cursor
 * Wine loaded from one of these modules. */
static const struct { WCHAR module[16]; WORD id; UINT cursor; } system_cursors[] =
{
    { u"user32.dll",   OCR_NORMAL, WSD_CURSOR_ARROW },
    { u"user32.dll",   OCR_IBEAM,  WSD_CURSOR_IBEAM },
    { u"user32.dll",   OCR_CROSS,  WSD_CURSOR_CROSSHAIR },
    { u"user32.dll",   OCR_SIZEWE, WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"user32.dll",   OCR_SIZENS, WSD_CURSOR_RESIZE_UP_DOWN },
    { u"user32.dll",   OCR_NO,     WSD_CURSOR_NOT_ALLOWED },
    { u"user32.dll",   OCR_HAND,   WSD_CURSOR_POINTING_HAND },
    { u"comctl32.dll", 102,        WSD_CURSOR_CLOSED_HAND },
    { u"comctl32.dll", 104,        WSD_CURSOR_DRAG_COPY },
    { u"comctl32.dll", 105,        WSD_CURSOR_ARROW },
    { u"comctl32.dll", 106,        WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"comctl32.dll", 107,        WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"comctl32.dll", 108,        WSD_CURSOR_POINTING_HAND },
    { u"comctl32.dll", 135,        WSD_CURSOR_RESIZE_UP_DOWN },
    { u"ole32.dll",    1,          WSD_CURSOR_NOT_ALLOWED },
    { u"ole32.dll",    2,          WSD_CURSOR_CLOSED_HAND },
    { u"ole32.dll",    3,          WSD_CURSOR_DRAG_COPY },
    { u"ole32.dll",    4,          WSD_CURSOR_DRAG_LINK },
    { u"riched20.dll", 105,        WSD_CURSOR_POINTING_HAND },
    { u"riched20.dll", 109,        WSD_CURSOR_DRAG_COPY },
    { u"riched20.dll", 110,        WSD_CURSOR_CLOSED_HAND },
    { u"riched20.dll", 111,        WSD_CURSOR_NOT_ALLOWED },
};

#define CURSOR_CACHE 64  /* wsd keeps at most 64 cursor images per client */

static pthread_mutex_t cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { HCURSOR handle; UINT cursor; } cursor_cache[CURSOR_CACHE];  /* cursor_lock held */
static UINT cursor_cache_next;
static HCURSOR current_cursor;

/* The Mac cursor of a Windows system cursor, or -1. */
static int system_cursor( const WCHAR *module, WORD id )
{
    const WCHAR *name = wcsrchr( module, '\\' ) ? wcsrchr( module, '\\' ) + 1 : module;
    UINT i;

    if (!id) return -1;  /* a named resource */
    for (i = 0; i < ARRAY_SIZE(system_cursors); i++)
        if (system_cursors[i].id == id && !wcsicmp( name, system_cursors[i].module )) return system_cursors[i].cursor;
    return -1;
}

/* The cursor's first frame as BGRA pixels with straight alpha, drawn as winemac.drv draws it (image.c,
 * create_cgimage_from_icon_bitmaps; mouse.c, create_monochrome_cursor): the color through DrawIconEx, with its alpha
 * or else the AND mask's; a monochrome cursor from its AND and XOR masks, an inverting pixel shown black. */
static UINT *cursor_pixels( HCURSOR cursor, const ICONINFO *info, UINT width, UINT height )
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    UINT mask_stride = (width + 31) / 32 * 4, rows = info->hbmColor ? height : height * 2, x, y, i;
    BYTE *mask = malloc( mask_stride * rows );
    UINT *pixels = calloc( width * height, sizeof(UINT) ), *bits;
    BOOL has_alpha = FALSE, ok = FALSE;
    HBITMAP dib = 0;
    HDC hdc = NtGdiCreateCompatibleDC( 0 );

    if (!mask || !pixels || !hdc) goto done;
    memset( bmi, 0, sizeof(buffer) );
    bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
    bmi->bmiHeader.biWidth = width;
    bmi->bmiHeader.biHeight = -(int)rows;
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 1;
    bmi->bmiHeader.biCompression = BI_RGB;
    if (!NtGdiGetDIBitsInternal( hdc, info->hbmMask, 0, rows, mask, bmi, DIB_RGB_COLORS, 0, 0 )) goto done;

    if (info->hbmColor)
    {
        bmi->bmiHeader.biHeight = -(int)height;
        bmi->bmiHeader.biBitCount = 32;
        if (!(dib = NtGdiCreateDIBSection( hdc, NULL, 0, bmi, DIB_RGB_COLORS, 0, 0, 0, (void **)&bits ))) goto done;
        memset( bits, 0, width * height * 4 );
        NtGdiSelectBitmap( hdc, dib );
        if (!NtUserDrawIconEx( hdc, 0, 0, cursor, width, height, 0, NULL, DI_NORMAL )) goto done;
        for (i = 0; i < width * height && !has_alpha; i++) has_alpha = (bits[i] & 0xff000000) != 0;
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
            {
                UINT p = bits[y * width + x], a = p >> 24;
                if (!has_alpha)  /* AND mask: 1 leaves the screen as it is */
                    pixels[y * width + x] = (mask[y * mask_stride + x / 8] & (0x80 >> (x % 8))) ? 0 : p | 0xff000000;
                else if (a && a < 255)  /* DrawIconEx blends onto the zeroed bits: the colors come premultiplied */
                    pixels[y * width + x] = (a << 24) | (min( 255, ((p >> 16) & 0xff) * 255 / a ) << 16) |
                                            (min( 255, ((p >> 8) & 0xff) * 255 / a ) << 8) | min( 255, (p & 0xff) * 255 / a );
                else pixels[y * width + x] = p;
            }
    }
    else
    {
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
            {
                BYTE bit = 0x80 >> (x % 8);
                BOOL and_bit = !!(mask[y * mask_stride + x / 8] & bit), xor_bit = !!(mask[(y + height) * mask_stride + x / 8] & bit);
                pixels[y * width + x] = (and_bit && !xor_bit) ? 0 : (and_bit ^ xor_bit) ? 0xffffffff : 0xff000000;
            }
    }
    ok = TRUE;

done:
    if (dib) NtGdiDeleteObjectApp( dib );
    if (hdc) NtGdiDeleteObjectApp( hdc );
    free( mask );
    if (!ok)
    {
        free( pixels );
        pixels = NULL;
    }
    return pixels;
}

/* Gives wsd the cursor's image under the given id; FALSE when it cannot be shown (wsd shows the arrow). */
static BOOL define_cursor( HCURSOR cursor, UINT id )
{
    WCHAR module_name[MAX_PATH], res_name[MAX_PATH];
    UNICODE_STRING module = { 0, sizeof(module_name) - sizeof(WCHAR), module_name };
    UNICODE_STRING res = { 0, sizeof(res_name) - sizeof(WCHAR), res_name };
    struct wsd_cursor_define define = { id };
    ICONINFO info;
    BITMAP bm;
    UINT *pixels = NULL;
    BOOL ret = FALSE;
    int fd = -1;

    if (!NtUserGetIconInfo( cursor, &info, &module, &res, NULL, 0 )) return FALSE;
    NtGdiExtGetObjectW( info.hbmMask, sizeof(bm), &bm );
    define.width = bm.bmWidth;
    define.height = info.hbmColor ? bm.bmHeight : max( 1, bm.bmHeight / 2 );
    /* winemac.drv's rule: a hot spot outside the cursor is its center */
    if (info.xHotspot >= define.width || info.yHotspot >= define.height)
    {
        info.xHotspot = define.width / 2;
        info.yHotspot = define.height / 2;
    }
    define.hot_x = info.xHotspot;
    define.hot_y = info.yHotspot;
    define.stride = define.width * 4;
    define.size = (UINT64)define.stride * define.height;
    if (define.width && define.width <= 256 && define.height <= 256 &&
        (pixels = cursor_pixels( cursor, &info, define.width, define.height )) &&
        (fd = syscall( __NR_memfd_create, "wsd-cursor", 1 /* MFD_CLOEXEC */ )) >= 0 &&
        pwrite( fd, pixels, define.size, 0 ) == (ssize_t)define.size)
        ret = wsd_send( WSD_CURSOR_DEFINE, &define, sizeof(define), NULL, 0, fd );
    if (fd >= 0) close( fd );
    free( pixels );
    if (info.hbmColor) NtGdiDeleteObjectApp( info.hbmColor );
    NtGdiDeleteObjectApp( info.hbmMask );
    TRACE( "cursor %p %ux%u hot spot %d,%d -> id %#x %d\n", cursor, define.width, define.height, define.hot_x,
           define.hot_y, id, ret );
    return ret;
}

/* The Mac cursor for a Windows one: the Mac's own for a system cursor, else its image, defined once (cursor_lock held). */
static UINT mac_cursor( HCURSOR cursor )
{
    WCHAR module_name[MAX_PATH], res_name[MAX_PATH];
    UNICODE_STRING module = { 0, sizeof(module_name) - sizeof(WCHAR), module_name };
    UNICODE_STRING res = { 0, sizeof(res_name) - sizeof(WCHAR), res_name };
    ICONINFO info;
    UINT i, slot;
    int sys = -1;

    for (i = 0; i < CURSOR_CACHE; i++) if (cursor_cache[i].handle == cursor) return cursor_cache[i].cursor;
    if (NtUserGetIconInfo( cursor, &info, &module, &res, NULL, 0 ))
    {
        module_name[module.Length] = 0;  /* win32u gives the lengths in characters */
        sys = system_cursor( module_name, res.Length ? 0 : LOWORD( res.Buffer ));
        TRACE( "cursor %p from %s %u -> Mac cursor %d\n", cursor, debugstr_w(module_name), LOWORD( res.Buffer ), sys );
        if (info.hbmColor) NtGdiDeleteObjectApp( info.hbmColor );
        NtGdiDeleteObjectApp( info.hbmMask );
    }
    /* never the slot of the cursor shown: wsd would show the new image in its place */
    do slot = cursor_cache_next++ % CURSOR_CACHE; while (current_cursor && cursor_cache[slot].handle == current_cursor);
    cursor_cache[slot].handle = cursor;
    if (sys >= 0) cursor_cache[slot].cursor = sys;
    else cursor_cache[slot].cursor = define_cursor( cursor, WSD_CURSOR_FIRST_DEFINED + slot )
                                     ? WSD_CURSOR_FIRST_DEFINED + slot : WSD_CURSOR_ARROW;
    return cursor_cache[slot].cursor;
}

static void OBELISK_DestroyCursorIcon( HCURSOR cursor )
{
    UINT i;

    pthread_mutex_lock( &cursor_lock );
    for (i = 0; i < CURSOR_CACHE; i++) if (cursor_cache[i].handle == cursor) cursor_cache[i].handle = 0;
    if (current_cursor == cursor) current_cursor = 0;
    pthread_mutex_unlock( &cursor_lock );
}

static void OBELISK_SetCursor( HWND hwnd, HCURSOR cursor )
{
    BOOL visible = cursor != 0;

    if (cursor)
    {
        pthread_mutex_lock( &cursor_lock );
        if (cursor != current_cursor)
        {
            struct wsd_cursor_set set = { mac_cursor( cursor ) };
            current_cursor = cursor;
            wsd_send( WSD_CURSOR_SET, &set, sizeof(set), NULL, 0, -1 );
        }
        pthread_mutex_unlock( &cursor_lock );
    }
    if (visible == cursor_visible) return;
    cursor_visible = visible;
    wsd_send( WSD_CURSOR_VISIBLE, &(struct wsd_window_flag){ 0, visible }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    {
        struct obelisk_win *win;
        UINT id = 0;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win( NtUserGetForegroundWindow() )) && win->created) id = win_id( win );
        pthread_mutex_unlock( &win_lock );
        update_relative_mouse( id );
    }
}

/* SetCursorPos and the moves of SendInput: the Mac pointer follows (winemac.drv's SetCursorPos). In relative mode
 * the pointer stands still and the game reads deltas. */
static BOOL OBELISK_SetCursorPos( INT x, INT y )
{
    if (relative_window) return TRUE;
    __atomic_store_n( &last_warp_ns, now_ns(), __ATOMIC_RELAXED );
    TRACE( "warp to %d,%d\n", x, y );
    return wsd_send( WSD_CURSOR_WARP, &(struct wsd_cursor_pos){ x, y }, sizeof(struct wsd_cursor_pos), NULL, 0, -1 );
}

static BOOL OBELISK_ClipCursor( const RECT *clip, BOOL reset )
{
    HWND foreground = NtUserGetForegroundWindow();
    struct wsd_cursor_clip req = { 0 };
    struct obelisk_win *win;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( foreground )) || !win->created)
    {
        pthread_mutex_unlock( &win_lock );
        return TRUE;
    }
    req.window_id = win_id( win );
    if (clip && !reset)
    {
        RECT rc;
        rc.left = max( clip->left, win->visible.left );
        rc.top = max( clip->top, win->visible.top );
        rc.right = min( clip->right, win->visible.right );
        rc.bottom = min( clip->bottom, win->visible.bottom );
        /* an empty rectangle holds the cursor on one point, as Unity's locked cursor at the window's center */
        if (rc.left <= rc.right && rc.top <= rc.bottom && rc.left < win->visible.right && rc.top < win->visible.bottom)
        {
            req.enable = 1;
            req.x = rc.left - win->visible.left;
            req.y = rc.top - win->visible.top;
            req.width = max( rc.right - rc.left, 1 );
            req.height = max( rc.bottom - rc.top, 1 );
        }
    }
    pthread_mutex_unlock( &win_lock );
    wsd_send( WSD_CURSOR_CLIP, &req, sizeof(req), NULL, 0, -1 );
    clip_active = req.enable;
    update_relative_mouse( req.window_id );
    return TRUE;
}

/* SPI_SETSCREENSAVEACTIVE: while a game turns the screen saver off, the display stays on (winemac.drv's
 * SystemParametersInfo); win32u keeps the value itself. */
static BOOL OBELISK_SystemParametersInfo( UINT action, UINT int_param, void *ptr_param, UINT flags )
{
    if (action == SPI_SETSCREENSAVEACTIVE)
        wsd_send( WSD_SCREEN_SAVER, &(struct wsd_window_flag){ 0, !!int_param }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    return FALSE;
}

/***********************************************************************
 *           display devices
 */
static UINT OBELISK_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param )
{
    static const struct { UINT w, h; } extra[] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,800},
                                                     {1600,900}, {1920,1080}, {2560,1440} };
    struct pci_id pci_id = { 0 };
    struct gdi_monitor monitor = { 0 };
    DEVMODEW current = { .dmSize = sizeof(current) }, modes[ARRAY_SIZE(extra) + 1];
    UINT i, count = 0, hz, width, height;

    pthread_mutex_lock( &display_lock );
    hz = hello.max_refresh_hz ? hello.max_refresh_hz : 60;
    width = hello.display_width_px;
    height = hello.display_height_px;
    pthread_mutex_unlock( &display_lock );

    if (!width || !height) return STATUS_NOT_IMPLEMENTED;

    manager->add_gpu( NULL, &pci_id, NULL, param );
    manager->add_source( "wsd", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE,
                         NtUserGetSystemDpiForProcess( NULL ), param );
    SetRect( &monitor.rc_monitor, 0, 0, width, height );
    /* The whole screen, not wsd's work area: Rocket League places its borderless window from the work area (at y 60
     * under a 30 px menu bar), which then no longer covers the screen and is not fullscreen. AppKit keeps a title
     * bar below the menu bar by itself (WSD_EV_FRAME). */
    monitor.rc_work = monitor.rc_monitor;
    manager->add_monitor( &monitor, param );

    current.dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                       DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
    current.dmBitsPerPel = 32;
    current.dmPelsWidth = width;
    current.dmPelsHeight = height;
    current.dmDisplayFrequency = hz;
    modes[count] = current;
    modes[count++].dmFields &= ~DM_POSITION;
    for (i = 0; i < ARRAY_SIZE(extra); i++)
    {
        if (extra[i].w >= width && extra[i].h >= height) continue;
        modes[count] = modes[0];
        modes[count].dmPelsWidth = extra[i].w;
        modes[count++].dmPelsHeight = extra[i].h;
    }
    manager->add_modes( &current, count, modes, param );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           input
 */
/* Mac virtual keycode -> virtual key and scan code (0x100: extended), by the key's place on a US keyboard: winemac.drv's
 * table (keyboard.c, default_map), with Command as the Windows keys and Option as Alt. */
static const struct { WORD vk; WORD scan; } keymap[128] =
{
    {'A',0x1e},{'S',0x1f},{'D',0x20},{'F',0x21},{'H',0x23},{'G',0x22},{'Z',0x2c},{'X',0x2d},          /* 0x00 */
    {'C',0x2e},{'V',0x2f},{VK_OEM_102,0x56},{'B',0x30},{'Q',0x10},{'W',0x11},{'E',0x12},{'R',0x13},  /* 0x08 */
    {'Y',0x15},{'T',0x14},{'1',0x02},{'2',0x03},{'3',0x04},{'4',0x05},{'6',0x07},{'5',0x06},          /* 0x10 */
    {VK_OEM_PLUS,0x0d},{'9',0x0a},{'7',0x08},{VK_OEM_MINUS,0x0c},{'8',0x09},{'0',0x0b},{VK_OEM_6,0x1b},{'O',0x18}, /* 0x18 */
    {'U',0x16},{VK_OEM_4,0x1a},{'I',0x17},{'P',0x19},{VK_RETURN,0x1c},{'L',0x26},{'J',0x24},{VK_OEM_7,0x28}, /* 0x20 */
    {'K',0x25},{VK_OEM_1,0x27},{VK_OEM_5,0x2b},{VK_OEM_COMMA,0x33},{VK_OEM_2,0x35},{'N',0x31},{'M',0x32},
    {VK_OEM_PERIOD,0x34},                                                                              /* 0x28 */
    {VK_TAB,0x0f},{VK_SPACE,0x39},{VK_OEM_3,0x29},{VK_BACK,0x0e},{0,0},{VK_ESCAPE,0x01},{VK_RWIN,0x15c},
    {VK_LWIN,0x15b},                                                                                   /* 0x30 */
    {VK_LSHIFT,0x2a},{VK_CAPITAL,0x3a},{VK_LMENU,0x38},{VK_LCONTROL,0x1d},{VK_RSHIFT,0x36},{VK_RMENU,0x138},
    {VK_RCONTROL,0x11d},{0,0} /* Fn */,                                                                 /* 0x38 */
    {VK_F17,0x68},{VK_DECIMAL,0x53},{0,0},{VK_MULTIPLY,0x37},{0,0},{VK_ADD,0x4e},{0,0},{VK_OEM_CLEAR,0x59}, /* 0x40 */
    {VK_VOLUME_UP,0x100},{VK_VOLUME_DOWN,0x100},{VK_VOLUME_MUTE,0x100},{VK_DIVIDE,0x135},{VK_RETURN,0x11c},
    {0,0},{VK_SUBTRACT,0x4a},{VK_F18,0x69},                                                            /* 0x48 */
    {VK_F19,0x6a},{VK_OEM_NEC_EQUAL,0x10d},{VK_NUMPAD0,0x52},{VK_NUMPAD1,0x4f},{VK_NUMPAD2,0x50},{VK_NUMPAD3,0x51},
    {VK_NUMPAD4,0x4b},{VK_NUMPAD5,0x4c},                                                               /* 0x50 */
    {VK_NUMPAD6,0x4d},{VK_NUMPAD7,0x47},{VK_F20,0x6b},{VK_NUMPAD8,0x48},{VK_NUMPAD9,0x49},{0xff,0x7d} /* JIS Yen */,
    {0xc1,0x73} /* JIS Underscore */,{VK_SEPARATOR,0x7e} /* JIS Keypad Comma */,                        /* 0x58 */
    {VK_F5,0x3f},{VK_F6,0x40},{VK_F7,0x41},{VK_F3,0x3d},{VK_F8,0x42},{VK_F9,0x43},{0xff,0x72} /* JIS Eisu */,
    {VK_F11,0x57},                                                                                     /* 0x60 */
    {VK_OEM_RESET,0x71} /* JIS Kana */,{VK_F13,0x64},{VK_F16,0x67},{VK_F14,0x65},{0,0},{VK_F10,0x44},{0,0},
    {VK_F12,0x58},                                                                                     /* 0x68 */
    {0,0},{VK_F15,0x66},{VK_INSERT,0x152} /* Help */,{VK_HOME,0x147},{VK_PRIOR,0x149},{VK_DELETE,0x153},
    {VK_F4,0x3e},{VK_END,0x14f},                                                                       /* 0x70 */
    {VK_F2,0x3c},{VK_NEXT,0x151},{VK_F1,0x3b},{VK_LEFT,0x14b},{VK_RIGHT,0x14d},{VK_DOWN,0x150},{VK_UP,0x148},
    {0,0},                                                                                             /* 0x78 */
};

static void send_key( HWND hwnd, const struct wsd_ev_key *ev )
{
    INPUT input = { .type = INPUT_KEYBOARD };
    UINT code = ev->mac_keycode;

    /* On an ISO keyboard kVK_ISO_Section is the key left of 1 and kVK_ANSI_Grave the one right of the left Shift:
     * each takes the other's Windows key, so that a key keeps its scan code wherever it is (winemac.drv swaps their
     * scan codes the same way). */
    if ((ev->flags & WSD_KEY_ISO) && (code == 0x0a || code == 0x32)) code ^= 0x0a ^ 0x32;
    if (code >= ARRAY_SIZE(keymap) || !keymap[code].vk)
    {
        TRACE( "unmapped mac keycode %#x\n", ev->mac_keycode );
        return;
    }
    input.ki.wVk = keymap[code].vk;
    input.ki.wScan = keymap[code].scan & 0xff;
    if (keymap[code].scan & 0x100) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!(ev->flags & WSD_KEY_DOWN)) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    TRACE( "hwnd %p kvk %#x vk %#x flags %#x\n", hwnd, ev->mac_keycode, input.ki.wVk, input.ki.dwFlags );
    NtUserSendHardwareInput( hwnd, 0, &input, 0 );
}

static void send_mouse( HWND hwnd, DWORD flags, int x, int y, DWORD data )
{
    INPUT input = { .type = INPUT_MOUSE };

    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    TRACE( "hwnd %p flags %#x %d,%d data %#x\n", hwnd, (UINT)flags, x, y, (UINT)data );
    NtUserSendHardwareInput( hwnd, 0, &input, 0 );
}

static void handle_event( const struct wsd_hdr *hdr, const char *payload )
{
    const struct wsd_window *w = (const struct wsd_window *)payload;
    struct obelisk_win *win = NULL;
    HWND hwnd = 0;
    RECT visible = { 0 };

    if (hdr->type != WSD_EV_RELEASE && hdr->type != WSD_EV_PRESENTED && hdr->type != WSD_EV_ERROR &&
        hdr->type != WSD_EV_DISPLAY && hdr->len >= 4)
    {
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( w->window_id )))
        {
            hwnd = win->hwnd;
            visible = win->visible;
        }
        pthread_mutex_unlock( &win_lock );
    }

    switch (hdr->type)
    {
    case WSD_EV_RELEASE:
    {
        const struct wsd_ev_release *ev = (const void *)payload;
        void (*listener)( UINT, UINT, UINT64 ) = release_listener;
        UINT i, j;
        if (listener && (ev->buffer_id & 0x80000000)) listener( ev->buffer_id, ev->shown, ev->serial );
        pthread_mutex_lock( &win_lock );
        for (i = 0; i < WSD_MAX_WINDOWS; i++)
            for (j = 0; j < NB_BUFFERS; j++)
                if (windows[i].buffers[j].id == ev->buffer_id) windows[i].buffers[j].busy = FALSE;
        pthread_mutex_unlock( &win_lock );
        break;
    }
    case WSD_EV_PRESENTED:
    {
        const struct wsd_ev_presented *ev = (const void *)payload;
        void (*listener)( UINT, UINT64, UINT64 ) = presented_listener;
        if (listener && (ev->buffer_id & 0x80000000)) listener( ev->buffer_id, ev->serial, ev->presented_ns );
        break;
    }
    case WSD_EV_KEY:
    {
        const struct wsd_ev_key *ev = (const void *)payload;
        UINT64 rx = latency_trace ? now_ns() : 0;
        if (hwnd) send_key( hwnd, ev );
        /* guest CLOCK_MONOTONIC is the host CLOCK_UPTIME_RAW under LKD, so these are comparable to wsd times */
        if (latency_trace)
            fprintf( stderr, "wineobelisk latency: key %#x %s event %llu read +%.3f ms sent +%.3f ms\n",
                     ev->mac_keycode, (ev->flags & WSD_KEY_DOWN) ? "down" : "up", (unsigned long long)ev->time_ns,
                     (rx - ev->time_ns) / 1e6, (now_ns() - ev->time_ns) / 1e6 );
        break;
    }
    case WSD_EV_MOTION:
    {
        const struct wsd_ev_motion *ev = (const void *)payload;
        /* sent before the last SetCursorPos: Wine's cursor would go back where the game moved it from */
        if (ev->time_ns <= __atomic_load_n( &last_warp_ns, __ATOMIC_RELAXED )) break;
        if (hwnd) send_mouse( hwnd, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                              visible.left + (int)ev->x, visible.top + (int)ev->y, 0 );
        break;
    }
    case WSD_EV_RELATIVE:
    {
        const struct wsd_ev_relative *ev = (const void *)payload;
        int dx, dy;
        if (!hwnd) break;
        relative_rest_x += ev->dx;
        relative_rest_y += ev->dy;
        dx = (int)relative_rest_x;
        dy = (int)relative_rest_y;
        relative_rest_x -= dx;
        relative_rest_y -= dy;
        if (dx || dy) send_mouse( hwnd, MOUSEEVENTF_MOVE, dx, dy, 0 );
        break;
    }
    case WSD_EV_BUTTON:
    {
        const struct wsd_ev_button *ev = (const void *)payload;
        DWORD flags, data = 0;
        if (!hwnd) break;
        switch (ev->button)
        {
        case 0: flags = MOUSEEVENTF_LEFTDOWN; break;
        case 1: flags = MOUSEEVENTF_RIGHTDOWN; break;
        case 2: flags = MOUSEEVENTF_MIDDLEDOWN; break;
        case 3: flags = MOUSEEVENTF_XDOWN; data = XBUTTON1; break;
        case 4: flags = MOUSEEVENTF_XDOWN; data = XBUTTON2; break;
        default: flags = 0; break;
        }
        if (!flags) break;
        if (!ev->down) flags <<= 1;
        /* in relative mode the Mac pointer stands still where the game's cursor is not */
        if (relative_window != ev->window_id)
            send_mouse( hwnd, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                        visible.left + (int)ev->x, visible.top + (int)ev->y, 0 );
        send_mouse( hwnd, flags, 0, 0, data );
        break;
    }
    case WSD_EV_WHEEL:
    {
        const struct wsd_ev_wheel *ev = (const void *)payload;
        if (!hwnd) break;
        if (ev->dy) send_mouse( hwnd, MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(int)(ev->dy * (ev->precise ? 4 : WHEEL_DELTA)) );
        if (ev->dx) send_mouse( hwnd, MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)(int)(-ev->dx * (ev->precise ? 4 : WHEEL_DELTA)) );
        break;
    }
    case WSD_EV_FOCUS:
    {
        const struct wsd_window_flag *ev = (const void *)payload;
        TRACE( "focus %u hwnd %p\n", ev->value, hwnd );
        if (hwnd && ev->value) NtUserPostMessage( hwnd, WM_OBELISK_TAKE_FOCUS, 0, 0 );
        break;
    }
    case WSD_EV_CLOSE:
        if (hwnd) NtUserPostMessage( hwnd, WM_SYSCOMMAND, SC_CLOSE, 0 );
        break;
    case WSD_EV_FRAME:
    {
        const struct wsd_window_frame *ev = (const void *)payload;
        BOOL post = FALSE;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( ev->window_id )) && win->hwnd == hwnd)
        {
            SetRect( &win->pending, ev->x, ev->y, ev->x + (int)ev->width, ev->y + (int)ev->height );
            post = !win->frame_pending;  /* one message for a burst of moves */
            win->frame_pending = TRUE;
        }
        pthread_mutex_unlock( &win_lock );
        if (hwnd && post) NtUserPostMessage( hwnd, WM_OBELISK_APPLY_FRAME, 0, 0 );
        break;
    }
    case WSD_EV_MINIMIZE:
    {
        const struct wsd_window_flag *ev = (const void *)payload;
        BOOL changed = FALSE;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( ev->window_id )) && win->hwnd == hwnd && win->minimized != !!ev->value)
        {
            win->minimized = !!ev->value;
            changed = TRUE;
        }
        pthread_mutex_unlock( &win_lock );
        TRACE( "hwnd %p minimized %u by the user (changed %u)\n", hwnd, ev->value, changed );
        if (hwnd && changed) NtUserPostMessage( hwnd, WM_SYSCOMMAND, ev->value ? SC_MINIMIZE : SC_RESTORE, 0 );
        /* restored by the user, as from the taskbar on Windows: the window takes the focus once restored */
        if (hwnd && changed && !ev->value) NtUserPostMessage( hwnd, WM_OBELISK_TAKE_FOCUS, 0, 0 );
        break;
    }
    case WSD_EV_ZOOM:
    {
        DWORD style = hwnd ? NtUserGetWindowLongW( hwnd, GWL_STYLE ) : 0;
        if (style & WS_MAXIMIZEBOX)
            NtUserPostMessage( hwnd, WM_SYSCOMMAND, (style & WS_MAXIMIZE) ? SC_RESTORE : SC_MAXIMIZE, 0 );
        break;
    }
    case WSD_EV_RESIZE:
    {
        const struct wsd_ev_resize *ev = (const void *)payload;
        TRACE( "hwnd %p resized to %ux%u\n", hwnd, ev->width_px, ev->height_px );
        break;
    }
    case WSD_EV_APP_DEACTIVATED:
    {
        HWND foreground = NtUserGetForegroundWindow();
        DWORD pid = 0;
        /* Another Mac app is in front: the game goes to the background as winemac.drv sends it there
         * (macdrv_app_deactivated), the desktop taking the foreground, and gets WM_ACTIVATEAPP. The process that has
         * the foreground does it. */
        if (!foreground || !NtUserGetWindowThread( foreground, &pid ) || pid != GetCurrentProcessId()) break;
        TRACE( "app deactivated, foreground %p to the desktop\n", foreground );
        NtUserClipCursor( NULL );
        NtUserSetForegroundWindowInternal( NtUserGetDesktopWindow() );
        break;
    }
    case WSD_EV_DISPLAY:
    {
        struct wsd_hello_reply display = { 0 };
        DWORD pid = 0;
        memcpy( &display, payload, min( hdr->len, sizeof(display) ));
        pthread_mutex_lock( &display_lock );
        hello = display;
        pthread_mutex_unlock( &display_lock );
        TRACE( "display now %ux%u @%u Hz\n", display.display_width_px, display.display_height_px, display.max_refresh_hz );
        /* Each process keeps the new size; the desktop's tells Wine, which asks UpdateDisplayDevices again
         * (winemac.drv's macdrv_displays_changed). */
        NtUserGetWindowThread( NtUserGetDesktopWindow(), &pid );
        if (pid == GetCurrentProcessId()) NtUserCallNoParam( NtUserCallNoParam_DisplayModeChanged );
        break;
    }
    case WSD_EV_ERROR:
    {
        const struct wsd_ev_error *ev = (const void *)payload;
        ERR( "wsd rejected request %u: %d\n", ev->request_type, ev->code );
        break;
    }
    default:
        break;
    }
}

/***********************************************************************
 *           API for other unix libraries of the process (nptunix.so, Neptune D3D presentation)
 *
 * Found with dlopen( "wineobelisk.so", RTLD_NOLOAD ) + dlsym. Buffer ids with bit 31 set belong to the caller;
 * their EV_RELEASE events go to the listener (called on the event thread).
 */
#define OBELISK_EXPORT __attribute__((visibility("default")))

OBELISK_EXPORT BOOL obelisk_wsd_send( UINT type, const void *payload, UINT len, int fd )
{
    return wsd_send( type, payload, len, NULL, 0, fd );
}

OBELISK_EXPORT void obelisk_wsd_set_release_listener( void (*listener)( UINT buffer_id, UINT shown, UINT64 serial ) )
{
    release_listener = listener;
}

OBELISK_EXPORT void obelisk_wsd_set_presented_listener( void (*listener)( UINT buffer_id, UINT64 serial, UINT64 presented_ns ) )
{
    presented_listener = listener;
}

/* client area of a wsd window, in window pixels (top-left of the window's visible rect) */
OBELISK_EXPORT BOOL obelisk_wsd_client_rect( UINT window_id, int *x, int *y, UINT *width, UINT *height )
{
    struct obelisk_win *win;
    BOOL ret = FALSE;
    pthread_mutex_lock( &win_lock );
    if ((win = get_win_by_id( window_id )) && win->client.right > win->client.left && win->client.bottom > win->client.top)
    {
        *x = win->client.left;
        *y = win->client.top;
        *width = win->client.right - win->client.left;
        *height = win->client.bottom - win->client.top;
        ret = TRUE;
    }
    pthread_mutex_unlock( &win_lock );
    return ret;
}

OBELISK_EXPORT void obelisk_wsd_set_external( UINT window_id, BOOL external )
{
    struct obelisk_win *win;
    pthread_mutex_lock( &win_lock );
    if ((win = get_win_by_id( window_id ))) win->external = external;
    pthread_mutex_unlock( &win_lock );
}

OBELISK_EXPORT BOOL obelisk_wsd_get_display( UINT *width, UINT *height, UINT *refresh_hz )
{
    BOOL ret;

    pthread_mutex_lock( &display_lock );
    if ((ret = hello.display_width_px != 0))
    {
        *width = hello.display_width_px;
        *height = hello.display_height_px;
        *refresh_hz = hello.max_refresh_hz ? hello.max_refresh_hz : 60;
    }
    pthread_mutex_unlock( &display_lock );
    return ret;
}

OBELISK_EXPORT UINT64 obelisk_wsd_next_serial(void)
{
    return __atomic_add_fetch( &present_serial, 1, __ATOMIC_SEQ_CST );
}

/***********************************************************************
 *           unix calls
 */
static const struct user_driver_funcs obeliskdrv_funcs =
{
    .pActivateWindow = OBELISK_ActivateWindow,
    .pClipCursor = OBELISK_ClipCursor,
    .pCreateWindowSurface = OBELISK_CreateWindowSurface,
    .pDestroyCursorIcon = OBELISK_DestroyCursorIcon,
    .pDestroyWindow = OBELISK_DestroyWindow,
    .pGetWindowStyleMasks = OBELISK_GetWindowStyleMasks,
    .pSetCursor = OBELISK_SetCursor,
    .pSetCursorPos = OBELISK_SetCursorPos,
    .pSetWindowText = OBELISK_SetWindowText,
    .pSystemParametersInfo = OBELISK_SystemParametersInfo,
    .pUpdateDisplayDevices = OBELISK_UpdateDisplayDevices,
    .pWindowMessage = OBELISK_WindowMessage,
    .pWindowPosChanged = OBELISK_WindowPosChanged,
    .pWindowPosChanging = OBELISK_WindowPosChanging,
};

static NTSTATUS obeliskdrv_init( void *arg )
{
    const char *path = getenv( "WINE_OBELISK_SOCKET" );
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct wsd_hello req = { WSD_PROTOCOL_VERSION, getpid() };
    char payload[WSD_MAX_PAYLOAD];
    struct wsd_hdr hdr;

    if (!path) path = "/run/obelisk/wsd.sock";
    if (strlen( path ) >= sizeof(addr.sun_path)) return STATUS_UNSUCCESSFUL;
    strcpy( addr.sun_path, path );
    if ((wsd_fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 )) < 0) return STATUS_UNSUCCESSFUL;
    if (connect( wsd_fd, (struct sockaddr *)&addr, sizeof(addr) ) < 0)
    {
        WARN( "cannot connect to wsd at %s: %s\n", path, strerror(errno) );
        goto fail;
    }
    if (!wsd_send( WSD_HELLO, &req, sizeof(req), NULL, 0, -1 )) goto fail;
    do
    {
        if (!wsd_read( &hdr, payload )) goto fail;
    } while (hdr.type != WSD_EV_HELLO);
    memcpy( &hello, payload, min( hdr.len, sizeof(hello) ));
    TRACE( "wsd pid %u display %ux%u @%u Hz scale %u/1000\n", hello.server_pid, hello.display_width_px,
           hello.display_height_px, hello.max_refresh_hz, hello.backing_scale_x1000 );
    __wine_set_user_driver( &obeliskdrv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;

fail:
    close( wsd_fd );
    wsd_fd = -1;
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS obeliskdrv_read_events( void *arg )
{
    char payload[WSD_MAX_PAYLOAD];
    struct wsd_hdr hdr;

    latency_trace = getenv( "WINE_OBELISK_LATENCY" ) != NULL;

    while (wsd_read( &hdr, payload )) handle_event( &hdr, payload );
    return STATUS_UNSUCCESSFUL;
}

/* the program's icon becomes the Dock and Cmd-Tab icon */
static NTSTATUS obeliskdrv_set_app_icon( void *arg )
{
    const struct obeliskdrv_app_icon_params *params = arg;
    struct wsd_app_icon icon = { params->width, params->height, params->width * 4 };
    size_t size = (size_t)icon.stride * icon.height;
    size_t map_size = (size + SHM_ALIGN - 1) & ~(size_t)(SHM_ALIGN - 1);
    void *bits;
    BOOL ret;
    int fd;

    if (!params->width || !params->height || params->width > 1024 || params->height > 1024)
        return STATUS_INVALID_PARAMETER;
    if ((fd = syscall( __NR_memfd_create, "wsd-icon", 1 /* MFD_CLOEXEC */ )) < 0) return STATUS_UNSUCCESSFUL;
    if (ftruncate( fd, map_size ) < 0 ||
        (bits = mmap( NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        close( fd );
        return STATUS_UNSUCCESSFUL;
    }
    memcpy( bits, params->bits, size );
    munmap( bits, map_size );
    icon.size = size;
    ret = wsd_send( WSD_APP_ICON, &icon, sizeof(icon), NULL, 0, fd );
    close( fd );
    TRACE( "icon %ux%u -> %d\n", params->width, params->height, ret );
    return ret ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    obeliskdrv_init,
    obeliskdrv_read_events,
    obeliskdrv_set_app_icon,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == obeliskdrv_unix_func_count );
