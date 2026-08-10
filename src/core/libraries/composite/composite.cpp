
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/composite/composite.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::Composite {

void* sce_compositor_system_address = nullptr;
u64 sce_compositor_system_size = 0;
void* sce_compositor_video_address = nullptr;
u64 sce_compositor_video_size = 0;

s32 PS4_SYSV_ABI sceCompositorInitWithProcessOrder() {
    LOG_ERROR(Lib_Composite, "called");
    sce_compositor_system_address = nullptr;
    sce_compositor_video_address = nullptr;
    Libraries::Kernel::sceKernelMapNamedSystemFlexibleMemory(
        &sce_compositor_system_address, 512_MB, 0, std::to_underlying(Core::MemoryMapFlags::System),
        "sceComposite HLE buffer 1");
    sce_compositor_system_size = 512_MB;
    Libraries::Kernel::sceKernelMapNamedSystemFlexibleMemory(
        &sce_compositor_video_address, 1_GB, 0, std::to_underlying(Core::MemoryMapFlags::System),
        "sceComposite HLE buffer 2");
    sce_compositor_video_size = 1_GB;
    constexpr u32 sce_composite_color_width = 1280;
    constexpr u32 sce_composite_color_height = 720;
    constexpr u32 color_target_size = sce_composite_color_width * sce_composite_color_height * 4;

    using namespace VideoOut;
    using namespace Kernel;

    // SceVideoOut::bufs[0].base will be patched by libSceComposite, but we need to allocate a
    // buffer for the first frame flip
    sce_composite_color_target_addr = nullptr;
    void* dmem_addr;
    // TODO: User proper flags when I emulate them
    sceKernelAllocateMainDirectMemory(color_target_size, 16_KB, 0, (s64*)&dmem_addr);
    sceKernelMapDirectMemory(&sce_composite_color_target_addr, color_target_size, 0, 0,
                             (s64)dmem_addr, 0x1000);

    ASSERT(sce_composite_color_target_addr != nullptr);

    BufferAttribute attrib = {};
    attrib.pixel_format = PixelFormat::A8R8G8B8Srgb;
    attrib.tiling_mode = TilingMode::Linear;
    attrib.aspect_ratio = 0;
    attrib.width = sce_composite_color_width;
    attrib.height = sce_composite_color_height;
    attrib.pitch_in_pixel = sce_composite_color_width;
    attrib.option = 0;
    attrib.reserved0 = 0;
    attrib.reserved1 = 0;

    void* addrs[1] = {sce_composite_color_target_addr};
    sceVideoOutOpen(0, 0, 0, nullptr);
    sceVideoOutRegisterBuffers(2, 0, addrs, 1, &attrib);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCompositorGetRenderTargetResolution(s16* width, s16* height) {
    LOG_ERROR(Lib_Composite, "(STUBBED) called");
    *width = 1920;
    *height = 1080;
    return ORBIS_OK;
}

void* PS4_SYSV_ABI sceCompositorGetSystemAddress() {
    LOG_ERROR(Lib_Composite, "called");
    return sce_compositor_system_address;
}

s32 PS4_SYSV_ABI sceCompositorGetSystemSize() {
    LOG_ERROR(Lib_Composite, "called");
    return sce_compositor_system_size;
}

void* PS4_SYSV_ABI sceCompositorGetVideoAddress() {
    LOG_ERROR(Lib_Composite, "called");
    return sce_compositor_video_address;
}

s32 PS4_SYSV_ABI sceCompositorGetVideoSize() {
    LOG_ERROR(Lib_Composite, "called");
    return sce_compositor_video_size;
}

s32 PS4_SYSV_ABI sceCompositorAllocateIndex() {
    LOG_ERROR(Lib_Composite, "(STUBBED) called");
    return 1;
}

s32 PS4_SYSV_ABI sceCompositorSetFlipCommand() {
    LOG_ERROR(Lib_Composite, "called");
    Libraries::VideoOut::sceVideoOutSubmitFlip(2, 0, 0, 0);
    return ORBIS_OK;
}

u32* compositor_dcb_gpu_addr = nullptr;
u32 compositor_dcb_size = 0;
s32 PS4_SYSV_ABI sceCompositorSetGnmContextCommand(u32* dcb_gpu_addr, u32 dcb_size,
                                                   u32* ccb_gpu_addr, u32 ccb_size) {
    LOG_ERROR(Lib_Composite, "called");
    compositor_dcb_gpu_addr = dcb_gpu_addr;
    compositor_dcb_size = dcb_size;
    return Libraries::GnmDriver::sceGnmSubmitCommandBuffers(
        1, (const u32**)&compositor_dcb_gpu_addr, &compositor_dcb_size, nullptr, nullptr);
}

s32 PS4_SYSV_ABI sceCompositorWaitPostEvent() {
    LOG_ERROR(Lib_Composite, "called");
    while (!liverpool->IsGpuIdle())
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceCompsoitorGetGpuClock(u64* gpu_clock) {
    LOG_ERROR(Lib_Composite, "called");
    *gpu_clock = std::chrono::system_clock::now().time_since_epoch().count();
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("IUlpGnuoR1c", "libSceComposite", 1, "libSceComposite",
                 sceCompositorInitWithProcessOrder);
    LIB_FUNCTION("twGXom56jw0", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetRenderTargetResolution);
    LIB_FUNCTION("T6CVkdCDO7o", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetSystemAddress);
    LIB_FUNCTION("N6ID0KNnzY8", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetSystemSize);
    LIB_FUNCTION("bxt+muwit0w", "libSceComposite", 1, "libSceComposite",
                 sceCompositorGetVideoAddress);
    LIB_FUNCTION("FTQCTDU0b4g", "libSceComposite", 1, "libSceComposite", sceCompositorGetVideoSize);
    LIB_FUNCTION("G4Q8KNkb5XE", "libSceComposite", 1, "libSceComposite",
                 sceCompositorAllocateIndex);
    LIB_FUNCTION("1oTrw-ivVpA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetFlipCommand);
    LIB_FUNCTION("DhtKelVAIaA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorSetGnmContextCommand);
    LIB_FUNCTION("deKovf3qViA", "libSceComposite", 1, "libSceComposite",
                 sceCompositorWaitPostEvent);
    LIB_FUNCTION("4yWqjTZtvs4", "libSceComposite", 1, "libSceComposite", sceCompsoitorGetGpuClock);
}
} // namespace Libraries::Composite