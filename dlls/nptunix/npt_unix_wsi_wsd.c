#if 0
#pragma makedep unix
#endif
/*
 * Copyright 2026 Turing Software LLC
 * Copyright 2026 Obelisk
 * SPDX-License-Identifier: MIT
 *
 * wsd WSI for nptunix.so: replaces the X11 DRI3/Present WSI (npt_unix_wsi.c). Each swapchain image is a host
 * blob (POSIX shm created by the macOS renderer, received through vtest); its fd is attached once to the wsd
 * window of the HWND with BUFFER_ATTACH, so wsd samples the pages DXMT rendered into (no copy). The wsd
 * connection is the one of wineobelisk.drv in the same process, reached through its exported functions.
 * The x11_window field of npt_unix_wsi_init_params carries the wsd window id (HWND property
 * __wine_obelisk_window, set by the display driver).
 *
 * Every swapchain has a state of its own, named by the handle wsi_init returns (the wsi field, as in neptune's X11
 * unixlib): a game that shows a second swapchain in another window (a popup) keeps presenting into the first.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "npt_unix_internal.h"
#include "npt_unixlib.h"
#include "wsd_protocol.h"

typedef int BOOL32;
static BOOL32 (*p_send)(unsigned int type, const void *payload, unsigned int len, int fd);
static void (*p_set_release_listener)(void (*)(unsigned int, unsigned int, uint64_t));
static void (*p_set_external)(unsigned int window_id, BOOL32 external);
static uint64_t (*p_next_serial)(void);
static void (*p_set_presented_listener)(void (*)(unsigned int, uint64_t, uint64_t));
static BOOL32 (*p_client_rect)(unsigned int window_id, int *x, int *y, unsigned int *w, unsigned int *h);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static unsigned int g_generation;

#define NPT_WSI_MAX_STATES 16

struct wsi_state {
   bool active;
   uint64_t handle;             /* the wsi handle of the swapchain, never 0 */
   uint32_t window_id;
   uint32_t num_images;
   uint32_t width, height;
   uint32_t buffer_ids[NPT_WSI_MAX_IMAGES];
   uint32_t released[NPT_WSI_MAX_IMAGES];   /* EV_RELEASE count not yet reported by drain */
   uint64_t presents;
   uint64_t first_present_ns;
   uint32_t in_flight;          /* PRESENT_REGION sent, EV_PRESENTED not yet received */
   uint64_t latch_serial;       /* serial of the frame sent last, until wsd releases it (0: none) */
   /* statistics, logged every 300 presents (and the first one) */
   uint64_t stat_fence_timeouts, stat_fence_ns, stat_pace_ns, stat_last_ns, stat_interval_ns, stat_pace_timeouts, stat_no_fence;
   uint64_t stat_presented;     /* EV_PRESENTED received */
   int rx, ry; uint32_t rw, rh; /* last overlay rectangle */
   uint64_t drains, last_log_ns;
};

static struct wsi_state g_states[NPT_WSI_MAX_STATES];
static uint64_t g_next_handle;

/* the state a call names, under g_lock; nothing for a destroyed handle or 0 (a swapchain whose wsi_init failed) */
static struct wsi_state *state_of(uint64_t handle)
{
   if (!handle) return NULL;
   for (uint32_t i = 0; i < NPT_WSI_MAX_STATES; i++)
      if (g_states[i].active && g_states[i].handle == handle) return &g_states[i];
   return NULL;
}

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static bool bind_driver(void)
{
   void *h;
   if (p_send) return true;
   if (!(h = dlopen("wineobelisk.so", RTLD_NOW | RTLD_NOLOAD))) {
      fprintf(stderr, "neptune unixlib wsi (wsd): wineobelisk.so is not loaded (%s)\n", dlerror());
      return false;
   }
   p_set_release_listener = dlsym(h, "obelisk_wsd_set_release_listener");
   p_set_external = dlsym(h, "obelisk_wsd_set_external");
   p_next_serial = dlsym(h, "obelisk_wsd_next_serial");
   p_set_presented_listener = dlsym(h, "obelisk_wsd_set_presented_listener");
   p_client_rect = dlsym(h, "obelisk_wsd_client_rect");
   p_send = dlsym(h, "obelisk_wsd_send");
   if (!p_send || !p_set_release_listener || !p_set_external || !p_next_serial || !p_set_presented_listener || !p_client_rect) {
      fprintf(stderr, "neptune unixlib wsi (wsd): wineobelisk.so lacks the wsd API\n");
      p_send = NULL;
      return false;
   }
   return true;
}

