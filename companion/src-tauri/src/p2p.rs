//! The wallet's chain data from Bitcoin's peer-to-peer network with compact
//! block filters (BIP157/158), through bdk_kyoto: no server learns which
//! addresses are the wallet's. Peers send a small filter per block; the
//! wallet checks them itself and fetches only whole blocks that match. Headers
//! are checked for proof of work, blocks against their headers, filters
//! against the filter-header chain.
//!
//! Each call runs a node for as long as it needs, then stops it: nothing
//! runs in the background and nothing is written to disk.

use std::net::SocketAddr;
use std::time::Duration;

use bdk_kyoto::bip157::{self, chain::ChainState, tokio, Builder as NodeBuilder, Event, Socks5Proxy};
use bdk_kyoto::builder::BuilderExt;
use bdk_kyoto::{state::Idle, wallets::Single};
use bdk_kyoto::{HashCheckpoint, LightClient, Requester, SyncConfig, TrustedPeer};
use bdk_wallet::bitcoin::{BlockHash, Network, Transaction, Txid};
use bdk_wallet::{Update, Wallet};

use crate::{err, FeeRates, Res};

/// A wallet's first sync scans filters from here: native segwit, so nothing
/// older on mainnet is ours.
fn anchor(network: Network) -> HashCheckpoint {
    match network {
        Network::Bitcoin => HashCheckpoint::segwit_activation(),
        n => HashCheckpoint::from_genesis(n),
    }
}

/// Stop waiting on the network after this long.
const SYNC_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const CALL_TIMEOUT: Duration = Duration::from_secs(3 * 60);

#[derive(Clone, Debug)]
pub struct P2p {
    network: Network,
    /// your own node, the only peer then; else peers from DNS seeds
    peer: Option<TrustedPeer>,
    proxy: Option<Socks5Proxy>,
    /// where the header chain starts for broadcasts and fee rates, from
    /// [`start_for`]: near the wallet's last synced block, so only a few
    /// headers are fetched
    pub start: Option<HashCheckpoint>,
}

/// A block a few below the wallet's tip, so a node started there has the
/// last blocks' headers for fee rates (a node knows no header at its start).
pub fn start_for(wallet: &Wallet) -> Option<HashCheckpoint> {
    let tip = wallet.latest_checkpoint();
    let cp = tip.iter().find(|cp| cp.height() + 4 <= tip.height())?;
    (cp.height() > 0).then(|| HashCheckpoint::new(cp.height(), cp.hash()))
}

impl P2p {
    /// `rest` is what follows `p2p://`: empty for peers found on the
    /// network, `host:port` for your own node. `proxy` is a SOCKS5 address
    /// (`socks5://127.0.0.1:9050` for Tor).
    pub fn new(network: Network, rest: &str, proxy: &str) -> Res<P2p> {
        let rest = rest.trim().trim_end_matches('/');
        let peer = if rest.is_empty() {
            None
        } else {
            let (host, port) = rest.rsplit_once(':').ok_or("your node needs a port, like p2p://192.168.1.10:8333")?;
            let port: u16 = port.parse().map_err(|_| "p2p: that port isn't a number")?;
            let host = host.trim_start_matches('[').trim_end_matches(']');
            Some(match host.parse::<std::net::IpAddr>() {
                Ok(ip) => TrustedPeer::from_socket_addr(SocketAddr::new(ip, port)),
                Err(_) => TrustedPeer::from_hostname(host, port),
            })
        };
        let proxy = match proxy.trim() {
            "" => None,
            p => {
                let addr = p.trim_start_matches("socks5h://").trim_start_matches("socks5://");
                let addr: SocketAddr = addr.parse().map_err(|_| "p2p takes a SOCKS5 proxy by IP, like socks5://127.0.0.1:9050 (Tor)")?;
                Some(Socks5Proxy::new(addr))
            }
        };
        Ok(P2p { network, peer, proxy, start: None })
    }

