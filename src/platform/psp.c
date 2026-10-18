#if defined(PORTABLE) && defined(__PSP__)
// The PSP backend: the frame loop, the buttons, the save files and the menu
// that switches between them. psp_hw.c does the talking to the system.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "platform.h"
#include "rtc.h"
#include "save.h"
#include "task.h"
#include "constants/characters.h"
#include "constants/flags.h"
#include "constants/rgb.h"
#include "fonts.h"
#include "string_util.h"
#include "strings.h"
#include "text_window.h"
#include "gba/flash_internal.h"
#include "platform/framedraw.h"
#include "platform/psp_hw.h"
#include "platform/system.h"
#include "cgb_audio.h"

extern void AgbMain(void);

#define NUM_SLOTS 3
#define SAVE_SIZE sizeof(FLASH_BASE)

// A frame of the display: the game runs a frame for each of them
#define FRAME_MICROSECONDS 16683

enum
{
    FRAMESKIP_AUTO,
    FRAMESKIP_OFF,
    FRAMESKIP_COUNT,
};

static struct
{
    int slot;
    int scale;
    bool filter;
    bool circleIsA;
    int frameskip;
    bool showSpeed;
    bool trace;
    int turboSpeed;
} sSettings = {
    .slot = 1,
    .scale = PSP_SCALE_FIT,
    .filter = false,
    .circleIsA = false,
    .frameskip = FRAMESKIP_AUTO,
    .showSpeed = false,
    .turboSpeed = 2,
};

// The folder EBOOT.PBP is in, ending with a slash
static char sBaseDir[256];
static u16 sKeys;
static int sPendingSlot;
static char sStatus[PSP_TEXT_COLUMNS + 1];
static u32 sTotalFrames;
static u32 sStatusFrames;

struct SaveFileInfo
{
    bool known;
    bool exists;
    bool hasGame;
    u8 name[PLAYER_NAME_LENGTH + 1]; // in the game's characters
    u8 gender;
    u16 hours;
    u8 minutes;
    u8 badges;
};
static struct SaveFileInfo sSlotInfo[NUM_SLOTS + 1];

static void ShowStatus(const char *text)
{
    snprintf(sStatus, sizeof(sStatus), "%s", text);
    sStatusFrames = 180;
}

static void SlotPath(int slot, char *path, size_t size)
{
    snprintf(path, size, "%ssaves/slot%d.sav", sBaseDir, slot);
}

static void SettingsPath(char *path, size_t size)
{
    snprintf(path, size, "%ssettings.ini", sBaseDir);
}

static int ClampSetting(int value, int min, int max, int fallback)
{
    return (value < min || value > max) ? fallback : value;
}

