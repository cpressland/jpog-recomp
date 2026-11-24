/* gui.h - the GUI (gui.c), for --gui and --park: one screen of it. */
#ifndef JPOG_GUI_H
#define JPOG_GUI_H
#include <stdint.h>
enum { GUI_W = 640, GUI_H = 480 };           /* what the dialogs are laid out for */

/* Its viewport is made last, over any other; it clears the picture only if
 * `clear`. 0 if it could not be. */
int  gui_open(uint32_t kernel, uint32_t r, const char *file, const char *locale, int clear);
/* A dialog, shown: x and y are what share of the room there is beside and
 * above it (0.5: the middle). 0 if there is none of that name. */
uint32_t gui_show(const char *dialog, float x, float y);
void gui_visible(uint32_t widget, int on);
void gui_frame(float seconds, float dt);     /* input and the clock, before the scene's frame */
extern void (*gui_pressed)(const char *widget);    /* the left button, over a widget that took it */
#endif
