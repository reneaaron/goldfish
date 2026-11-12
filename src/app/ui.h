#pragma once
// Shared UI and app logic. Runs on the board and in the desktop simulator;
// nothing in src/app may touch hardware directly.
//
// Flow (SeedQR only, one seed in RAM):
//   Home -> Scan SeedQR -> Seed loaded (fingerprint) -> Seed menu
//   Seed menu -> Scan transaction | Export xpub | Wipe seed

enum UiScreen {
    UI_HOME,
    UI_SCAN_SEED,
    UI_SEED_LOADED,
    UI_SEED_MENU,
    UI_SCAN_PSBT,
    UI_EXPORT_XPUB,
    UI_WIPE_CONFIRM,
};

void ui_build();               // shows the home screen
void ui_goto(UiScreen s);
UiScreen ui_current();