static void LoadSettings(void)
{
    char path[300];
    static char text[1024];
    long size;
    char *line;

    SettingsPath(path, sizeof(path));
    size = PspHw_ReadFile(path, text, sizeof(text) - 1);
    if (size <= 0)
        return;
    text[size] = '\0';
    for (line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n"))
    {
        char name[32];
        int value;

        if (sscanf(line, "%31[a-z_] = %d", name, &value) != 2)
            continue;
        if (strcmp(name, "slot") == 0)
            sSettings.slot = ClampSetting(value, 1, NUM_SLOTS, 1);
        else if (strcmp(name, "scale") == 0)
            sSettings.scale = ClampSetting(value, 0, PSP_SCALE_COUNT - 1, PSP_SCALE_FIT);
        else if (strcmp(name, "smoothing") == 0)
            sSettings.filter = value != 0;
        else if (strcmp(name, "circle_is_a") == 0)
            sSettings.circleIsA = value != 0;
        else if (strcmp(name, "frameskip") == 0)
            sSettings.frameskip = ClampSetting(value, 0, FRAMESKIP_COUNT - 1, FRAMESKIP_AUTO);
        else if (strcmp(name, "show_speed") == 0)
            sSettings.showSpeed = value != 0;
        else if (strcmp(name, "trace") == 0)
            sSettings.trace = value != 0;
        else if (strcmp(name, "turbo_speed") == 0)
            sSettings.turboSpeed = ClampSetting(value, 2, 3, 2);
    }
}

static void StoreSettings(void)
{
    char path[300];
    char text[768];
    int length;

    SettingsPath(path, sizeof(path));
    length = snprintf(text, sizeof(text),
                      "# Pokemon Emerald PSP settings. The settings menu (the music note button) changes them.\n"
                      "# The save file that's played, from saves/slot1.sav to saves/slot%d.sav\n"
                      "slot = %d\n"
                      "# 0 fits the screen keeping the shape, 1 stretches, 2 is twice the size cut at the\n"
                      "# top and bottom, 3 is the GBA's size\n"
                      "scale = %d\n"
                      "smoothing = %d\n"
                      "# 1 makes Circle the A button and Cross the B button\n"
                      "circle_is_a = %d\n"
                      "# 0 skips drawing frames when the PSP falls behind, 1 slows the game down instead\n"
                      "frameskip = %d\n"
                      "show_speed = %d\n"
                      "# How fast holding Square runs the game: 2 or 3 times\n"
                      "turbo_speed = %d\n"
                      "# 1 writes what the game is doing to trace.bin all the time, to find crashes (slow)\n"
                      "trace = %d\n",
                      NUM_SLOTS, sSettings.slot, sSettings.scale, sSettings.filter, sSettings.circleIsA,
                      sSettings.frameskip, sSettings.showSpeed, sSettings.turboSpeed, sSettings.trace);
    PspHw_WriteFile(path, text, length);
}

// The save's structs have the GBA's layout on the PSP, which has 32-bit
// pointers like the GBA, so these offsets are where they are in a save file
STATIC_ASSERT(offsetof(struct SaveBlock1, flags) == 0x1270, SaveBlock1FlagsOffset);
STATIC_ASSERT(offsetof(struct SaveBlock2, playTimeHours) == 0x0E, SaveBlock2PlayTimeOffset);

// What the save screen shows for a save: the newest of the two copies the game
// keeps (see save.c), read straight from the flash's sectors
static void ReadSaveInfo(const u8 *flash, struct SaveFileInfo *info)
{
    static u8 flags[NUM_FLAG_BYTES];
    u32 bestCounter = 0;
    int bestHalf = -1;

    info->hasGame = false;
    for (int half = 0; half < 2; half++)
    {
        for (int i = 0; i < NUM_SECTORS_PER_SLOT; i++)
        {
            const struct SaveSector *sector = (const void *)(flash + (half * NUM_SECTORS_PER_SLOT + i) * SECTOR_SIZE);

            if (sector->signature == SECTOR_SIGNATURE && sector->id == SECTOR_ID_SAVEBLOCK2
             && (bestHalf < 0 || sector->counter > bestCounter))
            {
                bestHalf = half;
                bestCounter = sector->counter;
            }
        }
    }
    if (bestHalf < 0)
        return;

    memset(flags, 0, sizeof(flags));
    for (int i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        const struct SaveSector *sector = (const void *)(flash + (bestHalf * NUM_SECTORS_PER_SLOT + i) * SECTOR_SIZE);

        if (sector->signature != SECTOR_SIGNATURE)
            continue;
        if (sector->id == SECTOR_ID_SAVEBLOCK2)
        {
            const u8 *block = sector->data;

            memcpy(info->name, block + offsetof(struct SaveBlock2, playerName), PLAYER_NAME_LENGTH);
            info->name[PLAYER_NAME_LENGTH] = EOS;
            info->gender = block[offsetof(struct SaveBlock2, playerGender)];
            info->hours = block[offsetof(struct SaveBlock2, playTimeHours)] | (block[offsetof(struct SaveBlock2, playTimeHours) + 1] << 8);
            info->minutes = block[offsetof(struct SaveBlock2, playTimeMinutes)];
            info->hasGame = true;
        }
        else if (sector->id >= SECTOR_ID_SAVEBLOCK1_START && sector->id <= SECTOR_ID_SAVEBLOCK1_END)
        {
            // The part of SaveBlock1's flags that's in this sector
            u32 start = (sector->id - SECTOR_ID_SAVEBLOCK1_START) * SECTOR_DATA_SIZE;

            for (u32 j = 0; j < NUM_FLAG_BYTES; j++)
            {
                u32 offset = offsetof(struct SaveBlock1, flags) + j;

                if (offset >= start && offset < start + SECTOR_DATA_SIZE)
                    flags[j] = sector->data[offset - start];
            }
        }
    }

    info->badges = 0;
    for (int badge = 0; badge < NUM_BADGES; badge++)
    {
        u32 flag = FLAG_BADGE01_GET + badge;

        if ((flags[flag / 8] >> (flag % 8)) & 1)
            info->badges++;
    }
}

static void RefreshSlotInfo(int slot)
{
    static u8 buffer[SAVE_SIZE];
    struct SaveFileInfo *info = &sSlotInfo[slot];
    char path[300];
    long size;

    memset(info, 0, sizeof(*info));
    info->known = true;
    if (slot == sSettings.slot)
    {
        info->exists = true;
        ReadSaveInfo(FLASH_BASE, info);
        return;
    }
    SlotPath(slot, path, sizeof(path));
    size = PspHw_ReadFile(path, buffer, sizeof(buffer));
    if (size < 0)
        return;
    info->exists = true;
    memset(buffer + size, 0xFF, sizeof(buffer) - size);
    ReadSaveInfo(buffer, info);
}

// A missing or short save reads as erased flash, like a new cartridge
static void ReadSaveFile(int slot)
{
    char path[300];
    long size;

    SlotPath(slot, path, sizeof(path));
    size = PspHw_ReadFile(path, FLASH_BASE, SAVE_SIZE);
    if (size < 0)
        size = 0;
    memset(FLASH_BASE + size, 0xFF, SAVE_SIZE - size);
}

void Platform_StoreSaveFile(void)
{
    char path[300];

    PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_SAVE);

    SlotPath(sSettings.slot, path, sizeof(path));
    if (!PspHw_WriteFileSafely(path, FLASH_BASE, SAVE_SIZE))
        ShowStatus("The save could not be written to the memory stick!");
    sSlotInfo[sSettings.slot].known = false;
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    u32 start = (sectorNum << gFlash->sector.shift) + offset;

    if (start >= SAVE_SIZE || size > SAVE_SIZE - start)
        return;
    memcpy(dest, FLASH_BASE + start, size);
}

static bool CopySlot(int from, int to)
{
    static u8 buffer[SAVE_SIZE];
    char path[300];
    long size;

    if (from == sSettings.slot)
    {
        memcpy(buffer, FLASH_BASE, SAVE_SIZE);
        size = SAVE_SIZE;
    }
    else
    {
        SlotPath(from, path, sizeof(path));
        size = PspHw_ReadFile(path, buffer, sizeof(buffer));
        if (size < 0)
            return false;
    }
    SlotPath(to, path, sizeof(path));
    sSlotInfo[to].known = false;
    return PspHw_WriteFileSafely(path, buffer, size);
}

// The backup of the save before it goes too, so nothing's left of it
static bool DeleteSlot(int slot)
{
    char path[300], backupPath[310];

    SlotPath(slot, path, sizeof(path));
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    sSlotInfo[slot].known = false;
    PspHw_RemoveFile(backupPath);
    return PspHw_RemoveFile(path);
}

// Earlier builds and other ports keep one pokeemerald.sav. If there's one next
// to the EBOOT and no save files yet, it becomes the first one.
static void AdoptOldSave(void)
{
    static u8 buffer[SAVE_SIZE];
    char path[300];
    long size;

    SlotPath(1, path, sizeof(path));
    if (PspHw_ReadFile(path, buffer, 1) >= 0)
        return;
    snprintf(path, sizeof(path), "%spokeemerald.sav", sBaseDir);
    size = PspHw_ReadFile(path, buffer, sizeof(buffer));
    if (size <= 0)
        return;
    SlotPath(1, path, sizeof(path));
    PspHw_WriteFile(path, buffer, size);
}

u16 Platform_GetKeyInput(void)
{
    return sKeys;
}

// Buttons held when a menu closed, which the game doesn't get until they're
// let go, so that closing it doesn't press them in the game too
static u32 sMaskedButtons;

