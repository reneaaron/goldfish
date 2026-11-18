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
//   Connect wallet -> Wallet type (Single-sig / Multisig key), RAM only
//   Scan transaction -> Review -> each output -> Swipe to sign -> Signed QR
//   Verify address -> scan address QR -> searching -> verified / not from this seed
//   Multisig: show the wallet's descriptor QR to the Sign or Verify scanner
//        -> Multisig wallet (M of N, key fingerprints, first address) -> Use wallet
//        -> back to that scanner, which now takes the wallet's transactions / addresses
//   Home -> Enter words (12, or 24 from the first word) -> one screen per word -> Seed loaded
//        camera -> Scan words (a photo of the written backup, experimental) -> Read
//        -> Reading words -> Check words (12 or 24, from how many were found): the
//        words in order, "?" where unread; tap one -> its picture -> Change /
//        Remove / Add a word before (keyboard, then back to the list) -> "Matches
//        my paper" once there are 12/24 words with a valid checksum -> Seed loaded
//   Scan seed -> "Written words instead?" -> Scan words
//   Home -> New seed (12 words) -> dice rolls | coin flips | draw words + coins for the last
//        -> offline warning -> write down the words -> QR card? (whole SeedQR,
//        each block, optional card check) -> Seed ready (not loaded) -> Home
//   Seed menu -> Back up as QR -> the same card flow, scan optional
//   Seed menu -> Child seed (BIP-85): child number -> the New seed
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
    UI_KEY_TYPE,         // single-sig or multisig key for Connect wallet
    UI_WIPE_CONFIRM,
    UI_PSBT_ERROR,
    UI_PSBT_OVERVIEW,
    UI_PSBT_OUTPUT,      // one screen per output, see ui_output_index()
    UI_PSBT_SIGN,
    UI_PSBT_SIGNED,
    UI_SCAN_ADDRESS,
    UI_VERIFY_RESULT,    // shows progress while searching, then the result
    UI_NEW_SEED,         // how to make a 12-word seed
    UI_ENTROPY_DICE,
    UI_ENTROPY_COINS,
    UI_ENTER_WORDS,      // one screen per word
    UI_BACKUP_WARNING,
    UI_BACKUP_WORDS,     // 4 words per page, new seeds only
    UI_BACKUP_CHOICE,    // add a QR card, or skip
    UI_BACKUP_QR,        // the whole SeedQR
    UI_BACKUP_BLOCK,     // one 5x5 block per screen
    UI_BACKUP_SCAN,      // scan the hand-made card; must hold the same seed
    UI_SEED_READY,       // end of New seed: fingerprint, nothing loaded
    UI_CHILD_INDEX,      // BIP-85 child seed: child number keypad, 12 words
    UI_MULTISIG,         // a scanned multisig descriptor, to check before it is used
    UI_SCAN_PAGE,        // photo of a handwritten backup, from Enter words
    UI_PAGE_READING,     // progress while the page's words are read
    UI_PAGE_REVIEW,      // the words found, as one list to check against the paper
    UI_PAGE_WORD,        // one of them: its picture; change, remove, or add one before it
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
void ui_child_seed(uint32_t index);
bool ui_page_read();           // "Read words" on Scan words: false if there's no picture
int ui_page_count();           // words on the Check words list
int ui_page_slot(int i);       // its word i (BIP-39 index), -1 if unread
void ui_page_open(int i);      // tap row i: its own screen
void ui_page_type();           // there: "Change" / "Type it" (keyboard, then back to the list)
void ui_page_insert();         // there: "Add a word before this"
void ui_page_remove();         // there: remove it from the list
void ui_page_add();            // on the list: "add a word" at the end
bool ui_page_confirm();        // "Matches my paper": loads the seed; false if not ready
int ui_page_suggestion();      // on the last word's screen: the word that fits the checksum, or -1
void ui_page_use_suggestion(); // tap it
void ui_child_digit(int k);           // a keypad digit 0-9, or -1 for Del
const char *ui_child_number();        // the child number as typed
// A key of the passphrase keyboard, by its label: one character, or "space",
// "Del", "abc", "ABC", "123", "#+=", "OK" (derive the key, back to Seed loaded).
void ui_passphrase_key(const char *key);
