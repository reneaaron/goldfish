#pragma once
#include <stdint.h>
// Shared UI and app logic. Runs on the board and in the desktop simulator;
// nothing in src/app may touch hardware directly.
//
// Flow (SeedQR only, one seed in RAM):
//   Home -> Scan SeedQR -> Seed loaded (fingerprint) -> Seed menu
//   Seed loaded -> Add passphrase (optional, BIP-39) -> keyboard -> Seed loaded
//        with the passphrase's fingerprint
//   Seed menu -> Sign transaction | Verify address | Connect wallet | Back up as QR | Wipe
//   Connect wallet -> Network (Mainnet / Testnet), RAM only
//   Scan transaction -> Review -> each output -> Swipe to sign -> Signed QR
//   Verify address -> scan address QR -> searching -> verified / not from this seed
//   Multisig: show the wallet's descriptor QR to the Sign or Verify scanner
//        -> Multisig wallet (M of N, key fingerprints, first address) -> Use wallet
//        -> back to that scanner, which now takes the wallet's transactions / addresses
//   Home -> Enter words (12/24) -> one screen per word -> Seed loaded
//   Home -> New seed (12/24 words) -> dice rolls | coin flips | draw words + coins for the last
//        -> offline warning -> write down the words -> QR card? (whole SeedQR,
//        each block, optional card check) -> Seed ready (not loaded) -> Home
//   Seed menu -> Back up as QR -> the same card flow, scan optional
//   Seed menu -> Child seed (BIP-85): 12/24 words -> child number -> the New seed
//        card flow from the warning on (words, QR card?, Seed ready)

enum UiScreen {
    UI_HOME,
    UI_SCAN_SEED,
    UI_SEED_LOADED,
    UI_PASSPHRASE,       // BIP-39 passphrase keyboard, from Seed loaded
    UI_SEED_MENU,
    UI_SCAN_PSBT,
    UI_EXPORT_XPUB,
    UI_NETWORK,          // network for Connect wallet
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
    UI_RESTORE,          // Enter words: 12 or 24
    UI_ENTER_WORDS,      // one screen per word
    UI_BACKUP_WARNING,
    UI_BACKUP_WORDS,     // 4 words per page, new seeds only
    UI_BACKUP_CHOICE,    // add a QR card, or skip
    UI_BACKUP_QR,        // the whole SeedQR
    UI_BACKUP_BLOCK,     // one 5x5 block per screen
    UI_BACKUP_SCAN,      // scan the hand-made card; must hold the same seed
    UI_SEED_READY,       // end of New seed: fingerprint, nothing loaded
    UI_CHILD_LENGTH,     // BIP-85 child seed: 12 or 24 words
    UI_CHILD_INDEX,      // BIP-85 child number keypad
    UI_MULTISIG,         // a scanned multisig descriptor, to check before it is used
};

void ui_build();               // shows the home screen
void ui_goto(UiScreen s);
UiScreen ui_current();

// For the simulator's scripted run: the same actions as the on-screen controls.
void ui_next_output();         // "Next" on an output screen
bool ui_swipe_sign();          // completes the swipe; false if signing failed
bool verify_done();            // the address check has a result
void ui_multisig_use();        // "Use wallet" on the Multisig wallet screen
// Start UI_ENTROPY_DICE, _COINS or UI_ENTER_WORDS; draw: the words are a new seed (last word from coins).
void ui_seed_entry(UiScreen method, int words, bool draw = false);
void ui_backup_loaded();       // "Back up as QR" in the seed menu
void ui_entropy_input(char c); // a dice key ('1'..'6') or coin key ('1' heads, '0' tails)
bool ui_enter_word(const char *word);           // tap the suggestion for this word
void ui_backup_next();         // "Next part"
// The backup QR as drawn on screen (qrcodegen buffer), nullptr if none. For
// the simulator, which scans exactly this as the hand-made card.
const uint8_t *ui_backup_qr();
// "Continue" on the child number screen: derive the BIP-85 child, start its card flow.
void ui_child_seed(int words, uint32_t index);
void ui_child_digit(int k);           // a keypad digit 0-9, or -1 for Del
const char *ui_child_number();        // the child number as typed
// A key of the passphrase keyboard, by its label: one character, or "space",
// "Del", "abc", "ABC", "123", "#+=", "OK" (derive the key, back to Seed loaded).
void ui_passphrase_key(const char *key);