static u16 MapButtons(u32 buttons)
{
    u16 keys = 0;
    u32 a = sSettings.circleIsA ? PSP_BTN_CIRCLE : PSP_BTN_CROSS;
    u32 b = sSettings.circleIsA ? PSP_BTN_CROSS : PSP_BTN_CIRCLE;

    if (buttons & a)
        keys |= A_BUTTON;
    if (buttons & b)
        keys |= B_BUTTON;
    if (buttons & PSP_BTN_SELECT)
        keys |= SELECT_BUTTON;
    if (buttons & PSP_BTN_START)
        keys |= START_BUTTON;
    if (buttons & PSP_BTN_RIGHT)
        keys |= DPAD_RIGHT;
    if (buttons & PSP_BTN_LEFT)
        keys |= DPAD_LEFT;
    if (buttons & PSP_BTN_UP)
        keys |= DPAD_UP;
    if (buttons & PSP_BTN_DOWN)
        keys |= DPAD_DOWN;
    if (buttons & PSP_BTN_R)
        keys |= R_BUTTON;
    if (buttons & PSP_BTN_L)
        keys |= L_BUTTON;
    // Both of a pair at once can't happen on the GBA's D-pad
    if ((keys & (DPAD_LEFT | DPAD_RIGHT)) == (DPAD_LEFT | DPAD_RIGHT))
        keys &= ~(DPAD_LEFT | DPAD_RIGHT);
    if ((keys & (DPAD_UP | DPAD_DOWN)) == (DPAD_UP | DPAD_DOWN))
        keys &= ~(DPAD_UP | DPAD_DOWN);
    return keys;
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    // samplesPerFrame is in bytes here, of stereo floats
    PspHw_AudioQueue(audioBuffer, samplesPerFrame / (2 * sizeof(float)));
}

void VBlankIntrWait(void)
{
}

// The clock: the PSP's, which the game can't set, like the PC builds
static struct SiiRtcInfo sClock;

u8 BinToBcd(u8 bin)
{
    return ((bin / 10) << 4) | (bin % 10);
}

static void UpdateClock(void)
{
    struct PspHwTime now;

    PspHw_GetLocalTime(&now);
    sClock.year = BinToBcd(now.year % 100);
    sClock.month = BinToBcd(now.month);
    sClock.day = BinToBcd(now.day);
    sClock.dayOfWeek = BinToBcd(now.dayOfWeek);
    sClock.hour = BinToBcd(now.hour);
    sClock.minute = BinToBcd(now.minute);
    sClock.second = BinToBcd(now.second);
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = sClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    sClock.status = rtc->status;
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateClock();
    rtc->year = sClock.year;
    rtc->month = sClock.month;
    rtc->day = sClock.day;
    rtc->dayOfWeek = sClock.dayOfWeek;
    rtc->hour = sClock.hour;
    rtc->minute = sClock.minute;
    rtc->second = sClock.second;
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateClock();
    rtc->hour = sClock.hour;
    rtc->minute = sClock.minute;
    rtc->second = sClock.second;
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
}

void Platform_SetAlarm(u8 *alarmData)
{
    // The game never sets an alarm
}

#define COLOR_WHITE  0xFFFFFFFF
#define COLOR_GRAY   0xFFA0A0A0
#define COLOR_YELLOW 0xFF40E0FF

// The settings menu, on the music note button (or Select and Triangle). It's
// drawn in the PSP's debug text over the game, dimmed.
enum
{
    ROW_SETTING_SCALE,
    ROW_SETTING_FILTER,
    ROW_SETTING_BUTTONS,
    ROW_SETTING_FRAMESKIP,
    ROW_SETTING_TURBO,
    ROW_SETTING_SPEED,
    ROW_BACK,
    ROW_COUNT,
};

static struct
{
    bool open;
    int row;
    bool changed;
} sSettingsMenu;

static void OpenSettings(void)
{
    sSettingsMenu.open = true;
    sSettingsMenu.row = 0;
    sSettingsMenu.changed = false;
}

static void CloseSettings(void)
{
    sSettingsMenu.open = false;
    if (sSettingsMenu.changed)
        StoreSettings();
}

static void ChangeSetting(int row, int delta)
{
    switch (row)
    {
    case ROW_SETTING_SCALE:
        sSettings.scale = (sSettings.scale + delta + PSP_SCALE_COUNT) % PSP_SCALE_COUNT;
        break;
    case ROW_SETTING_FILTER:
        sSettings.filter = !sSettings.filter;
        break;
    case ROW_SETTING_BUTTONS:
        sSettings.circleIsA = !sSettings.circleIsA;
        break;
    case ROW_SETTING_FRAMESKIP:
        sSettings.frameskip = (sSettings.frameskip + delta + FRAMESKIP_COUNT) % FRAMESKIP_COUNT;
        break;
    case ROW_SETTING_TURBO:
        sSettings.turboSpeed = sSettings.turboSpeed == 2 ? 3 : 2;
        break;
    case ROW_SETTING_SPEED:
        sSettings.showSpeed = !sSettings.showSpeed;
        break;
    default:
        return;
    }
    sSettingsMenu.changed = true;
}

static void UpdateSettings(u32 pressed)
{
    if (pressed & (PSP_BTN_NOTE | PSP_BTN_CIRCLE | PSP_BTN_TRIANGLE))
    {
        CloseSettings();
        return;
    }
    if (pressed & PSP_BTN_UP)
        sSettingsMenu.row = (sSettingsMenu.row + ROW_COUNT - 1) % ROW_COUNT;
    if (pressed & PSP_BTN_DOWN)
        sSettingsMenu.row = (sSettingsMenu.row + 1) % ROW_COUNT;
    if (pressed & PSP_BTN_LEFT)
        ChangeSetting(sSettingsMenu.row, -1);
    if (pressed & PSP_BTN_RIGHT)
        ChangeSetting(sSettingsMenu.row, 1);
    if (pressed & PSP_BTN_CROSS)
    {
        if (sSettingsMenu.row == ROW_BACK)
            CloseSettings();
        else
            ChangeSetting(sSettingsMenu.row, 1);
    }
}

static void DrawSettings(void)
{
    static const char *const sScaleNames[] = {"Fit", "Stretch", "2x (cropped)", "Original size"};
    static const char *const sFrameskipNames[] = {"Auto", "Off"};
    char text[PSP_TEXT_COLUMNS + 1];
    int row = 2;

    PspHw_Print(2, row, COLOR_YELLOW, "POKEMON EMERALD PSP SETTINGS");
    row += 2;
    for (int i = 0; i < ROW_COUNT; i++, row++)
    {
        bool selected = sSettingsMenu.row == i;
        u32 color = selected ? COLOR_WHITE : COLOR_GRAY;
        const char *name = "", *value = "";

        switch (i)
        {
        case ROW_SETTING_SCALE:
            name = "Screen size";
            value = sScaleNames[sSettings.scale];
            break;
        case ROW_SETTING_FILTER:
            name = "Smoothing";
            value = sSettings.filter ? "On" : "Off";
            break;
        case ROW_SETTING_BUTTONS:
            name = "A and B buttons";
            value = sSettings.circleIsA ? "Circle is A, Cross is B" : "Cross is A, Circle is B";
            break;
        case ROW_SETTING_FRAMESKIP:
            name = "Frame skip";
            value = sFrameskipNames[sSettings.frameskip];
            break;
        case ROW_SETTING_TURBO:
            name = "Turbo (hold Square)";
            value = sSettings.turboSpeed == 2 ? "2x" : "3x";
            break;
        case ROW_SETTING_SPEED:
            name = "Show speed";
            value = sSettings.showSpeed ? "On" : "Off";
            break;
        case ROW_BACK:
            row++;
            name = "Back to the game";
            break;
        }
        PspHw_Print(2, row, COLOR_WHITE, selected ? ">" : " ");
        PspHw_Print(4, row, color, name);
        if (*value != '\0')
        {
            snprintf(text, sizeof(text), "< %s >", value);
            PspHw_Print(26, row, color, text);
        }
    }
    PspHw_Print(2, 30, COLOR_GRAY, "Left and Right: change   Circle or the note button: back");
}

