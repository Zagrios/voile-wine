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
   unsigned int width, height, hz;
   struct npt_output_info *o;
   void *h;

   (void)expose_all_modes;
   if (!(h = dlopen("wineobelisk.so", RTLD_NOW | RTLD_NOLOAD))) return false;
   if (!(get_display = dlsym(h, "obelisk_wsd_get_display")) || !get_display(&width, &height, &hz)) return false;

   o = &list->outputs[0];
   memset(o, 0, sizeof(*o));
   snprintf(o->name, sizeof(o->name), "wsd-1");
   o->current_width = width;
   o->current_height = height;
   o->modes[o->num_modes++] = (struct npt_display_mode){ width, height, hz, 1, 32, true, false };
   for (unsigned int i = 0; i < sizeof(extra) / sizeof(extra[0]) && o->num_modes < NPT_DISPLAY_MAX_MODES; i++) {
      /* The same filter as wineobelisk.drv's OBELISK_UpdateDisplayDevices: the DXGI and Win32 mode lists must match. */
      if (extra[i].w > width || extra[i].h > height || (extra[i].w == width && extra[i].h == height)) continue;
      o->modes[o->num_modes++] = (struct npt_display_mode){ extra[i].w, extra[i].h, hz, 1, 32, false, false };
   }
   list->num_outputs = 1;
   return true;
}
