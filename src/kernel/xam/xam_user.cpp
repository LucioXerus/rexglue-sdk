/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");

// PARCHE LOCAL - privilegios de Xbox Live
//
// Apagado por defecto: encendido cambia lo que el juego cree poder hacer, y eso
// merece ser una decision y no una sorpresa.
//
// kHotReload y no kRequiresRestart porque XamUserCheckPrivilege lo lee en cada
// llamada. Se puede encender desde F4 con el juego abierto; basta con salir del
// menu que dio el aviso y volver a entrar.
//
// El texto va en ingles porque es lo que sale en la ventana de F4, que es del
// SDK y esta entera en ingles.
REXCVAR_DEFINE_BOOL(grant_user_privileges, false, "Kernel",
                    "Tell the game it has every Xbox Live privilege. Off by default, which "
                    "makes the game refuse to open its multiplayer menus. Turning it on only "
                    "opens the door: system link also needs the XNet layer, which is only "
                    "half implemented here.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// PARCHE LOCAL - multijugador local (hasta 4 jugadores).
//
// Cuantos jugadores locales existen. Es lo que separa "el juego ve un solo
// usuario, como siempre" de "el juego ve dos, tres o cuatro".
//
// POR QUE HACIA FALTA
// -------------------
// La consola tiene cuatro ranuras de usuario (0..3) y la capa de entrada de
// este SDK ya reparte los mandos entre ellas -rex::input::kMaxGuestUsers-, asi
// que el bloqueo no estaba ahi. Estaba en esta capa: todas las XamUser* daban
// por bueno el usuario 0 y contestaban X_E_NO_SUCH_USER a cualquier otro.
//
// El juego se lo cree al pie de la letra. En el codigo ya recompilado
// -app/generated/default/nfsmw_recomp.86.cpp, sub_82849310- hay este bucle,
// que es la traduccion literal de cuatro instrucciones por vuelta:
//
//     for (i = 0; i < 4; i++)
//       if (XamUserGetSigninState(i)) count++;
//
// Con el SDK de fabrica eso solo puede devolver 1. Y lo primero que hace la
// pantalla de "selecciona jugador 2" es preguntar. De ahi el muro.
//
// POR QUE 1 Y NO 4 POR DEFECTO
// ----------------------------
// Con 4 por defecto, cualquier partida guardada de un jugador arrancaria con
// la consola creyendo que hay cuatro sesiones abiertas. El valor 1 reproduce
// el comportamiento de siempre, bit a bit, y a partir de ahi es cosa del
// jugador subir el numero.
//
// QUE NO HACE
// -----------
// Abre la puerta de la capa de usuarios. No inventa los menus, los textos ni
// las vistas que la version de 360 no trae: los datos del juego solo conocen
// TWO_PLAYER (ver docs/diario/multijugador-local.md). Con 2 ya deberia
// comportarse como el splitscreen original; 3 y 4 dependen de que el motor
// acepte ranuras que sus datos no declaran.
//
// kHotReload como sus vecinos: se lee en cada llamada, asi que se puede
// cambiar desde F4 sin cerrar el juego.
REXCVAR_DEFINE_UINT32(local_player_count, 1, "Kernel",
                      "How many local players the console reports as signed in (1-4). 1 keeps "
                      "the original behaviour and nothing changes. Set it to 2 for the "
                      "original two-player split screen, or 3/4 to let the game see the "
                      "extra slots. Hot-reloadable.")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// PARCHE LOCAL - multijugador local.
//
// Cuantos usuarios de la consola se dan por buenos. Se lee en CADA llamada,
// no se cachea en una variable global, para que el ajuste de F4 surta efecto
// sin reiniciar.
//
// Sin static, y declarada en rex/kernel/xam/private.h, porque la usa tambien
// xam_input.cpp (XamUserGetDeviceContext).
uint32_t LocalPlayerCount() {
  uint32_t count = REXCVAR_GET(local_player_count);
  if (count < 1) {
    count = 1;
  }
  if (count > kMaxLocalUsers) {
    count = kMaxLocalUsers;
  }
  return count;
}

// Devuelve el perfil de esa ranura si la ranura esta activa, y nullptr si no.
// nullptr es exactamente lo que los XamUser* de abajo traducen a
// X_E_NO_SUCH_USER, asi que la condicion se escribe una sola vez.
static UserProfile* ActiveUserProfile(uint32_t user_index) {
  if (user_index >= LocalPlayerCount()) {
    return nullptr;
  }
  return REX_KERNEL_STATE()->user_profile(user_index);
}

// La mascara de "que usuario?" que usan casi todas las XamUser*: 0xFF quiere
// decir "cualquiera". En ese caso el usuario 0 es la respuesta de siempre.
static bool IsAnyUser(uint32_t user_index) { return (user_index & 0xFFu) == 0xFFu; }


i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  assert_true(type_mask == 1 || type_mask == 2 || type_mask == 3 || type_mask == 4 ||
              type_mask == 7);
  if (!xuid_ptr) {
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index >= kMaxLocalUsers) {
    result = X_E_INVALIDARG;
  } else {
    // PARCHE LOCAL - multijugador local: esto era  if (user_index == 0)  y
    // cualquier otra ranura se quedaba con X_E_NO_SUCH_USER.
    UserProfile* user_profile = ActiveUserProfile(user_index);
    if (user_profile != nullptr) {
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    }
  }
  *xuid_ptr = xuid;
  return result;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
  //
  // Aqui estaba el muro. Antes:
  //
  //     uint32_t signin_state = 0;
  //     if (user_index < 4) {
  //       if (user_index == 0) { ...unico perfil... }
  //     }
  //     return signin_state;
  //
  // O sea: las ranuras 1, 2 y 3 contestaban 0 -"aqui no hay nadie"- pasara lo
  // que pasara. Y esta es justo la funcion sobre la que el juego cuenta
  // jugadores (sub_82849310 en el codigo recompilado), asi que la cuenta le
  // salia 1 por mucho que hubiera cuatro mandos enchufados.
  //
  // Con local_player_count = 1 el resultado es identico al de antes: solo la
  // ranura 0 tiene perfil activo.
  UserProfile* user_profile = ActiveUserProfile(user_index);

  // PARCHE LOCAL - diagnostico de multijugador local (solo lectura).
  //
  // Lo primero que se imprime es el recuento efectivo -para que el log diga
  // con que se esta jugando-, y luego una linea por ranura sondeada. Responde
  // a la pregunta que decide donde esta el siguiente trabajo: "el juego
  // pregunta por el jugador 2, o se queda en el 0?". No cambia el valor que se
  // devuelve.
  {
    static bool seen[8] = {};
    static bool reported_count = false;
    if (!reported_count) {
      reported_count = true;
      REXKRNL_WARN("[LOCALMP] local_player_count={}; usuarios con sesion iniciada={}",
                   LocalPlayerCount(), LocalPlayerCount());
    }
    uint32_t u = user_index & 0xFFu;
    if (u < 8 && !seen[u]) {
      seen[u] = true;
      REXKRNL_WARN("[LOCALMP] XamUserGetSigninState user_index={} -> {}", u,
                   user_profile != nullptr ? user_profile->signin_state() : 0u);
    }
  }

  return user_profile != nullptr ? user_profile->signin_state() : 0;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;  // maybe zero?
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
  // Antes:  if (user_index) return X_E_NO_SUCH_USER;
  UserProfile* user_profile = ActiveUserProfile(user_index);
  if (user_profile == nullptr) {
    return X_E_NO_SUCH_USER;
  }

  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  rex::string::copy_truncating(info->name, user_profile->name(), rex::countof(info->name));
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (user_index >= kMaxLocalUsers) {
    return X_E_INVALIDARG;
  }

  // PARCHE LOCAL - multijugador local: antes  if (user_index) return
  // X_E_NO_SUCH_USER;  El jugador 2 pide su nombre al entrar en la partida.
  UserProfile* user_profile = ActiveUserProfile(user_index);
  if (user_profile == nullptr) {
    return X_E_NO_SUCH_USER;
  }

  const auto& user_name = user_profile->name();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= kMaxLocalUsers) {
    return X_E_INVALIDARG;
  }

  // PARCHE LOCAL - multijugador local: antes  if (user_index) return
  // X_E_NO_SUCH_USER;
  UserProfile* user_profile = ActiveUserProfile(user_index);
  if (user_profile == nullptr) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  auto user_name = rex::string::to_utf16(user_profile->name());
  rex::string::copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  if (!xuid_count) {
    assert_null(xuids);
  } else {
    assert_true(xuid_count == 1);
    assert_not_null(xuids);
    // TODO(gibbed): allow proper lookup of arbitrary XUIDs
    const auto& user_profile = REX_KERNEL_STATE()->user_profile();
    assert_true(static_cast<uint64_t>(xuids[0]) == user_profile->xuid());
    // TODO(gibbed): we assert here, but in case a title passes xuid_count > 1
    // until it's implemented for release builds...
    xuid_count = 1;
  }
  assert_zero(unk);  // probably flags

  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
  // Antes:  if (!xuids && user_index) -> X_ERROR_NO_SUCH_USER, o sea que leer
  // los ajustes del jugador 2 era imposible y la pantalla de seleccion de
  // coche del jugador 2 se quedaba sin datos del perfil.
  //
  // Con xuids la llamada pregunta por un perfil concreto por su XUID, y ese
  // camino sigue siendo el del jugador 1: no hay indice de XUIDs ajenos.
  UserProfile* user_profile = REX_KERNEL_STATE()->user_profile();
  if (!xuids) {
    user_profile = ActiveUserProfile(user_index);
    if (user_profile == nullptr) {
      if (overlapped) {
        REX_KERNEL_STATE()->CompleteOverlappedImmediate(
            REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
        return X_ERROR_IO_PENDING;
      }
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    auto setting = user_profile->GetSetting(setting_id);
    if (!setting) {
      any_missing = true;
      REXKRNL_ERROR(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_INVALID_PARAMETER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
    if (xuids) {
      out_setting->xuid = user_profile->xuid();
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    if (setting && setting->is_set) {
      setting->Append(&out_setting->data, &out_stream);
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
  // Antes:  if (user_index) -> X_ERROR_NO_SUCH_USER, asi que guardar los
  // ajustes del jugador 2 tambien fallaba.
  UserProfile* user_profile = ActiveUserProfile(user_index);
  if (user_profile == nullptr) {
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        uint8_t* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: Unimplemented data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= kMaxLocalUsers) {
      return X_ERROR_INVALID_PARAMETER;
    }

    // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
    // Antes:  if (user_index) return X_ERROR_NO_SUCH_USER;  Con eso el jugador
    // 2 salia del menu multijugador con el cartel de "no tienes privilegios",
    // porque ni siquiera se llegaba a mirar grant_user_privileges.
    if (ActiveUserProfile(user_index) == nullptr) {
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // PARCHE LOCAL - privilegios de Xbox Live
  //
  // Aqui decia esto, y hacia exactamente lo que dice:
  //
  //     // If we deny everything, games should hopefully not try to do stuff.
  //     *out_value = 0;
  //
  // Deniega todos los privilegios, siempre, sea cual sea el que pregunten. Es
  // de Xenia y para un emulador sin Xbox Live se entiende: si el juego se cree
  // sin permisos ni lo intenta, y no se cuelga contra servidores apagados.
  //
  // En Most Wanted el efecto es el cartel "Los privilegios que tienes en Xbox
  // Live no te permiten acceder a esta funcion" nada mas tocar el multijugador.
  // Que ademas se contradice con el resto del SDK: XamUserIsOnlineEnabled
  // devuelve 1, XamUserGetMembershipTier devuelve 6 -que es Gold- y el perfil
  // dice signin_state 1 y type local|online. El unico que decia que no era este.
  //
  // OJO CON LO QUE ESTO NO HACE. Abre la puerta del menu y nada mas. El System
  // Link de detras necesita la capa XNet, y en este SDK faltan XNetCreateKey,
  // XNetRegisterKey, XNetUnregisterKey, XNetConnect y XNetGetConnectStatus,
  // ademas de que los manejadores de sesion de xgi_app.cpp devuelven exito sin
  // hacer nada. Esto sirve para ver donde esta el siguiente muro, no para
  // jugar en red.
  *out_value = REXCVAR_GET(grant_user_privileges) ? 1 : 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  // PARCHE LOCAL - multijugador local: antes  if (user_index).
  if (ActiveUserProfile(user_index) == nullptr) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  // PARCHE LOCAL - multijugador local: antes  if (user_index).
  if (ActiveUserProfile(user_index) == nullptr) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  return 1;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= kMaxLocalUsers) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // PARCHE LOCAL - multijugador local: antes  if (user_index) return
  // X_ERROR_NO_SUCH_USER.  La membresia es la misma para todos: Gold.
  if (ActiveUserProfile(user_index) == nullptr) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= kMaxLocalUsers) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    // PARCHE LOCAL - multijugador local (hasta 4 jugadores).
    // Antes:  if (user_index == 0) { ... } else { X_ERROR_NO_SUCH_USER }
    UserProfile* user_profile = ActiveUserProfile(user_index);
    if (user_profile == nullptr) {
      result = X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    } else if (user_profile->signin_state() == 0) {
      result = X_ERROR_NOT_LOGGED_ON;
    } else {
      // No friends!
      are_friends = 0;
      result = X_ERROR_SUCCESS;
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u32 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  // ACHIEVED | ACHIEVED_ONLINE flags the game checks to consider an achievement earned.
  constexpr uint32_t kAchievedFlags = 0x00030000;

  auto fill_unlock = [](XStaticAchievementEnumerator::AchievementDetails& item, uint32_t id,
                        const rex::system::KernelState* ks) {
    uint64_t ft = ks->GetAchievementUnlockTime(id);
    if (ft) {
      item.flags |= kAchievedFlags;
      item.unlock_time.unk_0 = static_cast<uint32_t>(ft & 0xFFFF'FFFF);
      item.unlock_time.unk_4 = static_cast<uint32_t>(ft >> 32);
    }
  };

  const auto* ks = REX_KERNEL_STATE();

  // Prefer the runtime store (populated from TOML or XDBF at boot) so that
  // dev-edited labels/descriptions are visible to the game's own queries.
  const auto store = ks->loaded_achievements();
  if (!store.empty()) {
    for (const auto& info : store) {
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          info.id,
          rex::string::to_utf16(info.label),
          rex::string::to_utf16(info.description),
          rex::string::to_utf16(info.unachieved_description),
          info.image_id,
          info.gamerscore,
          {0, 0},
          info.flags};
      fill_unlock(item, info.id, ks);
      e->AppendItem(item);
    }
  } else {
    const util::XdbfGameData db = ks->title_xdbf();
    if (db.is_valid()) {
      const XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
      for (const util::XdbfAchievementTableEntry& entry : db.GetAchievements()) {
        auto item = XStaticAchievementEnumerator::AchievementDetails{
            entry.id,
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
            entry.image_id,
            entry.gamerscore,
            {0, 0},
            entry.flags};
        fill_unlock(item, entry.id, ks);
        e->AppendItem(item);
      }
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateStatsEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