// The save file screen, from FILE SELECT in the start menu and the main menu
// (see Platform_OpenFileSelect). It's drawn like the game's CONTINUE
// screen (see main_menu.c), with its font, window frame and colors, into an
// image of the GBA's size.
#define SAVES_BG_COLOR    RGB(17, 18, 31) // graphics/interface/main_menu_bg.pal
#define TEXT_WHITE        RGB_WHITE
#define TEXT_DARK         RGB(12, 12, 12)
#define TEXT_SHADOW       RGB(26, 26, 25)
#define TEXT_MALE         RGB(4, 16, 31)
#define TEXT_FEMALE       RGB(31, 3, 21)
#define HELP_SHADOW       RGB(8, 8, 16)

// Where the windows are, in tiles, like the main menu's
#define FILE_WINDOW_LEFT  2
#define FILE_WINDOW_WIDTH 26
#define FILE_WINDOW_HEIGHT 4
#define FILE_WINDOW_TOP(i) (1 + (i) * 6)

enum
{
    SAVES_LIST,     // picking a file
    SAVES_ACTIONS,  // what to do with it
    SAVES_COPY_TO,  // where to copy it
    SAVES_CONFIRM,  // yes or no
    SAVES_MESSAGE,  // a message, until A or B
};

enum
{
    ACTION_PLAY,
    ACTION_COPY,
    ACTION_DELETE,
    ACTION_CANCEL,
    ACTION_COUNT,
};

enum
{
    CONFIRM_SWITCH,
    CONFIRM_RELOAD,
    CONFIRM_DELETE,
    CONFIRM_OVERWRITE,
};

static struct
{
    bool open;
    int mode;
    int file;       // 0 to NUM_SLOTS - 1, the file the cursor is on
    int action;
    int source;     // the file being copied
    int confirm;
    bool yes;
    char message[2][40];
} sSaves;

static u16 __attribute__((aligned(16))) sSavesImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
// Like the main menu's darkening of the windows that aren't selected
static bool sDarken;

static u16 DarkenColor(u16 color)
{
    u32 r = color & 0x1F, g = (color >> 5) & 0x1F, b = (color >> 10) & 0x1F;

    // BLDY 7, as the main menu sets it
    r -= r * 7 / 16;
    g -= g * 7 / 16;
    b -= b * 7 / 16;
    return r | (g << 5) | (b << 10);
}

static inline void Plot(int x, int y, u16 color)
{
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT)
        sSavesImage[y * DISPLAY_WIDTH + x] = sDarken ? DarkenColor(color) : color;
}

static void FillRect(int x, int y, int width, int height, u16 color)
{
    for (int j = 0; j < height; j++)
    {
        for (int i = 0; i < width; i++)
            Plot(x + i, y + j, color);
    }
}

// An 8x8 4bpp tile, where color 0 is clear
static void DrawTile(const u8 *tile, const u16 *palette, int x, int y)
{
    for (int row = 0; row < 8; row++)
    {
        for (int col = 0; col < 8; col++)
        {
            u8 pair = tile[row * 4 + col / 2];
            u8 index = (col & 1) ? pair >> 4 : pair & 0xF;

            if (index != 0)
                Plot(x + col, y + row, palette[index]);
        }
    }
}

// A white window with the player's chosen frame around it, which the main
// menu draws with tiles 0 to 8 of the frame (see DrawMainMenuWindowBorder)
static void DrawWindow(int left, int top, int width, int height)
{
    const struct TilesPal *frame = GetWindowFrameTilesPal(gSaveBlock2Ptr->optionsWindowFrameType);
    const u8 *tiles = frame->tiles;

    FillRect(left * 8, top * 8, width * 8, height * 8, TEXT_WHITE);
    DrawTile(tiles + 0 * 32, frame->pal, (left - 1) * 8, (top - 1) * 8);
    DrawTile(tiles + 2 * 32, frame->pal, (left + width) * 8, (top - 1) * 8);
    DrawTile(tiles + 6 * 32, frame->pal, (left - 1) * 8, (top + height) * 8);
    DrawTile(tiles + 8 * 32, frame->pal, (left + width) * 8, (top + height) * 8);
    for (int i = 0; i < width; i++)
    {
        DrawTile(tiles + 1 * 32, frame->pal, (left + i) * 8, (top - 1) * 8);
        DrawTile(tiles + 7 * 32, frame->pal, (left + i) * 8, (top + height) * 8);
    }
    for (int j = 0; j < height; j++)
    {
        DrawTile(tiles + 3 * 32, frame->pal, (left - 1) * 8, (top + j) * 8);
        DrawTile(tiles + 5 * 32, frame->pal, (left + width) * 8, (top + j) * 8);
    }
}

// One 8x8 tile of a glyph: a row is 16 bits of 2-bit pixels, the left four
// in the high byte, leftmost in its top bits. 1 is the text, 2 its shadow.
static void DrawGlyphTile(const u16 *rows, int x, int y, u16 fg, u16 shadow)
{
    for (int row = 0; row < 8; row++)
    {
        for (int col = 0; col < 8; col++)
        {
            u8 half = (col < 4) ? rows[row] >> 8 : rows[row] & 0xFF;
            u8 pixel = (half >> (6 - 2 * (col & 3))) & 3;

            if (pixel == 1)
                Plot(x + col, y + row, fg);
            else if (pixel == 2)
                Plot(x + col, y + row, shadow);
        }
    }
}

// In FONT_NORMAL, like DecompressGlyph_Normal draws it
static int GlyphWidth(u8 c)
{
    return gFontNormalLatinGlyphWidths[c];
}

