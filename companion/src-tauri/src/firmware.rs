//! Firmware updates: the latest Goldfish release from GitHub, checked against
//! the release's SHA256SUMS, copied onto the board's RP2350 drive. Goldfish's
//! "Update firmware" restarts it as that drive; the board installs a .uf2 file
//! copied there and restarts. The check proves the file is the one the
//! release lists, not who built it: both come from GitHub.

use std::path::{Path, PathBuf};

use bdk_wallet::bitcoin::hashes::{sha256, Hash};
use serde::Serialize;

use crate::Res;

const REPO: &str = "reneaaron/goldfish";
const TIMEOUT_S: u64 = 60;
/// UF2 block: magic at 0 and 4, flags at 8, family ID at 28, magic at 508.
const UF2_MAGIC0: u32 = 0x0A32_4655;
const UF2_MAGIC1: u32 = 0x9E5D_5157;
const UF2_MAGIC_END: u32 = 0x0AB1_6F30;
const UF2_FAMILY_PRESENT: u32 = 0x2000;
const RP2350_ARM_S: u32 = 0xE48B_FF59;

#[derive(Serialize, Clone, Debug)]
pub struct Release {
    pub tag: String,
    pub sha256: String,
    pub size: usize,
    pub url: String,
}

fn get(url: &str, proxy: &str) -> Res<Vec<u8>> {
    let mut req = minreq::get(url).with_header("User-Agent", "goldfish-companion").with_timeout(TIMEOUT_S);
    if !proxy.is_empty() {
        // minreq speaks HTTP proxies only; going around a SOCKS one would leak
        if proxy.starts_with("socks") {
            return Err("Firmware downloads take an HTTP proxy. For Tor, set HTTPTunnelPort 9080 in torrc and use http://127.0.0.1:9080 in Settings, or update with the flasher page".into());
        }
        req = req.with_proxy(minreq::Proxy::new(proxy).map_err(|e| e.to_string())?);
    }
    let resp = req.send().map_err(|e| e.to_string())?;
    if resp.status_code != 200 {
        return Err(format!("GitHub answered {} for {url}", resp.status_code));
    }
    Ok(resp.into_bytes())
}

/// The latest release's firmware, checked. Downloads by tag, so a release
/// published in between can't mix files.
pub fn download_latest(proxy: &str) -> Res<(Release, Vec<u8>)> {
    let api = get(&format!("https://api.github.com/repos/{REPO}/releases/latest"), proxy)?;
    let tag = serde_json::from_slice::<serde_json::Value>(&api)
        .ok()
        .and_then(|v| v["tag_name"].as_str().map(String::from))
        .filter(|t| t.starts_with('v') && t[1..].chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '-'))
        .ok_or("GitHub's answer has no release tag")?;
    let base = format!("https://github.com/{REPO}/releases/download/{tag}");
    let sums = String::from_utf8(get(&format!("{base}/SHA256SUMS"), proxy)?).map_err(|_| "SHA256SUMS is not text")?;
    let uf2 = get(&format!("{base}/firmware.uf2"), proxy)?;
    let sha256 = check(&uf2, &sums)?;
    let url = format!("https://github.com/{REPO}/releases/tag/{tag}");
    Ok((Release { tag, sha256, size: uf2.len(), url }, uf2))
}

/// The file's SHA-256 if SHA256SUMS lists it for firmware.uf2 and the file is
/// a UF2 image for the RP2350 (Arm, secure), which is what Goldfish builds.
pub fn check(uf2: &[u8], sums: &str) -> Res<String> {
    let listed = sums
        .lines()
        .filter_map(|l| l.split_once(char::is_whitespace))
        .find(|(_, name)| name.trim().trim_start_matches('*') == "firmware.uf2")
        .map(|(hash, _)| hash.to_ascii_lowercase())
        .ok_or("SHA256SUMS doesn't list firmware.uf2")?;
    let actual = sha256::Hash::hash(uf2).to_string();
    if actual != listed {
        return Err(format!("firmware.uf2 doesn't match SHA256SUMS (got {actual}, listed {listed})"));
    }
    if uf2.is_empty() || !uf2.len().is_multiple_of(512) {
        return Err("firmware.uf2 is not a UF2 image".into());
    }
    let word = |b: &[u8], at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    for b in uf2.chunks(512) {
        if word(b, 0) != UF2_MAGIC0 || word(b, 4) != UF2_MAGIC1 || word(b, 508) != UF2_MAGIC_END {
            return Err("firmware.uf2 is not a UF2 image".into());
        }
        if word(b, 8) & UF2_FAMILY_PRESENT == 0 || word(b, 28) != RP2350_ARM_S {
            return Err("firmware.uf2 is not for the RP2350".into());
        }
    }
    Ok(actual)
}