/* called on the display driver's event thread */
static void on_release(unsigned int buffer_id, unsigned int shown, uint64_t serial)
{
   (void)shown;
   pthread_mutex_lock(&g_lock);
   for (uint32_t k = 0; k < NPT_WSI_MAX_STATES; k++) {
      struct wsi_state *s = &g_states[k];
      if (s->latch_serial && serial == s->latch_serial) {
         s->latch_serial = 0;
         pthread_cond_broadcast(&g_cond);
      }
      for (uint32_t i = 0; s->active && i < s->num_images; i++) {
         if (s->buffer_ids[i] == buffer_id) {
            s->released[i]++;
            pthread_cond_broadcast(&g_cond);
         }
      }
   }
   pthread_mutex_unlock(&g_lock);
}

/* called on the display driver's event thread: one frame left the queue (shown or not) */
static void on_presented(unsigned int buffer_id, uint64_t serial, uint64_t presented_ns)
{
   (void)serial; (void)presented_ns;
   pthread_mutex_lock(&g_lock);
   for (uint32_t k = 0; k < NPT_WSI_MAX_STATES; k++) {
      struct wsi_state *s = &g_states[k];
      for (uint32_t i = 0; s->active && i < s->num_images; i++) {
         if (s->buffer_ids[i] == buffer_id) s->stat_presented++;
         if (s->buffer_ids[i] == buffer_id && s->in_flight) {
            s->in_flight--;
            pthread_cond_broadcast(&g_cond);
         }
      }
   }
   pthread_mutex_unlock(&g_lock);
}

static uint32_t wsd_format(uint32_t virgl_format)
{
   switch (virgl_format) {
   case 67: return WSD_FMT_RGBA8;    /* R8G8B8A8_UNORM */
   case 8: return WSD_FMT_RGB10A2;   /* R10G10B10A2_UNORM */
   case 1:                          /* B8G8R8A8_UNORM */
   case 2:                           /* B8G8R8X8_UNORM: wsd presents opaque */
   default: return WSD_FMT_BGRA8;
   }
}

/* whether another live swapchain presents into the window of s (a game that makes a new swapchain on its window
 * before it releases the old one): the window's overlay is then that swapchain's. Under g_lock. */
static bool window_shared(const struct wsi_state *s)
{
   for (uint32_t k = 0; k < NPT_WSI_MAX_STATES; k++)
      if (&g_states[k] != s && g_states[k].active && g_states[k].window_id == s->window_id) return true;
   return false;
}

/* detaches the buffers of one state only, and empties the overlay of its own window unless another swapchain
 * presents there */
static void detach_all(const struct wsi_state *s, bool shared)
{
   for (uint32_t i = 0; i < s->num_images; i++) {
      if (!s->buffer_ids[i]) continue;
      struct wsd_buffer b = { s->buffer_ids[i] };
      p_send(WSD_BUFFER_DETACH, &b, sizeof(b), -1);
   }
   if (s->window_id && !shared) {
      struct wsd_present_region r;
      memset(&r, 0, sizeof(r));
      r.present.window_id = s->window_id;
      p_send(WSD_PRESENT_REGION, &r, sizeof(r), -1);  /* 0x0: remove the overlay plane */
   }
}