static int StringWidth(const u8 *text)
{
    int width = 0;

    for (; *text != EOS; text++)
        width += GlyphWidth(*text);
    return width;
}

static void DrawString(const u8 *text, int x, int y, u16 fg, u16 shadow)
{
    for (; *text != EOS; text++)
    {
        const u16 *glyph = gFontNormalLatinGlyphs + 0x20 * *text;
        int width = GlyphWidth(*text);

        if (width <= 8)
        {
            DrawGlyphTile(glyph, x, y, fg, shadow);
            DrawGlyphTile(glyph + 0x10, x, y + 8, fg, shadow);
        }
        else
        {
            DrawGlyphTile(glyph, x, y, fg, shadow);
            DrawGlyphTile(glyph + 0x8, x + 8, y, fg, shadow);
            DrawGlyphTile(glyph + 0x10, x, y + 8, fg, shadow);
            DrawGlyphTile(glyph + 0x18, x + 8, y + 8, fg, shadow);
        }
        x += width;
    }
}

static void DrawStringRightAligned(const u8 *text, int right, int y, u16 fg, u16 shadow)
{
    DrawString(text, right - StringWidth(text), y, fg, shadow);
}

// Plain text in the game's characters
static void AsciiToGame(const char *text, u8 *out, size_t size)
{
    size_t n = 0;

    for (; *text != '\0' && n + 1 < size; text++, n++)
    {
        char c = *text;

        if (c >= 'A' && c <= 'Z')
            out[n] = CHAR_A + (c - 'A');
        else if (c >= 'a' && c <= 'z')
            out[n] = CHAR_a + (c - 'a');
        else if (c >= '0' && c <= '9')
            out[n] = CHAR_0 + (c - '0');
        else if (c == '.')
            out[n] = CHAR_PERIOD;
        else if (c == ',')
            out[n] = CHAR_COMMA;
        else if (c == '?')
            out[n] = CHAR_QUESTION_MARK;
        else if (c == '!')
            out[n] = CHAR_EXCL_MARK;
        else if (c == ':')
            out[n] = CHAR_COLON;
        else if (c == '-')
            out[n] = CHAR_HYPHEN;
        else if (c == '\'')
            out[n] = CHAR_SGL_QUOTE_RIGHT;
        else
            out[n] = CHAR_SPACE;
    }
    out[n] = EOS;
}

static void DrawAscii(const char *text, int x, int y, u16 fg, u16 shadow)
{
    u8 game[64];

    AsciiToGame(text, game, sizeof(game));
    DrawString(game, x, y, fg, shadow);
}

static void DrawFileWindow(int file)
{
    const struct SaveFileInfo *info = &sSlotInfo[file + 1];
    int x = FILE_WINDOW_LEFT * 8, y = FILE_WINDOW_TOP(file) * 8;
    u16 infoColor = info->gender == MALE ? TEXT_MALE : TEXT_FEMALE;
    u8 text[16];
    char header[16];

    // The selected one isn't darkened, like on the main menu
    sDarken = file != sSaves.file;
    DrawWindow(FILE_WINDOW_LEFT, FILE_WINDOW_TOP(file), FILE_WINDOW_WIDTH, FILE_WINDOW_HEIGHT);

    snprintf(header, sizeof(header), "FILE %d", file + 1);
    DrawAscii(header, x, y + 1, TEXT_DARK, TEXT_SHADOW);
    if (!info->hasGame)
    {
        DrawAscii("NEW GAME", x, y + 17, TEXT_DARK, TEXT_SHADOW);
    }
    else
    {
        u8 *end;

        // Like MainMenu_FormatSavegamePlayer, Time and Badges
        DrawStringRightAligned(info->name, x + 100, y + 1, infoColor, TEXT_SHADOW);
        DrawString(gText_ContinueMenuTime, x, y + 17, infoColor, TEXT_SHADOW);
        end = ConvertIntToDecimalStringN(text, info->hours, STR_CONV_MODE_LEFT_ALIGN, 3);
        *end++ = CHAR_COLON;
        ConvertIntToDecimalStringN(end, info->minutes, STR_CONV_MODE_LEADING_ZEROS, 2);
        DrawStringRightAligned(text, x + 100, y + 17, infoColor, TEXT_SHADOW);
        DrawString(gText_ContinueMenuBadges, x + 0x6C, y + 17, infoColor, TEXT_SHADOW);
        ConvertIntToDecimalStringN(text, info->badges, STR_CONV_MODE_LEADING_ZEROS, 1);
        DrawStringRightAligned(text, x + 0xD0, y + 17, infoColor, TEXT_SHADOW);
    }
    if (file + 1 == sSettings.slot)
        DrawAscii("PLAYING", x + 0x6C, y + 1, TEXT_DARK, TEXT_SHADOW);
    sDarken = false;
}

static void DrawSavesScreen(void)
{
    static const char *const sActions[ACTION_COUNT] = {"PLAY", "COPY", "DELETE", "CANCEL"};
    const char *a = sSettings.circleIsA ? "O" : "X";
    const char *b = sSettings.circleIsA ? "X" : "O";
    char help[64];

    FillRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, SAVES_BG_COLOR);
    for (int file = 0; file < NUM_SLOTS; file++)
        DrawFileWindow(file);

    switch (sSaves.mode)
    {
    case SAVES_LIST:
        snprintf(help, sizeof(help), "%s CHOOSE   %s BACK", a, b);
        DrawAscii(help, 16, 145, TEXT_WHITE, HELP_SHADOW);
        break;
    case SAVES_ACTIONS:
        DrawWindow(20, 11, 8, 8);
        for (int i = 0; i < ACTION_COUNT; i++)
        {
            const char *label = sActions[i];

            if (i == ACTION_PLAY && sSaves.file + 1 == sSettings.slot)
                label = "RELOAD";
            DrawAscii(label, 168, 88 + 1 + i * 16, TEXT_DARK, TEXT_SHADOW);
        }
        // The game's menu cursor
        {
            u8 cursor[2] = {CHAR_BLACK_TRIANGLE, EOS};

            DrawString(cursor, 160, 88 + 1 + sSaves.action * 16, TEXT_DARK, TEXT_SHADOW);
        }
        break;
    case SAVES_COPY_TO:
        // On the help line, so the files stay in sight
        snprintf(help, sizeof(help), "Copy to which file?   %s OK   %s BACK", a, b);
        DrawAscii(help, 16, 145, TEXT_WHITE, HELP_SHADOW);
        break;
    case SAVES_CONFIRM:
    case SAVES_MESSAGE:
        DrawWindow(2, 15, 26, 4);
        DrawAscii(sSaves.message[0], 16, 121, TEXT_DARK, TEXT_SHADOW);
        DrawAscii(sSaves.message[1], 16, 137, TEXT_DARK, TEXT_SHADOW);
        if (sSaves.mode == SAVES_CONFIRM)
        {
            u8 cursor[2] = {CHAR_BLACK_TRIANGLE, EOS};

            DrawWindow(23, 9, 5, 4);
            DrawString(cursor, 184, 72 + 1 + (sSaves.yes ? 0 : 16), TEXT_DARK, TEXT_SHADOW);
            DrawString(gText_Yes, 192, 72 + 1, TEXT_DARK, TEXT_SHADOW);
            DrawString(gText_No, 192, 72 + 17, TEXT_DARK, TEXT_SHADOW);
        }
        break;
    }
}

