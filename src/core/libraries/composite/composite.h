// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Composite {
inline void* sce_composite_color_target_addr = nullptr;
void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Composite