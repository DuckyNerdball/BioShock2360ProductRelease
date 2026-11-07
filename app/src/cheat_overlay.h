// bioshock-bridge: F9 cheat panel for BioShock 2 (port of BS1's testing_overlay.h through Rosetta).
// Console buttons go through the engine's own Exec (bridge_cheats.cpp); Give buttons go through the
// native inventory path (bridge_give.cpp). Class names for BS2 items are discovered with the name
// scanner at the bottom - click a hit to give it.
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

void Bs2QueueCommand(std::string cmd);
std::vector<std::string> Bs2RecentResults();
bool Bs2EngineReady();
void Bs2QueueGive(std::string spec);
std::deque<std::string> Bs2GiveLog();
bool Bs2GiveReady();
void Bs2ScanNames(std::string needle);
std::vector<std::string> Bs2GiveAllWeapons();
std::vector<std::string> Bs2NameHits();
bool Bs2PadPoll(uint16_t* buttons, int16_t* lx, int16_t* ly);   // bridge_cheats.cpp (XInput)
void Bs2SetInfinite(int what, bool on); bool Bs2GetInfinite(int what); void Bs2AddAdam(int n);   // bridge_give.cpp
std::deque<std::string> Bs2EventFeed(); void Bs2FeedLog(bool on); bool Bs2FeedLogOn();            // bridge_probe.cpp

class CheatDialog : public rex::ui::ImGuiDialog {
 public:
  explicit CheatDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
  static bool Visible() { return visible_.load(); }
  static void Toggle() { visible_ = !visible_.load(); just_opened_ = visible_.load(); }

 protected:
  // Feed controller state to ImGui navigation; detect the L3+R3 toggle. Same mapping as BS1.
  void PollPad(ImGuiIO& io) {
    uint16_t b = 0; int16_t lx = 0, ly = 0;
    bool have = Bs2PadPoll(&b, &lx, &ly);
    if (!have) b = 0, lx = ly = 0;
    constexpr uint16_t kL3 = 0x0040, kR3 = 0x0080;
    bool combo = (b & kL3) && (b & kR3);
    if (combo && !combo_prev_) Toggle();
    combo_prev_ = combo;
    if (!visible_) { b = 0; lx = ly = 0; }
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map { uint16_t bit; ImGuiKey key; };
    static const Map kMap[] = {
        {0x0001, ImGuiKey_GamepadDpadUp}, {0x0002, ImGuiKey_GamepadDpadDown},
        {0x0004, ImGuiKey_GamepadDpadLeft}, {0x0008, ImGuiKey_GamepadDpadRight},
        {0x0010, ImGuiKey_GamepadStart}, {0x0020, ImGuiKey_GamepadBack},
        {0x0100, ImGuiKey_GamepadL1}, {0x0200, ImGuiKey_GamepadR1},
        {0x1000, ImGuiKey_GamepadFaceDown}, {0x2000, ImGuiKey_GamepadFaceRight},
        {0x4000, ImGuiKey_GamepadFaceLeft}, {0x8000, ImGuiKey_GamepadFaceUp}};
    for (const auto& m : kMap) {
      bool down = (b & m.bit) != 0;
      if (down != ((pad_prev_ & m.bit) != 0)) io.AddKeyEvent(m.key, down);
    }
    pad_prev_ = b;
    auto axis = [&](ImGuiKey k, float v) { float a = v > 0.3f ? std::min(1.0f, (v - 0.3f) / 0.7f) : 0.0f; io.AddKeyAnalogEvent(k, a > 0.0f, a); };
    float fx = lx / 32767.0f, fy = ly / 32767.0f;
    axis(ImGuiKey_GamepadLStickRight, fx); axis(ImGuiKey_GamepadLStickLeft, -fx);
    axis(ImGuiKey_GamepadLStickUp, fy);    axis(ImGuiKey_GamepadLStickDown, -fy);
  }

