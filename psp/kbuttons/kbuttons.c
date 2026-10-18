// A kernel module that reads the buttons only the kernel can see, like the
// music note button, and writes them where the game asked (see psp_hw.c).
// Games can't read those buttons themselves.
#include <pspkernel.h>
#include <pspctrl.h>

PSP_MODULE_INFO("EmeraldButtons", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

// Shared with psp_hw.c
struct KernelButtons
{
    volatile unsigned int run;
    volatile unsigned int buttons;
    // The game's main thread, set before the module starts
    SceUID gameThread;
};

static struct KernelButtons *sShared;
// Kept in the module's own (kernel) memory: once the game has exited, the
// game's memory belongs to the XMB, and reading run from it there could keep
// this thread writing into the XMB
static volatile int sStop;
static SceUID sGameThread;
static SceUID sThread = -1;

static int ButtonThread(SceSize args, void *argp)
{
    // The game clears run before it exits, and the system stops the module.
    // In case neither happens in time, the thread also ends as soon as the
    // game's thread is gone, before touching the game's memory again.
    for (;;)
    {
        SceCtrlData pad;
        SceKernelThreadInfo info;

        if (sStop)
            break;
        info.size = sizeof(info);
        if (sceKernelReferThreadStatus(sGameThread, &info) < 0)
            break;
        if (!sShared->run)
            break;

        sceCtrlPeekBufferPositive(&pad, 1);
        sShared->buttons = pad.Buttons;
        sceKernelDelayThread(10 * 1000);
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

// The game passes the address of its struct KernelButtons as the argument
int module_start(SceSize args, void *argp)
{
    int thread;

    if (args != sizeof(struct KernelButtons *))
        return -1;
    sShared = *(struct KernelButtons **)argp;
    sGameThread = sShared->gameThread;
    thread = sceKernelCreateThread("EmeraldButtons", ButtonThread, 0x18, 0x800, 0, NULL);
    if (thread < 0)
        return thread;
    sThread = thread;
    sceKernelStartThread(thread, 0, NULL);
    return 0;
}

// Called when the game unloads the module, and by the system when the game
// exits without doing so: the thread is gone before the game's memory is
int module_stop(SceSize args, void *argp)
{
    SceUInt timeout = 200 * 1000;

    sStop = 1;
    if (sThread >= 0)
        sceKernelWaitThreadEnd(sThread, &timeout);
    return 0;
}
