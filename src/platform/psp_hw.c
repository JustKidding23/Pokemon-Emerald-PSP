#if defined(PORTABLE) && defined(__PSP__)
// The PSP's side of the port: video with the GU, sound, the buttons, files and
// the clock. Nothing here includes the game's headers (see psp_hw.h).
#include <stdio.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>

#include "platform/psp_hw.h"

PSP_MODULE_INFO("Pokemon Emerald PSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// Everything but 1 MB for the system's threads and their stacks
PSP_HEAP_SIZE_KB(-1024);
// pret's code has some big local arrays
PSP_MAIN_THREAD_STACK_SIZE_KB(512);

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define BUFFER_WIDTH 512
#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define FRAME_SIZE (BUFFER_WIDTH * SCREEN_HEIGHT * 4)

static volatile bool sRunning = true;
static unsigned int __attribute__((aligned(64))) sDisplayList[64 * 1024];
// Where the GU is drawing, as an offset into VRAM
static void *sDrawBuffer;

static int ExitCallback(int arg1, int arg2, void *common)
{
    sRunning = false;
    return 0;
}

static int CallbackThread(SceSize args, void *argp)
{
    int id = sceKernelCreateCallback("Exit Callback", ExitCallback, NULL);

    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

void PspHw_Init(void)
{
    int thread = sceKernelCreateThread("callbacks", CallbackThread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);

    if (thread >= 0)
        sceKernelStartThread(thread, 0, NULL);

    scePowerSetClockFrequency(333, 333, 166);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    // Two 32-bit frame buffers at the start of VRAM, and no depth buffer
    sDrawBuffer = (void *)0;
    sceGuInit();
    sceGuStart(GU_DIRECT, sDisplayList);
    sceGuDrawBuffer(GU_PSM_8888, sDrawBuffer, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)FRAME_SIZE, BUFFER_WIDTH);
    sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    pspDebugScreenInitEx((void *)(0x44000000 + (uintptr_t)sDrawBuffer), PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
    pspDebugScreenEnableBackColor(0);
}

bool PspHw_Running(void)
{
    return sRunning;
}

void PspHw_Exit(void)
{
    sceKernelExitGame();
}

uint32_t PspHw_ReadButtons(void)
{
    SceCtrlData pad;
    uint32_t buttons;

    sceCtrlPeekBufferPositive(&pad, 1);
    buttons = pad.Buttons;
    // The stick works as the D-pad once it's past half way
    if (pad.Lx < 64)
        buttons |= PSP_BTN_LEFT;
    else if (pad.Lx > 192)
        buttons |= PSP_BTN_RIGHT;
    if (pad.Ly < 64)
        buttons |= PSP_BTN_UP;
    else if (pad.Ly > 192)
        buttons |= PSP_BTN_DOWN;
    return buttons;
}

uint64_t PspHw_Microseconds(void)
{
    return sceKernelGetSystemTimeWide();
}

struct TexVertex
{
    unsigned short u, v;
    short x, y, z;
};

struct ColorVertex
{
    unsigned int color;
    short x, y, z;
};

void PspHw_DrawGame(const uint16_t *image, int scale, bool filter, bool dim)
{
    int x = 0, y = 0, w = SCREEN_WIDTH, h = SCREEN_HEIGHT;
    int u0 = 0, v0 = 0, u1 = GBA_WIDTH, v1 = GBA_HEIGHT;
    struct TexVertex *verts;

    switch (scale)
    {
    case PSP_SCALE_FIT:
        // 1.7 times, which keeps the shape and fills the height
        w = GBA_WIDTH * SCREEN_HEIGHT / GBA_HEIGHT;
        break;
    case PSP_SCALE_2X_CROP:
        // 480x320 with 24 lines cut off at the top and bottom
        v0 = (2 * GBA_HEIGHT - SCREEN_HEIGHT) / 4;
        v1 = GBA_HEIGHT - v0;
        break;
    case PSP_SCALE_1X:
        w = GBA_WIDTH;
        h = GBA_HEIGHT;
        break;
    }
    x = (SCREEN_WIDTH - w) / 2;
    y = (SCREEN_HEIGHT - h) / 2;

    sceKernelDcacheWritebackRange(image, GBA_WIDTH * GBA_HEIGHT * 2);

    sceGuStart(GU_DIRECT, sDisplayList);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    // The rows are 240 pixels apart in the image, in a 256x256 texture
    sceGuTexImage(0, 256, 256, GBA_WIDTH, image);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(filter ? GU_LINEAR : GU_NEAREST, filter ? GU_LINEAR : GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFlush();

    // In strips 32 texels wide, which the texture cache handles best
    {
        int strips = (u1 - u0 + 31) / 32;

        verts = sceGuGetMemory(strips * 2 * sizeof(struct TexVertex));
        for (int i = 0; i < strips; i++)
        {
            int su0 = u0 + i * 32;
            int su1 = su0 + 32 > u1 ? u1 : su0 + 32;

            verts[i * 2].u = su0;
            verts[i * 2].v = v0;
            verts[i * 2].x = x + (su0 - u0) * w / (u1 - u0);
            verts[i * 2].y = y;
            verts[i * 2].z = 0;
            verts[i * 2 + 1].u = su1;
            verts[i * 2 + 1].v = v1;
            verts[i * 2 + 1].x = x + (su1 - u0) * w / (u1 - u0);
            verts[i * 2 + 1].y = y + h;
            verts[i * 2 + 1].z = 0;
        }
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, strips * 2, NULL, verts);
    }
    sceGuDisable(GU_TEXTURE_2D);

    if (dim)
    {
        struct ColorVertex *rect = sceGuGetMemory(2 * sizeof(struct ColorVertex));

        rect[0].color = rect[1].color = 0xE0000000;
        rect[0].x = rect[0].y = rect[0].z = 0;
        rect[1].x = SCREEN_WIDTH;
        rect[1].y = SCREEN_HEIGHT;
        rect[1].z = 0;
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, rect);
        sceGuDisable(GU_BLEND);
    }
    sceGuFinish();
}

// Text waits until the GU has drawn the frame, and then goes on top
#define MAX_TEXT 4096
static struct { short column, row; uint32_t color; } sTextRuns[128];
static char sText[MAX_TEXT];
static int sNumTextRuns;
static int sTextUsed;

void PspHw_Print(int column, int row, uint32_t color, const char *text)
{
    size_t length = strlen(text) + 1;

    if (sNumTextRuns == (int)(sizeof(sTextRuns) / sizeof(sTextRuns[0])) || sTextUsed + length > MAX_TEXT)
        return;
    sTextRuns[sNumTextRuns].column = column;
    sTextRuns[sNumTextRuns].row = row;
    sTextRuns[sNumTextRuns].color = color;
    memcpy(sText + sTextUsed, text, length);
    sTextUsed += length;
    sNumTextRuns++;
}

void PspHw_Present(void)
{
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    if (sNumTextRuns != 0)
    {
        const char *text = sText;

        pspDebugScreenSetBase((u32 *)(0x44000000 + (uintptr_t)sDrawBuffer));
        for (int i = 0; i < sNumTextRuns; i++)
        {
            int column = sTextRuns[i].column;

            for (; *text != '\0'; text++, column++)
            {
                if (column >= 0 && column < PSP_TEXT_COLUMNS && sTextRuns[i].row >= 0 && sTextRuns[i].row < PSP_TEXT_ROWS)
                {
                    // A shadow, so that the text reads over the game too
                    pspDebugScreenPutChar(column * 8 + 1, sTextRuns[i].row * 8 + 1, 0xFF000000, *text);
                    pspDebugScreenPutChar(column * 8, sTextRuns[i].row * 8, sTextRuns[i].color, *text);
                }
            }
            text++;
        }
        sNumTextRuns = 0;
        sTextUsed = 0;
    }

    sceDisplayWaitVblankStartCB();
    sDrawBuffer = sceGuSwapBuffers();
}

// Sound. The game's frames go into a ring that the sound thread reads at
// 44100 Hz, a little faster when it fills up and a little slower when it runs
// low, so that the game's 60 frames a second and the display's 59.94 never
// make it run dry or fall behind.
#define RING_FRAMES 8192
#define OUT_FRAMES 512
static float sRing[RING_FRAMES][2];
static volatile uint32_t sRingWrite, sRingRead;
static volatile bool sRingClear;
static int sInRate;
static int sAudioChannel = -1;

static int AudioThread(SceSize args, void *argp)
{
    static int16_t out[2][OUT_FRAMES][2];
    int buffer = 0;
    float pos = 0;
    bool primed = false;
    // Two of the game's frames plus a buffer's worth: enough to ride out a
    // slow frame without much delay
    const uint32_t target = sInRate / 30 + OUT_FRAMES * sInRate / 44100;

    while (sRunning)
    {
        uint32_t read = sRingRead;
        uint32_t avail;
        float step;

        if (sRingClear)
        {
            read = sRingWrite;
            sRingClear = false;
            primed = false;
        }
        avail = sRingWrite - read;
        if (!primed && avail >= target)
            primed = true;
        // Up to half a percent faster or slower, which can't be heard
        step = (float)sInRate / 44100.0f;
        step *= 1.0f + 0.005f * ((float)avail - (float)target) / (float)target;
        if (step < (float)sInRate / 44100.0f * 0.995f)
            step = (float)sInRate / 44100.0f * 0.995f;
        if (step > (float)sInRate / 44100.0f * 1.005f)
            step = (float)sInRate / 44100.0f * 1.005f;

        for (int i = 0; i < OUT_FRAMES; i++)
        {
            for (int c = 0; c < 2; c++)
            {
                float sample = 0;

                if (primed && avail >= 2)
                {
                    float a = sRing[read % RING_FRAMES][c];
                    float b = sRing[(read + 1) % RING_FRAMES][c];

                    sample = (a + (b - a) * pos) * 32767.0f;
                    if (sample > 32767.0f)
                        sample = 32767.0f;
                    else if (sample < -32768.0f)
                        sample = -32768.0f;
                }
                out[buffer][i][c] = (int16_t)sample;
            }
            if (primed && avail >= 2)
            {
                pos += step;
                while (pos >= 1.0f)
                {
                    pos -= 1.0f;
                    read++;
                    avail--;
                }
            }
            else
            {
                // Ran dry: wait for it to fill up again
                primed = false;
            }
        }
        sRingRead = read;
        sceAudioOutputPannedBlocking(sAudioChannel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, out[buffer]);
        buffer ^= 1;
    }
    return 0;
}

void PspHw_AudioInit(int inRate)
{
    int thread;

    sInRate = inRate;
    sAudioChannel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (sAudioChannel < 0)
        return;
    // Above the game's thread, which runs at 0x20 and never waits for long
    thread = sceKernelCreateThread("sound", AudioThread, 0x12, 0x10000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (thread >= 0)
        sceKernelStartThread(thread, 0, NULL);
}

void PspHw_AudioQueue(const float *frames, int count)
{
    uint32_t write = sRingWrite;

    // When the game runs ahead, like when it's sped up, what doesn't fit is dropped
    if (count > (int)(RING_FRAMES - 1 - (write - sRingRead)))
        return;
    for (int i = 0; i < count; i++, write++)
    {
        sRing[write % RING_FRAMES][0] = frames[i * 2];
        sRing[write % RING_FRAMES][1] = frames[i * 2 + 1];
    }
    sRingWrite = write;
}

void PspHw_AudioClear(void)
{
    sRingClear = true;
}

long PspHw_ReadFile(const char *path, void *buffer, size_t size)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    long total = 0;

    if (fd < 0)
        return -1;
    while ((size_t)total < size)
    {
        int got = sceIoRead(fd, (char *)buffer + total, size - total);

        if (got <= 0)
            break;
        total += got;
    }
    sceIoClose(fd);
    return total;
}

bool PspHw_WriteFile(const char *path, const void *data, size_t size)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    size_t done = 0;

    if (fd < 0)
        return false;
    while (done < size)
    {
        int written = sceIoWrite(fd, (const char *)data + done, size - done);

        if (written <= 0)
        {
            sceIoClose(fd);
            return false;
        }
        done += written;
    }
    return sceIoClose(fd) >= 0;
}

