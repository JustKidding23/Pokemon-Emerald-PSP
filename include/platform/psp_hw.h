#ifndef GUARD_PLATFORM_PSP_HW_H
#define GUARD_PLATFORM_PSP_HW_H

// The PSP's system calls, kept in psp_hw.c so that the pspsdk's headers and
// the game's never meet. Only standard C types cross over.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PSP buttons, as sceCtrl reports them
#define PSP_BTN_SELECT   0x000001
#define PSP_BTN_START    0x000008
#define PSP_BTN_UP       0x000010
#define PSP_BTN_RIGHT    0x000020
#define PSP_BTN_DOWN     0x000040
#define PSP_BTN_LEFT     0x000080
#define PSP_BTN_L        0x000100
#define PSP_BTN_R        0x000200
#define PSP_BTN_TRIANGLE 0x001000
#define PSP_BTN_CIRCLE   0x002000
#define PSP_BTN_CROSS    0x004000
#define PSP_BTN_SQUARE   0x008000

enum
{
    PSP_SCALE_FIT,      // as big as fits with the GBA's shape, 408x272
    PSP_SCALE_STRETCH,  // the whole screen, 480x272
    PSP_SCALE_2X_CROP,  // twice the size, cropped at the top and bottom
    PSP_SCALE_1X,       // the GBA's size, 240x160
    PSP_SCALE_COUNT,
};

// Sets the CPU clock, the HOME button's exit callback, the controller and GU
void PspHw_Init(void);
// The watchdog: if the game stops calling PspHw_Heartbeat for 5 seconds, a
// thread shows what it was doing, from what was given to PspHw_SetDiag
enum
{
    PSP_DIAG_STEP,       // one of the PSP_STEP values
    PSP_DIAG_FRAME,
    PSP_DIAG_CALLBACK1,
    PSP_DIAG_CALLBACK2,
    PSP_DIAG_VBLANK,
    PSP_DIAG_TASKS,      // and the next 15: the active tasks' functions
    PSP_DIAG_CALL = PSP_DIAG_TASKS + 16, // the task or callback running
    PSP_DIAG_COUNT,
};
enum
{
    PSP_STEP_GAME = 1,   // MainLoop
    PSP_STEP_DRAW,       // DrawFrame or SkipFrame
    PSP_STEP_VBLANK,     // DMAs and the VBlank interrupt
    PSP_STEP_SOUND,      // the sound engine
    PSP_STEP_PRESENT,    // the GU, waiting for the display
    PSP_STEP_MENU,
    PSP_STEP_SAVE,       // writing to the memory stick
};
void PspHw_StartWatchdog(void);
void PspHw_Heartbeat(void);
void PspHw_SetDiag(int index, uint32_t value);
// Writes the diagnostics to path at every new step (see psp_hw.c)
void PspHw_StartTrace(const char *path);
bool PspHw_Tracing(void);
void PspHw_WriteTrace(void);
// False once the player has chosen to exit from the HOME menu
bool PspHw_Running(void);
void PspHw_Exit(void);

// The buttons held, with the analog stick as the D-pad
uint32_t PspHw_ReadButtons(void);

// Microseconds since some point at startup
uint64_t PspHw_Microseconds(void);

// A frame: the game's picture (240x160, ABGR1555), maybe dimmed under the
// menu, then text on top, then shown at the next vertical blank
void PspHw_DrawGame(const uint16_t *image, int scale, bool filter, bool dim);
void PspHw_Print(int column, int row, uint32_t color, const char *text);
void PspHw_Present(void);
// Text is 8x8 pixels, so the screen holds 60x34 characters
#define PSP_TEXT_COLUMNS 60
#define PSP_TEXT_ROWS 34

// Sound: stereo float frames at inRate, played at 44100 Hz
void PspHw_AudioInit(int inRate);
void PspHw_AudioQueue(const float *frames, int count);
void PspHw_AudioClear(void);

// Files on the memory stick, by absolute path
long PspHw_ReadFile(const char *path, void *buffer, size_t size);
// Written to path.tmp first, the old file kept as path.bak
bool PspHw_WriteFileSafely(const char *path, const void *data, size_t size);
bool PspHw_WriteFile(const char *path, const void *data, size_t size);
bool PspHw_RemoveFile(const char *path);
void PspHw_MakeDir(const char *path);

// Writes what's on the screen, 480x272 RGBA, for the emulator tests
void PspHw_DumpScreen(const char *path);

// The local date and time
struct PspHwTime
{
    int year, month, day, dayOfWeek, hour, minute, second;
};
void PspHw_GetLocalTime(struct PspHwTime *time);

#endif // GUARD_PLATFORM_PSP_HW_H
