#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

// PARCHE LOCAL - multijugador local (hasta 4 jugadores).
//
// Cuantos usuarios de la consola se dan por buenos (1..4). Esta definida en
// xam_user.cpp, que es donde vive el ajuste  local_player_count, y la usa
// tambien xam_input.cpp para XamUserGetDeviceContext. Se declara aqui -el
// encabezado que ya compartian los dos- a proposito: dos copias del limite
// acabarian desincronizandose.
uint32_t LocalPlayerCount();

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                       \
  void Register##n##Exports(rex::runtime::ExportResolver* export_resolver, \
                            system::KernelState* kernel_state);
#include "module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace rex
