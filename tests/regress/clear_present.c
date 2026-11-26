/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include <deko3d.h>
#include <stdio.h>
#include <switch.h>

enum { Width = 640, Height = 360, ImageCount = 2 };

static void check(Result rc, const char *operation)
{
    if (R_SUCCEEDED(rc))
        return;
    char message[96];
    int length = snprintf(message, sizeof message, "clear_present: %s failed: %#x\n", operation, rc);
    svcOutputDebugString(message, length);
    fatalThrow(rc);
}

int main(void)
{
    static const char ready[] = "clear_present: guest VSync and GPU present completed\n";
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    DkDevice device = dkDeviceCreate(&device_maker);
    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device);
    DkQueue queue = dkQueueCreate(&queue_maker);

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device);
    layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = Width;
    layout_maker.dimensions[1] = Height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layout_maker);

    uint32_t alignment = dkImageLayoutGetAlignment(&layout);
    uint32_t stride = (dkImageLayoutGetSize(&layout) + alignment - 1) & ~(alignment - 1);
    DkMemBlockMaker memory_maker;
    dkMemBlockMakerDefaults(&memory_maker, device, ImageCount * stride);
    memory_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock image_memory = dkMemBlockCreate(&memory_maker);

    DkImage images[ImageCount];
    const DkImage *image_refs[ImageCount];
    for (int i = 0; i < ImageCount; ++i) {
        dkImageInitialize(&images[i], &layout, image_memory, i * stride);
        image_refs[i] = &images[i];
    }
    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, device, nwindowGetDefault(), image_refs, ImageCount);
    DkSwapchain swapchain = dkSwapchainCreate(&swapchain_maker);
    dkSwapchainSetSwapInterval(swapchain, 1);

    dkMemBlockMakerDefaults(&memory_maker, device, DK_MEMBLOCK_ALIGNMENT);
    DkMemBlock commands_memory = dkMemBlockCreate(&memory_maker);
    DkCmdBufMaker cmd_maker;
    dkCmdBufMakerDefaults(&cmd_maker, device);
    DkCmdBuf commands = dkCmdBufCreate(&cmd_maker);
    dkCmdBufAddMemory(commands, commands_memory, 0, DK_MEMBLOCK_ALIGNMENT);

    DkCmdList lists[ImageCount];
    for (int i = 0; i < ImageCount; ++i) {
        DkImageView view;
        dkImageViewDefaults(&view, &images[i]);
        dkCmdBufBindRenderTarget(commands, &view, NULL);
        DkScissor full = {0, 0, Width, Height};
        DkScissor center = {Width / 4, Height / 4, Width / 2, Height / 2};
        dkCmdBufSetScissors(commands, 0, &full, 1);
        dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, 0.125f, 0.375f, 0.75f, 1.0f);
        dkCmdBufSetScissors(commands, 0, &center, 1);
        dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, 0.875f, 0.625f, 0.25f, 1.0f);
        lists[i] = dkCmdBufFinishList(commands);
    }

    ViDisplay display;
    Event vsync;
    check(viOpenDefaultDisplay(&display), "open display");
    check(viGetDisplayVsyncEvent(&display, &vsync), "get VSync event");

    // The regression runner stops the Runtime and captures a frame after 3 s.
    u64 deadline = armGetSystemTick() + armNsToTicks(15000000000ull);
    for (unsigned frame = 0; armGetSystemTick() < deadline; ++frame) {
        // viGetDisplayVsyncEvent returns an autoclear libnx Event.
        check(eventWait(&vsync, 1000000000ull), "wait for VSync");
        int slot = dkQueueAcquireImage(queue, swapchain);
        if (slot < 0 || slot >= ImageCount)
            fatalThrow(MAKERESULT(Module_Libnx, LibnxError_BadInput));
        dkQueueSubmitCommands(queue, lists[slot]);
        dkQueuePresentImage(queue, swapchain, slot);
        if (frame == 1)
            svcOutputDebugString(ready, sizeof ready - 1);
    }

    eventClose(&vsync);
    viCloseDisplay(&display);
    dkQueueWaitIdle(queue);
    dkCmdBufDestroy(commands);
    dkMemBlockDestroy(commands_memory);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(image_memory);
    dkQueueDestroy(queue);
    dkDeviceDestroy(device);
    return 0;
}
