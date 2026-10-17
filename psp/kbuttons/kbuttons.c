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
};

static struct KernelButtons *sShared;

static int ButtonThread(SceSize args, void *argp)
{
    // The game clears run before it exits
    while (sShared->run)
    {
        SceCtrlData pad;

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
    thread = sceKernelCreateThread("EmeraldButtons", ButtonThread, 0x18, 0x800, 0, NULL);
    if (thread < 0)
        return thread;
    sceKernelStartThread(thread, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    return 0;
}
