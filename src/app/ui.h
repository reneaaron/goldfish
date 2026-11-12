#pragma once
// Shared UI and app logic. Runs on the board and in the desktop simulator;
// nothing in src/app may touch hardware directly.
//
// Flow (SeedQR only, one seed in RAM):
//   Home -> Scan SeedQR -> Seed loaded (fingerprint) -> Seed menu
//   Seed menu -> Scan transaction | Export xpub | Wipe seed
//   Scan transaction -> Review -> each output -> Swipe to sign -> Signed QR

enum UiScreen {
    UI_HOME,
    UI_SCAN_SEED,
    UI_SEED_LOADED,
    UI_SEED_MENU,
    UI_SCAN_PSBT,
    UI_EXPORT_XPUB,
    UI_WIPE_CONFIRM,
    UI_PSBT_ERROR,
    UI_PSBT_OVERVIEW,
    UI_PSBT_OUTPUT,      // one screen per output, see ui_output_index()
    UI_PSBT_SIGN,
    UI_PSBT_SIGNED,
};

void ui_build();               // shows the home screen
void ui_goto(UiScreen s);
UiScreen ui_current();

// For the simulator's scripted run: the same actions as the on-screen controls.
void ui_next_output();         // "Next" on an output screen
bool ui_swipe_sign();          // completes the swipe; false if signing failed
