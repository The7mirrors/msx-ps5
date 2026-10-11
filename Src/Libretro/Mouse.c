#include "MsxTypes.h"
#include "ArchInput.h"

/* MSX PS5 (2026-10-10): the frontend's mouse (libretro.c gathers it each frame) */
void libretro_mouse_take(int* dx, int* dy);
int libretro_mouse_buttons(void);

void archMouseGetState(int* dx, int* dy)
{
    libretro_mouse_take(dx, dy);
}

int  archMouseGetButtonState(int checkAlways)
{
   return libretro_mouse_buttons();
}

void  archMouseEmuEnable(AmEnableMode mode)
{
}
