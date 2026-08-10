// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <boost/asio/io_context.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/va_ctx.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/coredump/coredump.h"
#include "core/libraries/kernel/debug.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/sys_net.h"

#ifdef _WIN64
#include <Rpc.h>
#else
#include <uuid/uuid.h>
#endif
#include <common/singleton.h>
#include <core/libraries/network/net_error.h>
#include <core/libraries/network/sockets.h>
#include <core/linker.h>
#include "aio.h"

namespace Libraries::Kernel {

static u64 g_stack_chk_guard = 0xDEADBEEF54321ABC; // dummy return
static const char* internal_environ[32];
static const char** g_environ;
static const char* g_progname = "eboot.bin";

boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

Core::EntryParams entry_params{};

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:KernelServiceThread");

    while (!stoken.stop_requested()) {
        HLE_TRACE;
        {
            std::unique_lock lock{m_asio_req};
            Common::CondvarWait(cv_asio_req, lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        io_context.run();
        io_context.restart();

        asio_requests = 0;
    }
}

static PS4_SYSV_ABI void stack_chk_fail() {
    UNREACHABLE();
}

static thread_local s32 g_posix_errno = 0;

s32* PS4_SYSV_ABI __Error() {
    return &g_posix_errno;
}

void ErrSceToPosix(s32 error) {
    g_posix_errno = error - ORBIS_KERNEL_ERROR_UNKNOWN;
}

s32 ErrnoToSceKernelError(s32 error) {
    return error + ORBIS_KERNEL_ERROR_UNKNOWN;
}

s32 PS4_SYSV_ABI sceKernelError(s32 posix_error) {
    if (posix_error == 0) {
        return 0;
    }
    return posix_error + ORBIS_KERNEL_ERROR_UNKNOWN;
}

void SetPosixErrno(s32 e) {
    // Some error numbers are different between supported OSes
    switch (e) {
    case EPERM:
        g_posix_errno = POSIX_EPERM;
        break;
    case ENOENT:
        g_posix_errno = POSIX_ENOENT;
        break;
    case EINTR:
        g_posix_errno = POSIX_EINTR;
        break;
    case EDEADLK:
        g_posix_errno = POSIX_EDEADLK;
        break;
    case ENOMEM:
        g_posix_errno = POSIX_ENOMEM;
        break;
    case EACCES:
        g_posix_errno = POSIX_EACCES;
        break;
    case EFAULT:
        g_posix_errno = POSIX_EFAULT;
        break;
    case EINVAL:
        g_posix_errno = POSIX_EINVAL;
        break;
    case ENOSPC:
        g_posix_errno = POSIX_ENOSPC;
        break;
    case ERANGE:
        g_posix_errno = POSIX_ERANGE;
        break;
    case EAGAIN:
        g_posix_errno = POSIX_EAGAIN;
        break;
    case ETIMEDOUT:
        g_posix_errno = POSIX_ETIMEDOUT;
        break;
    default:
        LOG_WARNING(Kernel, "Unhandled errno {}", e);
        g_posix_errno = e;
    }
}

struct OrbisKernelUuid {
    u32 timeLow;
    u16 timeMid;
    u16 timeHiAndVersion;
    u8 clockSeqHiAndReserved;
    u8 clockSeqLow;
    u8 node[6];
};
static_assert(sizeof(OrbisKernelUuid) == 0x10);

s32 PS4_SYSV_ABI sceKernelUuidCreate(OrbisKernelUuid* orbisUuid) {
    if (!orbisUuid) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
#ifdef _WIN64
    UUID uuid;
    if (UuidCreate(&uuid) != RPC_S_OK) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
#else
    uuid_t uuid;
    uuid_generate(uuid);
#endif
    std::memcpy(orbisUuid, &uuid, sizeof(OrbisKernelUuid));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI kernel_ioctl(s32 fd, u64 cmd, VA_ARGS) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        LOG_INFO(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file == nullptr", fd, cmd);
        g_posix_errno = POSIX_EBADF;
        return -1;
    }
    if (file->type != Core::FileSys::FileType::Device) {
        LOG_WARNING(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file->type != Device", fd, cmd);
        g_posix_errno = ENOTTY;
        return -1;
    }
    VA_CTX(ctx);
    s32 result = file->device->ioctl(cmd, &ctx);
    LOG_TRACE(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} result = {}", fd, cmd, result);
    if (result < 0) {
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

const char* PS4_SYSV_ABI sceKernelGetFsSandboxRandomWord() {
    const char* path = "sys";
    return path;
}

s32 PS4_SYSV_ABI _sigprocmask() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_getpagesize() {
    return 16_KB;
}

// stubbed on non-devkit consoles
s32 PS4_SYSV_ABI sceKernelGetGPI() {
    LOG_DEBUG(Kernel, "called");
    return ORBIS_OK;
}

// stubbed on non-devkit consoles
s32 PS4_SYSV_ABI sceKernelSetGPO() {
    LOG_DEBUG(Kernel, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetAllowedSdkVersionOnSystem(s32* ver) {
    if (ver == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    // Returns the highest game SDK version this PS4 allows.
    *ver = CURRENT_FIRMWARE_VERSION | 0xfff;
    LOG_INFO(Lib_Kernel, "called, returned sw version: {}", *ver);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetSystemSwVersion(SwVersionStruct* ret) {
    if (ret == nullptr) {
        return ORBIS_OK;
    }
    u32 fake_fw = CURRENT_FIRMWARE_VERSION;
    ret->hex_representation = fake_fw;
    std::snprintf(ret->text_representation, 28, "%2x.%03x.%03x", fake_fw >> 0x18,
                  fake_fw >> 0xc & 0xfff, fake_fw & 0xfff);
    LOG_INFO(Lib_Kernel, "called, returned sw version: {}", ret->text_representation);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI getargc() {
    return entry_params.argc;
}

const char** PS4_SYSV_ABI getargv() {
    return entry_params.argv;
}

s32 PS4_SYSV_ABI get_authinfo(s32 pid, AuthInfoData* p2) {
    LOG_WARNING(Lib_Kernel, "(STUBBED) called, pid: {}", pid);
    if (p2 == nullptr) {
        *Kernel::__Error() = POSIX_EPERM;
        return -1;
    }
    if (pid != 0 && pid != GLOBAL_PID) {
        *Kernel::__Error() = POSIX_ESRCH;
        return -1;
    }

    *p2 = {};
    p2->caps[0] = 0x2000000000000000;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetAppInfo(s32 pid, OrbisKernelAppInfo* app_info) {
    LOG_WARNING(Lib_Kernel, "(STUBBED) called, pid: {}", pid);
    if (pid != GLOBAL_PID) {
        return ORBIS_KERNEL_ERROR_EPERM;
    }
    if (app_info == nullptr) {
        return ORBIS_OK;
    }

    auto& game_info = Common::ElfInfo::Instance();
    *app_info = {};
    app_info->has_param_sfo = 1;
    strncpy(app_info->cusa_name, game_info.GameSerial().data(), 10);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelTitleWorkaroundIsEnabled(OrbisKernelTitleWorkaround* tw, s32 bit,
                                                   s32* result) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, bit {:#x}", bit);
    if (!tw || !result) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    // Maximum bit value is known to change with new firmwares.
    if (bit >= 0x3a) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Straight from decompilation, most uses rely on workaround data from sceKernelGetAppInfo.
    *result = ((tw->ids[bit >> 6] >> (bit & 0x3f)) & 1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetProcessType(s32 pid) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, pid: {}", pid);
    if (pid != GLOBAL_PID) {
        return ORBIS_KERNEL_ERROR_ENOSYS;
    }
    return 0;
}

enum OrbisRegMgrOp : u32 {
    SetInt = 2,
};

enum OrbisRegMgrEntryKey : u32 {
    SystemLanguage = 0x2020000,
    SystemInitialize = 0x2040000,
    SystemButtonAssign = 0x20b0000,
    VideoOutResetResolutionFlag = 0xa130000,
};

s32 PS4_SYSV_ABI __sys_regmgr_call(OrbisRegMgrOp op, OrbisRegMgrEntryKey key, void* result,
                                   void* value, u64 len) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) called, op: {:#x}, key: {:#x}, len: {}", static_cast<u32>(op),
              static_cast<u32>(key), len);

    switch (op) {
    case OrbisRegMgrOp::SetInt: {
        u32* val_int = reinterpret_cast<u32*>(value);
        u32* result_int = reinterpret_cast<u32*>(result);
        switch (key) {
        case OrbisRegMgrEntryKey::SystemLanguage:
        case OrbisRegMgrEntryKey::SystemInitialize:
        case OrbisRegMgrEntryKey::SystemButtonAssign:
        case OrbisRegMgrEntryKey::VideoOutResetResolutionFlag: {
            if (val_int) {
                *val_int = 1;
            }
            if (result_int) {
                *result_int = 0;
            }
            break;
        }
        default: {
            LOG_ERROR(Lib_Kernel, "Unhandled regmgr key {:#x}", static_cast<u32>(key));
            if (val_int) {
                *val_int = 0;
            }
            if (result_int) {
                *result_int = 0;
            }
            break;
        }
        }
        break;
    }
    default: {
        LOG_ERROR(Lib_Kernel, "Unhandled regmgr op {}", static_cast<u32>(op));
        u32* val_int = reinterpret_cast<u32*>(value);
        u32* result_int = reinterpret_cast<u32*>(result);
        if (val_int) {
            *val_int = 0;
        }
        if (result_int) {
            *result_int = 0;
        }
        break;
    }
    }

    return ORBIS_OK;
}

// Nominally: long sysconf(int name);
u64 PS4_SYSV_ABI posix_sysconf(s32 name) {
    switch (name) {
    case 0:
        return 0x20000;
    case POSIX_SC_ARG_MAX:
        return 0x588bc000;
    case POSIX_SC_CHILD_MAX:
        return 0x64;
    case POSIX_SC_CLK_TCK:
        return 0x20;
    case POSIX_SC_NGROUPS_MAX:
        return 0x644;
    case POSIX_SC_OPEN_MAX:
        return -0x1;
    case POSIX_SC_JOB_CONTROL:
        return 0x6;
    case POSIX_SC_SAVED_IDS:
        return 0x1;
    case POSIX_SC_VERSION:
        return 0x1;
    case POSIX_SC_BC_BASE_MAX:
        return 0x31069;
    case POSIX_SC_BC_DIM_MAX:
        return -0x1;
    case POSIX_SC_BC_SCALE_MAX:
        return 0x31069;
    case POSIX_SC_BC_STRING_MAX:
        return 0x31069;
    case POSIX_SC_COLL_WEIGHTS_MAX:
        return -0x1;
    case POSIX_SC_EXPR_NEST_MAX:
        return -0x1;
    case POSIX_SC_LINE_MAX:
        return 0x31069;
    case POSIX_SC_RE_DUP_MAX:
        return 0x31069;
    case POSIX_SC_2_VERSION:
        return 0x31069;
    case POSIX_SC_2_C_BIND:
        return 0x31069;
    case POSIX_SC_2_C_DEV:
        return 0x31069;
    case POSIX_SC_2_CHAR_TERM:
        return 0x31069;
    case POSIX_SC_2_FORT_DEV:
        return 0x31069;
    case POSIX_SC_2_FORT_RUN:
        return 0x31069;
    case POSIX_SC_2_LOCALEDEF:
        return -0x1;
    case POSIX_SC_2_SW_DEV:
        return -0x1;
    case POSIX_SC_2_UPE:
        return 0x0;
    case POSIX_SC_STREAM_MAX:
        return 0x7fffffff;
    case POSIX_SC_TZNAME_MAX:
        return -0x1;
    case POSIX_SC_ASYNCHRONOUS_IO:
        return 0x8000;
    case POSIX_SC_MAPPED_FILES:
        return 0x31069;
    case POSIX_SC_MEMLOCK:
        return 0x4000;
    case POSIX_SC_MEMLOCK_RANGE:
        return 0x1e;
    case POSIX_SC_MEMORY_PROTECTION:
        return 0x100;
    case POSIX_SC_MESSAGE_PASSING:
        return 0x7fffffff;
    case POSIX_SC_PRIORITIZED_IO:
        return -0x1;
    case POSIX_SC_PRIORITY_SCHEDULING:
        return -0x1;
    case POSIX_SC_REALTIME_SIGNALS:
        return 0x63;
    case POSIX_SC_SEMAPHORES:
        return 0x800;
    case POSIX_SC_FSYNC:
        return 0x63;
    case POSIX_SC_SHARED_MEMORY_OBJECTS:
        return 0x3e8;
    case POSIX_SC_SYNCHRONIZED_IO:
        return 0x2;
    case POSIX_SC_THREAD_ATTR_STACKSIZE:
        return 0x1;
    case POSIX_SC_THREAD_CPUTIME:
        return 0x1;
    case POSIX_SC_THREAD_DESTRUCTOR_ITERATIONS:
        return 0x48000;
    case POSIX_SC_THREAD_KEYS_MAX:
        return 0x1a078630b2dd7;
    case POSIX_SC_THREAD_PRIO_INHERIT:
        return -0x1;
    case POSIX_SC_THREAD_PRIO_PROTECT:
        return -0x1;
    case POSIX_SC_THREAD_PRIORITY_SCHEDULING:
        return 0x2bc;
    case POSIX_SC_THREAD_PROCESS_SHARED:
        return 0x2bc;
    case POSIX_SC_THREAD_SAFE_FUNCTIONS:
        return 0x1;
    case POSIX_SC_THREAD_SPORADIC_SERVER:
        return -0x1;
    case POSIX_SC_THREAD_STACK_MIN:
        return 0x1;
    case POSIX_SC_THREAD_THREADS_MAX:
        return 0x1;
    case POSIX_SC_TIMEOUTS:
        return -0x1;
    // Manually specified
    case POSIX_SC_PAGESIZE:
        return posix_getpagesize();
    default:
        LOG_ERROR(Lib_Kernel, "unhandled {}", name);
        return 0;
    }
}

enum IpmiMgrOp : u32 {
    CreateServer = 0,
    DestroyServer = 1,
    CreateClient = 2,
    DestroyClient = 3,
    CreateSession = 4,
    DestroySession  = 5,
    Trace = 0x10,
    ServerReceivePacket = 0x201,
    SendConnectResult = 0x212,
    SessionRespondSync = 0x232,
    ClientInvokeAsyncMethod = 0x241,
    SessionRespondAsync = 0x242,
    ClientTryGetResult = 0x243,
    ClientGetMessage = 0x251,
    ClientTryGetMessage = 0x252,
    SessionTrySendMessage = 0x254,
    SessionGetClientPid = 0x302,
    ClientDisconnect = 0x310,
    ClientInvokeSyncMethod = 0x320,
    ClientConnect = 0x400,
    SessionGetClientAppId = 0x463,
    SessionGetUserData = 0x468,
    ServerGetName = 0x46a,
    ClientGetName = 0x46b,
    ClientWaitEventFlag = 0x490,
    ClientPollEventFlag = 0x491,
    SessionSetEventFlag = 0x493,
};

s32 PS4_SYSV_ABI ipmimgr_call(IpmiMgrOp op, u32 kid, u32* result, void* args, u64 args_size) {
    switch (op) {
    case IpmiMgrOp::CreateClient: {
        std::string name = *(char**)((u8*)args + 8);
        LOG_ERROR(Lib_Kernel, "Create client {}", name);
        if (name == "SceMorpheusUpdService" || name == "SceCompAppProxyUtil" ||
            name == "SceCompAppProxy" || name == "SceShellAppProxy" ||
            name == "SceStickerCoreServer" || name == "SceNpPartyIpc" ||
            name == "ScePartyIpcService" || name == "SceAppDbIpc") {
            *result = 0;
        } else {
            *result = -1;
        }

        if (name == "ScePartyIpcService" || name == "SceVnaIpcServer") {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        break;
    }
    case IpmiMgrOp::ServerReceivePacket: {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        *result = 0;
        break;
    }
    case IpmiMgrOp::ClientGetMessage: {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        *result = 0;
        break;
    }
    case IpmiMgrOp::ClientTryGetMessage: {
        *result = ORBIS_KERNEL_ERROR_EAGAIN;
        break;
    }
    case IpmiMgrOp::ClientWaitEventFlag: {
        *result = 0;
        break;
    }
    default: {
        LOG_ERROR(Lib_Kernel, "(STUBBED) called, op: {:#x}", static_cast<u32>(op));
        *result = 0;
        break;
    }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetPsmIntdevModeForRcmgr() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMbusInit() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMbusEventCreate_() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMbusGetDeviceInfoByCondition_() {
    // LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMbusEventReceive() {
    // LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

char* PS4_SYSV_ABI dlerror() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return nullptr;
}

s32 PS4_SYSV_ABI chdir(const char* path) {
    LOG_ERROR(Lib_Kernel, "(STUBBED) path = {}", path);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceApplicationInitialize() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI getrlimit(s32 rid, u64* limits) {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    if (limits) {
        if (rid == 8) {
            limits[0] = 256;
            limits[1] = 256;
        } else {
            limits[0] = 0xffffffffffffffff;
            limits[1] = 0xffffffffffffffff;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_spin_init() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_spin_lock() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_spin_trylock() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_spin_unlock() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_spin_destroy() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sysctl() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

static s32 shm_index = 0x10000;
s32 PS4_SYSV_ABI shm_open(const char* path) {
    LOG_ERROR(Lib_Kernel, "(STUBBED), path = {}", path);
    return shm_index++;
}

s32 PS4_SYSV_ABI shm_unlink(const char* path) {
    LOG_ERROR(Lib_Kernel, "(STUBBED), path = {}", path);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_mutex_setname_np(void* mutex, const char* name) {
    LOG_ERROR(Lib_Kernel, "(STUBBED), name = {}", name);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI pthread_cond_setname_np(void* mutex, const char* name) {
    LOG_ERROR(Lib_Kernel, "(STUBBED), name = {}", name);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutSysAddSetModeEvent() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_fcntl() {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetProcessName(s32 pid, char* name) {
    LOG_ERROR(Lib_Kernel, "(STUBBED)");
    std::strncpy(name, "eboot.bin", 10);
    return ORBIS_OK;
}

void PS4_SYSV_ABI sceLibcMspaceCreateForMonoMutex(u64 param1, u32 param2, u32 param3, u64 param4) {
    static bool is_called = false;
    ASSERT_MSG(!is_called, "cannot be called twice");
    ASSERT_MSG(!param1 && !param2 && !param3 && !param4, "parameters should all be 0");

    static constexpr std::string_view mspace_name = "SceLibcMutexPoolForMonoVM";
    void* addr_in = nullptr;
    s32 ret = sceKernelMapNamedSystemFlexibleMemory(&addr_in, 0x100000, 3, 0, mspace_name.data());
    ASSERT_MSG(!ret, "sceKernelMapNamedSystemFlexibleMemory must succeed");

    auto linker = Common::Singleton<Core::Linker>::Instance();
    auto* libcinternal_module = linker->GetModule(linker->FindByName("libSceLibcInternal.sprx"));

    static PS4_SYSV_ABI void* (*sceLibcMspaceCreate)(const char*, void*, u64, s32) = nullptr;
    static PS4_SYSV_ABI void (*nid_wUqJ0psUjDo)(void*) = nullptr;
    if (libcinternal_module) {
        sceLibcMspaceCreate =
            reinterpret_cast<PS4_SYSV_ABI void* (*)(const char*, void*, u64, s32)>(
                libcinternal_module->FindByName("sceLibcMspaceCreate"));
        nid_wUqJ0psUjDo = reinterpret_cast<PS4_SYSV_ABI void (*)(void*)>(
            libcinternal_module->FindByNid("wUqJ0psUjDo"));
    }

    if (!libcinternal_module) {
        LOG_ERROR(Lib_Kernel, "(STUBBED) called");
    } else {
        is_called = true;
        void* internal_mspace = sceLibcMspaceCreate(mspace_name.data(), addr_in, 0x100000, 12);
        nid_wUqJ0psUjDo(internal_mspace);
    }
}

s32 PS4_SYSV_ABI sceKernelIsCEX() {
    return 1;
}

s32 PS4_SYSV_ABI sceBgftServiceIntGetNotificationEvent() {
    return -1;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    service_thread = std::jthread{KernelServiceThread};
    std::memset(internal_environ, 0, sizeof(internal_environ));
    internal_environ[0] = "MONO_GC_PARAMS=nursery-size=64m,max-heap-size=256m";
    internal_environ[1] = "MONO_LOG_LEVEL=debug";
    internal_environ[2] = "MONO_LOG_MASK=all";
    // internal_environ[3] = "MONO_DISABLE_SHM=1";
    g_environ = internal_environ;

    Libraries::Kernel::RegisterFileSystem(sym);
    Libraries::Kernel::RegisterTime(sym);
    Libraries::Kernel::RegisterThreads(sym);
    Libraries::Kernel::RegisterKernelEventFlag(sym);
    Libraries::Kernel::RegisterMemory(sym);
    Libraries::Kernel::RegisterEventQueue(sym);
    Libraries::Kernel::RegisterProcess(sym);
    Libraries::Kernel::RegisterException(sym);
    Libraries::Kernel::RegisterAio(sym);
    Libraries::Kernel::RegisterDebug(sym);
    Libraries::Kernel::RegisterCoredump(sym);

    LIB_OBJ("f7uOxY9mM1U", "libkernel", 1, "libkernel", &g_stack_chk_guard);
    LIB_OBJ("+2thxYZ4syk", "libkernel", 1, "libkernel", &g_environ);
    LIB_OBJ("djxxOmW6-aw", "libkernel", 1, "libkernel", &g_progname);

    LIB_FUNCTION("Hk7iHmGxB18", "libkernel", 1, "libkernel", ipmimgr_call);
    LIB_FUNCTION("C2ltEJILIGE", "libkernel", 1, "libkernel", sceKernelGetPsmIntdevModeForRcmgr);
    LIB_FUNCTION("wRPXMGtkOq0", "libSceMbus", 1, "libSceMbus", sceMbusInit);
    LIB_FUNCTION("c08SEHicDNU", "libSceMbus", 1, "libSceMbus", sceMbusEventCreate_);
    LIB_FUNCTION("KRL-S9qBqXw", "libSceMbus", 1, "libSceMbus", sceMbusGetDeviceInfoByCondition_);
    LIB_FUNCTION("puHrnP8V-dY", "libSceMbus", 1, "libSceMbus", sceMbusEventReceive);
    LIB_FUNCTION("ucFJiTO1EUw", "libkernel", 1, "libkernel", dlerror);
    LIB_FUNCTION("6mMQ1MSPW-Q", "libkernel", 1, "libkernel", chdir);
    LIB_FUNCTION("XFYItOxS6r0", "libSceSysCore", 1, "libSceSysCore", sceApplicationInitialize);
    LIB_FUNCTION("Wh7HbV7JFqc", "libkernel", 1, "libkernel", getrlimit);
    LIB_FUNCTION("ZMn3clnAGBA", "libkernel", 1, "libkernel", pthread_spin_init);
    LIB_FUNCTION("pw+70ClLYlY", "libkernel", 1, "libkernel", pthread_spin_lock);
    LIB_FUNCTION("rCTGkBIHfPY", "libkernel", 1, "libkernel", pthread_spin_trylock);
    LIB_FUNCTION("LEfMMCT+SlM", "libkernel", 1, "libkernel", pthread_spin_unlock);
    LIB_FUNCTION("IJIggoPZExk", "libkernel", 1, "libkernel", pthread_spin_destroy);
    LIB_FUNCTION("DFmMT80xcNI", "libkernel", 1, "libkernel", sysctl);
    LIB_FUNCTION("QuJYZ2KVGGQ", "libkernel", 1, "libkernel", shm_open);
    LIB_FUNCTION("tPWsbOUGO8k", "libkernel", 1, "libkernel", shm_unlink);
    LIB_FUNCTION("nTxZBp8YNGc", "libkernel", 1, "libkernel", pthread_mutex_setname_np);
    LIB_FUNCTION("EZ8h70dtFLg", "libkernel", 1, "libkernel", pthread_cond_setname_np);
    LIB_FUNCTION("X8FN-5Nk-yE", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutSysAddSetModeEvent);
    LIB_FUNCTION("8nY19bKoiZk", "libkernel", 1, "libkernel", posix_fcntl);
    LIB_FUNCTION("fUJRLEbJOuQ", "libkernel", 1, "libkernel", sceKernelGetProcessName);
    LIB_FUNCTION("8aCOCGoRkUI", "libkernel", 1, "libkernel", sceKernelIsCEX);
    LIB_FUNCTION("vJhYrkgTYWY", "libSceBgft", 1, "libSceBgft", sceBgftServiceIntGetNotificationEvent);

    LIB_FUNCTION("D4yla3vx4tY", "libkernel", 1, "libkernel", sceKernelError);
    LIB_FUNCTION("YeU23Szo3BM", "libkernel", 1, "libkernel", sceKernelGetAllowedSdkVersionOnSystem);
    LIB_FUNCTION("Mv1zUObHvXI", "libkernel", 1, "libkernel", sceKernelGetSystemSwVersion);
    LIB_FUNCTION("igMefp4SAv0", "libkernel", 1, "libkernel", get_authinfo);
    LIB_FUNCTION("G-MYv5erXaU", "libkernel", 1, "libkernel", sceKernelGetAppInfo);
    LIB_FUNCTION("1yca4VvfcNA", "libkernel", 1, "libkernel", sceKernelTitleWorkaroundIsEnabled);
    LIB_FUNCTION("+g+UP8Pyfmo", "libkernel", 1, "libkernel", sceKernelGetProcessType);
    LIB_FUNCTION("PfccT7qURYE", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("wW+k21cmbwQ", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("JGfTMBOdUJo", "libkernel", 1, "libkernel", sceKernelGetFsSandboxRandomWord);
    LIB_FUNCTION("JGfTMBOdUJo", "libkernel_psmkit", 1, "libkernel",
                 sceKernelGetFsSandboxRandomWord);
    LIB_FUNCTION("6xVpy0Fdq+I", "libkernel", 1, "libkernel", _sigprocmask);
    LIB_FUNCTION("Xjoosiw+XPI", "libkernel", 1, "libkernel", sceKernelUuidCreate);
    LIB_FUNCTION("Ou3iL1abvng", "libkernel", 1, "libkernel", stack_chk_fail);
    LIB_FUNCTION("9BcDykPmo1I", "libkernel", 1, "libkernel", __Error);
    LIB_FUNCTION("k+AXqu2-eBc", "libkernel", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("k+AXqu2-eBc", "libScePosix", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("7NwggrWJ5cA", "libkernel", 1, "libkernel", __sys_regmgr_call);
    LIB_FUNCTION("pi90NsG3zPA", "libkernel", 1, "libkernel", sceLibcMspaceCreateForMonoMutex);

    LIB_FUNCTION("mkawd0NA9ts", "libkernel", 1, "libkernel", posix_sysconf);
    LIB_FUNCTION("mkawd0NA9ts", "libScePosix", 1, "libkernel", posix_sysconf);

    // network
    LIB_FUNCTION("XVL8So3QJUk", "libkernel", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("pG70GT5yRo4", "libkernel", 1, "libkernel", Libraries::Net::sys_socketex);
    LIB_FUNCTION("KuOmgKoqCdY", "libkernel", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("6O8EwYOgH9Y", "libkernel", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libkernel", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("pxnCmagrtao", "libkernel", 1, "libkernel", Libraries::Net::sys_listen);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libkernel", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("TUuiYS2kE8s", "libkernel", 1, "libkernel", Libraries::Net::sys_shutdown);
    LIB_FUNCTION("TU-d9PfIHPM", "libkernel", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel_ps2emu", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("K1S8oc61xiM", "libkernel", 1, "libkernel", Libraries::Net::sys_htonl);
    LIB_FUNCTION("jogUIsOV3-U", "libkernel", 1, "libkernel", Libraries::Net::sys_htons);
    LIB_FUNCTION("fZOeZIOEmLw", "libkernel", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libkernel", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libkernel", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libkernel", 1, "libkernel", Libraries::Net::sys_recvfrom);

    LIB_FUNCTION("TU-d9PfIHPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("fZOeZIOEmLw", "libScePosix", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libScePosix", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvfrom);
    LIB_FUNCTION("hI7oVeOluPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvmsg);
    LIB_FUNCTION("TXFFFiNldU8", "libScePosix", 1, "libkernel", Libraries::Net::sys_getpeername);
    LIB_FUNCTION("6O8EwYOgH9Y", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libScePosix", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("RenI1lL1WFk", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockname);
    LIB_FUNCTION("KuOmgKoqCdY", "libScePosix", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("5jRCs2axtr4", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetNtop); // TODO fix it to sys_ ...
    LIB_FUNCTION("4n51s0zEf0c", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetPton); // TODO fix it to sys_ ...
    LIB_FUNCTION("XVL8So3QJUk", "libScePosix", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libScePosix", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("aNeavPDNKzA", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendmsg);
    LIB_FUNCTION("pxnCmagrtao", "libScePosix", 1, "libkernel", Libraries::Net::sys_listen);

    LIB_FUNCTION("4oXYe9Xmk0Q", "libkernel", 1, "libkernel", sceKernelGetGPI);
    LIB_FUNCTION("ca7v6Cxulzs", "libkernel", 1, "libkernel", sceKernelSetGPO);
    LIB_FUNCTION("iKJMWrAumPE", "libkernel", 1, "libkernel", getargc);
    LIB_FUNCTION("FJmglmTMdr4", "libkernel", 1, "libkernel", getargv);
}

} // namespace Libraries::Kernel
