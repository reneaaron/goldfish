#pragma once
// Shared UI and app logic. Runs on the board and in the desktop simulator;
// nothing in src/app may touch hardware directly.
//
// Flow (SeedQR only, one seed in RAM):
//   Home -> Scan SeedQR -> Seed loaded (fingerprint) -> Seed menu
//   Seed menu -> Sign transaction | Verify address | Connect wallet | Wipe
//   Scan transaction -> Review -> each output -> Swipe to sign -> Signed QR
//   Verify address -> scan address QR -> searching -> verified / not from this seed
//   Home -> New seed (12/24 words) -> dice rolls | coin flips | enter words
//        -> grid format -> offline warning -> whole SeedQR -> each 5x5 block
//        -> scan the finished card -> Seed loaded

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
    UI_SCAN_ADDRESS,
    UI_VERIFY_RESULT,    // shows progress while searching, then the result
    UI_NEW_SEED,         // 12/24 words and how to make the seed
    UI_ENTROPY_DICE,
    UI_ENTROPY_COINS,
    UI_ENTER_WORDS,      // one screen per word
    UI_BACKUP_FORMAT,    // Compact or Standard SeedQR
    UI_BACKUP_WARNING,
    UI_BACKUP_QR,        // the whole SeedQR
    UI_BACKUP_BLOCK,     // one 5x5 block per screen
    UI_BACKUP_SCAN,      // scan the hand-made card; must hold the new seed
};

void ui_build();               // shows the home screen
void ui_goto(UiScreen s);
UiScreen ui_current();

// For the simulator's scripted run: the same actions as the on-screen controls.
void ui_next_output();         // "Next" on an output screen
bool ui_swipe_sign();          // completes the swipe; false if signing failed
bool verify_done();            // the address check has a result
void ui_new_seed(UiScreen method, int words);   // start a new seed: UI_ENTROPY_DICE, _COINS or UI_ENTER_WORDS
void ui_entropy_input(char c); // a dice key ('1'..'6') or coin key ('1' heads, '0' tails)
bool ui_enter_word(const char *word);           // tap the suggestion for this word
void ui_backup_next();         // "Next block"
