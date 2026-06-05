// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <unordered_map>
#include <pugixml.hpp>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/slot_vector.h"
#include "core/emulator_settings.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/np/trophy_ui.h"
#include "core/libraries/system/userservice.h"
#include "core/memory.h"

namespace Libraries::Np::NpTrophy {

// PS4 system language IDs map directly to TROP00.XML .. TROP30.XML.
// Index = OrbisSystemServiceParamId language value reported by the system.
// clang-format off
static constexpr std::array<std::string_view, 31> s_language_xml_names = {
    "TROP_00.XML", // 00 Japanese
    "TROP_01.XML", // 01 English (US)
    "TROP_02.XML", // 02 French
    "TROP_03.XML", // 03 Spanish (ES)
    "TROP_04.XML", // 04 German
    "TROP_05.XML", // 05 Italian
    "TROP_06.XML", // 06 Dutch
    "TROP_07.XML", // 07 Portuguese (PT)
    "TROP_08.XML", // 08 Russian
    "TROP_09.XML", // 09 Korean
    "TROP_10.XML", // 10 Traditional Chinese
    "TROP_11.XML", // 11 Simplified Chinese
    "TROP_12.XML", // 12 Finnish
    "TROP_13.XML", // 13 Swedish
    "TROP_14.XML", // 14 Danish
    "TROP_15.XML", // 15 Norwegian
    "TROP_16.XML", // 16 Polish
    "TROP_17.XML", // 17 Portuguese (BR)
    "TROP_18.XML", // 18 English (GB)
    "TROP_19.XML", // 19 Turkish
    "TROP_20.XML", // 20 Spanish (LA)
    "TROP_21.XML", // 21 Arabic
    "TROP_22.XML", // 22 French (CA)
    "TROP_23.XML", // 23 Czech
    "TROP_24.XML", // 24 Hungarian
    "TROP_25.XML", // 25 Greek
    "TROP_26.XML", // 26 Romanian
    "TROP_27.XML", // 27 Thai
    "TROP_28.XML", // 28 Vietnamese
    "TROP_29.XML", // 29 Indonesian
    "TROP_30.XML", // 30 Unkrainian
};
// clang-format on

// Returns the best available trophy XML path for the current system language.
// Resolution order:
//   1. TROP_XX.XML for the active system language (e.g. TROP01.XML for English)
//   2. TROP.XML    (master / language-neutral fallback)
static std::filesystem::path GetTrophyXmlPath(const std::filesystem::path& xml_dir,
                                              int system_language) {
    // Try the exact language file first.
    if (system_language >= 0 && system_language < static_cast<int>(s_language_xml_names.size())) {
        auto lang_path = xml_dir / s_language_xml_names[system_language];
        if (std::filesystem::exists(lang_path)) {
            return lang_path;
        }
    }
    // Final fallback: master TROP.XML (always present).
    return xml_dir / "TROP.XML";
}

static bool IsEmptyTitleId(const OrbisNpTitleId& title_id) {
    return title_id.id[0] == '\0';
}

static bool IsZeroTitleSecret(const OrbisNpTitleSecret& title_secret) {
    return std::all_of(std::begin(title_secret.data), std::end(title_secret.data),
                       [](u8 byte) { return byte == 0; });
}

static bool IsSupportedTrophyConfVersion(std::string_view version) {
    return version == "1.0" || version == "1.1";
}

static bool IsSupportedTrophyConfPolicy(std::string_view policy) {
    return policy == "small" || policy == "large";
}

static bool HasNonZeroTrophySetVersion(std::string_view version) {
    for (char ch : version) {
        if (ch >= '1' && ch <= '9') {
            return true;
        }
        if ((ch < '0' || ch > '9') && ch != '.') {
            return false;
        }
    }
    return false;
}

static s32 ValidateTrophyConfStructure(const pugi::xml_node& trophy_conf) {
    std::unordered_map<int, bool> group_ids{};
    std::unordered_map<int, char> trophy_types{};
    int platinum_count = 0;

    for (const auto& node : trophy_conf.children()) {
        const auto node_name = std::string_view{node.name()};
        if (node_name == "group") {
            const int group_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_GROUP_ID);
            if (group_id < 0 || group_id >= 0x20 || group_ids.contains(group_id)) {
                return ORBIS_NP_TROPHY_ERROR_INVALID_GROUP_ID;
            }
            group_ids[group_id] = true;
            continue;
        }

        if (node_name != "trophy") {
            continue;
        }

        const int trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
        if (trophy_id < 0 || trophy_id >= ORBIS_NP_TROPHY_NUM_MAX ||
            trophy_types.contains(trophy_id)) {
            return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
        }

        const auto trophy_type = std::string_view{node.attribute("ttype").value()};
        if (trophy_type.size() != 1 || (trophy_type[0] != 'B' && trophy_type[0] != 'S' &&
                                        trophy_type[0] != 'G' && trophy_type[0] != 'P')) {
            return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
        }

        if (trophy_type[0] == 'P') {
            platinum_count++;
        }

        trophy_types[trophy_id] = trophy_type[0];
    }

    if (platinum_count > 1) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }

    for (const auto& node : trophy_conf.children()) {
        if (std::string_view{node.name()} != "trophy") {
            continue;
        }

        const int trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
        const auto trophy_it = trophy_types.find(trophy_id);
        if (trophy_it == trophy_types.end()) {
            return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
        }

        const int parent_id = node.attribute("pid").as_int(-1);
        if (trophy_it->second == 'P') {
            if (parent_id != -1) {
                return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
            }
        } else if (parent_id != -1 && !trophy_types.contains(parent_id)) {
            return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
        }

        const int group_id = node.attribute("gid").as_int(-1);
        if (group_id != -1 && !group_ids.contains(group_id)) {
            return ORBIS_NP_TROPHY_ERROR_INVALID_GROUP_ID;
        }
    }

    return ORBIS_OK;
}