/// Where Linux desktops mount removable drives.
pub fn mount_roots() -> Vec<PathBuf> {
    let user = std::env::var("USER").unwrap_or_default();
    let mut roots = vec![];
    if !user.is_empty() {
        roots.push(PathBuf::from("/run/media").join(&user));
        roots.push(PathBuf::from("/media").join(&user));
    }
    roots.push(PathBuf::from("/media"));
    roots
}

/// A mounted RP2350 boot drive: its INFO_UF2.TXT names the board as RP2350.
pub fn find_drive(roots: &[PathBuf]) -> Option<PathBuf> {
    roots.iter().filter_map(|r| std::fs::read_dir(r).ok()).flatten().flatten().map(|e| e.path()).find(|p| {
        std::fs::read_to_string(p.join("INFO_UF2.TXT")).is_ok_and(|info| info.contains("RP2350"))
    })
}

/// Copy onto the drive and flush. The board restarts into the new firmware
/// once the last block is in.
pub fn write(drive: &Path, uf2: &[u8]) -> Res<()> {
    use std::io::Write;
    let mut f = std::fs::File::create(drive.join("firmware.uf2")).map_err(|e| e.to_string())?;
    f.write_all(uf2).map_err(|e| e.to_string())?;
    f.sync_all().map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn block(family: u32) -> Vec<u8> {
        let mut b = vec![0u8; 512];
        for (at, w) in [(0, UF2_MAGIC0), (4, UF2_MAGIC1), (8, UF2_FAMILY_PRESENT), (28, family), (508, UF2_MAGIC_END)] {
            b[at..at + 4].copy_from_slice(&u32::to_le_bytes(w));
        }
        b
    }
    fn sums(data: &[u8]) -> String {
        format!("{}  firmware.uf2\n", sha256::Hash::hash(data))
    }

    #[test]
    fn checks_hash_and_image() {
        let uf2 = [block(RP2350_ARM_S), block(RP2350_ARM_S)].concat();
        assert_eq!(check(&uf2, &sums(&uf2)).unwrap(), sha256::Hash::hash(&uf2).to_string());
        // sha256sum's binary-mode "*name" and upper-case hex
        assert!(check(&uf2, &sums(&uf2).to_uppercase().replace("  FIRMWARE.UF2", " *firmware.uf2")).is_ok());

        let mut changed = uf2.clone();
        changed[100] ^= 1;
        assert!(check(&changed, &sums(&uf2)).unwrap_err().contains("doesn't match"));
        assert!(check(&uf2, "abcd  other.bin\n").unwrap_err().contains("doesn't list"));

        let rp2040 = block(0xE48B_FF56);
        assert!(check(&rp2040, &sums(&rp2040)).unwrap_err().contains("not for the RP2350"));
        let html = b"<html>Not Found</html>".to_vec();
        assert!(check(&html, &sums(&html)).unwrap_err().contains("not a UF2"));
    }

    /// Reaches GitHub: `cargo test -- --ignored latest_release`
    #[test]
    #[ignore]
    fn latest_release() {
        let (r, uf2) = download_latest("").unwrap();
        assert!(r.tag.starts_with('v') && uf2.len() == r.size && r.size > 100_000, "{r:?}");
        assert!(download_latest("socks5://127.0.0.1:9050").unwrap_err().contains("HTTP proxy"));
    }

    #[test]
    fn finds_and_writes_the_drive() {
        let root = std::env::temp_dir().join(format!("goldfish-fw-{}", std::process::id()));
        let usb = root.join("USBSTICK");
        let boot = root.join("RP2350");
        std::fs::create_dir_all(&usb).unwrap();
        std::fs::create_dir_all(&boot).unwrap();
        std::fs::write(usb.join("notes.txt"), "hi").unwrap();
        assert_eq!(find_drive(&[root.clone(), PathBuf::from("/nonexistent")]), None);

        std::fs::write(boot.join("INFO_UF2.TXT"), "UF2 Bootloader v1.0\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n").unwrap();
        let drive = find_drive(&[PathBuf::from("/nonexistent"), root.clone()]).unwrap();
        assert_eq!(drive, boot);
        write(&drive, b"data").unwrap();
        assert_eq!(std::fs::read(boot.join("firmware.uf2")).unwrap(), b"data");
        std::fs::remove_dir_all(&root).unwrap();
    }
}
