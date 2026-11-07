// bioshock2mp - ReXGlue recomp of BioShock 2's MULTIPLAYER executable (Bioshock2MP_Xenon.xex), offline only.
// Purpose: logging/research (spawn chain, MP classes, loadouts) to bring MP features into the BS2 campaign recomp.
// No Xbox Live / online services: the SDK's XSession/XNet calls are local stubs.
#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <filesystem>
#include <cstdio>
#include <memory>
void Bs2InstallCrashTrace();   // crash_trace.cpp

class Bioshock2MPApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bioshock2MPApp>(new Bioshock2MPApp(ctx, "bioshock2mp", PPCImageConfig));
  }
  // The disc's Default.xex is the SP/MP launcher; boot the MP executable directly.
  void OnLoadXexImage(std::string& xex_image) override { xex_image = "game:\\Bioshock2MP_Xenon.xex"; }
  void OnPostLoadXexImage() override {
    Bs2InstallCrashTrace();
    auto out = std::filesystem::current_path() / "logs" / "bioshock2mp_image.bin";
    if (std::filesystem::exists(out)) return;
    std::filesystem::create_directories(out.parent_path());
    const uint8_t* p = runtime()->memory()->TranslateVirtual(PPCImageConfig.image_base);
    if (FILE* fp = std::fopen(out.string().c_str(), "wb")) { std::fwrite(p, 1, PPCImageConfig.image_size, fp); std::fclose(fp); }
  }
};