NTSTATUS npt_do_wsi_init(void *args)
{
   struct npt_unix_wsi_init_params *p = args;
   struct wsi_state *s = NULL;
   unsigned int generation;
   p->result = -1;
   p->wsi = 0;

   if (!p->num_images || p->num_images > NPT_WSI_MAX_IMAGES) return STATUS_SUCCESS;
   if (!bind_driver()) return STATUS_SUCCESS;
   if (!p->x11_window) {
      fprintf(stderr, "neptune unixlib wsi (wsd): no wsd window supplied\n");
      return STATUS_SUCCESS;
   }

   pthread_mutex_lock(&g_lock);
   /* a free state; the handle claims it, while it stays inactive until its buffers are attached below */
   for (uint32_t i = 0; i < NPT_WSI_MAX_STATES && !s; i++)
      if (!g_states[i].handle) s = &g_states[i];
   if (!s) {
      pthread_mutex_unlock(&g_lock);
      fprintf(stderr, "neptune unixlib wsi (wsd): no free swapchain state (%u in use)\n", NPT_WSI_MAX_STATES);
      return STATUS_SUCCESS;
   }
   memset(s, 0, sizeof(*s));
   s->handle = ++g_next_handle;
   generation = g_generation = (g_generation + 1) & 0x7ffffff; /* buffer ids unique across states and inits */
   s->window_id = p->x11_window;
   s->num_images = p->num_images;
   s->width = p->width;
   s->height = p->height;
   pthread_mutex_unlock(&g_lock);
   p_set_release_listener(on_release);
   p_set_presented_listener(on_presented);

   for (uint32_t i = 0; i < p->num_images; i++) {
      struct wsd_buffer_attach a;
      if (p->fds[i] < 0) continue;
      memset(&a, 0, sizeof(a));
      a.buffer_id = 0x80000000u | (generation << 4) | i;
      a.width = p->width;
      a.height = p->height;
      a.stride = p->strides[i];
      a.format = wsd_format(p->virgl_format);
      a.offset = 0;
      a.size = (uint64_t)p->strides[i] * p->height;
      if (!p_send(WSD_BUFFER_ATTACH, &a, sizeof(a), p->fds[i])) {
         fprintf(stderr, "neptune unixlib wsi (wsd): BUFFER_ATTACH failed for image %u\n", i);
         continue;
      }
      s->buffer_ids[i] = a.buffer_id;
      fprintf(stderr, "neptune unixlib wsi (wsd): image %u -> buffer %#x (%ux%u stride %u format %u)\n",
              i, a.buffer_id, a.width, a.height, a.stride, a.format);
   }
   pthread_mutex_lock(&g_lock);
   s->active = true;
   pthread_mutex_unlock(&g_lock);
   fprintf(stderr, "neptune unixlib wsi (wsd): initialized for wsd window %u (%u images, %ux%u, swapchain %llu)\n",
           s->window_id, p->num_images, p->width, p->height, (unsigned long long)s->handle);
   p->wsi = s->handle;
   p->result = 0;
   return STATUS_SUCCESS;
}

/* Pacing follows the game's vsync: a Present with a SyncInterval uses "latch", one without uses "off".
 * "latch" sends each frame as soon as its fence signals, then waits until wsd has taken it at a display-link tick
 * (EV_RELEASE of that frame: copied into a drawable, or replaced), bounded by 100 ms. The game is held to the display
 * rate with one frame in the queue, while the macOS composition that follows (two or three refreshes) overlaps the
 * next frames. "off" never waits: wsd keeps only the newest unshown frame, as no tearing is possible.
 * NPT_WSD_PACING forces one mode for every frame: "latch", "off", "after" (waits until wsd has shown the frame,
 * EV_PRESENTED: the composition is then part of every frame, which caps a 120 Hz display at about 30 frames per
 * second) or "before" (a ready frame waits for the previous one to be shown, then is queued in wsd for the next
 * refresh: one more frame of input latency). */
static int forced_pacing_mode(void)
{
   static int mode = -2;
   if (mode == -2) {
      const char *e = getenv("NPT_WSD_PACING");
      mode = !e ? -1 : !strcmp(e, "latch") ? 3 : !strcmp(e, "after") ? 2 : !strcmp(e, "before") ? 1 : !strcmp(e, "off") ? 0 : -1;
      fprintf(stderr, "neptune unixlib wsi (wsd): pacing %s\n", mode == 3 ? "latch" : mode == 2 ? "after send" :
              mode == 1 ? "before send" : mode == 0 ? "off" : "from the game's vsync");
   }
   return mode;
}

static int pacing_mode(const struct npt_unix_wsi_present_params *p)
{
   int mode = forced_pacing_mode();
   return mode >= 0 ? mode : p->sync_interval ? 3 : 0;
}

static void deadline_in_ms(struct timespec *deadline, long ms)
{
   clock_gettime(CLOCK_REALTIME, deadline);
   deadline->tv_nsec += ms * 1000000;
   if (deadline->tv_nsec >= 1000000000) { deadline->tv_sec++; deadline->tv_nsec -= 1000000000; }
}

