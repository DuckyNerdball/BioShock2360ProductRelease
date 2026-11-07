// bioshock2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_drawer.h>
#include <filesystem>
#include <cstdio>
#include <memory>
#include "cheat_overlay.h"
#include "weapon_wheel.h"
void Bs2InstallCrashTrace();   // crash_trace.cpp: symbolized host stack of any fault -> log + logs\crash_*.txt

class Bioshock2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bioshock2App>(new Bioshock2App(ctx, "bioshock2",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // BioShock 2: Default.xex is the front-end launcher; the game is Bioshock2_Xenon.xex.
    void OnLoadXexImage(std::string& xex_image) override { xex_image = "game:\\Bioshock2_Xenon.xex"; }
  void OnPostLoadXexImage() override {
    Bs2InstallCrashTrace();
    auto out = std::filesystem::current_path() / "logs" / "bioshock2_image.bin";
    if (std::filesystem::exists(out)) return;
    const uint8_t* p = runtime()->memory()->TranslateVirtual(PPCImageConfig.image_base);
    if (FILE* fp = std::fopen(out.string().c_str(), "wb")) { std::fwrite(p, 1, PPCImageConfig.image_size, fp); std::fclose(fp); }
  }
  // bridge: F9 cheat panel (bridge_cheats.cpp runs the commands through the engine's own Exec).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    cheats_ = std::make_unique<CheatDialog>(drawer);
    rex::ui::RegisterBind("bind_cheats", "F9", "Toggle BioShock 2 cheat panel", [] { CheatDialog::Toggle(); });
    wheel_ = std::make_unique<WeaponWheelDialog>(drawer);   // second weapon wheel: D-pad Down (hold) or G
    rex::ui::RegisterBind("bind_wheel2", "G", "Second weapon wheel (MP weapons)", [] { WeaponWheelDialog::ToggleKb(); });
  }
  // While the panel is open the game gets no pad/keyboard input (the panel uses it).
  void OnPreLaunchModule() override {
    auto* in = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    auto* d = imgui_drawer();
    if (in && d) in->SetActiveCallbacks([] { return !CheatDialog::Visible() && !WeaponWheelDialog::KbOpen(); }, [] { return !CheatDialog::Visible() && !WeaponWheelDialog::KbOpen(); });
  }
  std::unique_ptr<CheatDialog> cheats_;
  std::unique_ptr<WeaponWheelDialog> wheel_;
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};



