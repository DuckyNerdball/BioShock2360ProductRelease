// bioshock-bridge: second weapon wheel (rough overlay version) for BioShock 2.
// BS2's own wheel is a Scaleform movie (HUDRadial) with fixed slots; foreign (BS1/MP) weapons have no slot there.
// This is the quick version: an ImGui radial drawn over the game. The real version (a second ring inside the
// HUDRadial movie) comes later - see docs.
//   Controller: tap D-pad Left 3 times to open, aim with the right stick, A (or D-pad Left) equips, B cancels.
//   Keyboard:   G opens/closes it; click a slot, or press 1-8.
// Equipping happens on the game thread (bridge_loader.cpp, Bs2WheelRequest).
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

void Bs2WheelRequest(int slot);   // bridge_loader.cpp (game thread picks it up)
struct Bs2WheelPad { std::atomic<bool> held{false}, cancel{false}; std::atomic<int> rx{0}, ry{0}; };
Bs2WheelPad& Bs2WheelPadState();  // bridge_cheats.cpp (filled by the guest pad hook, which also hides the input from the game)

class WeaponWheelDialog : public rex::ui::ImGuiDialog {
 public:
  explicit WeaponWheelDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
  static constexpr int kSlots = 8;
  static const char* SlotName(int i) {
    // MP (Fall of Rapture) first-person weapons: each has an FP mesh in MP ShockGame.U and matching DE_FPArms anims.
    static const char* n[kSlots] = {"Revolver", "Tommy Gun", "Shotgun", "Grenade Launcher",
                                    "Crossbow", "Nail Gun", "Elephant Gun", "Wrench"};
    return n[i];
  }
  static bool Ported(int i) { return i < 7; }   // v1: slots 2-7 are look-only on the revolver's firing logic; Wrench (8) not yet
  static bool KbOpen() { return kb_open_.load(); }
  static void ToggleKb() { kb_open_ = !kb_open_.load(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& pad = Bs2WheelPadState();
    const bool pad_open = pad.held.load(), kb = kb_open_.load();
    const ImVec2 ds = io.DisplaySize, c(ds.x * 0.5f, ds.y * 0.5f);
    const float R = std::min(ds.x, ds.y) * 0.30f;

    // pick: right stick (pad) or mouse direction (keyboard)
    int pick = -1;
    auto slot_from = [&](float dx, float dy) {   // dy screen-down positive; slot 0 at the top, clockwise
      float a = std::atan2(dx, -dy); if (a < 0) a += 6.2831853f;
      return int(std::floor((a + 3.14159265f / kSlots) / (6.2831853f / kSlots))) % kSlots; };
    if (pad_open) {
      float x = pad.rx.load() / 32767.0f, y = pad.ry.load() / 32767.0f;
      if (x * x + y * y > 0.35f) pick = slot_from(x, -y), last_pad_pick_ = pick;
      else pick = last_pad_pick_;
    } else if (kb) {
      float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
      if (dx * dx + dy * dy > (R * 0.25f) * (R * 0.25f)) pick = slot_from(dx, dy);
    }

    // commit
    if (was_pad_open_ && !pad_open && last_pad_pick_ >= 0 && !pad.cancel.exchange(false)) { Fire(last_pad_pick_); }
    if (!pad_open) last_pad_pick_ = -1;
    was_pad_open_ = pad_open;
    if (kb) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pick >= 0) { Fire(pick); kb_open_ = false; }
      for (int i = 0; i < kSlots; ++i) if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_1 + i), false)) { Fire(i); kb_open_ = false; }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) kb_open_ = false;
    }
    if (!pad_open && !kb) return;

    // draw
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddCircleFilled(c, R * 1.18f, IM_COL32(10, 14, 18, 170), 64);
    dl->AddCircle(c, R * 1.18f, IM_COL32(200, 170, 90, 200), 64, 3.0f);
    dl->AddCircle(c, R * 0.42f, IM_COL32(200, 170, 90, 120), 48, 2.0f);
    const float fs = std::max(18.0f, R * 0.075f);
    for (int i = 0; i < kSlots; ++i) {
      float a = i * 6.2831853f / kSlots;
      ImVec2 p(c.x + std::sin(a) * R * 0.82f, c.y - std::cos(a) * R * 0.82f);
      const bool sel = i == pick, ok = Ported(i);
      if (sel) dl->AddCircleFilled(p, R * 0.26f, ok ? IM_COL32(200, 170, 90, 150) : IM_COL32(120, 60, 60, 140), 40);
      dl->AddCircle(p, R * 0.26f, IM_COL32(200, 170, 90, ok ? 220 : 90), 40, 2.0f);
      const char* nm = SlotName(i);
      ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, nm);
      dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), ok ? IM_COL32(255, 240, 200, 255) : IM_COL32(150, 150, 150, 200), nm);
      char k[4]; std::snprintf(k, sizeof k, "%d", i + 1);
      dl->AddText(ImGui::GetFont(), fs * 0.7f, ImVec2(p.x - fs * 0.2f, p.y + ts.y * 0.6f), IM_COL32(200, 170, 90, 160), k);
    }
    const char* title = pick >= 0 ? (Ported(pick) ? SlotName(pick) : "not ported yet") : "MP WEAPONS";
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 1.1f, 1e9f, 0, title);
    dl->AddText(ImGui::GetFont(), fs * 1.1f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 240, 200, 255), title);
  }

 private:
  static void Fire(int slot) { if (slot >= 0 && slot < kSlots && Ported(slot)) Bs2WheelRequest(slot); }
  static inline std::atomic<bool> kb_open_{false};
  bool was_pad_open_ = false;
  int last_pad_pick_ = -1;
};