/* "latch": until wsd releases the frame sent last */
static void latch_wait(struct wsi_state *s)
{
   struct timespec deadline;
   int timedout = 0;
   uint64_t w0;
   deadline_in_ms(&deadline, 100);
   pthread_mutex_lock(&g_lock);
   w0 = now_ns();
   while (s->latch_serial && !timedout)
      timedout = pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT;
   if (timedout) s->stat_pace_timeouts++;
   s->stat_pace_ns += now_ns() - w0;
   s->latch_serial = 0;
   pthread_mutex_unlock(&g_lock);
}

static void pace_wait(struct wsi_state *s)
{
   {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 100 * 1000000;
      if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
      pthread_mutex_lock(&g_lock);
      uint64_t w0 = now_ns();
      int timedout = 0;
      while (s->in_flight && !timedout)
         timedout = pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT;
      if (timedout) s->stat_pace_timeouts++;
      s->stat_pace_ns += now_ns() - w0;
      s->in_flight = 0;
      pthread_mutex_unlock(&g_lock);
   }

}

NTSTATUS npt_do_wsi_present(void *args)
{
   struct npt_unix_wsi_present_params *p = args;
   struct wsd_present pr;
   struct wsi_state *s;
   uint64_t t0 = now_ns();
   int mode;

   p->result = -1;
   p->wait_fence_consumed = 0;
   pthread_mutex_lock(&g_lock);
   s = state_of(p->wsi);
   pthread_mutex_unlock(&g_lock);
   if (!s) {
      /* a destroyed swapchain's present: never another swapchain's state */
      if (p->wait_fence_fd >= 0) { close(p->wait_fence_fd); p->wait_fence_consumed = 1; }
      fprintf(stderr, "neptune unixlib wsi (wsd): present of image %u for unknown swapchain %llu ignored\n",
              p->image_index, (unsigned long long)p->wsi);
      return STATUS_SUCCESS;
   }

   /* wsd has no fenced present yet: wait here for the renderer's fence (end of the frame on the host). */
   if (p->wait_fence_fd >= 0) {
      struct pollfd pfd = { .fd = p->wait_fence_fd, .events = POLLIN };
      int rc;
      /* Bounded: a fence that stays parked on the host must not freeze the swapchain (the frame is shown anyway,
       * possibly before the GPU finished it; counted in the statistics). */
      do rc = poll(&pfd, 1, 1000); while (rc < 0 && errno == EINTR);
      if (rc == 0) {
         s->stat_fence_timeouts++;
         fprintf(stderr, "neptune unixlib wsi (wsd): fence of present %llu not signaled after 1 s (image %u)\n",
                 (unsigned long long)s->presents + 1, p->image_index);
      }
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
   }
   else s->stat_no_fence++;
   s->stat_fence_ns += now_ns() - t0;
   if (!s->active || p->image_index >= s->num_images || !s->buffer_ids[p->image_index]) {
      fprintf(stderr, "neptune unixlib wsi (wsd): present of image %u ignored (active %d, %u images)\n",
              p->image_index, s->active, s->num_images);
      return STATUS_SUCCESS;
   }

   mode = pacing_mode(p);
   if (mode == 1) pace_wait(s);

   /* the swapchain covers the client area of the window (Wine draws the frame): present into an overlay plane */
   {
      struct wsd_present_region r;
      int x = 0, y = 0; unsigned int w = s->width, h = s->height;
      if (!p_client_rect(s->window_id, &x, &y, &w, &h)) { x = y = 0; w = s->width; h = s->height; }
      memset(&r, 0, sizeof(r));
      r.present.window_id = s->window_id;
      r.present.buffer_id = s->buffer_ids[p->image_index];
      r.present.serial = p_next_serial();
      r.present.client_time_ns = now_ns();
      r.x = x; r.y = y; r.width = w; r.height = h;
      pr = r.present;
      pthread_mutex_lock(&g_lock);
      if (s->handle != p->wsi) { pthread_mutex_unlock(&g_lock); return STATUS_SUCCESS; } /* destroyed meanwhile */
      s->in_flight++;
      if (mode == 3) s->latch_serial = r.present.serial;
      pthread_mutex_unlock(&g_lock);
      if (!p_send(WSD_PRESENT_REGION, &r, sizeof(r), -1)) {
         pthread_mutex_lock(&g_lock);
         s->latch_serial = 0;
         pthread_mutex_unlock(&g_lock);
         return STATUS_SUCCESS;
      }
   }
   if (mode == 3) latch_wait(s);
   else if (mode == 2) pace_wait(s);

   if (!s->presents++) s->first_present_ns = pr.client_time_ns;
   if (s->stat_last_ns) s->stat_interval_ns += pr.client_time_ns - s->stat_last_ns;
   s->stat_last_ns = pr.client_time_ns;
   if (s->presents <= 3 || s->presents % 60 == 0) {
      uint64_t n = s->presents <= 3 ? 1 : (s->presents == 60 ? 57 : 60);
      fprintf(stderr, "neptune unixlib wsi (wsd): %llu presents (%llu shown events), last %llu: interval avg %.3f ms, "
              "fence wait avg %.3f ms, pacing wait avg %.3f ms, pacing timeouts %llu, presents without fence %llu, fence timeouts %llu"
              ", window %u\n",
              (unsigned long long)s->presents, (unsigned long long)s->stat_presented, (unsigned long long)n,
              s->stat_interval_ns / (double)n / 1e6, s->stat_fence_ns / (double)n / 1e6,
              s->stat_pace_ns / (double)n / 1e6, (unsigned long long)s->stat_pace_timeouts,
              (unsigned long long)s->stat_no_fence, (unsigned long long)s->stat_fence_timeouts, s->window_id);
      s->stat_interval_ns = s->stat_fence_ns = s->stat_pace_ns = 0;
      s->stat_pace_timeouts = s->stat_no_fence = 0;
   }
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_drain(void *args)
{
   struct npt_unix_wsi_drain_params *p = args;
   struct wsi_state *s;
   struct timespec deadline;
   bool any = false;

   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++) p->released_count[i] = 0;
   p->result = -1;
   pthread_mutex_lock(&g_lock);
   s = state_of(p->wsi);
   pthread_mutex_unlock(&g_lock);
   if (!s || !s->active) return STATUS_SUCCESS;

   clock_gettime(CLOCK_REALTIME, &deadline);
   deadline.tv_sec += p->timeout_ms / 1000;
   deadline.tv_nsec += (long)(p->timeout_ms % 1000) * 1000000;
   if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }

   pthread_mutex_lock(&g_lock);
   for (;;) {
      /* the state is reused once destroyed: stop reading it when it no longer belongs to this caller */
      if (s->handle != p->wsi) { pthread_mutex_unlock(&g_lock); return STATUS_SUCCESS; }
      for (uint32_t i = 0; i < s->num_images; i++) if (s->released[i]) any = true;
      if (any || !p->timeout_ms) break;
      if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT) break;
   }
   for (uint32_t i = 0; i < s->num_images; i++) {
      p->released_count[i] = s->released[i];
      s->released[i] = 0;
   }
   s->drains++;
   if (now_ns() - s->last_log_ns > 10000000000ull) {
      /* once every 10 s: shows whether the swapchain still presents at all */
      s->last_log_ns = now_ns();
      fprintf(stderr, "neptune unixlib wsi (wsd): alive: %llu presents, %llu shown events, %llu drains, in flight %u, window %u\n",
              (unsigned long long)s->presents, (unsigned long long)s->stat_presented,
              (unsigned long long)s->drains, s->in_flight, s->window_id);
   }
   pthread_mutex_unlock(&g_lock);
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_destroy(void *args)
{
   const struct npt_unix_wsi_destroy_params *p = args;
   struct wsi_state *s, old;
   bool shared;
   pthread_mutex_lock(&g_lock);
   s = p ? state_of(p->wsi) : NULL;
   if (!s) { pthread_mutex_unlock(&g_lock); return STATUS_SUCCESS; }
   /* gone from the table before the detach: of two swapchains of one window destroyed at once, the last empties it */
   old = *s;
   memset(s, 0, sizeof(*s));
   shared = window_shared(&old);
   pthread_cond_broadcast(&g_cond);
   pthread_mutex_unlock(&g_lock);
   detach_all(&old, shared);
   return STATUS_SUCCESS;
}
