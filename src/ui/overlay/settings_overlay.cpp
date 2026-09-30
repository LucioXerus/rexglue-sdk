/**
 * @file        ui/overlay/settings_overlay.cpp
 *
 * @brief       Settings overlay implementation. See settings_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/cvar.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <rex/logging.h>  // PARCHE LOCAL - para dejar constancia del restaurado
#include <rex/ui/keybinds.h>
#include <imgui.h>

#include <cstdlib>  // PARCHE LOCAL - std::_Exit, para el boton de reiniciar
#include <utility>  // PARCHE LOCAL - std::pair, que llegaba de rebote por <map>

// PARCHE LOCAL - para relanzar el juego desde el boton de reiniciar.
//
// Las dos guardas van como en src/ui/windowed_app_main_sdl.cpp, que ya incluye
// windows.h de esta manera. NOMINMAX importa aqui de verdad: sin el, windows.h
// define min y max como macros y este fichero usa std::min y std::max.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rex::ui {

SettingsDialog::SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {}

SettingsDialog::~SettingsDialog() {}

static const char* LifecycleBadge(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return " [live]";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return " [restart]";
    case rex::cvar::Lifecycle::kInitOnly:
      return " [init-only]";
  }
  return "";
}

static ImVec4 LifecycleColor(rex::cvar::Lifecycle lc, const SettingsStyle& style) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return style.lifecycle_live;
    case rex::cvar::Lifecycle::kRequiresRestart:
      return style.lifecycle_restart;
    case rex::cvar::Lifecycle::kInitOnly:
      return style.lifecycle_init_only;
  }
  return style.lifecycle_unknown;
}

static rex::ui::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::ui::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
  }
  switch (key) {
    case ImGuiKey_Space:
      return VK::kSpace;
    case ImGuiKey_Enter:
      return VK::kReturn;
    case ImGuiKey_Escape:
      return VK::kEscape;
    case ImGuiKey_Tab:
      return VK::kTab;
    case ImGuiKey_Backspace:
      return VK::kBack;
    case ImGuiKey_Delete:
      return VK::kDelete;
    case ImGuiKey_Insert:
      return VK::kInsert;
    case ImGuiKey_Home:
      return VK::kHome;
    case ImGuiKey_End:
      return VK::kEnd;
    case ImGuiKey_PageUp:
      return VK::kPrior;
    case ImGuiKey_PageDown:
      return VK::kNext;
    case ImGuiKey_LeftArrow:
      return VK::kLeft;
    case ImGuiKey_RightArrow:
      return VK::kRight;
    case ImGuiKey_UpArrow:
      return VK::kUp;
    case ImGuiKey_DownArrow:
      return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return VK::kShift;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return VK::kControl;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return VK::kMenu;
    case ImGuiKey_CapsLock:
      return VK::kCapital;
    case ImGuiKey_NumLock:
      return VK::kNumLock;
    case ImGuiKey_ScrollLock:
      return VK::kScroll;
    case ImGuiKey_PrintScreen:
      return VK::kSnapshot;
    case ImGuiKey_Pause:
      return VK::kPause;
    case ImGuiKey_GraveAccent:
      return VK::kOem3;
    case ImGuiKey_Minus:
      return VK::kOemMinus;
    case ImGuiKey_Equal:
      return VK::kOemPlus;
    case ImGuiKey_LeftBracket:
      return VK::kOem4;
    case ImGuiKey_RightBracket:
      return VK::kOem6;
    case ImGuiKey_Backslash:
      return VK::kOem5;
    case ImGuiKey_Semicolon:
      return VK::kOem1;
    case ImGuiKey_Apostrophe:
      return VK::kOem7;
    case ImGuiKey_Comma:
      return VK::kOemComma;
    case ImGuiKey_Period:
      return VK::kOemPeriod;
    case ImGuiKey_Slash:
      return VK::kOem2;
    case ImGuiKey_KeypadDecimal:
      return VK::kDecimal;
    case ImGuiKey_KeypadDivide:
      return VK::kDivide;
    case ImGuiKey_KeypadMultiply:
      return VK::kMultiply;
    case ImGuiKey_KeypadSubtract:
      return VK::kSubtract;
    case ImGuiKey_KeypadAdd:
      return VK::kAdd;
    case ImGuiKey_KeypadEnter:
      return VK::kReturn;
    default:
      return VK::kNone;
  }
}

void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  auto& registry = rex::cvar::GetRegistry();

  // PARCHE LOCAL - foto de la configuracion de partida
  //
  // Es a lo que vuelve el boton "Restore defaults", y no a los valores de
  // fabrica del SDK. La diferencia importa: el nfsmw.toml deja puestas cosas
  // -el motor de video, el filtrado- que son la configuracion buena de este
  // juego. Restaurar a los valores del SDK dejaria una que nunca se ha usado,
  // y encima peor.
  //
  // Se toma la primera vez que se dibuja esta ventana. En la practica eso es
  // el arranque, porque nada de aqui cambia solo. Si alguien hubiera tocado
  // algo por la consola antes de abrir F4, esa seria la foto; el aviso del
  // boton lo dice para que no sorprenda.
  //
  // Los cvars del complemento de GPU se registran al cargar su DLL, que es
  // mucho antes de que nadie pulse F4, asi que entran en la foto igual.
  static std::vector<std::pair<std::string, std::string>> configuracion_de_partida;
  static bool foto_tomada = false;
  if (!foto_tomada) {
    foto_tomada = true;
    configuracion_de_partida.reserve(registry.size());
    for (auto& e : registry) {
      configuracion_de_partida.emplace_back(e.name, e.getter());
    }
    REXLOG_DEBUG("[ajustes] foto de partida: {} valores", configuracion_de_partida.size());
  }

  // Collect sorted unique category paths.
  std::set<std::string> category_set;
  for (auto& entry : registry) {
    category_set.insert(entry.category);
  }

  // Build tree: for each category path like "Input/Keybinds/Controller",
  // also register the parent paths "Input" and "Input/Keybinds" as nodes.
  struct CatNode {
    std::string full_path;
    std::string label;  // leaf segment (e.g. "Controller")
    std::map<std::string, CatNode> children;
    bool has_direct_entries = false;
  };
  std::map<std::string, CatNode> tree;

  for (auto& cat : category_set) {
    std::map<std::string, CatNode>* level = &tree;
    std::string path_so_far;
    size_t start = 0;
    while (start < cat.size()) {
      size_t slash = cat.find('/', start);
      std::string segment =
          (slash == std::string::npos) ? cat.substr(start) : cat.substr(start, slash - start);
      if (!path_so_far.empty())
        path_so_far += "/";
      path_so_far += segment;
      auto& node = (*level)[segment];
      node.full_path = path_so_far;
      node.label = segment;
      if (path_so_far == cat)
        node.has_direct_entries = true;
      level = &node.children;
      start = (slash == std::string::npos) ? cat.size() : slash + 1;
    }
  }

  const std::string search(search_buf_);
  const bool searching = !search.empty();

  ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Settings##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Search bar at the top (full width).
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##search", search_buf_, sizeof(search_buf_));
  ImGui::SameLine(0, 0);
  ImGui::Dummy(ImVec2(0, 0));

  ImGui::Separator();

  const float panel_width = 160.0f;
  ImGui::BeginChild("##cats", ImVec2(panel_width, -30.0f), true);

  // Recursive lambda to draw the category tree.
  std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree;
  draw_tree = [&](const std::map<std::string, CatNode>& nodes, int depth) {
    for (auto& [key, node] : nodes) {
      if (node.children.empty()) {
        // Leaf node - selectable
        bool selected = (selected_category_ == node.full_path);
        if (depth > 0)
          ImGui::Indent(8.0f);
        if (ImGui::Selectable(node.label.c_str(), selected)) {
          selected_category_ = node.full_path;
        }
        if (depth > 0)
          ImGui::Unindent(8.0f);
      } else {
        // Parent node with children - use tree node
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node.has_direct_entries) {
          // Can be selected as well as expanded
          if (selected_category_ == node.full_path)
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(node.label.c_str(), flags);
        if (ImGui::IsItemClicked() && node.has_direct_entries) {
          selected_category_ = node.full_path;
        }
        if (open) {
          draw_tree(node.children, depth + 1);
          ImGui::TreePop();
        }
      }
    }
  };
  // Root node named after the config file
  std::string root_label = config_path_.stem().string();
  ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen;
  if (selected_category_.empty())
    root_flags |= ImGuiTreeNodeFlags_Selected;
  bool root_open = ImGui::TreeNodeEx(root_label.c_str(), root_flags);
  if (ImGui::IsItemClicked()) {
    selected_category_.clear();
  }
  if (root_open) {
    draw_tree(tree, 1);
    ImGui::TreePop();
  }

  ImGui::EndChild();

  ImGui::SameLine();

  // Helper: check if a CVAR's category matches the selected category.
  // Exact match or prefix match (e.g. selecting "Input" shows all "Input/*").
  auto category_matches = [&](const std::string& cat) -> bool {
    if (selected_category_.empty())
      return true;  // Root selected - show all
    if (cat == selected_category_)
      return true;
    if (cat.size() > selected_category_.size() &&
        cat.compare(0, selected_category_.size(), selected_category_) == 0 &&
        cat[selected_category_.size()] == '/') {
      return true;
    }
    return false;
  };

  // Helper: check if a category is a keybind category.
  auto is_keybind_category = [](const std::string& cat) -> bool {
    return cat == "Input/Keybinds" ||
           (cat.size() > 15 && cat.compare(0, 15, "Input/Keybinds/") == 0);
  };

  ImGui::BeginChild("##cvars", ImVec2(0, -30.0f), false);
  for (auto& entry : registry) {
    // Filter by category (unless searching).
    if (!searching) {
      if (!category_matches(entry.category)) {
        continue;
      }
    } else {
      // Search matches name or description (case-insensitive substring).
      std::string name_lower = entry.name;
      std::string search_lower = search;
      auto to_lower = [](std::string& s) {
        for (auto& c : s)
          c = static_cast<char>(std::tolower(c));
      };
      to_lower(name_lower);
      to_lower(search_lower);
      if (name_lower.find(search_lower) == std::string::npos &&
          entry.description.find(search_lower) == std::string::npos) {
        continue;
      }
    }

    bool read_only = (entry.lifecycle == rex::cvar::Lifecycle::kInitOnly);

    ImGui::PushID(entry.name.c_str());

    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = entry.getter();

    // Use description as display label if available, otherwise CVAR name
    const char* display_label =
        (!entry.description.empty()) ? entry.description.c_str() : entry.name.c_str();

    if (is_keybind_category(entry.category)) {
      // Grey out controller keybinds when MnK mode is disabled
      bool mnk_disabled =
          (entry.category == "Input/Keybinds/Controller" && !REXCVAR_QUERY(bool, mnk_mode));
      if (mnk_disabled)
        ImGui::BeginDisabled();

      // Show description as label (e.g. "A button"), not the raw CVAR name
      ImGui::Text("%-20s", entry.description.c_str());
      ImGui::SameLine(240.0f);

      bool is_capturing = (capturing_bind_name_ == entry.name);

      if (is_capturing) {
        ImGui::Button("Press any key...##v", ImVec2(140.0f, 0));

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          capturing_bind_name_.clear();
        } else {
          for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            auto imgui_key = static_cast<ImGuiKey>(k);
            if (imgui_key == ImGuiKey_Escape)
              continue;
            if (ImGui::IsKeyPressed(imgui_key)) {
              auto vk = ImGuiKeyToVirtualKey(imgui_key);
              std::string name = rex::ui::VirtualKeyToString(vk);
              if (!name.empty()) {
                rex::cvar::SetFlagByName(entry.name, name);
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          for (int mb = 0; mb < 3; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              const char* names[] = {"LMB", "RMB", "MMB"};
              rex::cvar::SetFlagByName(entry.name, names[mb]);
              capturing_bind_name_.clear();
              break;
            }
          }
        }
      } else {
        ImGui::SetNextItemWidth(80.0f);
        ImGui::Text("%-10s", current_val.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rebind##v")) {
          capturing_bind_name_ = entry.name;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##v")) {
          rex::cvar::SetFlagByName(entry.name, entry.default_value);
        }
      }

      // Conflict detection
      if (!current_val.empty()) {
        int conflict_count = 0;
        for (auto& other : registry) {
          if (is_keybind_category(other.category) && other.name != entry.name &&
              other.getter() == current_val) {
            conflict_count++;
          }
        }
        if (conflict_count > 0) {
          ImGui::SameLine();
          ImGui::TextColored(imgui_drawer()->style().settings.warning, "(!)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Key '%s' is also bound to %d other action(s)", current_val.c_str(),
                              conflict_count);
          }
        }
      }

      // Skip the generic name + lifecycle badge rendering for keybinds
      if (mnk_disabled)
        ImGui::EndDisabled();
      if (read_only)
        ImGui::EndDisabled();
      ImGui::PopID();
      continue;
    } else {
      // Non-keybind CVARs: colored label on left, value widget on right
      ImGui::TextColored(LifecycleColor(entry.lifecycle, imgui_drawer()->style().settings), "%-20s",
                         entry.name.c_str());
      if (ImGui::IsItemHovered()) {
        const char* lifecycle_label = "";
        switch (entry.lifecycle) {
          case rex::cvar::Lifecycle::kHotReload:
            lifecycle_label = "Live - changes apply immediately";
            break;
          case rex::cvar::Lifecycle::kRequiresRestart:
            lifecycle_label = "Requires restart to take effect";
            break;
          case rex::cvar::Lifecycle::kInitOnly:
            lifecycle_label = "Read-only - set at initialization only";
            break;
        }
        if (!entry.description.empty()) {
          ImGui::SetTooltip("%s\n[%s]", entry.description.c_str(), lifecycle_label);
        } else {
          ImGui::SetTooltip("[%s]", lifecycle_label);
        }
      }
      ImGui::SameLine(240.0f);

      ImGui::SetNextItemWidth(160.0f);
      if (entry.type == rex::cvar::FlagType::Boolean) {
        bool v = rex::string::from_string<bool>(current_val, false);
        if (ImGui::Checkbox("##v", &v)) {
          rex::cvar::SetFlagByName(entry.name, v ? "true" : "false");
        }
      } else if (entry.type == rex::cvar::FlagType::String &&
                 !entry.constraints.allowed_values.empty()) {
        const auto& opts = entry.constraints.allowed_values;
        int cur_idx = 0;
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
          if (opts[i] == current_val) {
            cur_idx = i;
            break;
          }
        }
        if (ImGui::BeginCombo("##v", opts[cur_idx].c_str())) {
          for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
            bool sel = (i == cur_idx);
            if (ImGui::Selectable(opts[i].c_str(), sel)) {
              rex::cvar::SetFlagByName(entry.name, opts[i]);
            }
            if (sel)
              ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
      } else if (entry.type == rex::cvar::FlagType::Int32 ||
                 entry.type == rex::cvar::FlagType::Int64 ||
                 entry.type == rex::cvar::FlagType::Uint32 ||
                 entry.type == rex::cvar::FlagType::Uint64) {
        int v = std::atoi(current_val.c_str());
        int vmin =
            entry.constraints.min.has_value() ? static_cast<int>(*entry.constraints.min) : INT_MIN;
        int vmax =
            entry.constraints.max.has_value() ? static_cast<int>(*entry.constraints.max) : INT_MAX;
        if (ImGui::InputInt("##v", &v)) {
          v = std::clamp(v, vmin, vmax);
          rex::cvar::SetFlagByName(entry.name, std::to_string(v));
        }
      } else if (entry.type == rex::cvar::FlagType::Double) {
        // PARCHE LOCAL - deslizador cuando el ajuste trae minimo y maximo.
        //
        // Con la caja de texto hay que escribir el numero y darle a Enter. Para
        // un ajuste que se busca a tanteo -la velocidad del juego, sin ir mas
        // lejos- eso es incomodo: no puedes arrastrar y ver el efecto. Con
        // limites conocidos, un deslizador es lo suyo; sin ellos no hay por
        // donde deslizar, asi que se queda la caja de siempre.
        //
        // Ctrl+clic sobre el deslizador sigue dejando escribir el valor exacto.
        if (entry.constraints.min.has_value() && entry.constraints.max.has_value()) {
          float v = static_cast<float>(std::atof(current_val.c_str()));
          const float vmin = static_cast<float>(*entry.constraints.min);
          const float vmax = static_cast<float>(*entry.constraints.max);
          // Cuantos decimales enseniar. Un recorrido largo -0 a 200, que es un
          // porcentaje- se lee mejor entero; uno corto -0 a 1- necesita
          // decimales o la barra parece que no hace nada.
          const char* formato = (vmax - vmin >= 10.0f) ? "%.0f" : "%.3f";
          if (ImGui::SliderFloat("##v", &v, vmin, vmax, formato)) {
            v = std::clamp(v, vmin, vmax);
            rex::cvar::SetFlagByName(entry.name, std::to_string(v));
          }
        } else {
          double v = std::atof(current_val.c_str());
          if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
            if (entry.constraints.min)
              v = std::max(v, *entry.constraints.min);
            if (entry.constraints.max)
              v = std::min(v, *entry.constraints.max);
            rex::cvar::SetFlagByName(entry.name, std::to_string(v));
          }
        }
      } else if (entry.type == rex::cvar::FlagType::Command) {
        if (ImGui::Button(std::string(entry.name + "##v").c_str())) {
          entry.command_callback("");
        }
      } else {
        char buf[256];
        rex::string::copy_truncating(buf, current_val, sizeof(buf));
        if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
          rex::cvar::SetFlagByName(entry.name, buf);
        }
      }
    }

    if (read_only)
      ImGui::EndDisabled();

    ImGui::PopID();
  }
  ImGui::EndChild();

  // PARCHE LOCAL - aviso de reinicio pendiente, con boton para reiniciar
  //
  // El SDK ya llevaba la cuenta de los ajustes cambiados que piden reinicio
  // -SetFlagFromSource llama a MarkPendingRestart, y GetPendingRestartFlags los
  // devuelve-, pero no lo ensenaba en ninguna parte. Sin eso, cambiar la API
  // grafica o cualquier otro de esos parecia no hacer nada: pones el valor,
  // vuelves al juego, y todo sigue igual sin una sola pista de por que.
  //
  // Sirve para todos, no solo para el de la API.
  {
    // La API grafica, leida del registro de cvars por nombre.
    //
    // La primera version de esto usaba una variable global compartida con
    // rex_app.cpp, y NO ENLAZABA: rex_app.cpp no se compila dentro del SDK,
    // se instala como fuente y lo compila cada aplicacion, asi que la
    // definicion acaba en el .exe y esta referencia en la DLL. El registro de
    // cvars, en cambio, esta hecho justo para hablar entre modulos.
    //
    // Y desde que se quito "any" de las opciones, el ajuste ES la API: el
    // unico caso en que no coincide es que la elegida no este compilada en
    // esta copia, y entonces el arranque lo deja escrito en el log.
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", rex::cvar::GetFlagByName("gpu_backend").c_str());

    const auto pendientes_reinicio = rex::cvar::GetPendingRestartFlags();
    if (!pendientes_reinicio.empty()) {
      std::string lista;
      for (const auto& n : pendientes_reinicio) {
        if (!lista.empty()) {
          lista += ", ";
        }
        lista += n;
      }
      ImGui::TextColored(imgui_drawer()->style().settings.warning,
                         "Restart needed to apply: %s", lista.c_str());
#if defined(_WIN32)
      // Guarda ANTES de reiniciar, y no es un extra: estos ajustes solo viven
      // en memoria hasta que se guardan. Reiniciar sin guardar volveria con el
      // valor viejo y pareceria que el boton no hace nada.
      if (ImGui::Button("Save and restart")) {
        rex::cvar::SaveConfig(config_path_);

        // Sin std::size a proposito: sale de <iterator>, que este fichero no
        // incluye y que llega de rebote segun la implementacion. Una constante
        // no depende de nada.
        constexpr DWORD kMaxRuta = 1024;
        wchar_t ruta[kMaxRuta];
        const DWORD largo = GetModuleFileNameW(nullptr, ruta, kMaxRuta);
        bool relanzado = false;
        if (largo > 0 && largo < kMaxRuta) {
          // La misma linea de comandos con la que se abrio, para no perder la
          // ISO ni las opciones que le pasa el lanzador. CreateProcessW puede
          // escribir en ese buffer, asi que se le da una copia propia.
          std::wstring linea = GetCommandLineW();
          std::vector<wchar_t> linea_editable(linea.begin(), linea.end());
          linea_editable.push_back(L'\0');

          STARTUPINFOW si{};
          si.cb = sizeof(si);
          PROCESS_INFORMATION pi{};
          if (CreateProcessW(ruta, linea_editable.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            relanzado = true;
          }
        }

        if (relanzado) {
          REXLOG_INFO("[ajustes] reiniciando para aplicar: {}", lista);
          // Misma salida que usa la app al cerrar la ventana: vaciar el log y
          // cortar en seco. El desmontaje ordenado puede quedarse colgado en un
          // cerrojo que dejo algun hilo del juego, y por eso el SDK tampoco lo
          // hace ahi.
          rex::FlushLogging();
          std::_Exit(0);
        }
        REXLOG_ERROR("[ajustes] no se pudo relanzar el juego (error {}). Cierralo y abrelo tu.",
                     GetLastError());
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(saves first, then opens again with the same options)");
#else
      ImGui::TextDisabled("Close the game and open it again to apply.");
#endif
    }
  }

  // Bottom bar: Save button.
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
  }
  ImGui::SameLine();

  // PARCHE LOCAL - boton de restaurar la configuracion de partida
  //
  // Va detras de una confirmacion a proposito: esta pegado al de guardar y es
  // destructivo. Un clic de mas no puede costar media tarde de ajustes.
  if (ImGui::Button("Restore defaults")) {
    ImGui::OpenPopup("Restore defaults?##rex_restore");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());

  if (ImGui::BeginPopupModal("Restore defaults?##rex_restore", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Every setting goes back to how it was when the game started,");
    ImGui::TextUnformatted("with your config file already applied.");
    ImGui::Spacing();
    ImGui::BulletText("Read-only settings are left alone: they are fixed when the\n"
                      "game starts, so changing them now would do nothing.");
    ImGui::BulletText("Settings that need a restart do change, but only take\n"
                      "effect the next time you start the game.");
    ImGui::BulletText("%s is NOT written. Press \"Save to config\"\n"
                      "afterwards if you want this to survive a restart.",
                      config_path_.filename().string().c_str());
    ImGui::Separator();

    if (ImGui::Button("Restore", ImVec2(120.0f, 0))) {
      // Se recoge la lista ANTES de tocar nada. SetFlagByName escribe en la
      // entrada del registro -al menos su origen-, y no me apetece estar
      // recorriendo el contenedor mientras se modifica.
      std::vector<std::pair<std::string, std::string>> pendientes;
      for (auto& e : registry) {
        // Solo lectura: fijados al arrancar. Entre ellos esta el motor de
        // video, que es justo el que no queremos perder.
        if (e.lifecycle == rex::cvar::Lifecycle::kInitOnly) {
          continue;
        }
        // Los comandos son botones, no tienen valor que restaurar.
        if (e.type == rex::cvar::FlagType::Command) {
          continue;
        }

        // El valor de la foto de partida. Si el ajuste no estaba -se registro
        // despues de abrir esta ventana la primera vez, que es raro pero
        // posible- se cae al valor de fabrica, que es lo unico que se sabe.
        const std::string* objetivo = nullptr;
        for (auto& [nombre, valor] : configuracion_de_partida) {
          if (nombre == e.name) {
            objetivo = &valor;
            break;
          }
        }
        const std::string& valor_bueno = objetivo ? *objetivo : e.default_value;

        if (e.getter() == valor_bueno) {
          continue;
        }
        pendientes.emplace_back(e.name, valor_bueno);
      }

      int restaurados = 0;
      for (auto& [nombre, valor] : pendientes) {
        if (rex::cvar::SetFlagByName(nombre, valor)) {
          ++restaurados;
        }
      }
      REXLOG_INFO("[ajustes] {} de {} valores devueltos a la configuracion de partida",
                  restaurados, pendientes.size());
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::End();
}

}  // namespace rex::ui