static s32 ValidateTitleTrophyXml(const std::filesystem::path& xml_path,
                                  std::string_view expected_np_comm_id) {
    pugi::xml_document doc;
    const auto result = doc.load_file(xml_path.native().c_str());
    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse trophy xml {}: {}", xml_path.string(),
                  result.description());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }

    const auto trophy_conf = doc.child("trophyconf");
    if (!trophy_conf) {
        LOG_ERROR(Lib_NpTrophy, "Missing trophyconf root in {}", xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }

    const auto version = std::string_view{trophy_conf.attribute("version").value()};
    if (version.empty()) {
        LOG_ERROR(Lib_NpTrophy, "Missing trophyconf version in {}", xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }
    if (!IsSupportedTrophyConfVersion(version)) {
        LOG_ERROR(Lib_NpTrophy, "Unsupported trophyconf version {} in {}", version,
                  xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_UNSUPPORTED_TROPHY_CONF;
    }

    const auto platform = std::string_view{trophy_conf.attribute("platform").value()};
    if (platform.empty()) {
        LOG_ERROR(Lib_NpTrophy, "Missing trophyconf platform in {}", xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }
    if (platform != "ps4") {
        LOG_ERROR(Lib_NpTrophy, "Unsupported trophyconf platform {} in {}", platform,
                  xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_UNSUPPORTED_TROPHY_CONF;
    }

    const auto policy = std::string_view{trophy_conf.attribute("policy").value()};
    if (policy.empty()) {
        LOG_ERROR(Lib_NpTrophy, "Missing trophyconf policy in {}", xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }
    if (!IsSupportedTrophyConfPolicy(policy)) {
        LOG_ERROR(Lib_NpTrophy, "Unsupported trophyconf policy {} in {}", policy,
                  xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_UNSUPPORTED_TROPHY_CONF;
    }

    const auto np_comm_id = std::string_view{trophy_conf.child("npcommid").text().as_string()};
    if (np_comm_id.empty()) {
        LOG_ERROR(Lib_NpTrophy, "Missing npcommid in {}", xml_path.string());
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }
    if (np_comm_id != expected_np_comm_id) {
        LOG_ERROR(Lib_NpTrophy, "npcommid mismatch in {}: expected {}, got {}", xml_path.string(),
                  expected_np_comm_id, np_comm_id);
        return ORBIS_NP_TROPHY_ERROR_INCONSISTENT_TITLE_CONF;
    }

    const auto trophy_set_version =
        std::string_view{trophy_conf.child("trophyset-version").text().as_string()};
    if (trophy_set_version.empty() || !HasNonZeroTrophySetVersion(trophy_set_version)) {
        LOG_ERROR(Lib_NpTrophy, "Invalid trophyset-version in {}: {}", xml_path.string(),
                  trophy_set_version);
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_CONF_FORMAT;
    }

    const auto structure_result = ValidateTrophyConfStructure(trophy_conf);
    if (structure_result < 0) {
        return structure_result;
    }

    return ORBIS_OK;
}

static s32 EnsureUserTrophyXmlExists(const std::filesystem::path& trophy_conf_path,
                                     const std::filesystem::path& user_xml_path) {
    if (std::filesystem::exists(user_xml_path)) {
        return ORBIS_OK;
    }

    std::error_code ec;
    std::filesystem::create_directories(user_xml_path.parent_path(), ec);
    if (ec) {
        LOG_ERROR(Lib_NpTrophy, "Failed to create trophy user directory {}: {}",
                  user_xml_path.parent_path().string(), ec.message());
        return ORBIS_NP_TROPHY_ERROR_BROKEN_TITLE_CONF;
    }

    std::filesystem::copy_file(trophy_conf_path, user_xml_path, std::filesystem::copy_options::none,
                               ec);
    if (ec) {
        LOG_ERROR(Lib_NpTrophy, "Failed to create user trophy xml {}: {}", user_xml_path.string(),
                  ec.message());
        return ORBIS_NP_TROPHY_ERROR_BROKEN_TITLE_CONF;
    }

    return ORBIS_OK;
}

static s32 ValidateUserTrophyXml(const std::filesystem::path& xml_path,
                                 std::string_view expected_np_comm_id) {
    const auto validation_result = ValidateTitleTrophyXml(xml_path, expected_np_comm_id);
    if (validation_result == ORBIS_OK) {
        return ORBIS_OK;
    }
    if (validation_result == ORBIS_NP_TROPHY_ERROR_INCONSISTENT_TITLE_CONF) {
        return validation_result;
    }

    return ORBIS_NP_TROPHY_ERROR_BROKEN_TITLE_CONF;
}

static void ApplyUnlockToXmlFile(const std::filesystem::path& xml_path, OrbisNpTrophyId trophyId,
                                 u64 trophyTimestamp, bool unlock_platinum,
                                 OrbisNpTrophyId platinumId, u64 platinumTimestamp) {
    pugi::xml_document doc;
    if (!doc.load_file(xml_path.native().c_str())) {
        LOG_WARNING(Lib_NpTrophy, "ApplyUnlock: failed to load {}", xml_path.string());
        return;
    }

    auto trophyconf = doc.child("trophyconf");
    for (pugi::xml_node& node : trophyconf.children()) {
        if (std::string_view(node.name()) != "trophy") {
            continue;
        }
        int id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);

        auto set_unlock = [&](u64 ts) {
            if (node.attribute("unlockstate").empty()) {
                node.append_attribute("unlockstate") = "true";
            } else {
                node.attribute("unlockstate").set_value("true");
            }
            const auto ts_str = std::to_string(ts);
            if (node.attribute("timestamp").empty()) {
                node.append_attribute("timestamp") = ts_str.c_str();
            } else {
                node.attribute("timestamp").set_value(ts_str.c_str());
            }
        };

        if (id == trophyId) {
            set_unlock(trophyTimestamp);
        } else if (unlock_platinum && id == platinumId) {
            set_unlock(platinumTimestamp);
        }
    }

    doc.save_file(xml_path.native().c_str());
}

static constexpr auto MaxTrophyHandles = 4u;
static constexpr auto MaxTrophyContexts = 8u;

using ContextKey = std::pair<u32, u32>; // <user_id, service label>
struct ContextKeyHash {
    size_t operator()(const ContextKey& key) const {
        return key.first + (u64(key.second) << 32u);
    }
};

struct TrophyContext {
    u32 context_id;
    u32 service_label;
    u32 user_id;
    bool registered = false;
    bool user_logged_out = false;
    std::filesystem::path trophy_xml_path; // resolved once at RegisterContext
    std::filesystem::path xml_dir;         // .../Xml/
    std::filesystem::path xml_save_file;   // The actual file for tracking progress per-user.
    std::filesystem::path icons_dir;       // .../Icons/
};
enum class TrophyHandleKind : u8 {
    Trophy = 0,
    System = 1,
    Internal = 2,
};

struct TrophyHandleState {
    TrophyHandleKind kind;
};

static Common::SlotVector<TrophyHandleState> trophy_handles{};
static Common::SlotVector<ContextKey> trophy_contexts{};
static std::unordered_map<ContextKey, TrophyContext, ContextKeyHash> contexts_internal{};
static bool g_trophy_module_initialized = false;
static u32 g_trophy_user_service_ref_count = 0;
static bool g_trophy_user_service_callback_registered = false;

static void MarkTrophyContextsUserLoggedOut(
    Libraries::UserService::OrbisUserServiceUserId user_id) {
    for (auto& [_, ctx] : contexts_internal) {
        if (ctx.user_id == user_id) {
            ctx.user_logged_out = true;
        }
    }
}

static void PS4_SYSV_ABI
OnTrophyUserServiceEvent(const Libraries::UserService::OrbisUserServiceEvent* event, void*) {
    if (event == nullptr) {
        return;
    }
    if (event->event == Libraries::UserService::OrbisUserServiceEventType::Logout) {
        MarkTrophyContextsUserLoggedOut(event->userId);
    }
}

static s32 AcquireTrophyUserServiceCallback() {
    if (g_trophy_user_service_ref_count == 0) {
        const auto result = Libraries::UserService::sceUserServiceRegisterEventCallback(
            &OnTrophyUserServiceEvent, nullptr);
        if (result < 0) {
            return result;
        }
        g_trophy_user_service_callback_registered = true;
    }

    g_trophy_user_service_ref_count++;
    return ORBIS_OK;
}

static void ReleaseTrophyUserServiceCallback() {
    if (g_trophy_user_service_ref_count == 0) {
        return;
    }

    g_trophy_user_service_ref_count--;
    if (g_trophy_user_service_ref_count == 0 && g_trophy_user_service_callback_registered) {
        const auto result = Libraries::UserService::sceUserServiceUnregisterEventCallback(
            &OnTrophyUserServiceEvent, nullptr);
        if (result < 0) {
            LOG_WARNING(Lib_NpTrophy, "Failed to unregister trophy user-service callback: {:#x}",
                        result);
        }
        g_trophy_user_service_callback_registered = false;
    }
}

static s32 CreateHandleWithKind(TrophyHandleKind kind, OrbisNpTrophyHandle* handle,
                                std::string_view handle_name) {
    if (!g_trophy_module_initialized) {
        return ORBIS_NP_TROPHY_ERROR_NOT_INITIALIZED;
    }

    if (!handle) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    }

    *handle = ORBIS_NP_TROPHY_INVALID_HANDLE;

    if (trophy_handles.size() >= MaxTrophyHandles) {
        return ORBIS_NP_TROPHY_ERROR_HANDLE_EXCEEDS_MAX;
    }

    const auto callback_result = AcquireTrophyUserServiceCallback();
    if (callback_result < 0) {
        return callback_result;
    }

    const auto handle_id = trophy_handles.insert(TrophyHandleState{kind});
    *handle = handle_id.index + 1;
    LOG_INFO(Lib_NpTrophy, "{}: New handle = {}", handle_name, *handle);
    return ORBIS_OK;
}

static s32 DestroyHandleWithKind(TrophyHandleKind kind, OrbisNpTrophyHandle handle,
                                 std::string_view handle_name) {
    if (!g_trophy_module_initialized) {
        return ORBIS_NP_TROPHY_ERROR_NOT_INITIALIZED;
    }

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    const s32 handle_index = handle - 1;
    if (handle_index < 0 || handle_index >= static_cast<s32>(trophy_handles.size())) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    const Common::SlotId handle_id{static_cast<u32>(handle_index)};
    if (!trophy_handles.is_allocated(handle_id) || trophy_handles[handle_id].kind != kind) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    trophy_handles.erase(handle_id);
    ReleaseTrophyUserServiceCallback();
    LOG_INFO(Lib_NpTrophy, "{}: Handle {} destroyed", handle_name, handle);
    return ORBIS_OK;
}

static s32 AbortHandleWithKind(TrophyHandleKind kind, OrbisNpTrophyHandle handle,
                               std::string_view handle_name) {
    if (!g_trophy_module_initialized) {
        return ORBIS_NP_TROPHY_ERROR_NOT_INITIALIZED;
    }

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    const s32 handle_index = handle - 1;
    if (handle_index < 0 || handle_index >= static_cast<s32>(trophy_handles.size())) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    const Common::SlotId handle_id{static_cast<u32>(handle_index)};
    if (!trophy_handles.is_allocated(handle_id) || trophy_handles[handle_id].kind != kind) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    LOG_INFO(Lib_NpTrophy, "{}: Handle {} aborted", handle_name, handle);
    return ORBIS_OK;
}

void ORBIS_NP_TROPHY_FLAG_ZERO(OrbisNpTrophyFlagArray* p) {
    for (int i = 0; i < ORBIS_NP_TROPHY_NUM_MAX; i++) {
        uint32_t array_index = i / 32;
        uint32_t bit_position = i % 32;

        p->flag_bits[array_index] &= ~(1U << bit_position);
    }
}

void ORBIS_NP_TROPHY_FLAG_SET(int32_t trophyId, OrbisNpTrophyFlagArray* p) {
    uint32_t array_index = trophyId / 32;
    uint32_t bit_position = trophyId % 32;

    p->flag_bits[array_index] |= (1U << bit_position);
}

void ORBIS_NP_TROPHY_FLAG_SET_ALL(OrbisNpTrophyFlagArray* p) {
    for (int i = 0; i < ORBIS_NP_TROPHY_NUM_MAX; i++) {
        uint32_t array_index = i / 32;
        uint32_t bit_position = i % 32;

        p->flag_bits[array_index] |= (1U << bit_position);
    }
}

void ORBIS_NP_TROPHY_FLAG_CLR(int32_t trophyId, OrbisNpTrophyFlagArray* p) {
    uint32_t array_index = trophyId / 32;
    uint32_t bit_position = trophyId % 32;

    p->flag_bits[array_index] &= ~(1U << bit_position);
}

bool ORBIS_NP_TROPHY_FLAG_ISSET(int32_t trophyId, OrbisNpTrophyFlagArray* p) {
    uint32_t array_index = trophyId / 32;
    uint32_t bit_position = trophyId % 32;

    return (p->flag_bits[array_index] & (1U << bit_position)) ? 1 : 0;
}

OrbisNpTrophyGrade GetTrophyGradeFromChar(char trophyType) {
    switch (trophyType) {
    default:
        return ORBIS_NP_TROPHY_GRADE_UNKNOWN;
        break;
    case 'B':
        return ORBIS_NP_TROPHY_GRADE_BRONZE;
        break;
    case 'S':
        return ORBIS_NP_TROPHY_GRADE_SILVER;
        break;
    case 'G':
        return ORBIS_NP_TROPHY_GRADE_GOLD;
        break;
    case 'P':
        return ORBIS_NP_TROPHY_GRADE_PLATINUM;
        break;
    }
}

s32 PS4_SYSV_ABI sceNpTrophyCreateContext(OrbisNpTrophyContext* context,
                                          Libraries::UserService::OrbisUserServiceUserId user_id,
                                          uint32_t service_label, u64 options) {
    if (!g_trophy_module_initialized) {
        return ORBIS_NP_TROPHY_ERROR_NOT_INITIALIZED;
    }

    if (!context || options != 0ull) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    }

    if (user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_USER_ID;
    }

    if (service_label >= 100) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_NP_SERVICE_LABEL;
    }

    *context = ORBIS_NP_TROPHY_INVALID_CONTEXT;

    if (trophy_contexts.size() >= MaxTrophyContexts) {
        return ORBIS_NP_TROPHY_ERROR_CONTEXT_EXCEEDS_MAX;
    }

    const auto& key = ContextKey{user_id, service_label};
    if (contexts_internal.contains(key)) {
        return ORBIS_NP_TROPHY_ERROR_CONTEXT_ALREADY_EXISTS;
    }

    const auto callback_result = AcquireTrophyUserServiceCallback();
    if (callback_result < 0) {
        return callback_result;
    }

    const auto ctx_id = trophy_contexts.insert(user_id, service_label);

    *context = ctx_id.index + 1;

    auto& ctx = contexts_internal[key];
    ctx.context_id = *context;
    ctx.service_label = service_label;
    ctx.user_id = user_id;
    ctx.user_logged_out = false;

    LOG_INFO(Lib_NpTrophy, "New context = {}, user_id = {} service label = {}", *context, user_id,
             service_label);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTrophyCreateHandle(OrbisNpTrophyHandle* handle) {
    return CreateHandleWithKind(TrophyHandleKind::Trophy, handle, "sceNpTrophyCreateHandle");
}

int PS4_SYSV_ABI sceNpTrophyDestroyContext(OrbisNpTrophyContext context) {
    LOG_INFO(Lib_NpTrophy, "Destroyed Context {}", context);

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    Common::SlotId contextId;
    contextId.index = context - 1;

    if (contextId.index >= trophy_contexts.size()) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    if (!trophy_contexts.is_allocated(contextId)) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    ContextKey contextkey = trophy_contexts[contextId];
    trophy_contexts.erase(contextId);
    contexts_internal.erase(contextkey);
    ReleaseTrophyUserServiceCallback();

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTrophyDestroyHandle(OrbisNpTrophyHandle handle) {
    return DestroyHandleWithKind(TrophyHandleKind::Trophy, handle, "sceNpTrophyDestroyHandle");
}

u64 ReadFile(Common::FS::IOFile& file, void* buf, u64 nbytes) {
    const auto* memory = Core::Memory::Instance();
    // Invalidate up to the actual number of bytes that could be read.
    const auto remaining = file.GetSize() - file.Tell();
    memory->InvalidateMemory(reinterpret_cast<VAddr>(buf), std::min<u64>(nbytes, remaining));

    return file.ReadRaw<u8>(buf, nbytes);
}

int PS4_SYSV_ABI sceNpTrophyGetGameIcon(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                        void* buffer, u64* size) {
    ASSERT(size != nullptr);

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size()) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }
    ContextKey contextkey = trophy_contexts[contextId];

    const auto& ctx = contexts_internal[contextkey];

    auto icon_file = ctx.icons_dir / "ICON0.PNG";

    Common::FS::IOFile icon(icon_file, Common::FS::FileAccessMode::Read);
    if (!icon.IsOpen()) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy icon file: {}", icon_file.string());
        return ORBIS_NP_TROPHY_ERROR_ICON_FILE_NOT_FOUND;
    }
    u64 icon_size = icon.GetSize();

    if (buffer != nullptr) {
        ReadFile(icon, buffer, *size);
    } else {
        *size = icon_size;
    }
    return ORBIS_OK;
}

struct GameTrophyInfo {
    uint32_t num_groups;
    uint32_t num_trophies;
    uint32_t num_trophies_by_rarity[5];
    uint32_t unlocked_trophies;
    uint32_t unlocked_trophies_by_rarity[5];
};

int PS4_SYSV_ABI sceNpTrophyGetGameInfo(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                        OrbisNpTrophyGameDetails* details,
                                        OrbisNpTrophyGameData* data) {
    LOG_INFO(Lib_NpTrophy, "Getting Game Trophy");

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (details == nullptr || data == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (details->size != 0x4A0 || data->size != 0x20)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size()) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }
    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;
    const auto& trophy_file = ctx.trophy_xml_path;
    const auto& trophy_save_file = ctx.xml_save_file;

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(trophy_file.native().c_str());

    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse trophy xml : {}", result.description());
        return ORBIS_OK;
    }

    GameTrophyInfo game_info{};

    auto trophyconf = doc.child("trophyconf");
    for (const pugi::xml_node& node : trophyconf.children()) {
        std::string_view node_name = node.name();

        if (node_name == "title-name") {
            strncpy(details->title, node.text().as_string(), ORBIS_NP_TROPHY_GAME_TITLE_MAX_SIZE);
        }

        if (node_name == "title-detail") {
            strncpy(details->description, node.text().as_string(),
                    ORBIS_NP_TROPHY_GAME_DESCR_MAX_SIZE);
        }

        if (node_name == "group")
            game_info.num_groups++;
    }

    pugi::xml_document save_doc;
    pugi::xml_parse_result save_result = save_doc.load_file(ctx.xml_save_file.native().c_str());

    if (!save_result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse user trophy xml : {}", result.description());
        return ORBIS_OK;
    }
    auto save_trophyconf = save_doc.child("trophyconf");
    for (const pugi::xml_node& node : save_trophyconf.children()) {
        std::string_view node_name = node.name();
        if (node_name == "trophy") {
            bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();
            std::string_view current_trophy_grade = node.attribute("ttype").value();

            if (current_trophy_grade.empty()) {
                continue;
            }

            game_info.num_trophies++;
            int trophy_grade = GetTrophyGradeFromChar(current_trophy_grade.at(0));
            game_info.num_trophies_by_rarity[trophy_grade]++;

            if (current_trophy_unlockstate) {
                game_info.unlocked_trophies++;
                game_info.unlocked_trophies_by_rarity[trophy_grade]++;
            }
        }
    }

    details->num_groups = game_info.num_groups;
    details->num_trophies = game_info.num_trophies;
    details->num_platinum = game_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_PLATINUM];
    details->num_gold = game_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_GOLD];
    details->num_silver = game_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_SILVER];
    details->num_bronze = game_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_BRONZE];
    data->unlocked_trophies = game_info.unlocked_trophies;
    data->unlocked_platinum = game_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_PLATINUM];
    data->unlocked_gold = game_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_GOLD];
    data->unlocked_silver = game_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_SILVER];
    data->unlocked_bronze = game_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_BRONZE];

    data->progress_percentage = (game_info.num_trophies > 0)
                                    ? (game_info.unlocked_trophies * 100u) / game_info.num_trophies
                                    : 0;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyGetGroupIcon(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                         OrbisNpTrophyGroupId groupId, void* buffer, u64* size) {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

struct GroupTrophyInfo {
    uint32_t num_trophies;
    uint32_t num_trophies_by_rarity[5];
    uint32_t unlocked_trophies;
    uint32_t unlocked_trophies_by_rarity[5];
};

int PS4_SYSV_ABI sceNpTrophyGetGroupInfo(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                         OrbisNpTrophyGroupId groupId,
                                         OrbisNpTrophyGroupDetails* details,
                                         OrbisNpTrophyGroupData* data) {
    LOG_INFO(Lib_NpTrophy, "Getting Trophy Group Info for id {}", groupId);

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (details == nullptr || data == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (details->size != 0x4A0 || data->size != 0x28)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size()) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }
    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;
    const auto& trophy_file = ctx.trophy_xml_path;

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(trophy_file.native().c_str());

    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy xml : {}", result.description());
        return ORBIS_OK;
    }

    GroupTrophyInfo group_info{};

    auto trophyconf = doc.child("trophyconf");
    for (const pugi::xml_node& node : trophyconf.children()) {
        std::string_view node_name = node.name();

        if (node_name == "group") {
            int current_group_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_GROUP_ID);
            if (current_group_id != ORBIS_NP_TROPHY_INVALID_GROUP_ID) {
                if (current_group_id == groupId) {
                    std::string_view current_group_name = node.child("name").text().as_string();
                    std::string_view current_group_description =
                        node.child("detail").text().as_string();

                    strncpy(details->title, current_group_name.data(),
                            ORBIS_NP_TROPHY_GROUP_TITLE_MAX_SIZE);
                    strncpy(details->description, current_group_description.data(),
                            ORBIS_NP_TROPHY_GAME_DESCR_MAX_SIZE);
                }
            }
        }

        details->group_id = groupId;
        data->group_id = groupId;
    }

    pugi::xml_document save_doc;
    pugi::xml_parse_result save_result = save_doc.load_file(ctx.xml_save_file.native().c_str());

    if (!save_result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse user trophy xml : {}", result.description());
        return ORBIS_OK;
    }
    auto save_trophyconf = save_doc.child("trophyconf");
    for (const pugi::xml_node& node : save_trophyconf.children()) {
        std::string_view node_name = node.name();
        if (node_name == "trophy") {
            bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();
            std::string_view current_trophy_grade = node.attribute("ttype").value();
            int current_trophy_group_id = node.attribute("gid").as_int(-1);

            if (current_trophy_grade.empty()) {
                continue;
            }

            if (current_trophy_group_id == groupId) {
                group_info.num_trophies++;
                int trophyGrade = GetTrophyGradeFromChar(current_trophy_grade.at(0));
                group_info.num_trophies_by_rarity[trophyGrade]++;
                if (current_trophy_unlockstate) {
                    group_info.unlocked_trophies++;
                    group_info.unlocked_trophies_by_rarity[trophyGrade]++;
                }
            }
        }
    }

    details->num_trophies = group_info.num_trophies;
    details->num_platinum = group_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_PLATINUM];
    details->num_gold = group_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_GOLD];
    details->num_silver = group_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_SILVER];
    details->num_bronze = group_info.num_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_BRONZE];
    data->unlocked_trophies = group_info.unlocked_trophies;
    data->unlocked_platinum =
        group_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_PLATINUM];
    data->unlocked_gold = group_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_GOLD];
    data->unlocked_silver = group_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_SILVER];
    data->unlocked_bronze = group_info.unlocked_trophies_by_rarity[ORBIS_NP_TROPHY_GRADE_BRONZE];

    data->progress_percentage =
        (group_info.num_trophies > 0)
            ? (group_info.unlocked_trophies * 100u) / group_info.num_trophies
            : 0;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyGetTrophyIcon(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                          OrbisNpTrophyId trophyId, void* buffer, u64* size) {
    if (size == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (trophyId < 0 || trophyId >= ORBIS_NP_TROPHY_NUM_MAX)
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size() || !trophy_contexts.is_allocated(contextId)) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    s32 handle_index = handle - 1;
    if (handle_index >= trophy_handles.size() ||
        !trophy_handles.is_allocated({static_cast<u32>(handle_index)})) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;

    // Check that the trophy is unlocked and icons are only available for earned trophies.
    pugi::xml_document doc;
    if (!doc.load_file(ctx.xml_save_file.native().c_str())) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy XML: {}", ctx.xml_save_file.string());
        return ORBIS_NP_TROPHY_ERROR_ICON_FILE_NOT_FOUND;
    }

    bool unlocked = false;
    bool found = false;
    for (const pugi::xml_node& node : doc.child("trophyconf").children()) {
        if (std::string_view(node.name()) != "trophy")
            continue;
        if (node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID) == trophyId) {
            found = true;
            unlocked = node.attribute("unlockstate").as_bool();
            break;
        }
    }

    if (!found)
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    if (!unlocked)
        return ORBIS_NP_TROPHY_ERROR_TROPHY_NOT_UNLOCKED;

    const std::string icon_name = fmt::format("TROP{:03d}.PNG", trophyId);
    const auto icon_path = ctx.icons_dir / icon_name;

    Common::FS::IOFile icon(icon_path, Common::FS::FileAccessMode::Read);
    if (!icon.IsOpen()) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy icon: {}", icon_path.string());
        return ORBIS_NP_TROPHY_ERROR_ICON_FILE_NOT_FOUND;
    }

    if (buffer != nullptr) {
        ReadFile(icon, buffer, *size);
    } else {
        *size = icon.GetSize();
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyGetTrophyInfo(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                          OrbisNpTrophyId trophyId, OrbisNpTrophyDetails* details,
                                          OrbisNpTrophyData* data) {
    LOG_INFO(Lib_NpTrophy, "Getting trophy info for id {}", trophyId);

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (trophyId >= ORBIS_NP_TROPHY_NUM_MAX)
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    if (details == nullptr || data == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (details->size != 0x498 || data->size != 0x18)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size()) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }
    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;
    const auto& trophy_file = ctx.trophy_xml_path;

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(trophy_file.native().c_str());

    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy xml : {}", result.description());
        return ORBIS_OK;
    }

    auto trophyconf = doc.child("trophyconf");

    for (const pugi::xml_node& node : trophyconf.children()) {
        std::string_view node_name = node.name();

        if (node_name == "trophy") {
            int current_trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
            if (current_trophy_id == trophyId) {
                std::string_view current_trophy_name = node.child("name").text().as_string();
                std::string_view current_trophy_description =
                    node.child("detail").text().as_string();

                strncpy(details->name, current_trophy_name.data(), ORBIS_NP_TROPHY_NAME_MAX_SIZE);
                strncpy(details->description, current_trophy_description.data(),
                        ORBIS_NP_TROPHY_DESCR_MAX_SIZE);
            }
        }
    }

    pugi::xml_document save_doc;
    pugi::xml_parse_result save_result = save_doc.load_file(ctx.xml_save_file.native().c_str());

    if (!save_result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse user trophy xml : {}", result.description());
        return ORBIS_OK;
    }
    auto save_trophyconf = save_doc.child("trophyconf");
    for (const pugi::xml_node& node : save_trophyconf.children()) {
        std::string_view node_name = node.name();

        if (node_name == "trophy") {
            int current_trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
            if (current_trophy_id == trophyId) {
                bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();
                std::string_view current_trophy_grade = node.attribute("ttype").value();

                uint64_t current_trophy_timestamp = node.attribute("timestamp").as_ullong();
                int current_trophy_groupid = node.attribute("gid").as_int(-1);
                bool current_trophy_hidden = node.attribute("hidden").as_bool();

                details->trophy_id = trophyId;
                details->trophy_grade = GetTrophyGradeFromChar(current_trophy_grade.at(0));
                details->group_id = current_trophy_groupid;
                details->hidden = current_trophy_hidden;

                data->trophy_id = trophyId;
                data->unlocked = current_trophy_unlockstate;
                data->timestamp.tick = current_trophy_timestamp;
            }
        }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTrophyGetTrophyUnlockState(OrbisNpTrophyContext context,
                                                 OrbisNpTrophyHandle handle,
                                                 OrbisNpTrophyFlagArray* flags, u32* count) {
    LOG_INFO(Lib_NpTrophy, "called");

    if (flags == nullptr || count == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size() || !trophy_contexts.is_allocated(contextId)) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    s32 handle_index = handle - 1;
    if (handle_index >= trophy_handles.size() ||
        !trophy_handles.is_allocated({static_cast<u32>(handle_index)})) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;
    const auto& trophy_file = ctx.xml_save_file;

    ORBIS_NP_TROPHY_FLAG_ZERO(flags);

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(trophy_file.native().c_str());

    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to open trophy XML: {}", result.description());
        *count = 0;
        return ORBIS_OK;
    }

    int num_trophies = 0;
    auto trophyconf = doc.child("trophyconf");

    for (const pugi::xml_node& node : trophyconf.children()) {
        std::string_view node_name = node.name();
        int current_trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
        bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();

        if (node_name == "trophy") {
            num_trophies++;
            if (current_trophy_unlockstate) {
                ORBIS_NP_TROPHY_FLAG_SET(current_trophy_id, flags);
            }
        }
    }

    *count = num_trophies;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyRegisterContext(OrbisNpTrophyContext context,
                                            OrbisNpTrophyHandle handle, uint64_t options) {
    if (!g_trophy_module_initialized) {
        return ORBIS_NP_TROPHY_ERROR_NOT_INITIALIZED;
    }

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (options != 0ull)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    s32 handle_index = handle - 1;
    if (handle_index < 0 || handle_index >= static_cast<s32>(trophy_handles.size()) ||
        !trophy_handles.is_allocated({static_cast<u32>(handle_index)})) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }
    s32 context_index = context - 1;
    if (context_index < 0 || context_index >= static_cast<s32>(trophy_contexts.size())) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    Common::SlotId contextId;
    contextId.index = static_cast<u32>(context_index);
    if (!trophy_contexts.is_allocated(contextId)) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    ContextKey contextkey = trophy_contexts[contextId];
    auto& ctx = contexts_internal[contextkey];
    if (ctx.user_logged_out) {
        return ORBIS_NP_TROPHY_ERROR_CONTEXT_USER_LOGOUT;
    }

    OrbisNpTitleId title_id{};
    OrbisNpTitleSecret title_secret{};
    const auto title_secret_result =
        NpManager::sceNpIntGetNpTitleIdSecret(&title_id, &title_secret);
    if (title_secret_result < 0) {
        return title_secret_result;
    }

    if (IsEmptyTitleId(title_id) || IsZeroTitleSecret(title_secret)) {
        LOG_ERROR(Lib_NpTrophy, "NP title id or title secret is unavailable.");
        return ORBIS_NP_TROPHY_ERROR_INVALID_NP_TITLE_ID;
    }

    const auto& trophyMap = Common::ElfInfo::Instance().GetTrophyIndexMap();
    auto it = trophyMap.find(ctx.service_label);
    if (it == trophyMap.end()) {
        LOG_ERROR(Lib_NpTrophy, "No npCommId found for trophy index/service_label: {}",
                  ctx.service_label);
        return ORBIS_NP_TROPHY_ERROR_TITLE_CONF_NOT_INSTALLED;
    }

    const auto& np_comm_id = it->second;
    const auto trophy_base =
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "trophy" / np_comm_id;
    ctx.xml_save_file = EmulatorSettings.GetHomeDir() / std::to_string(ctx.user_id) / "trophy" /
                        (np_comm_id + ".xml");
    ctx.xml_dir = trophy_base / "Xml";
    ctx.icons_dir = trophy_base / "Icons";
    ctx.trophy_xml_path = GetTrophyXmlPath(ctx.xml_dir, EmulatorSettings.GetConsoleLanguage());

    const auto trophy_conf_path = ctx.xml_dir / "TROPCONF.XML";
    if (!std::filesystem::exists(ctx.trophy_xml_path) ||
        !std::filesystem::exists(trophy_conf_path)) {
        LOG_ERROR(Lib_NpTrophy, "Could not find trophy files.");
        return ORBIS_NP_TROPHY_ERROR_TITLE_CONF_NOT_INSTALLED;
    }

    auto validation_result = ValidateTitleTrophyXml(trophy_conf_path, np_comm_id);
    if (validation_result < 0) {
        return validation_result;
    }

    validation_result = ValidateTitleTrophyXml(ctx.trophy_xml_path, np_comm_id);
    if (validation_result < 0) {
        return validation_result;
    }

    validation_result = EnsureUserTrophyXmlExists(trophy_conf_path, ctx.xml_save_file);
    if (validation_result < 0) {
        return validation_result;
    }

    validation_result = ValidateUserTrophyXml(ctx.xml_save_file, np_comm_id);
    if (validation_result < 0) {
        return validation_result;
    }

    ctx.registered = true;
    LOG_INFO(Lib_NpTrophy, "Context {} registered", context);

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyUnlockTrophy(OrbisNpTrophyContext context, OrbisNpTrophyHandle handle,
                                         OrbisNpTrophyId trophyId, OrbisNpTrophyId* platinumId) {
    LOG_INFO(Lib_NpTrophy, "Unlocking trophy id {}", trophyId);

    if (context == ORBIS_NP_TROPHY_INVALID_CONTEXT)
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle == ORBIS_NP_TROPHY_INVALID_HANDLE)
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (trophyId >= ORBIS_NP_TROPHY_NUM_MAX)
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    if (platinumId == nullptr)
        return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    Common::SlotId contextId;
    contextId.index = context - 1;
    if (contextId.index >= trophy_contexts.size() || !trophy_contexts.is_allocated(contextId)) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
    }

    s32 handle_index = handle - 1;
    if (handle_index >= trophy_handles.size() ||
        !trophy_handles.is_allocated({static_cast<u32>(handle_index)})) {
        return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
    }

    ContextKey contextkey = trophy_contexts[contextId];
    const auto& ctx = contexts_internal[contextkey];
    if (!ctx.registered)
        return ORBIS_NP_TROPHY_ERROR_NOT_REGISTERED;
    const auto& xml_dir = ctx.xml_dir;
    const auto& trophy_file = ctx.trophy_xml_path;

    pugi::xml_document save_doc;
    pugi::xml_parse_result save_result = save_doc.load_file(ctx.xml_save_file.native().c_str());

    if (!save_result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse user trophy xml : {}", save_result.description());
        return ORBIS_OK;
    }
    auto save_trophyconf = save_doc.child("trophyconf");
    for (const pugi::xml_node& node : save_trophyconf.children()) {
        std::string_view node_name = node.name();
        if (std::string_view(node.name()) != "trophy")
            continue;

        int current_trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
        bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();

        if (current_trophy_id == trophyId) {
            if (current_trophy_unlockstate) {
                LOG_INFO(Lib_NpTrophy, "Trophy already unlocked");
                return ORBIS_NP_TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED;
            }
        }
    }

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(trophy_file.native().c_str());

    if (!result) {
        LOG_ERROR(Lib_NpTrophy, "Failed to parse trophy xml : {}", result.description());
        return ORBIS_NP_TROPHY_ERROR_TITLE_NOT_FOUND;
    }

    *platinumId = ORBIS_NP_TROPHY_INVALID_TROPHY_ID;

    int num_trophies = 0;
    int num_trophies_unlocked = 0;
    pugi::xml_node platinum_node;

    // Outputs filled during the scan.
    bool trophy_found = false;
    const char* trophy_name = "";
    std::string_view trophy_type;
    std::filesystem::path trophy_icon_path;

    auto trophyconf = doc.child("trophyconf");

    for (pugi::xml_node& node : trophyconf.children()) {
        if (std::string_view(node.name()) != "trophy")
            continue;

        int current_trophy_id = node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
        bool current_trophy_unlockstate = node.attribute("unlockstate").as_bool();
        std::string_view current_trophy_type = node.attribute("ttype").value();

        if (current_trophy_type == "P") {
            platinum_node = node;
            if (trophyId == current_trophy_id) {
                return ORBIS_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK;
            }
        }

        if (node.attribute("pid").as_int(-1) != ORBIS_NP_TROPHY_INVALID_TROPHY_ID) {
            num_trophies++;
            if (current_trophy_unlockstate) {
                num_trophies_unlocked++;
            }
        }

        if (current_trophy_id == trophyId) {
            trophy_found = true;
            trophy_name = node.child("name").text().as_string();
            trophy_type = current_trophy_type;

            const std::string icon_file = fmt::format("TROP{:03d}.PNG", current_trophy_id);
            trophy_icon_path = ctx.icons_dir / icon_file;
        }
    }

    if (!trophy_found)
        return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    // Capture timestamps once so every file gets the exact same value.
    const auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const u64 trophy_timestamp = static_cast<u64>(now_secs);

    // Decide platinum.
    bool unlock_platinum = false;
    OrbisNpTrophyId platinum_id = ORBIS_NP_TROPHY_INVALID_TROPHY_ID;
    u64 platinum_timestamp = 0;
    const char* platinum_name = "";
    std::filesystem::path platinum_icon_path;

    if (!platinum_node.attribute("unlockstate").as_bool()) {
        if ((num_trophies - 1) == num_trophies_unlocked) {
            unlock_platinum = true;
            platinum_id = platinum_node.attribute("id").as_int(ORBIS_NP_TROPHY_INVALID_TROPHY_ID);
            platinum_timestamp = trophy_timestamp; // same second is fine
            platinum_name = platinum_node.child("name").text().as_string();

            const std::string plat_icon_file = fmt::format("TROP{:03d}.PNG", platinum_id);
            platinum_icon_path = ctx.icons_dir / plat_icon_file;

            *platinumId = platinum_id;
        }
    }

    // Queue UI notifications (only once, using the primary XML's strings).
    AddTrophyToQueue(trophy_icon_path, trophy_name, trophy_type);
    if (unlock_platinum) {
        AddTrophyToQueue(platinum_icon_path, platinum_name, "P");
    }

    ApplyUnlockToXmlFile(ctx.xml_save_file, trophyId, trophy_timestamp, unlock_platinum,
                         platinum_id, platinum_timestamp);
    LOG_INFO(Lib_NpTrophy, "Trophy {} successfully saved.", trophyId);

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyGroupArrayGetNum() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntAbortHandle(OrbisNpTrophyHandle handle) {
    return AbortHandleWithKind(TrophyHandleKind::Internal, handle, "sceNpTrophyIntAbortHandle");
}