static void ShowSavesMessage(const char *line1, const char *line2)
{
    snprintf(sSaves.message[0], sizeof(sSaves.message[0]), "%s", line1);
    snprintf(sSaves.message[1], sizeof(sSaves.message[1]), "%s", line2);
    sSaves.mode = SAVES_MESSAGE;
}

static void AskSaves(int confirm, const char *line1, const char *line2)
{
    ShowSavesMessage(line1, line2);
    sSaves.mode = SAVES_CONFIRM;
    sSaves.confirm = confirm;
    // NO first for anything that loses something
    sSaves.yes = false;
}

static void OpenSaves(void)
{
    sSaves.open = true;
    sSaves.mode = SAVES_LIST;
    sSaves.file = sSettings.slot - 1;
    for (int slot = 1; slot <= NUM_SLOTS; slot++)
    {
        if (!sSlotInfo[slot].known)
            RefreshSlotInfo(slot);
    }
}

void Platform_OpenFileSelect(void)
{
    OpenSaves();
}

static void SavesConfirmed(void)
{
    char text[40];
    int slot = sSaves.file + 1;

    switch (sSaves.confirm)
    {
    case CONFIRM_SWITCH:
    case CONFIRM_RELOAD:
        // Done between frames once the game allows a reset (see SwitchSlotIfPending)
        sPendingSlot = slot;
        sSaves.open = false;
        return;
    case CONFIRM_DELETE:
        DeleteSlot(slot);
        RefreshSlotInfo(slot);
        snprintf(text, sizeof(text), "FILE %d was deleted.", slot);
        ShowSavesMessage(text, "");
        break;
    case CONFIRM_OVERWRITE:
        if (CopySlot(sSaves.source + 1, slot))
            snprintf(text, sizeof(text), "FILE %d was copied to FILE %d.", sSaves.source + 1, slot);
        else
            snprintf(text, sizeof(text), "FILE %d couldn't be copied.", sSaves.source + 1);
        RefreshSlotInfo(slot);
        ShowSavesMessage(text, "");
        break;
    }
}

static void ChooseSavesAction(void)
{
    char text[40];
    int slot = sSaves.file + 1;

    switch (sSaves.action)
    {
    case ACTION_PLAY:
        if (slot == sSettings.slot)
        {
            AskSaves(CONFIRM_RELOAD, "Go back to this file's last save?", "Anything not saved will be lost.");
        }
        else
        {
            snprintf(text, sizeof(text), "Play FILE %d?", slot);
            AskSaves(CONFIRM_SWITCH, text, "Anything not saved will be lost.");
        }
        break;
    case ACTION_COPY:
        if (!sSlotInfo[slot].hasGame)
        {
            ShowSavesMessage("There's no game in this file", "to copy.");
            break;
        }
        sSaves.source = sSaves.file;
        sSaves.file = (sSaves.file + 1) % NUM_SLOTS;
        if (sSaves.file + 1 == sSettings.slot)
            sSaves.file = (sSaves.file + 1) % NUM_SLOTS;
        sSaves.mode = SAVES_COPY_TO;
        break;
    case ACTION_DELETE:
        if (slot == sSettings.slot)
        {
            ShowSavesMessage("This file is being played.", "Play another file to delete it.");
        }
        else if (!sSlotInfo[slot].exists)
        {
            ShowSavesMessage("This file is already empty.", "");
        }
        else
        {
            snprintf(text, sizeof(text), "Delete FILE %d?", slot);
            AskSaves(CONFIRM_DELETE, text, "It can't be brought back.");
        }
        break;
    default:
        sSaves.mode = SAVES_LIST;
        break;
    }
}

static void UpdateSaves(u32 pressed)
{
    u32 aButton = sSettings.circleIsA ? PSP_BTN_CIRCLE : PSP_BTN_CROSS;
    u32 bButton = sSettings.circleIsA ? PSP_BTN_CROSS : PSP_BTN_CIRCLE;
    char text[40];

    switch (sSaves.mode)
    {
    case SAVES_LIST:
        if (pressed & (bButton | PSP_BTN_TRIANGLE))
        {
            sSaves.open = false;
            return;
        }
        if (pressed & PSP_BTN_UP)
            sSaves.file = (sSaves.file + NUM_SLOTS - 1) % NUM_SLOTS;
        if (pressed & PSP_BTN_DOWN)
            sSaves.file = (sSaves.file + 1) % NUM_SLOTS;
        if (pressed & aButton)
        {
            sSaves.action = ACTION_PLAY;
            sSaves.mode = SAVES_ACTIONS;
        }
        break;
    case SAVES_ACTIONS:
        if (pressed & PSP_BTN_UP)
            sSaves.action = (sSaves.action + ACTION_COUNT - 1) % ACTION_COUNT;
        if (pressed & PSP_BTN_DOWN)
            sSaves.action = (sSaves.action + 1) % ACTION_COUNT;
        if (pressed & (bButton | PSP_BTN_TRIANGLE))
            sSaves.mode = SAVES_LIST;
        else if (pressed & aButton)
            ChooseSavesAction();
        break;
    case SAVES_COPY_TO:
        // The source and the file being played can't be picked
        for (int dir = (pressed & PSP_BTN_UP) ? -1 : (pressed & PSP_BTN_DOWN) ? 1 : 0, tries = 0; dir != 0 && tries < NUM_SLOTS; tries++)
        {
            sSaves.file = (sSaves.file + dir + NUM_SLOTS) % NUM_SLOTS;
            if (sSaves.file != sSaves.source && sSaves.file + 1 != sSettings.slot)
                break;
        }
        if (pressed & (bButton | PSP_BTN_TRIANGLE))
        {
            sSaves.file = sSaves.source;
            sSaves.mode = SAVES_LIST;
        }
        else if (pressed & aButton)
        {
            if (sSaves.file == sSaves.source || sSaves.file + 1 == sSettings.slot)
            {
                ShowSavesMessage("Pick a file that isn't being", "played to copy it to.");
            }
            else
            {
                snprintf(text, sizeof(text), "Copy FILE %d over FILE %d?", sSaves.source + 1, sSaves.file + 1);
                AskSaves(CONFIRM_OVERWRITE, text, sSlotInfo[sSaves.file + 1].exists ? "What's in it now will be lost." : "");
            }
        }
        break;
    case SAVES_CONFIRM:
        if (pressed & (PSP_BTN_UP | PSP_BTN_DOWN))
            sSaves.yes = !sSaves.yes;
        if (pressed & (bButton | PSP_BTN_TRIANGLE))
            sSaves.mode = SAVES_LIST;
        else if (pressed & aButton)
        {
            if (sSaves.yes)
                SavesConfirmed();
            else
                sSaves.mode = SAVES_LIST;
        }
        break;
    case SAVES_MESSAGE:
        if (pressed & (aButton | bButton | PSP_BTN_TRIANGLE))
            sSaves.mode = SAVES_LIST;
        break;
    }
}