  void OnDraw(ImGuiIO& io) override {
    PollPad(io);
    if (!visible_) { io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad); return; }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (just_opened_) { ImGui::SetNextWindowFocus(); just_opened_ = false; }
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("BioShock 2 - Cheats (F9)", nullptr, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    ImGui::SetWindowFontScale(font_scale_);
    if (ImGui::SmallButton("A-")) font_scale_ = std::max(0.8f, font_scale_ - 0.1f);
    ImGui::SameLine();
    if (ImGui::SmallButton("A+")) font_scale_ = std::min(2.5f, font_scale_ + 0.1f);
    ImGui::SameLine();
    ImGui::TextDisabled("console %s | give %s", Bs2EngineReady() ? "ready" : "not captured (open the pause menu once)",
                        Bs2GiveReady() ? "ready" : "not calibrated (pick up any item once)");

    ImGui::SeparatorText("Player (console)");
    if (ImGui::Checkbox("God mode", &god_)) Bs2QueueCommand("god");
    ImGui::SameLine();
    if (ImGui::Checkbox("Noclip", &noclip_)) Bs2QueueCommand(noclip_ ? "ghost" : "walk");
    ImGui::SameLine();
    if (ImGui::Checkbox("Fly", &fly_)) Bs2QueueCommand(fly_ ? "fly" : "walk");
    ImGui::SameLine();
    if (ImGui::Button("Teleport to crosshair")) Bs2QueueCommand("teleport");
    // BS2's own cheat functions (found by the name scanner; GiveAllPlasmids confirmed handled 2026-11-04)
    if (ImGui::Button("Give all (GiveAll)")) Bs2QueueCommand("GiveAll");
    ImGui::SameLine();
    if (ImGui::Button("All plasmids")) Bs2QueueCommand("GiveAllPlasmids");
    ImGui::SameLine();
    if (ImGui::Button("Health +100")) Bs2QueueCommand("GiveHealth 100");   // GiveFullHealth is not handled on this build
    ImGui::SameLine();
    if (ImGui::Button("EVE +100")) Bs2QueueCommand("GiveBioAmmo 100");
    ImGui::SameLine();
    if (ImGui::Button("Loadout")) Bs2QueueCommand("GiveLoadout");
    ImGui::SameLine();
    if (ImGui::Button("All weapons")) { allw_ = Bs2GiveAllWeapons(); }
    ImGui::SameLine();
    if (ImGui::Button("BS1 Pistol")) {   // loads BS1's class from BS1Game.u, then gives it. The holdable list holds 8: when it is
      Bs2QueueGive("BS1Game.Pistol");    // full GiveWeapon silently does nothing (use the armed autotest at level start instead;
      Bs2QueueCommand("GiveWeapon BS1Game.Pistol");   // RemoveAllWeapons mid-equip crashed the console parser, crash_20261105_220229)
    }
    ImGui::SameLine();
    if (ImGui::Button("BS1 package test")) {   // every route we have at a BioShock 1 package dropped in as BS1Game.u
      for (const char* g : {"BS1Game.Pistol", "BS1Game.Pistol_Bullet x6", "BS1Game.PistolPickupItem", "BS1Game.Wrench"}) Bs2QueueGive(g);
      for (const char* c : {"GiveWeapon BS1Game.Pistol", "GiveItem BS1Game.Pistol_Bullet"}) Bs2QueueCommand(c);
    }
    if (!allw_.empty()) { ImGui::SameLine(); ImGui::Text("(%d queued, see give results)", (int)allw_.size()); }
    if (ImGui::Button("Final mod")) Bs2QueueCommand("GiveFinalMod");
    ImGui::SameLine();
    if (ImGui::Button("Mod A")) Bs2QueueCommand("GiveModA");
    ImGui::SameLine();
    if (ImGui::Button("Mod B")) Bs2QueueCommand("GiveModB");
    ImGui::SameLine();
    if (ImGui::Button("Kill all")) Bs2QueueCommand("killall");
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##gw", gw_, sizeof(gw_));
    ImGui::SameLine();
    if (ImGui::Button("GiveWeapon") && gw_[0]) Bs2QueueCommand(std::string("GiveWeapon ") + gw_);
    ImGui::SameLine();
    if (ImGui::Button("GiveItem") && gw_[0]) Bs2QueueCommand(std::string("GiveItem ") + gw_);
    ImGui::SameLine();
    if (ImGui::Button("GiveItemClass") && gw_[0]) Bs2QueueCommand(std::string("GiveItemClass ") + gw_);
    ImGui::SameLine();
    if (ImGui::Button("Suicide")) Bs2QueueCommand("suicide");
    ImGui::SliderFloat("Game speed", &slomo_, 0.1f, 3.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Apply")) Bs2QueueCommand("slomo " + std::to_string(slomo_));

    ImGui::SeparatorText("Native (fields written every frame; needs the player captured)");
    bool ih = Bs2GetInfinite(0), ie = Bs2GetInfinite(1), ia = Bs2GetInfinite(2);
    if (ImGui::Checkbox("Infinite health", &ih)) Bs2SetInfinite(0, ih);
    ImGui::SameLine();
    if (ImGui::Checkbox("Infinite EVE", &ie)) Bs2SetInfinite(1, ie);
    ImGui::SameLine();
    if (ImGui::Checkbox("Infinite ammo (reserve topped up)", &ia)) Bs2SetInfinite(2, ia);
    ImGui::SameLine();
    if (ImGui::Button("ADAM +100")) Bs2AddAdam(100);

    ImGui::SeparatorText("Give (native inventory)");
    ImGui::TextDisabled("Health / EVE are inventory items in this engine; ammo x99 is effectively infinite.");
    // BS1 names as the first guess (same engine family); the scanner below finds BS2's real ones.
    // confirmed 2026-11-04: both load from ShockDesignerClasses (same item class names as BS1)
    if (ImGui::Button("First aid +5")) Bs2QueueGive("ShockDesignerClasses.MedHypo x5");
    ImGui::SameLine();
    if (ImGui::Button("EVE hypos +5")) Bs2QueueGive("ShockDesignerClasses.BioAmmoHypo x5");
    ImGui::SameLine();
    if (ImGui::Button("Rivets x99")) Bs2QueueGive("Rivet_Ammo x99");
    ImGui::SameLine();
    if (ImGui::Button("Trap rivets x99")) Bs2QueueGive("Rivet_TrapAmmo x99");
    ImGui::SameLine();
    if (ImGui::Button("Hack darts x99")) Bs2QueueGive("Hacking_Ammo x99");
    if (ImGui::Button("Drill fuel x99")) Bs2QueueGive("Drill_Ammo x99");
    ImGui::SameLine();
    if (ImGui::Button("Money +500")) Bs2QueueGive("Credits x500");   // item class seen in the game's own pickup record
    ImGui::SameLine();
    if (ImGui::Button("Shotgun shells x99")) Bs2QueueGive("Shotgun_SolidSlug x99");
    ImGui::SameLine();
    if (ImGui::Button("MG rounds x99")) Bs2QueueGive("MachineGun_ArmorPiercingBullet x99");
    ImGui::SameLine();
    if (ImGui::Button("Spears x99")) Bs2QueueGive("Speargun_RocketSpearAmmo x99");
    ImGui::SameLine();
    if (ImGui::Button("Grenades x99")) Bs2QueueGive("GrenadeLauncher_RPG x99");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##give", give_, sizeof(give_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##count", &give_n_);
    ImGui::SameLine();
    if (ImGui::Button("Give item") && give_[0]) Bs2QueueGive(std::string(give_) + " x" + std::to_string(std::max(1, give_n_)));
    ImGui::SameLine();
    if (ImGui::Button("Give plasmid/tonic") && give_[0]) Bs2QueueGive(std::string("plasmid:") + give_);
    // BS2 plasmid classes are tiered: <Name>{Basic,Advanced,Master}Plasmid (from the memory scan). The BS1
    // names (Incineration, Telekinesis...) still exist here as unconfigured leftovers and give a blank
    // "FriendlyName needs to be configured" item - never use them.
    static const char* kPlasmids[] = {"ElectroBolt", "Incineration", "Telekinesis", "WinterBlast", "Swarm", "CycloneTrap", "Decoy", "Hypnotize", "SecurityCommand", "Scout"};
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##ptier", &ptier_, "Basic\0Advanced\0Master\0");
    ImGui::SameLine();
    if (ImGui::Button("Give all plasmids at this tier")) {
      static const char* kTier[] = {"Basic", "Advanced", "Master"};
      for (const char* p : kPlasmids) Bs2QueueGive(std::string("plasmid:Plasmids.") + p + kTier[ptier_] + "Plasmid");
    }
    for (int i = 0; i < 10; ++i) {
      static const char* kTier[] = {"Basic", "Advanced", "Master"};
      if (i) ImGui::SameLine();
      if (i == 5) ImGui::NewLine();
      if (ImGui::SmallButton(kPlasmids[i])) Bs2QueueGive(std::string("plasmid:Plasmids.") + kPlasmids[i] + kTier[ptier_] + "Plasmid");
    }
    if (ImGui::SmallButton("Copy give results")) CopyLines(Bs2GiveLog());
    ImGui::BeginChild("##givelog", ImVec2(0, 100 * font_scale_), true);
    for (const auto& l : Bs2GiveLog()) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();

    ImGui::SeparatorText("Player events (PlayerStatsManager, ShockPlayer, controller; newest at the bottom)");
    bool fl = Bs2FeedLogOn();
    if (ImGui::Checkbox("Also write to the log", &fl)) Bs2FeedLog(fl);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy events")) CopyLines(Bs2EventFeed());
    ImGui::BeginChild("##feed", ImVec2(0, 150 * font_scale_), true);
    for (const auto& l : Bs2EventFeed()) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::SeparatorText("Find class names in memory (e.g. Ammo, Hypo, Plasmid, PickupItem) - click a hit to give it");
    ImGui::SetNextItemWidth(260);
    bool scan_enter = ImGui::InputText("##scan", scan_, sizeof(scan_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Scan") || scan_enter) && scan_[0]) Bs2ScanNames(scan_);
    ImGui::BeginChild("##hits", ImVec2(0, 120 * font_scale_), true);
    for (const auto& h : Bs2NameHits()) {
      if (ImGui::SmallButton(h.c_str())) Bs2QueueGive(h + " x" + std::to_string(std::max(1, give_n_)));
      ImGui::SameLine();
      ImGui::PushID(h.c_str());
      if (ImGui::SmallButton("plasmid")) Bs2QueueGive("plasmid:" + h);
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Summon (class) / custom console command");
    ImGui::SetNextItemWidth(300);
    bool senter = ImGui::InputText("##summon", summon_, sizeof(summon_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Summon") || senter) && summon_[0]) Bs2QueueCommand(std::string("summon ") + summon_);
    ImGui::SetNextItemWidth(300);
    bool enter = ImGui::InputText("##cmd", custom_, sizeof(custom_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run") || enter) && custom_[0]) { Bs2QueueCommand(custom_); custom_[0] = 0; }
    if (ImGui::SmallButton("Copy results")) CopyLines(Bs2RecentResults());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy everything")) {
      std::string all = "# give\n"; for (const auto& l : Bs2GiveLog()) all += l + "\n";
      all += "# console\n"; for (const auto& r : Bs2RecentResults()) all += r + "\n";
      all += "# events\n"; for (const auto& l : Bs2EventFeed()) all += l + "\n";
      ImGui::SetClipboardText(all.c_str());
    }
    for (const auto& r : Bs2RecentResults()) ImGui::TextUnformatted(r.c_str());
    ImGui::End();
  }

 private:
  static inline std::atomic<bool> visible_{false};
  static inline bool just_opened_ = false;
  static inline float font_scale_ = 1.3f;
  uint16_t pad_prev_ = 0;
  bool combo_prev_ = false;
  bool god_ = false, noclip_ = false, fly_ = false;
  float slomo_ = 1.0f;
  int give_n_ = 1;
  int ptier_ = 0;
  char give_[128] = "";
  template <class C> static void CopyLines(const C& lines) {   // clipboard, one line each (paste into chat)
    std::string all; for (const auto& l : lines) { all += l; all += '\n'; }
    ImGui::SetClipboardText(all.c_str());
  }
  char gw_[128] = "";
  std::vector<std::string> allw_;
  char scan_[64] = "Ammo";
  char summon_[128] = "";
  char custom_[256] = "";
};