bool PspHw_WriteFileSafely(const char *path, const void *data, size_t size)
{
    char tempPath[256], backupPath[256];

    snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    if (!PspHw_WriteFile(tempPath, data, size))
    {
        sceIoRemove(tempPath);
        return false;
    }
    // Only replace the save once the new one is all on the memory stick.
    // sceIoRename doesn't overwrite, so the old one becomes the backup.
    sceIoRemove(backupPath);
    sceIoRename(path, backupPath);
    if (sceIoRename(tempPath, path) < 0)
    {
        sceIoRename(backupPath, path);
        return false;
    }
    return true;
}

bool PspHw_RemoveFile(const char *path)
{
    return sceIoRemove(path) >= 0;
}

void PspHw_MakeDir(const char *path)
{
    sceIoMkdir(path, 0777);
}

void PspHw_DumpScreen(const char *path)
{
    static uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
    void *frame;
    int width, format;

    sceDisplayGetFrameBuf(&frame, &width, &format, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (frame == NULL)
        return;
    for (int y = 0; y < SCREEN_HEIGHT; y++)
        memcpy(&pixels[y * SCREEN_WIDTH], (uint32_t *)((uintptr_t)frame | 0x40000000) + y * width, SCREEN_WIDTH * 4);
    PspHw_WriteFile(path, pixels, sizeof(pixels));
}

void PspHw_GetLocalTime(struct PspHwTime *time)
{
    ScePspDateTime now;

    sceRtcGetCurrentClockLocalTime(&now);
    time->year = now.year;
    time->month = now.month;
    time->day = now.day;
    time->dayOfWeek = sceRtcGetDayOfWeek(now.year, now.month, now.day);
    time->hour = now.hour;
    time->minute = now.minute;
    time->second = now.second;
}
#endif // PORTABLE && __PSP__
