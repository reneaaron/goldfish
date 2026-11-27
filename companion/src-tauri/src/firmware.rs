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
    let (name, _) = listed_uf2(&sums)?;
    let uf2 = get(&format!("{base}/{name}"), proxy)?;
    let sha256 = check(&uf2, &sums)?;
    let url = format!("https://github.com/{REPO}/releases/tag/{tag}");
    Ok((Release { tag, sha256, size: uf2.len(), url }, uf2))
}

/// The one .uf2 file SHA256SUMS lists, and its hash: the firmware's name
/// comes from the release (firmware.uf2 in v0.1.0), not from here, so a
/// later release can rename it.
pub fn listed_uf2(sums: &str) -> Res<(String, String)> {
    let mut found = sums
        .lines()
        .filter_map(|l| l.split_once(char::is_whitespace))
        .map(|(hash, name)| (name.trim().trim_start_matches('*').to_string(), hash.to_ascii_lowercase()))
        .filter(|(name, _)| name.ends_with(".uf2"));
    let (name, hash) = found.next().ok_or("SHA256SUMS lists no .uf2 file")?;
    if found.next().is_some() {
        return Err("SHA256SUMS lists more than one .uf2 file".into());
    }
    // It goes into the download URL.
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_')) {
        return Err(format!("SHA256SUMS names the firmware oddly: {name}"));
    }
    Ok((name, hash))
}

/// The file's SHA-256 if it matches the .uf2 SHA256SUMS lists and is a UF2
/// image for the RP2350 (Arm, secure), which is what Goldfish builds.
pub fn check(uf2: &[u8], sums: &str) -> Res<String> {
    let (name, listed) = listed_uf2(sums)?;
    let actual = sha256::Hash::hash(uf2).to_string();
    if actual != listed {
        return Err(format!("{name} doesn't match SHA256SUMS (got {actual}, listed {listed})"));
    }
    if uf2.is_empty() || !uf2.len().is_multiple_of(512) {
        return Err(format!("{name} is not a UF2 image"));
    }
    let word = |b: &[u8], at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    for b in uf2.chunks(512) {
        if word(b, 0) != UF2_MAGIC0 || word(b, 4) != UF2_MAGIC1 || word(b, 508) != UF2_MAGIC_END {
            return Err(format!("{name} is not a UF2 image"));
        }
        if word(b, 8) & UF2_FAMILY_PRESENT == 0 || word(b, 28) != RP2350_ARM_S {
            return Err(format!("{name} is not for the RP2350"));
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
/// once the last block is in, so the drive can vanish under the final flush:
/// a failed flush with the drive gone counts as done (the user then checks
/// the version on Goldfish); one with the drive still there doesn't.
pub fn write(drive: &Path, uf2: &[u8]) -> Res<()> {
    use std::io::Write;
    let mut f = std::fs::File::create(drive.join("firmware.uf2")).map_err(|e| e.to_string())?;
    f.write_all(uf2).map_err(|e| e.to_string())?;
    match f.sync_all() {
        Err(e) if drive.join("INFO_UF2.TXT").exists() => Err(e.to_string()),
        _ => Ok(()),
    }
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
        assert!(check(&uf2, "abcd  other.bin\n").unwrap_err().contains("no .uf2"));

        // a later release's own name; SHA256SUMS may list other files too
        let hash = sha256::Hash::hash(&uf2);
        let renamed = format!("abcd  dove-0.1.0-amd64.deb\n{hash}  goldfish-0.2.0.uf2\n");
        assert_eq!(listed_uf2(&renamed).unwrap().0, "goldfish-0.2.0.uf2");
        assert!(check(&uf2, &renamed).is_ok());
        assert!(listed_uf2(&format!("{hash}  a.uf2\n{hash}  b.uf2\n")).unwrap_err().contains("more than one"));
        assert!(listed_uf2(&format!("{hash}  ../x.uf2\n")).unwrap_err().contains("oddly"));

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
