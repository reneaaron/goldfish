// Runs the firmware's PSBT review and signing on PSBTs from stdin, so
// oracle.py can compare it against an independent implementation (embit).
// Input lines:  <mnemonic>\t<base64 psbt>
// Output lines: one JSON object per input line.
#include <stdio.h>
#include <string.h>
#include <string>
#include <wally_core.h>
#include <wally_bip39.h>
#include "../src/app/psbt_review.h"
#include "../src/app/wallet.h"
#include "../src/app/ur.h"

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", *s);
        else putchar(*s);
    }
    putchar('"');
}

int main() {
    wally_init(0);
    static char line[65536];
    static uint8_t raw[UR_MAX_MESSAGE * 2], out[UR_MAX_MESSAGE * 2];
    static char b64[UR_MAX_MESSAGE * 3];
    std::string loaded;
    while (fgets(line, sizeof(line), stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) { puts("{\"bad_input\":true}"); continue; }
        *tab = 0;
        char *p = tab + 1;
        p[strcspn(p, "\r\n")] = 0;
        if (loaded != line) {
            SeedEntropy e;
            memset(&e, 0, sizeof(e));
            if (bip39_mnemonic_to_bytes(nullptr, line, e.bytes, sizeof(e.bytes), &e.len) != WALLY_OK || !wallet_load(e)) { puts("{\"bad_seed\":true}"); continue; }
            loaded = line;
        }
        size_t n = 0;
        if (wally_base64_to_bytes(p, 0, raw, sizeof(raw), &n) != WALLY_OK || n > sizeof(raw)) n = 0;
        PsbtReview r;
        PsbtError err = n ? psbt_load(raw, n, &r) : PSBT_PARSE_FAILED;
        if (!n) memset(&r, 0, sizeof(r));
        printf("{\"error\":%d,\"testnet\":%s,\"input_total\":%llu,\"spend\":%llu,\"self\":%llu,\"change\":%llu,\"fee\":%llu,\"outputs\":[",
               int(err), r.testnet ? "true" : "false", (unsigned long long)r.input_total, (unsigned long long)r.spend,
               (unsigned long long)r.self, (unsigned long long)r.change, (unsigned long long)r.fee);
        for (int i = 0; i < r.n_outputs && i < PSBT_MAX_OUTPUTS; i++) {
            const PsbtOutput &o = r.outputs[i];
            printf("%s{\"address\":", i ? "," : "");
            json_str(o.address);
            printf(",\"amount\":%llu,\"change\":%s,\"self\":%s,\"suspicious\":%s,\"far\":%s,\"path\":", (unsigned long long)o.amount,
                   o.is_change ? "true" : "false", o.is_self ? "true" : "false", o.suspicious ? "true" : "false",
                   o.far ? "true" : "false");
            json_str(o.path);
            putchar('}');
        }
        printf("],\"sigs\":");
        if (err == PSBT_OK) {   // the UI only offers signing for PSBT_OK
            int sigs = psbt_sign();
            size_t m = psbt_serialize(out, sizeof(out));
            char *s = nullptr;
            b64[0] = 0;
            if (m && wally_base64_from_bytes(out, m, 0, &s) == WALLY_OK) { snprintf(b64, sizeof(b64), "%s", s); wally_free_string(s); }
            printf("%d,\"signed\":", sigs);
            json_str(b64);
        } else {
            printf("0,\"signed\":null");
        }
        fflush(stdout);
        psbt_clear();          // a crash here is caught by finishing the line only after it
        puts("}");
        fflush(stdout);
    }
    return 0;
}
