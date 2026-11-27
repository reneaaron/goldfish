//! Broadcast straight to Bitcoin's peer-to-peer network over the built-in
//! Tor ([`crate::tor`]), through a short-lived bdk_kyoto node: not to the
//! server the wallet syncs from, which knows the wallet's addresses and
//! would tie the transaction to them, and not from this computer's IP.
//! Nothing runs once the broadcast is done, and nothing is written to disk.

use std::net::SocketAddr;
use std::time::Duration;

use bdk_kyoto::bip157::{self, chain::ChainState, tokio, Builder as NodeBuilder, GossipMonitorRequestBuilder, Socks5Proxy};
use bdk_kyoto::HashCheckpoint;
use bdk_wallet::bitcoin::{Network, Transaction, Txid};
use bdk_wallet::Wallet;

use crate::{err, Res};

/// A broadcast goes out up to this many times, each waiting this long to
/// hear it back from the network.
const SEND_ROUNDS: usize = 3;
const ROUND: Duration = Duration::from_secs(60);

/// Where the node's header chain starts: the wallet's last synced block, so
/// it fetches only a few headers; a wallet that never synced starts from
/// the network's first block.
pub fn start(wallet: &Wallet) -> HashCheckpoint {
    let tip = wallet.latest_checkpoint();
    if tip.height() > 0 {
        HashCheckpoint::new(tip.height(), tip.hash())
    } else {
        HashCheckpoint::from_genesis(wallet.network())
    }
}

/// Send `tx` to one random peer, and done once it comes back over a second
/// connection that hears what the network relays: so another peer took it,
/// not just the one we told (which may have dropped it, say for a fee below
/// its minimum). Until then it goes out again, to a random peer each time.
/// `proxy` is the built-in Tor's SOCKS5 address (every connection gets a
/// circuit of its own); start from [`start`].
pub fn broadcast(network: Network, start: HashCheckpoint, proxy: Option<SocketAddr>, tx: &Transaction) -> Res<Txid> {
    let tx = tx.clone();
    // on a thread of its own: the node's runtime can't start inside the
    // caller's (Tauri runs commands on a tokio runtime)
    std::thread::spawn(move || run(network, start, proxy, tx)).join().map_err(|_| "the broadcast stopped unexpectedly".to_string())?
}

fn run(network: Network, start: HashCheckpoint, proxy: Option<SocketAddr>, tx: Transaction) -> Res<Txid> {
    let txid = tx.compute_txid();
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().map_err(err)?;
    rt.block_on(async move {
        let mut b = NodeBuilder::new(network)
            .response_timeout(Duration::from_secs(10))
            .required_peers(2)
            .chain_state(ChainState::Checkpoint(start))
            .headers_only_sync();
        if let Some(proxy) = proxy {
            b = b.socks5_proxy(Socks5Proxy::new(proxy));
        }
        let (node, client) = b.build();
        let bip157::Client { requester: r, info_rx, warn_rx, event_rx: _events } = client;
        tokio::spawn(log(info_rx, warn_rx));
        let running = tokio::spawn(node.run());
        let result = async {
            let watch = GossipMonitorRequestBuilder::from_expected_inputs(tx.input.iter().map(|i| i.previous_output)).build();
            let mut heard = r.subscribe_to_gossip(watch).await.map_err(err)?.ok_or("already listening for transactions")?;
            let mut taken = false; // a peer asked for it at least once
            for _ in 0..SEND_ROUNDS {
                let round = async {
                    // queued until a peer is connected; a peer that already
                    // has it doesn't ask again, so this may never finish
                    let sent = r.submit_package(tx.clone());
                    tokio::pin!(sent);
                    let mut sending = true;
                    loop {
                        tokio::select! {
                            s = &mut sent, if sending => {
                                s.map_err(err)?;
                                sending = false;
                                taken = true;
                            }
                            t = heard.recv() => match t {
                                Some(t) if t.compute_txid() == txid => return Ok::<_, String>(()),
                                Some(_) => {}
                                None => return Err("the node stopped".to_string()),
                            },
                        }
                    }
                };
                if let Ok(done) = tokio::time::timeout(ROUND, round).await {
                    return done.map(|_| txid);
                }
            }
            Err(if taken {
                "A peer took it, but it hasn't spread yet. Try Broadcast again in a few minutes."
            } else {
                "No peer answered over Tor. Try Broadcast again in a few minutes."
            }
            .to_string())
        }
        .await;
        let _ = r.shutdown();
        let _ = running.await;
        result
    })
}

async fn log(mut info: bip157::Receiver<bip157::Info>, mut warn: bip157::Receiver<bip157::Warning>) {
    loop {
        tokio::select! {
            i = info.recv() => match i {
                Some(bip157::Info::NewChainHeight(_)) | Some(bip157::Info::FilterAdded(_)) => {}
                Some(i) => eprintln!("p2p: {i}"),
                None => break,
            },
            w = warn.recv() => match w {
                Some(w) => eprintln!("p2p warning: {w}"),
                None => break,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A recent signet block, so the node fetches only a few headers.
    fn signet_start() -> HashCheckpoint {
        HashCheckpoint::try_from((194932, "0000014197afc2080256e22cb07ade59fbc1f9360a424ea97f55b38df43c7f08".to_string())).unwrap()
    }

    /// Tauri runs commands on a tokio runtime; a broadcast from there must
    /// fail or succeed, not panic on starting its own. Regtest has no peers
    /// to find, so it ends in an error.
    #[test]
    fn broadcast_inside_a_runtime() {
        use bdk_wallet::bitcoin::{absolute::LockTime, transaction::Version};
        let tx = Transaction { version: Version::TWO, lock_time: LockTime::ZERO, input: vec![], output: vec![] };
        let rt = tokio::runtime::Builder::new_multi_thread().enable_all().build().unwrap();
        let r = rt.block_on(async { broadcast(Network::Regtest, HashCheckpoint::from_genesis(Network::Regtest), None, &tx) });
        assert!(r.is_err());
    }

    /// A transaction no peer accepts (it spends a coin that doesn't exist)
    /// is asked for but never relayed back, so the broadcast fails instead
    /// of claiming it went out, here over the built-in Tor:
    /// `cargo test p2p_broadcast_unrelayed -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn p2p_broadcast_unrelayed_signet() {
        use bdk_wallet::bitcoin::{absolute::LockTime, hashes::Hash, transaction::Version, Amount, OutPoint, ScriptBuf, TxIn, TxOut};
        let tx = Transaction {
            version: Version::TWO,
            lock_time: LockTime::ZERO,
            input: vec![TxIn { previous_output: OutPoint::new(Txid::from_byte_array([7; 32]), 0), ..Default::default() }],
            output: vec![TxOut { value: Amount::from_sat(10_000), script_pubkey: ScriptBuf::new_op_return([1u8; 8]) }],
        };
        crate::tor::set_dir(std::env::temp_dir().join("dove-tor-test"));
        let t = std::time::Instant::now();
        let proxy = crate::tor::proxy().unwrap();
        eprintln!("tor up at {proxy} in {:?}", t.elapsed());
        let t = std::time::Instant::now();
        let e = broadcast(Network::Signet, signet_start(), Some(proxy), &tx).unwrap_err();
        eprintln!("{e} after {:?}", t.elapsed());
        assert!(e.contains("hasn't spread") || e.contains("No peer answered"), "{e}");
    }
}