int PS4_SYSV_ABI sceNpTrophyIntCheckNetSyncTitles() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntCreateHandle(OrbisNpTrophyHandle* handle) {
    return CreateHandleWithKind(TrophyHandleKind::Internal, handle, "sceNpTrophyIntCreateHandle");
}

int PS4_SYSV_ABI sceNpTrophyIntDestroyHandle(OrbisNpTrophyHandle handle) {
    return DestroyHandleWithKind(TrophyHandleKind::Internal, handle, "sceNpTrophyIntDestroyHandle");
}

int PS4_SYSV_ABI sceNpTrophyIntGetLocalTrophySummary() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntGetProgress() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntGetRunningTitle() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntGetRunningTitles() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntGetTrpIconByUri() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntNetSyncTitle() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyIntNetSyncTitles() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyNumInfoGetTotal() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySetInfoGetTrophyFlagArray() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySetInfoGetTrophyNum() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyShowTrophyList(OrbisNpTrophyContext context,
                                           OrbisNpTrophyHandle handle) {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemAbortHandle(OrbisNpTrophyHandle handle) {
    return AbortHandleWithKind(TrophyHandleKind::System, handle, "sceNpTrophySystemAbortHandle");
}

int PS4_SYSV_ABI sceNpTrophySystemBuildGroupIconUri() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemBuildNetTrophyIconUri() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemBuildTitleIconUri() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemBuildTrophyIconUri() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemCheckNetSyncTitles() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemCheckRecoveryRequired() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemCloseStorage() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemCreateContext() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemCreateHandle(OrbisNpTrophyHandle* handle) {
    return CreateHandleWithKind(TrophyHandleKind::System, handle, "sceNpTrophySystemCreateHandle");
}

int PS4_SYSV_ABI sceNpTrophySystemDbgCtl() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemDebugLockTrophy() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemDebugUnlockTrophy() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemDestroyContext() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemDestroyHandle(OrbisNpTrophyHandle handle) {
    return DestroyHandleWithKind(TrophyHandleKind::System, handle,
                                 "sceNpTrophySystemDestroyHandle");
}

int PS4_SYSV_ABI sceNpTrophySystemDestroyTrophyConfig() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetDbgParam() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetDbgParamInt() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetGroupIcon() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetLocalTrophySummary() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetNextTitleFileEntryStatus() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetProgress() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTitleFileStatus() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTitleIcon() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTitleSyncStatus() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyConfig() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyData() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyGroupData() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyIcon() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyTitleData() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetTrophyTitleIds() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetUserFileInfo() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemGetUserFileStatus() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemIsServerAvailable() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemNetSyncTitle() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemNetSyncTitles() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemOpenStorage() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemPerformRecovery() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemRemoveAll() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemRemoveTitleData() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemRemoveUserData() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemSetDbgParam() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophySystemSetDbgParamInt() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyAbortHandle(OrbisNpTrophyHandle handle) {
    return AbortHandleWithKind(TrophyHandleKind::Trophy, handle, "sceNpTrophyAbortHandle");
}

int PS4_SYSV_ABI sceNpTrophyCaptureScreenshot() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophyDetails() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophyFlagArray() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophyGroupArray() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophyGroupDetails() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophySetInfo() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophySetInfoInGroup() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophySetVersion() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigGetTrophyTitleDetails() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpTrophyConfigHasGroupFeature() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_149656DA81D41C59() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_9F80071876FFA5F6() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_F8EF6F5350A91990() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_FA7A2DD770447552() {
    LOG_ERROR(Lib_NpTrophy, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    g_trophy_module_initialized = true;

    LIB_FUNCTION("aTnHs7W-9Uk", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyAbortHandle);
    LIB_FUNCTION("cqGkYAN-gRw", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyCaptureScreenshot);
    LIB_FUNCTION("lhE4XS9OJXs", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophyDetails);
    LIB_FUNCTION("qJ3IvrOoXg0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophyFlagArray);
    LIB_FUNCTION("zDjF2G+6tI0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophyGroupArray);
    LIB_FUNCTION("7Kh86vJqtxw", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophyGroupDetails);
    LIB_FUNCTION("ndLeNWExeZE", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophySetInfo);
    LIB_FUNCTION("6EOfS5SDgoo", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophySetInfoInGroup);
    LIB_FUNCTION("MW5ygoZqEBs", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophySetVersion);
    LIB_FUNCTION("3tWKpNKn5+I", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigGetTrophyTitleDetails);
    LIB_FUNCTION("iqYfxC12sak", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyConfigHasGroupFeature);
    LIB_FUNCTION("XbkjbobZlCY", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyCreateContext);
    LIB_FUNCTION("q7U6tEAQf7c", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyCreateHandle);
    LIB_FUNCTION("E1Wrwd07Lr8", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyDestroyContext);
    LIB_FUNCTION("GNcF4oidY0Y", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyDestroyHandle);
    LIB_FUNCTION("HLwz1fRIycA", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetGameIcon);
    LIB_FUNCTION("YYP3f2W09og", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetGameInfo);
    LIB_FUNCTION("w4uMPmErD4I", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetGroupIcon);
    LIB_FUNCTION("wTUwGfspKic", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetGroupInfo);
    LIB_FUNCTION("eBL+l6HG9xk", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetTrophyIcon);
    LIB_FUNCTION("qqUVGDgQBm0", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGetTrophyInfo);
    LIB_FUNCTION("LHuSmO3SLd8", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyGetTrophyUnlockState);
    LIB_FUNCTION("Ht6MNTl-je4", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyGroupArrayGetNum);
    LIB_FUNCTION("u9plkqa2e0k", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntAbortHandle);
    LIB_FUNCTION("pE5yhroy9m0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyIntCheckNetSyncTitles);
    LIB_FUNCTION("edPIOFpEAvU", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntCreateHandle);
    LIB_FUNCTION("DSh3EXpqAQ4", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntDestroyHandle);
    LIB_FUNCTION("sng98qULzPA", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyIntGetLocalTrophySummary);
    LIB_FUNCTION("t3CQzag7-zs", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntGetProgress);
    LIB_FUNCTION("jF-mCgGuvbQ", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyIntGetRunningTitle);
    LIB_FUNCTION("PeAyBjC5kp8", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyIntGetRunningTitles);
    LIB_FUNCTION("PEo09Dkqv0o", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophyIntGetTrpIconByUri);
    LIB_FUNCTION("kF9zjnlAzIA", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntNetSyncTitle);
    LIB_FUNCTION("UXiyfabxFNQ", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyIntNetSyncTitles);
    LIB_FUNCTION("hvdThnVvwdY", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyNumInfoGetTotal);
    LIB_FUNCTION("TJCAxto9SEU", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyRegisterContext);
    LIB_FUNCTION("ITUmvpBPaG0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySetInfoGetTrophyFlagArray);
    LIB_FUNCTION("BSoSgiMVHnY", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySetInfoGetTrophyNum);
    LIB_FUNCTION("d9jpdPz5f-8", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyShowTrophyList);
    LIB_FUNCTION("JzJdh-JLtu0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemAbortHandle);
    LIB_FUNCTION("z8RCP536GOM", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemBuildGroupIconUri);
    LIB_FUNCTION("Rd2FBOQE094", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemBuildNetTrophyIconUri);
    LIB_FUNCTION("Q182x0rT75I", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemBuildTitleIconUri);
    LIB_FUNCTION("lGnm5Kg-zpA", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemBuildTrophyIconUri);
    LIB_FUNCTION("20wAMbXP-u0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemCheckNetSyncTitles);
    LIB_FUNCTION("sKGFFY59ksY", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemCheckRecoveryRequired);
    LIB_FUNCTION("JMSapEtDH9Q", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemCloseStorage);
    LIB_FUNCTION("dk27olS4CEE", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemCreateContext);
    LIB_FUNCTION("cBzXEdzVzvs", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemCreateHandle);
    LIB_FUNCTION("8aLlLHKP+No", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophySystemDbgCtl);
    LIB_FUNCTION("NobVwD8qcQY", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemDebugLockTrophy);
    LIB_FUNCTION("yXJlgXljItk", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemDebugUnlockTrophy);
    LIB_FUNCTION("U0TOSinfuvw", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemDestroyContext);
    LIB_FUNCTION("-LC9hudmD+Y", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemDestroyHandle);
    LIB_FUNCTION("q6eAMucXIEM", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemDestroyTrophyConfig);
    LIB_FUNCTION("WdCUUJLQodM", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetDbgParam);
    LIB_FUNCTION("4QYFwC7tn4U", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetDbgParamInt);
    LIB_FUNCTION("OcllHFFcQkI", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetGroupIcon);
    LIB_FUNCTION("tQ3tXfVZreU", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetLocalTrophySummary);
    LIB_FUNCTION("g0dxBNTspC0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetNextTitleFileEntryStatus);
    LIB_FUNCTION("sJSDnJRJHhI", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetProgress);
    LIB_FUNCTION("X47s4AamPGg", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTitleFileStatus);
    LIB_FUNCTION("7WPj4KCF3D8", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTitleIcon);
    LIB_FUNCTION("pzL+aAk0tQA", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTitleSyncStatus);
    LIB_FUNCTION("Ro4sI9xgYl4", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyConfig);
    LIB_FUNCTION("7+OR1TU5QOA", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyData);
    LIB_FUNCTION("aXhvf2OmbiE", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyGroupData);
    LIB_FUNCTION("Rkt0bVyaa4Y", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyIcon);
    LIB_FUNCTION("nXr5Rho8Bqk", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyTitleData);
    LIB_FUNCTION("eV1rtLr+eys", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetTrophyTitleIds);
    LIB_FUNCTION("SsGLKTfWfm0", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetUserFileInfo);
    LIB_FUNCTION("XqLLsvl48kA", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemGetUserFileStatus);
    LIB_FUNCTION("-qjm2fFE64M", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemIsServerAvailable);
    LIB_FUNCTION("50BvYYzPTsY", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemNetSyncTitle);
    LIB_FUNCTION("yDJ-r-8f4S4", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemNetSyncTitles);
    LIB_FUNCTION("mWtsnHY8JZg", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemOpenStorage);
    LIB_FUNCTION("tAxnXpzDgFw", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemPerformRecovery);
    LIB_FUNCTION("tV18n8OcheI", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophySystemRemoveAll);
    LIB_FUNCTION("kV4DP0OTMNo", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemRemoveTitleData);
    LIB_FUNCTION("lZSZoN8BstI", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemRemoveUserData);
    LIB_FUNCTION("nytN-3-pdvI", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemSetDbgParam);
    LIB_FUNCTION("JsRnDKRzvRw", "libSceNpTrophy", 1, "libSceNpTrophy",
                 sceNpTrophySystemSetDbgParamInt);
    LIB_FUNCTION("28xmRUFao68", "libSceNpTrophy", 1, "libSceNpTrophy", sceNpTrophyUnlockTrophy);
    LIB_FUNCTION("FJZW2oHUHFk", "libSceNpTrophy", 1, "libSceNpTrophy", Func_149656DA81D41C59);
    LIB_FUNCTION("n4AHGHb-pfY", "libSceNpTrophy", 1, "libSceNpTrophy", Func_9F80071876FFA5F6);
    LIB_FUNCTION("+O9vU1CpGZA", "libSceNpTrophy", 1, "libSceNpTrophy", Func_F8EF6F5350A91990);
    LIB_FUNCTION("+not13BEdVI", "libSceNpTrophy", 1, "libSceNpTrophy", Func_FA7A2DD770447552);
};

} // namespace Libraries::Np::NpTrophy
