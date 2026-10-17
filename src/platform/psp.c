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
#include "gba/flash_internal.h"
#include "platform/framedraw.h"
#include "platform/psp_hw.h"
#include "platform/system.h"
#include "cgb_audio.h"

extern void AgbMain(void);

#define NUM_SLOTS 10
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
} sSettings = {
    .slot = 1,
    .scale = PSP_SCALE_FIT,
    .filter = false,
    .circleIsA = false,
    .frameskip = FRAMESKIP_AUTO,
    .showSpeed = false,
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
    char name[PLAYER_NAME_LENGTH + 1];
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
    }
}

static void StoreSettings(void)
{
    char path[300];
    char text[768];
    int length;

    SettingsPath(path, sizeof(path));
    length = snprintf(text, sizeof(text),
                      "# Pokemon Emerald PSP settings. The menu (Triangle) changes them.\n"
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
                      "# 1 writes what the game is doing to trace.bin all the time, to find crashes (slow)\n"
                      "trace = %d\n",
                      NUM_SLOTS, sSettings.slot, sSettings.scale, sSettings.filter, sSettings.circleIsA,
                      sSettings.frameskip, sSettings.showSpeed, sSettings.trace);
    PspHw_WriteFile(path, text, length);
}

// What the menu shows for a save: the newest of the two copies the game keeps
// (see save.c), read straight from the flash's sectors
static void ReadSaveInfo(const u8 *flash, struct SaveFileInfo *info)
{
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

    for (int i = 0; i < NUM_SECTORS_PER_SLOT; i++)
    {
        const struct SaveSector *sector = (const void *)(flash + (bestHalf * NUM_SECTORS_PER_SLOT + i) * SECTOR_SIZE);

        if (sector->signature != SECTOR_SIGNATURE)
            continue;
        if (sector->id == SECTOR_ID_SAVEBLOCK2)
        {
            // SaveBlock2 starts with the name, and has the play time at 0x0E
            const u8 *name = sector->data;
            int length;

            for (length = 0; length < PLAYER_NAME_LENGTH; length++)
            {
                u8 c = name[length];

                if (c == EOS)
                    break;
                if (c >= CHAR_A && c <= CHAR_Z)
                    info->name[length] = 'A' + (c - CHAR_A);
                else if (c >= CHAR_a && c <= CHAR_z)
                    info->name[length] = 'a' + (c - CHAR_a);
                else if (c >= CHAR_0 && c <= CHAR_0 + 9)
                    info->name[length] = '0' + (c - CHAR_0);
                else
                    info->name[length] = ' ';
            }
            info->name[length] = '\0';
            info->hours = sector->data[0x0E] | (sector->data[0x0F] << 8);
            info->minutes = sector->data[0x10];
            info->hasGame = true;
        }
        else if (sector->id >= SECTOR_ID_SAVEBLOCK1_START && sector->id <= SECTOR_ID_SAVEBLOCK1_END)
        {
            // The badges are flags, and SaveBlock1's flags start at 0x1270 on the GBA
            u32 start = (sector->id - SECTOR_ID_SAVEBLOCK1_START) * SECTOR_DATA_SIZE;

            info->badges = 0;
            for (int badge = 0; badge < NUM_BADGES; badge++)
            {
                u32 flag = FLAG_BADGE01_GET + badge;
                u32 offset = 0x1270 + flag / 8;

                if (offset >= start && offset < start + SECTOR_DATA_SIZE && (sector->data[offset - start] >> (flag % 8)) & 1)
                    info->badges++;
            }
        }
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

// The menu, on Triangle. Its rows are the save files, then the settings.
enum
{
    ROW_SETTING_SCALE = NUM_SLOTS,
    ROW_SETTING_FILTER,
    ROW_SETTING_BUTTONS,
    ROW_SETTING_FRAMESKIP,
    ROW_SETTING_SPEED,
    ROW_BACK,
    ROW_COUNT,
};

enum
{
    ACTION_NONE,
    ACTION_CHOOSE,  // what to do with a save file
    ACTION_COPY_TO, // picking where to copy it
    ACTION_CONFIRM, // yes or no
};

enum
{
    CONFIRM_SWITCH,
    CONFIRM_RELOAD,
    CONFIRM_DELETE,
    CONFIRM_OVERWRITE,
};

static const char *const sChoices[] = {"Play this file", "Copy it to another file", "Delete it", "Cancel"};

static struct
{
    bool open;
    int row;
    int action;
    int choice;
    int slot;       // the file the action is for
    int target;     // where it's copied to
    int confirm;
    bool settingsChanged;
} sMenu;

#define COLOR_WHITE  0xFFFFFFFF
#define COLOR_GRAY   0xFFA0A0A0
#define COLOR_YELLOW 0xFF40E0FF
#define COLOR_GREEN  0xFF60FF60
#define COLOR_RED    0xFF6060FF

static void OpenMenu(void)
{
    sMenu.open = true;
    sMenu.row = sSettings.slot - 1;
    sMenu.action = ACTION_NONE;
    sMenu.settingsChanged = false;
    for (int slot = 1; slot <= NUM_SLOTS; slot++)
    {
        if (!sSlotInfo[slot].known)
            RefreshSlotInfo(slot);
    }
}

static void CloseMenu(void)
{
    sMenu.open = false;
    if (sMenu.settingsChanged)
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
    case ROW_SETTING_SPEED:
        sSettings.showSpeed = !sSettings.showSpeed;
        break;
    default:
        return;
    }
    sMenu.settingsChanged = true;
}

static void Confirmed(void)
{
    char text[80];

    switch (sMenu.confirm)
    {
    case CONFIRM_SWITCH:
    case CONFIRM_RELOAD:
        // Done between frames once the game allows a reset (see RunFrame)
        sPendingSlot = sMenu.slot;
        CloseMenu();
        return;
    case CONFIRM_DELETE:
        if (DeleteSlot(sMenu.slot))
            snprintf(text, sizeof(text), "File %d deleted.", sMenu.slot);
        else
            snprintf(text, sizeof(text), "File %d could not be deleted.", sMenu.slot);
        ShowStatus(text);
        break;
    case CONFIRM_OVERWRITE:
        if (CopySlot(sMenu.slot, sMenu.target))
            snprintf(text, sizeof(text), "File %d copied to file %d.", sMenu.slot, sMenu.target);
        else
            snprintf(text, sizeof(text), "File %d could not be copied.", sMenu.slot);
        ShowStatus(text);
        break;
    }
    RefreshSlotInfo(sMenu.slot);
    if (sMenu.confirm == CONFIRM_OVERWRITE)
        RefreshSlotInfo(sMenu.target);
    sMenu.action = ACTION_NONE;
}

static void ChooseAction(void)
{
    switch (sMenu.choice)
    {
    case 0:
        sMenu.confirm = (sMenu.slot == sSettings.slot) ? CONFIRM_RELOAD : CONFIRM_SWITCH;
        sMenu.action = ACTION_CONFIRM;
        break;
    case 1:
        sMenu.target = sMenu.slot % NUM_SLOTS + 1;
        sMenu.action = ACTION_COPY_TO;
        break;
    case 2:
        if (sMenu.slot == sSettings.slot)
        {
            ShowStatus("This file is being played. Switch to another one first.");
            sMenu.action = ACTION_NONE;
        }
        else if (!sSlotInfo[sMenu.slot].exists)
        {
            ShowStatus("This file is already empty.");
            sMenu.action = ACTION_NONE;
        }
        else
        {
            sMenu.confirm = CONFIRM_DELETE;
            sMenu.action = ACTION_CONFIRM;
        }
        break;
    default:
        sMenu.action = ACTION_NONE;
        break;
    }
}

static void UpdateMenu(u32 pressed)
{
    switch (sMenu.action)
    {
    case ACTION_NONE:
        if (pressed & PSP_BTN_TRIANGLE)
        {
            CloseMenu();
            return;
        }
        if (pressed & PSP_BTN_UP)
            sMenu.row = (sMenu.row + ROW_COUNT - 1) % ROW_COUNT;
        if (pressed & PSP_BTN_DOWN)
            sMenu.row = (sMenu.row + 1) % ROW_COUNT;
        if (pressed & PSP_BTN_LEFT)
            ChangeSetting(sMenu.row, -1);
        if (pressed & PSP_BTN_RIGHT)
            ChangeSetting(sMenu.row, 1);
        if (pressed & PSP_BTN_CIRCLE)
            CloseMenu();
        else if (pressed & PSP_BTN_CROSS)
        {
            if (sMenu.row < NUM_SLOTS)
            {
                sMenu.slot = sMenu.row + 1;
                sMenu.choice = 0;
                sMenu.action = ACTION_CHOOSE;
            }
            else if (sMenu.row == ROW_BACK)
                CloseMenu();
            else
                ChangeSetting(sMenu.row, 1);
        }
        break;
    case ACTION_CHOOSE:
        if (pressed & PSP_BTN_UP)
            sMenu.choice = (sMenu.choice + ARRAY_COUNT(sChoices) - 1) % ARRAY_COUNT(sChoices);
        if (pressed & PSP_BTN_DOWN)
            sMenu.choice = (sMenu.choice + 1) % ARRAY_COUNT(sChoices);
        if (pressed & (PSP_BTN_CIRCLE | PSP_BTN_TRIANGLE))
            sMenu.action = ACTION_NONE;
        else if (pressed & PSP_BTN_CROSS)
            ChooseAction();
        break;
    case ACTION_COPY_TO:
        if (pressed & PSP_BTN_UP)
            sMenu.target = (sMenu.target + NUM_SLOTS - 2) % NUM_SLOTS + 1;
        if (pressed & PSP_BTN_DOWN)
            sMenu.target = sMenu.target % NUM_SLOTS + 1;
        if (pressed & (PSP_BTN_CIRCLE | PSP_BTN_TRIANGLE))
            sMenu.action = ACTION_NONE;
        else if (pressed & PSP_BTN_CROSS)
        {
            if (sMenu.target == sMenu.slot)
                ShowStatus("Pick a different file to copy it to.");
            else if (sMenu.target == sSettings.slot)
                ShowStatus("That file is being played. Switch to another one first.");
            else
            {
                sMenu.confirm = CONFIRM_OVERWRITE;
                sMenu.action = ACTION_CONFIRM;
            }
        }
        break;
    case ACTION_CONFIRM:
        if (pressed & (PSP_BTN_CIRCLE | PSP_BTN_TRIANGLE))
            sMenu.action = ACTION_NONE;
        else if (pressed & PSP_BTN_CROSS)
            Confirmed();
        break;
    }
}

static void DescribeSlot(int slot, char *text, size_t size)
{
    const struct SaveFileInfo *info = &sSlotInfo[slot];

    if (!info->exists)
        snprintf(text, size, "File %-2d  (empty)", slot);
    else if (!info->hasGame)
        snprintf(text, size, "File %-2d  (no game saved yet)", slot);
    else
        snprintf(text, size, "File %-2d  %-8s %4u:%02u   %u badge%s", slot, info->name,
                 info->hours, info->minutes, info->badges, info->badges == 1 ? "" : "s");
}

static void DrawMenu(void)
{
    static const char *const sScaleNames[] = {"Fit", "Stretch", "2x (cropped)", "Original size"};
    static const char *const sFrameskipNames[] = {"Auto", "Off"};
    char text[PSP_TEXT_COLUMNS + 1];
    int row = 1;

    PspHw_Print(2, row, COLOR_YELLOW, "POKEMON EMERALD PSP");
    PspHw_Print(40, row, COLOR_GRAY, "Triangle: back");
    row += 2;
    PspHw_Print(2, row++, COLOR_YELLOW, "SAVE FILES");
    for (int slot = 1; slot <= NUM_SLOTS; slot++, row++)
    {
        bool selected = sMenu.row == slot - 1;
        u32 color = selected ? COLOR_WHITE : COLOR_GRAY;

        DescribeSlot(slot, text, sizeof(text));
        PspHw_Print(2, row, COLOR_WHITE, selected ? ">" : " ");
        PspHw_Print(4, row, color, text);
        if (slot == sSettings.slot)
            PspHw_Print(50, row, COLOR_GREEN, "PLAYING");
        if (sMenu.action == ACTION_COPY_TO && slot == sMenu.target)
            PspHw_Print(50, row, COLOR_YELLOW, "<- HERE");
    }
    row++;
    PspHw_Print(2, row++, COLOR_YELLOW, "SETTINGS");
    for (int i = ROW_SETTING_SCALE; i < ROW_COUNT; i++, row++)
    {
        bool selected = sMenu.row == i;
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
            PspHw_Print(24, row, color, text);
        }
    }

    row = 28;
    switch (sMenu.action)
    {
    case ACTION_NONE:
        if (sMenu.row < NUM_SLOTS)
            PspHw_Print(2, row, COLOR_GRAY, "Cross: choose   Circle: back to the game");
        else if (sMenu.row < ROW_BACK)
            PspHw_Print(2, row, COLOR_GRAY, "Left and Right: change   Circle: back to the game");
        break;
    case ACTION_CHOOSE:
        snprintf(text, sizeof(text), "File %d:", sMenu.slot);
        PspHw_Print(2, row - 1, COLOR_YELLOW, text);
        for (int i = 0; i < (int)ARRAY_COUNT(sChoices); i++)
        {
            const char *label = sChoices[i];

            if (i == 0 && sMenu.slot == sSettings.slot)
                label = "Go back to its last save";
            PspHw_Print(4, row + i, i == sMenu.choice ? COLOR_WHITE : COLOR_GRAY, i == sMenu.choice ? ">" : " ");
            PspHw_Print(6, row + i, i == sMenu.choice ? COLOR_WHITE : COLOR_GRAY, label);
        }
        break;
    case ACTION_COPY_TO:
        snprintf(text, sizeof(text), "Copy file %d to which file? Up and Down pick, Cross copies.", sMenu.slot);
        PspHw_Print(2, row, COLOR_YELLOW, text);
        break;
    case ACTION_CONFIRM:
        switch (sMenu.confirm)
        {
        case CONFIRM_SWITCH:
            snprintf(text, sizeof(text), "Play file %d?", sMenu.slot);
            PspHw_Print(2, row, COLOR_YELLOW, text);
            PspHw_Print(2, row + 1, COLOR_WHITE, "What you've done since you last saved will be lost.");
            break;
        case CONFIRM_RELOAD:
            PspHw_Print(2, row, COLOR_YELLOW, "Go back to this file's last save?");
            PspHw_Print(2, row + 1, COLOR_WHITE, "What you've done since you last saved will be lost.");
            break;
        case CONFIRM_DELETE:
            snprintf(text, sizeof(text), "Delete file %d? This can't be undone.", sMenu.slot);
            PspHw_Print(2, row, COLOR_RED, text);
            break;
        case CONFIRM_OVERWRITE:
            snprintf(text, sizeof(text), "Copy file %d over file %d?", sMenu.slot, sMenu.target);
            PspHw_Print(2, row, COLOR_YELLOW, text);
            if (sSlotInfo[sMenu.target].exists)
                PspHw_Print(2, row + 1, COLOR_RED, "What's in that file now will be lost.");
            break;
        }
        PspHw_Print(2, row + 3, COLOR_GRAY, "Cross: yes   Circle: no");
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
    char speedText[80] = "";

    PspHw_Init();
    FindBaseDir(argc > 0 ? argv[0] : NULL);
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

        if (sMenu.open)
        {
            PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_MENU);
            UpdateMenu(pressed);
            PspHw_DrawGame(image, sSettings.scale, sSettings.filter, sMenu.open);
            if (sMenu.open)
                DrawMenu();
            if (sStatusFrames != 0)
            {
                PspHw_Print(2, 32, COLOR_YELLOW, sStatus);
                sStatusFrames--;
            }
            PspHw_Present();
            // The game was stopped, so its clock starts again from here
            clockStart = PspHw_Microseconds();
            framesRun = 0;
            continue;
        }
        if (pressed & PSP_BTN_TRIANGLE)
        {
            OpenMenu();
            continue;
        }

        // How many frames the game should have run by now. It runs one a
        // display frame, and catches up when it falls behind by running frames
        // without drawing them. Holding Square runs it four times as fast.
        now = PspHw_Microseconds();
        framesDue = (int)((now - clockStart) / FRAME_MICROSECONDS - framesRun) + 1;
        if (framesDue < 1 || sSettings.frameskip == FRAMESKIP_OFF)
            framesDue = 1;
        if (framesDue > 4)
        {
            // Too far behind to catch up, like after loading: carry on from here
            framesDue = 4;
            clockStart = now;
            framesRun = 0;
        }
        if (buttons & PSP_BTN_SQUARE)
            framesDue = 4;
#ifdef PSP_TEST_FORCE_SKIP
        framesDue = 2;
#endif
        sKeys = MapButtons(buttons);

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
            PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_SOUND);
            AudioUpdate();
            timeSound += PspHw_Microseconds() - timeStart;
        }

        PspHw_SetDiag(PSP_DIAG_STEP, PSP_STEP_PRESENT);
        PspHw_DrawGame(image, sSettings.scale, sSettings.filter, false);
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