    fn node_builder(&self) -> NodeBuilder {
        let mut b = NodeBuilder::new(self.network).response_timeout(Duration::from_secs(10));
        if let Some(peer) = self.peer.clone() {
            b = b.add_peer(peer).whitelist_only().required_peers(1);
        } else {
            // more than one, so a peer hiding a filter is caught by the others
            b = b.required_peers(2);
        }
        if let Some(proxy) = self.proxy.clone() {
            b = b.socks5_proxy(proxy);
        }
        b
    }

    /// Set up a sync of `wallet` (cheap: hold the wallet's lock for this,
    /// not for [`P2p::run`]). A wallet that never synced scans from
    /// [`anchor`]; after that, from its last block.
    pub fn prepare(&self, wallet: &Wallet) -> Res<LightClient<Idle, Single>> {
        let builder = self.node_builder();
        if wallet.latest_checkpoint().height() == 0 {
            builder.build_with_wallet(wallet, SyncConfig::wallet_recovery_sync(anchor(self.network)).build())
        } else {
            builder.build_with_wallet(wallet, SyncConfig::sync_from_last_checkpoint().build())
        }
        .map_err(err)
    }

    /// Run a prepared sync to the chain's tip.
    pub fn run(client: LightClient<Idle, Single>) -> Res<Update> {
        runtime()?.block_on(async move {
            let (client, logs, mut updates) = client.subscribe();
            tokio::spawn(log(logs.info_subscriber, logs.warning_subscriber));
            let client = client.start();
            let update = tokio::time::timeout(SYNC_TIMEOUT, updates.update()).await;
            let _ = client.requester().shutdown();
            update.map_err(|_| "no answer from the network in time".to_string())?.map_err(err)
        })
    }

    /// A node that follows headers only, from `start`, until it's at the tip.
    async fn with_node<T, F, Fut>(&self, f: F) -> Res<T>
    where
        F: FnOnce(Requester, HashCheckpoint, Vec<BlockHash>) -> Fut,
        Fut: std::future::Future<Output = Res<T>>,
    {
        let start = self.start.unwrap_or_else(|| anchor(self.network));
        let (node, client) = self.node_builder().chain_state(ChainState::Checkpoint(start)).headers_only_sync().build();
        let bip157::Client { requester, info_rx, warn_rx, mut event_rx } = client;
        tokio::spawn(log(info_rx, warn_rx));
        let running = tokio::spawn(node.run());
        let synced = tokio::time::timeout(CALL_TIMEOUT, async {
            while let Some(event) = event_rx.recv().await {
                if let Event::FiltersSynced(s) = event {
                    return Some(s);
                }
            }
            None
        })
        .await;
        let result = match synced {
            Ok(Some(s)) => f(requester.clone(), s.tip(), s.recent_history().values().rev().map(|h| h.block_hash()).collect()).await,
            Ok(None) => Err("the node stopped: no peers".into()),
            Err(_) => Err("no answer from the network in time".into()),
        };
        let _ = requester.shutdown();
        let _ = running.await;
        result
    }

    /// Send to peers; done once one of them has asked for the transaction.
    pub fn broadcast(&self, tx: &Transaction) -> Res<Txid> {
        let tx = tx.clone();
        runtime()?.block_on(self.with_node(|r, _, _| async move {
            tokio::time::timeout(CALL_TIMEOUT, r.submit_package(tx.clone()))
                .await
                .map_err(|_| "no peer took the transaction in time".to_string())?
                .map_err(err)?;
            Ok(tx.compute_txid())
        }))
    }

    /// Rough rates from what the last three blocks paid on average (sat/vB).
    /// No mempool here, so this lags when fees move fast.
    pub fn fee_rates(&self) -> Res<FeeRates> {
        runtime()?.block_on(self.with_node(|r, _, recent| async move {
            let mut rates = Vec::new();
            for hash in recent.into_iter().take(3) {
                let rate = r.average_fee_rate(hash).await.map_err(err)?;
                rates.push(rate.to_sat_per_kwu() as f64 / 250.0);
            }
            if rates.is_empty() {
                return Err("no fee estimates".into());
            }
            rates.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let round = |r: f64| (r.max(1.0) * 10.0).ceil() / 10.0;
            let (slow, normal, fast) = (rates[0], rates[rates.len() / 2], rates[rates.len() - 1]);
            Ok(FeeRates { fast: round(fast), normal: round(normal), slow: round(slow) })
        }))
    }
}

