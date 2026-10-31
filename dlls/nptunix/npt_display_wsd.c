#if 0
#pragma makedep unix
#endif
/*
 * Copyright 2026 Obelisk
 * SPDX-License-Identifier: MIT
 *
 * DXGI outputs from the wsd window server (main screen of EV_HELLO, through wineobelisk.so) instead of DRM KMS.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "npt_display.h"
#include "npt_unix_internal.h"

bool npt_enumerate_outputs(struct npt_output_list *list, bool expose_all_modes)
{
   static const struct { uint32_t w, h; } extra[] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,800},
                                                      {1600,900}, {1920,1080}, {2560,1440} };
   int (*get_display)(unsigned int *, unsigned int *, unsigned int *);
   int (*get_native)(unsigned int *, unsigned int *);
   unsigned int width, height, hz, native_width, native_height;
   struct npt_output_info *o;
   void *h;

   (void)expose_all_modes;
   if (!(h = dlopen("wineobelisk.so", RTLD_NOW | RTLD_NOLOAD))) return false;
   if (!(get_display = dlsym(h, "obelisk_wsd_get_display")) || !get_display(&width, &height, &hz)) return false;
   /* The modes are those of the screen without the display mode a game chose (the current mode is one of them): a game
    * that switched to 1280x720 can switch back. A wineobelisk.so from before it has only the current screen. */
   native_width = width;
   native_height = height;
   if ((get_native = dlsym(h, "obelisk_wsd_get_native_display"))) get_native(&native_width, &native_height);

   o = &list->outputs[0];
   memset(o, 0, sizeof(*o));
   snprintf(o->name, sizeof(o->name), "wsd-1");
   o->current_width = width;
   o->current_height = height;
   o->modes[o->num_modes++] = (struct npt_display_mode){ native_width, native_height, hz, 1, 32, width == native_width && height == native_height, false };
   for (unsigned int i = 0; i < sizeof(extra) / sizeof(extra[0]) && o->num_modes < NPT_DISPLAY_MAX_MODES; i++) {
      /* The same filter as wineobelisk.drv's OBELISK_UpdateDisplayDevices: the DXGI and Win32 mode lists must match. */
      if (extra[i].w > native_width || extra[i].h > native_height ||
          (extra[i].w == native_width && extra[i].h == native_height)) continue;
      o->modes[o->num_modes++] = (struct npt_display_mode){ extra[i].w, extra[i].h, hz, 1, 32, extra[i].w == width && extra[i].h == height, false };
   }
   if ((width != native_width || height != native_height) && o->num_modes < NPT_DISPLAY_MAX_MODES) {
      unsigned int i;
      for (i = 0; i < o->num_modes; i++) if (o->modes[i].width == width && o->modes[i].height == height) break;
      if (i == o->num_modes) /* a desktop wsd held above 640x480 */
         o->modes[o->num_modes++] = (struct npt_display_mode){ width, height, hz, 1, 32, true, false };
   }
   list->num_outputs = 1;
   return true;
}