// Switches the save file between frames, when the game isn't saving, and
// starts the game over so it reads the new one
static void SwitchSlotIfPending(void)
{
    char text[80];

    if (sPendingSlot == 0 || gSoftResetDisabled)
        return;
    sSettings.slot = sPendingSlot;
    sPendingSlot = 0;
    ReadSaveFile(sSettings.slot);
    StoreSettings();
    sSlotInfo[sSettings.slot].known = false;
    RequestSoftReset();
    snprintf(text, sizeof(text), "Playing file %d.", sSettings.slot);
    ShowStatus(text);
}

#ifdef PSP_TEST
// The emulator tests: PSP buttons from host0:/test_input.txt, a line of
// "<frames> <buttons>" each, like "30 CROSS+UP" or "60 -". After a line with
// SHOT in it, like "60 SHOT", the screen is written to host0:/shot_NNN.rgba.
// The game exits at the end.
static u32 TestButtons(bool *done, bool *shot)
{
    static const struct { const char *name; u32 button; } sNames[] = {
        {"CROSS", PSP_BTN_CROSS}, {"CIRCLE", PSP_BTN_CIRCLE}, {"SQUARE", PSP_BTN_SQUARE},
        {"TRIANGLE", PSP_BTN_TRIANGLE}, {"START", PSP_BTN_START}, {"SELECT", PSP_BTN_SELECT},
        {"UP", PSP_BTN_UP}, {"DOWN", PSP_BTN_DOWN}, {"LEFT", PSP_BTN_LEFT}, {"RIGHT", PSP_BTN_RIGHT},
        {"L", PSP_BTN_L}, {"R", PSP_BTN_R},
    };
    static char *sInput;
    static char *sNext;
    static u32 sHoldFrames, sHoldButtons;
    static bool sLineShot;

    *done = false;
    *shot = false;
    if (sInput == NULL)
    {
        static char text[16384];
        long size = PspHw_ReadFile("host0:/test_input.txt", text, sizeof(text) - 1);

        text[size > 0 ? size : 0] = '\0';
        sInput = sNext = text;
    }
    if (sHoldFrames == 0)
    {
        char buttons[128];
        char *line;

        *shot = sLineShot;
        sLineShot = false;
        for (;;)
        {
            if (*sNext == '\0')
            {
                *done = true;
                return 0;
            }
            line = sNext;
            sNext = strchr(sNext, '\n');
            if (sNext == NULL)
                sNext = line + strlen(line);
            else
                *sNext++ = '\0';
            if (line[0] != '#' && sscanf(line, "%u %127s", &sHoldFrames, buttons) == 2 && sHoldFrames != 0)
                break;
        }
        sHoldButtons = 0;
        for (char *name = strtok(buttons, "+"); name != NULL; name = strtok(NULL, "+"))
        {
            if (strcmp(name, "SHOT") == 0)
                sLineShot = true;
            for (size_t i = 0; i < ARRAY_COUNT(sNames); i++)
            {
                if (strcmp(name, sNames[i].name) == 0)
                    sHoldButtons |= sNames[i].button;
            }
        }
    }
    sHoldFrames--;
    return sHoldButtons;
}
#endif

// What the watchdog shows if the game stops responding
static void SetGameDiag(u32 frame)
{
    int n = 0;

    PspHw_SetDiag(PSP_DIAG_FRAME, frame);
    PspHw_SetDiag(PSP_DIAG_CALL, 0);
    PspHw_SetDiag(PSP_DIAG_CALLBACK1, (uintptr_t)gMain.callback1);
    PspHw_SetDiag(PSP_DIAG_CALLBACK2, (uintptr_t)gMain.callback2);
    PspHw_SetDiag(PSP_DIAG_VBLANK, (uintptr_t)gMain.vblankCallback);
    for (int i = 0; i < NUM_TASKS && n < 16; i++)
    {
        if (gTasks[i].isActive)
            PspHw_SetDiag(PSP_DIAG_TASKS + n++, (uintptr_t)gTasks[i].func);
    }
    while (n < 16)
        PspHw_SetDiag(PSP_DIAG_TASKS + n++, 0);
    // Last, so the trace's record has all of the above
    PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_GAME);
}

void Platform_TraceCall(const void *func)
{
    PspHw_SetDiag(PSP_DIAG_CALL, (uintptr_t)func);
    if (PspHw_Tracing())
        PspHw_WriteTrace();
}

static void FindBaseDir(const char *exePath)
{
    const char *slash = exePath != NULL ? strrchr(exePath, '/') : NULL;

    if (slash != NULL && (size_t)(slash - exePath + 1) < sizeof(sBaseDir))
    {
        memcpy(sBaseDir, exePath, slash - exePath + 1);
        sBaseDir[slash - exePath + 1] = '\0';
    }
    else
    {
        strcpy(sBaseDir, "ms0:/PSP/GAME/EMERALD_PSP/");
    }
}

