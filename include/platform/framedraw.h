#ifndef GUARD_FRAMEDRAW_H
#define GUARD_FRAMEDRAW_H

#include "global.h"

void DrawFrame(uint16_t *pixels);
// Runs a frame's line interrupts and HBlank DMAs like DrawFrame, without
// drawing it, for a frame that's skipped
void SkipFrame(void);
#ifdef __PS2__
// Draws the frame with the GS (see ps2_gs.c), for PS2GS_Present to show
void DrawFramePS2(void);
#endif
#endif