fn runtime() -> Res<tokio::runtime::Runtime> {
    tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().map_err(err)
}

async fn log(mut info: bip157::Receiver<bip157::Info>, mut warn: bip157::Receiver<bip157::Warning>) {
    loop {
        tokio::select! {
            i = info.recv() => match i {
                Some(bip157::Info::FilterAdded(h)) if h % 5_000 != 0 => {}
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

    /// Against signet's real peers: `cargo test p2p -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn p2p_syncs_signet() {
        let desc = "wpkh([9122d9e0/84'/1'/0']tpubDCYVtmaSaDzTxcgvoP5AHZNbZKZzrvoNH9KARep88vESc6MxRqAp4LmePc2eeGX6XUxBcdhAmkthWTDqygPz2wLAyHWisD299Lkdrj5egY6/<0;1>/*)";
        let mut w = Wallet::create_from_two_path_descriptor(desc.to_string()).network(Network::Signet).create_wallet_no_persist().unwrap();
        let mut p = P2p::new(Network::Signet, "", "").unwrap();
        let t = std::time::Instant::now();
        let update = P2p::run(p.prepare(&w).unwrap()).unwrap();
        w.apply_update(update).unwrap();
        eprintln!(
            "synced in {:?}: tip {}, {} txs, balance {}",
            t.elapsed(),
            w.latest_checkpoint().height(),
            w.transactions().count(),
            w.balance().total()
        );
        // reopened from what settings.json would keep: syncs only new blocks
        let json = serde_json::to_vec(&w.take_staged().unwrap()).unwrap();
        eprintln!("saved state: {} KB", json.len() / 1024);
        let mut w = Wallet::load().load_wallet_no_persist(serde_json::from_slice(&json).unwrap()).unwrap().unwrap();
        let txs = w.transactions().count();
        let t = std::time::Instant::now();
        let update = P2p::run(p.prepare(&w).unwrap()).unwrap();
        w.apply_update(update).unwrap();
        eprintln!("reopened and synced in {:?}", t.elapsed());
        assert!(w.transactions().count() >= txs);
        p.start = start_for(&w);
        eprintln!("fee node starts at {:?}", p.start);
        eprintln!("fees {:?}", p.fee_rates().unwrap());
    }

    /// Headers only, from a known signet block: `cargo test p2p -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn p2p_fee_rates_signet() {
        let mut p = P2p::new(Network::Signet, "", "").unwrap();
        p.start = Some(HashCheckpoint::try_from((194932, "0000014197afc2080256e22cb07ade59fbc1f9360a424ea97f55b38df43c7f08".to_string())).unwrap());
        let t = std::time::Instant::now();
        let fees = p.fee_rates().unwrap();
        eprintln!("fees {fees:?} in {:?}", t.elapsed());
        assert!(fees.slow <= fees.normal && fees.normal <= fees.fast);
    }

    /// The same over the built-in Tor: `cargo test p2p_over_tor -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn p2p_over_tor_signet() {
        let dir = std::env::temp_dir().join("dove-tor-test");
        crate::tor::set_dir(dir);
        let t = std::time::Instant::now();
        let proxy = crate::tor::proxy().unwrap();
        eprintln!("tor up at {proxy} in {:?}", t.elapsed());
        let mut p = P2p::new(Network::Signet, "", &format!("socks5://{proxy}")).unwrap();
        p.start = Some(HashCheckpoint::try_from((194932, "0000014197afc2080256e22cb07ade59fbc1f9360a424ea97f55b38df43c7f08".to_string())).unwrap());
        let t = std::time::Instant::now();
        let fees = p.fee_rates().unwrap();
        eprintln!("fees over tor {fees:?} in {:?}", t.elapsed());
    }
}