int main(int argc, char **argv)
{
    static u16 __attribute__((aligned(16))) image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    char path[300];
    u32 held = 0;
    u64 clockStart, framesRun = 0;
    u64 speedStart;
    u32 speedFrames = 0, speedDrawn = 0;
    // Where the time goes, in microseconds a second
    u64 timeGame = 0, timeDraw = 0, timeSound = 0, timeStart;
    int speed, lastSpeed = 1;
    bool menuWasOpen;
    u32 soundFrame = 0;
    char speedText[80] = "";

    PspHw_Init();
    FindBaseDir(argc > 0 ? argv[0] : NULL);
    snprintf(path, sizeof(path), "%skbuttons.prx", sBaseDir);
    PspHw_StartKernelButtons(path);
    snprintf(path, sizeof(path), "%sexit.log", sBaseDir);
    PspHw_StartExitLog(path);
    snprintf(path, sizeof(path), "%ssaves", sBaseDir);
    PspHw_MakeDir(path);
    LoadSettings();
    if (sSettings.trace)
    {
        snprintf(path, sizeof(path), "%strace.bin", sBaseDir);
        PspHw_StartTrace(path);
    }
    AdoptOldSave();
    ReadSaveFile(sSettings.slot);
    sClock.status = SIIRTCINFO_24HOUR;
    UpdateClock();

    cgb_audio_init(AUDIO_SAMPLE_RATE);
    PspHw_AudioInit(AUDIO_SAMPLE_RATE);
    PspHw_StartWatchdog();
    AgbMain();

    clockStart = speedStart = PspHw_Microseconds();
    while (PspHw_Running())
    {
#ifdef PSP_TEST
        static int sShots;
        bool testDone, testShot;
        u32 buttons = TestButtons(&testDone, &testShot);

        if (testShot)
        {
            snprintf(path, sizeof(path), "host0:/shot_%03d.rgba", sShots++);
            PspHw_DumpScreen(path);
        }
        if (testDone)
            break;
#else
        u32 buttons = PspHw_ReadButtons();
#endif
        u32 pressed = buttons & ~held;
        u64 now;
        int framesDue;

        held = buttons;
        PspHw_Heartbeat();

        // The menus go over the game, which keeps running without the buttons.
        // The note button opens the settings. Where it can't be read, like in
        // PPSSPP, Select and Triangle do.
        menuWasOpen = sSaves.open || sSettingsMenu.open;
        if (sSaves.open)
            UpdateSaves(pressed);
        else if (sSettingsMenu.open)
            UpdateSettings(pressed);
        else if ((pressed & PSP_BTN_NOTE) || ((pressed & PSP_BTN_TRIANGLE) && (buttons & PSP_BTN_SELECT)))
            OpenSettings();
        if (menuWasOpen && !sSaves.open && !sSettingsMenu.open)
            sMaskedButtons = buttons;
        sMaskedButtons &= buttons;
        menuWasOpen |= sSaves.open || sSettingsMenu.open;

        // How many frames the game should have run by now. It runs one a
        // display frame, or with Square held two or three (the turbo speed),
        // and catches up when it falls behind by running frames without
        // drawing them.
        now = PspHw_Microseconds();
        speed = ((buttons & PSP_BTN_SQUARE) && !menuWasOpen) ? sSettings.turboSpeed : 1;
        if (speed != lastSpeed)
        {
            clockStart = now;
            framesRun = 0;
            lastSpeed = speed;
        }
        framesDue = (int)((now - clockStart) * speed / FRAME_MICROSECONDS - framesRun) + 1;
        if (framesDue < 1 || sSettings.frameskip == FRAMESKIP_OFF)
            framesDue = speed;
        if (framesDue > 4 * speed)
        {
            // Too far behind to catch up, like after loading: carry on from here
            framesDue = 4 * speed;
            clockStart = now;
            framesRun = 0;
        }
#ifdef PSP_TEST_FORCE_SKIP
        framesDue = 2;
#endif
        sKeys = menuWasOpen ? 0 : MapButtons(buttons & ~sMaskedButtons);

        for (int i = 0; i < framesDue; i++)
        {
            SwitchSlotIfPending();
            ENTER_VBLANK();
            framesRun++;
            speedFrames++;
            timeStart = PspHw_Microseconds();
            SetGameDiag(++sTotalFrames);
            if (!RunMainLoop())
            {
                // Soft reset: drop the old sound, and start the next frame with MainLoop
                PspHw_AudioClear();
                continue;
            }
            now = PspHw_Microseconds();
            timeGame += now - timeStart;
            timeStart = now;
            PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_DRAW);
            if (i == framesDue - 1)
            {
                memset(image, 0, sizeof(image));
                DrawFrame(image);
                speedDrawn++;
            }
            else
            {
                SkipFrame();
            }
            REG_VCOUNT = 161;
            PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_VBLANK);
            RunDMAsAndVBlank();
            now = PspHw_Microseconds();
            timeDraw += now - timeStart;
            timeStart = now;
            // In turbo the sound engine runs once for every turbo-speed
            // frames: the music keeps its tempo, and costs no more than usual
            if (soundFrame++ % speed == 0)
            {
                PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_SOUND);
                AudioUpdate();
                timeSound += PspHw_Microseconds() - timeStart;
            }
        }

        PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_PRESENT);
        if (sSaves.open)
        {
            DrawSavesScreen();
            PspHw_DrawGame(sSavesImage, sSettings.scale, sSettings.filter, false);
        }
        else
        {
            PspHw_DrawGame(image, sSettings.scale, sSettings.filter, sSettingsMenu.open);
            if (sSettingsMenu.open)
                DrawSettings();
        }
        if (sSettings.showSpeed)
        {
            now = PspHw_Microseconds();
            if (now - speedStart >= 1000000)
            {
                // Milliseconds per frame on average
                snprintf(speedText, sizeof(speedText), "%u fps, %u drawn  game %.1f draw %.1f sound %.1f",
                         (unsigned)speedFrames, (unsigned)speedDrawn,
                         speedFrames ? timeGame / 1000.0f / speedFrames : 0.0f,
                         speedDrawn ? timeDraw / 1000.0f / speedDrawn : 0.0f,
                         speedFrames ? timeSound / 1000.0f / speedFrames : 0.0f);
                speedFrames = speedDrawn = 0;
                timeGame = timeDraw = timeSound = 0;
                speedStart = now;
            }
            PspHw_Print(0, 0, COLOR_WHITE, speedText);
        }
        if (sStatusFrames != 0)
        {
            PspHw_Print(1, 33, COLOR_YELLOW, sStatus);
            sStatusFrames--;
        }
        PspHw_Present();
    }

    PspHw_Exit();
    return 0;
}
#endif // PORTABLE && __PSP__
