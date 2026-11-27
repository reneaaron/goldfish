//! The companion's logic without the window, for driving against the
//! Goldfish simulator (see ../../e2e). The wallet is funded with a made-up
//! confirmed coin, so no network is needed.
//!
//!   companion-cli demo-psbt <key export> <to address> <amount sat>
//!       PSBT <base64>, then FRAME <ur part> lines: three cycles of the animation
//!   companion-cli finalize <key export> <base64>  < frames on stdin
//!       merges the signed PSBT read from the frames into <base64> (unsigned,
//!       or already signed by other cosigners): SIGNED <have>/<needed>, then
//!       TXID <txid> and TX <hex> once enough keys have signed
//!
//!   companion-cli multisig <threshold> <key>...
//!       DESCRIPTOR <the wallet's descriptor> and ADDRESS <its receive address 0>
//!
//! <key export> is anything the app links: a Goldfish key or a descriptor.

use std::io::BufRead;

use bdk_wallet::bitcoin::consensus::encode::serialize_hex;
use companion::*;

fn demo_wallet(export: &str) -> Res<bdk_wallet::Wallet> {
    let mut w = parse_wallet(export)?.wallet()?;
    fund_for_demo(&mut w, 100_000);
    Ok(w)
}

fn run(args: &[String]) -> Res<()> {
    match args {
        [cmd, export, to, amount] if cmd == "demo-psbt" => {
            let mut w = demo_wallet(export)?;
            let psbt = build_psbt(&mut w, to, amount.parse().map_err(|e| format!("{e}"))?, 2.0)?;
            println!("PSBT {}", psbt_base64(&psbt));
            let mut enc = psbt_encoder(&psbt)?;
            let first = enc.next_part().map_err(|e| e.to_string())?;
            let n = fragment_count(&first);
            println!("FRAME {}", first.to_uppercase());
            for _ in 1..3 * n {
                println!("FRAME {}", enc.next_part().map_err(|e| e.to_string())?.to_uppercase());
            }
            Ok(())
        }
        [cmd, export, unsigned] if cmd == "finalize" => {
            let w = demo_wallet(export)?;
            let needed = parse_wallet(export)?.threshold;
            let mut psbt = psbt_from_base64(unsigned)?;
            let mut scanner = PsbtScanner::default();
            for line in std::io::stdin().lock().lines() {
                let line = line.map_err(|e| e.to_string())?;
                if line.trim().is_empty() {
                    continue;
                }
                if let Some(signed) = scanner.receive(&line)? {
                    let (sigs, tx) = add_signatures(&w, &mut psbt, signed, needed)?;
                    println!("SIGNED {}/{} {}", sigs.have, sigs.needed, sigs.signers.join(","));
                    if let Some(tx) = tx {
                        println!("TXID {}", tx.compute_txid());
                        println!("TX {}", serialize_hex(&tx));
                    }
                    return Ok(());
                }
            }
            Err("stdin ended before the signed PSBT was complete".into())
        }
        [cmd, threshold, keys @ ..] if cmd == "multisig" => {
            let keys: Vec<&str> = keys.iter().map(String::as_str).collect();
            let a = multisig_from_keys(threshold.parse().map_err(|e| format!("{e}"))?, &keys)?;
            println!("DESCRIPTOR {}", a.descriptor);
            println!("ADDRESS {}", a.wallet()?.peek_address(bdk_wallet::KeychainKind::External, 0).address);
            Ok(())
        }
        _ => Err("usage: companion-cli demo-psbt <key> <to> <sat> | finalize <key> <unsigned b64>".into()),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = run(&args) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}
