// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/devices/gc_device.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/memory.h"

namespace Core::Devices {

std::shared_ptr<BaseDevice> GcDevice::Create(u32 handle, const char*, int, u16) {
    return std::shared_ptr<BaseDevice>(
        reinterpret_cast<Devices::BaseDevice*>(new GcDevice(handle)));
}

s32* submits_addr = 0;

s32 GcDevice::ioctl(u64 cmd, Common::VaCtx* args) {
    auto command = GcCommands(cmd);
    switch (command) {
    case GcCommands::FlushGarlic: {
        LOG_ERROR(Lib_GnmDriver, "ioctl FlushGarlic");
        break;
    }
    case GcCommands::SubmitDone: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl SubmitDone");
        break;
    }
    case GcCommands::WaitIdle: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl WaitIdle");
        break;
    }
    case GcCommands::WaitFree: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl WaitFree");
        break;
    }
    case GcCommands::GetNumTcaUnits: {
        auto data = vaArgPtr<s32*>(&args->va_list);
        *data = 0;
        break;
    }
    case GcCommands::SwitchBuffer: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl SwitchBuffer");
        break;
    }
    case GcCommands::DebugHardwareStatus: {
        break;
    }
    case GcCommands::InitializeSubmits: {
        LOG_INFO(Lib_GnmDriver, "ioctl InitializeSubmits");
        if (submits_addr == nullptr) {
            auto* memory = Core::Memory::Instance();
            s32* out_addr;
            VAddr in_addr{0xfe0100000};
            auto prot = Core::MemoryProt::CpuRead;
            auto flags = Core::MemoryMapFlags::Shared | Core::MemoryMapFlags::Anon |
                         Core::MemoryMapFlags::System;
            auto type = Core::VMAType::Flexible;
            s32 result = memory->MapMemory(reinterpret_cast<void**>(&out_addr), in_addr, 0x4000,
                                           prot, flags, type, "SceAppCommArea");
            if (result != 0) {
                return POSIX_ENOMEM;
            }
            submits_addr = out_addr;
        }
        auto data = vaArgPtr<s32*>(&args->va_list);
        *data = submits_addr;

        *submits_addr = 0;
        break;
    }
    case GcCommands::UnmapComputeQueue: {
        LOG_ERROR(Lib_GnmDriver, "ioctl UnmapComputeQueue");
        break;
    }
    case GcCommands::SetGsRingSizes: {
        auto data = vaArgPtr<SetGsRingSizesArgs>(&args->va_list);
        LOG_ERROR(Lib_GnmDriver,
                  "unhandled ioctl SetGsRingSizes, esgs size = {:#x}, gsvs size = {:#x}",
                  data->esgs_ring_size, data->gsvs_ring_size);
        break;
    }
    case GcCommands::Submit: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl Submit");
        auto data = vaArgPtr<SubmitArgs>(&args->va_list);
        // Submit ioctl receives an indirect buffer packet
        break;
    }
    case GcCommands::GetCuMask: {
        auto data = vaArgPtr<s32>(&args->va_list);
        data[0] = 0x10;
        data[1] = 0x10;
        data[2] = 0;
        data[3] = 0;
        break;
    }
    case GcCommands::DingDong: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl DingDong");
        break;
    }
    case GcCommands::RequiresNeoCompat: {
        return POSIX_ENODEV;
    }
    case GcCommands::SubmitEop: {
        ASSERT(true);
        LOG_ERROR(Lib_GnmDriver, "ioctl SubmitEop");
        auto data = vaArgPtr<SubmitEopArgs>(&args->va_list);
        // Submit ioctl receives an indirect buffer packet
        break;
    }
    case GcCommands::MapComputeQueue: {
        ASSERT(true);
        auto data = vaArgPtr<MapComputeQueueArgs>(&args->va_list);
        auto pipe_id = data->pipe_lo - 1;
        auto ring_size = pow(2, data->ring_size_dw);
        data->pipe_priority = 0;
        LOG_ERROR(Lib_GnmDriver, "ioctl MapComputeQueue, pipe_id = {}", pipe_id);
        Libraries::GnmDriver::sceGnmMapComputeQueue(pipe_id, data->queue_id, data->ring_base_addr,
                                                    ring_size, data->read_ptr_addr);
        break;
    }
    case GcCommands::MapComputeQueueWithPriority: {
        ASSERT(true);
        auto data = vaArgPtr<MapComputeQueueArgs>(&args->va_list);
        auto pipe_id = data->pipe_lo - 1;
        auto ring_size = pow(2, data->ring_size_dw);
        LOG_ERROR(Lib_GnmDriver, "ioctl MapComputeQueueWithPriority, pipe_id = {}", pipe_id);
        Libraries::GnmDriver::sceGnmMapComputeQueueWithPriority(
            pipe_id, data->queue_id, data->ring_base_addr, ring_size, data->read_ptr_addr,
            data->pipe_priority);
        break;
    }
    case GcCommands::SetWaveLimitMultipliers: {
        LOG_ERROR(Lib_GnmDriver, "ioctl SetWaveLimitMultipliers");
        auto data = vaArgPtr<SetWaveLimitMultipliersArgs>(&args->va_list);
        break;
    }
    case GcCommands::MipStatsReport: {
        auto data = vaArgPtr<SetMipStatsReportArgs>(&args->va_list);
        switch (data->type) {
        case 0x10001:
        case 0x18001: {
            break;
        }
        default: {
            return POSIX_EINVAL;
        }
        }
        break;
    }
    default: {
        LOG_ERROR(Lib_GnmDriver, "unhandled ioctl cmd = {:#x} called", cmd);
        break;
    }
    }
    return 0;
}

s64 GcDevice::write(const void* buf, size_t nbytes) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

size_t GcDevice::writev(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

size_t GcDevice::readv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::preadv(const Libraries::Kernel::SceKernelIovec* iov, int iovcnt, u64 offset) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::lseek(s64 offset, int whence) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::read(void* buf, size_t nbytes) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::fstat(Libraries::Kernel::OrbisKernelStat* sb) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s32 GcDevice::fsync() {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::ftruncate(s64 length) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

int GcDevice::getdents(void* buf, u32 nbytes, s64* basep) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

s64 GcDevice::pwrite(const void* buf, size_t nbytes, u64 offset) {
    LOG_ERROR(Kernel_Pthread, "(STUBBED) called");
    return 0;
}

} // namespace Core::Devices