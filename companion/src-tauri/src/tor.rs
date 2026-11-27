//! Tor built in, with arti (the Tor Project's Tor in Rust): nothing to
//! install. It starts on first use and runs until Dove quits, behind a
//! SOCKS5 proxy on a random localhost port that the broadcast's p2p node
//! ([`crate::p2p`]) uses like any other proxy.

use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::sync::{mpsc, Mutex, OnceLock};
use std::time::Duration;

use arti_client::{config::TorClientConfigBuilder, DangerouslyIntoTorAddr, IntoTorAddr, StreamPrefs, TorAddr, TorClient};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

use crate::Res;

/// Where arti keeps its state and its copy of the network's directory, so
/// later starts are quicker. Set once at startup.
static DIR: OnceLock<PathBuf> = OnceLock::new();
/// Off, starting (the bootstrap's answer still to come), or up at a port.
/// A start that outlasts one caller's wait stays here for the next caller,
/// so there's only ever one arti on the state folder.
enum State {
    Off,
    Starting(mpsc::Receiver<Res<SocketAddr>>),
    Up(SocketAddr),
}
static STATE: Mutex<State> = Mutex::new(State::Off);

const BOOTSTRAP_TIMEOUT: Duration = Duration::from_secs(120);

pub fn set_dir(dir: PathBuf) {
    let _ = DIR.set(dir);
}

/// The local SOCKS5 address of the built-in Tor, starting it if it isn't
/// running yet (that takes a few seconds, longer the first time).
pub fn proxy() -> Res<SocketAddr> {
    let mut state = STATE.lock().unwrap();
    if let State::Up(addr) = *state {
        return Ok(addr);
    }
    if let State::Off = *state {
        let dir = DIR.get().cloned().ok_or("Tor: no folder for its state")?;
        let (tx, rx) = mpsc::channel::<Res<SocketAddr>>();
        std::thread::Builder::new()
            .name("tor".into())
            .spawn(move || {
                let rt = match tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build() {
                    Ok(rt) => rt,
                    Err(e) => return drop(tx.send(Err(e.to_string()))),
                };
                rt.block_on(serve(dir, tx));
            })
            .map_err(|e| e.to_string())?;
        *state = State::Starting(rx);
    }
    let State::Starting(rx) = &*state else { unreachable!() };
    match rx.recv_timeout(BOOTSTRAP_TIMEOUT) {
        Ok(Ok(addr)) => {
            *state = State::Up(addr);
            Ok(addr)
        }
        // arti gave up and its thread ended, so a later call can try again
        Ok(Err(e)) => {
            *state = State::Off;
            Err(e)
        }
        Err(mpsc::RecvTimeoutError::Timeout) => Err("Tor didn't connect in time. Try again in a minute.".into()),
        Err(mpsc::RecvTimeoutError::Disconnected) => {
            *state = State::Off;
            Err("Tor stopped".into())
        }
    }
}

async fn serve(dir: PathBuf, ready: mpsc::Sender<Res<SocketAddr>>) {
    let config = match TorClientConfigBuilder::from_directories(dir.join("state"), dir.join("cache")).build() {
        Ok(c) => c,
        Err(e) => return drop(ready.send(Err(format!("Tor: {e}")))),
    };
    let tor = match TorClient::create_bootstrapped(config).await {
        Ok(t) => t,
        Err(e) => return drop(ready.send(Err(format!("Tor: {e}")))),
    };
    let listener = match TcpListener::bind("127.0.0.1:0").await {
        Ok(l) => l,
        Err(e) => return drop(ready.send(Err(e.to_string()))),
    };
    let _ = ready.send(listener.local_addr().map_err(|e| e.to_string()));
    while let Ok((stream, _)) = listener.accept().await {
        let tor = tor.clone();
        tokio::spawn(async move {
            if let Err(e) = socks5(stream, &tor).await {
                eprintln!("tor: {e}");
            }
        });
    }
}

/// Just enough SOCKS5 (RFC 1928) for our own clients: no authentication,
/// CONNECT, and Tor's RESOLVE (0xF0), which the p2p node uses to look up its
/// DNS seeds through Tor. Every connection gets a circuit of its own, so
/// each peer is reached through its own exit (most likely another one), and
/// the peer we send to and the one we hear it back from don't share one.
async fn socks5(mut s: TcpStream, tor: &TorClient<tor_rtcompat::PreferredRuntime>) -> Res<()> {
    let e = |e: std::io::Error| e.to_string();
    let mut prefs = StreamPrefs::new();
    prefs.isolate_every_stream();
    let mut head = [0u8; 2];
    s.read_exact(&mut head).await.map_err(e)?;
    let mut methods = vec![0u8; head[1] as usize];
    s.read_exact(&mut methods).await.map_err(e)?;
    if head[0] != 5 || !methods.contains(&0) {
        s.write_all(&[5, 0xff]).await.map_err(e)?;
        return Err("SOCKS: no supported method".into());
    }
    s.write_all(&[5, 0]).await.map_err(e)?;
    let mut req = [0u8; 4];
    s.read_exact(&mut req).await.map_err(e)?;
    if req[0] == 5 && req[1] == 0xF0 && req[3] == 3 {
        let len = s.read_u8().await.map_err(e)? as usize;
        let mut name = vec![0u8; len];
        s.read_exact(&mut name).await.map_err(e)?;
        let _port = s.read_u16().await.map_err(e)?;
        let name = String::from_utf8(name).map_err(|_| "SOCKS: bad host name".to_string())?;
        let ips = tor.resolve_with_prefs(&name, &prefs).await.map_err(|e| e.to_string());
        let reply = match ips.as_deref() {
            Ok([IpAddr::V4(ip), ..]) => [&[5, 0, 0, 1][..], &ip.octets(), &[0, 0]].concat(),
            Ok([IpAddr::V6(ip), ..]) => [&[5, 0, 0, 4][..], &ip.octets(), &[0, 0]].concat(),
            _ => vec![5, 4, 0, 1, 0, 0, 0, 0, 0, 0],
        };
        s.write_all(&reply).await.map_err(e)?;
        return ips.map(|_| ());
    }
    if req[0] != 5 || req[1] != 1 {
        s.write_all(&[5, 7, 0, 1, 0, 0, 0, 0, 0, 0]).await.map_err(e)?;
        return Err("SOCKS: only CONNECT".into());
    }
    let target: Res<TorAddr> = match req[3] {
        1 | 4 => {
            let mut ip = vec![0u8; if req[3] == 1 { 4 } else { 16 }];
            s.read_exact(&mut ip).await.map_err(e)?;
            let port = s.read_u16().await.map_err(e)?;
            let ip: IpAddr = if ip.len() == 4 {
                <[u8; 4]>::try_from(ip).unwrap().into()
            } else {
                <[u8; 16]>::try_from(ip).unwrap().into()
            };
            // a peer's address, not a name we looked up ourselves
            (ip, port).into_tor_addr_dangerously().map_err(|e| e.to_string())
        }
        3 => {
            let len = s.read_u8().await.map_err(e)? as usize;
            let mut name = vec![0u8; len];
            s.read_exact(&mut name).await.map_err(e)?;
            let port = s.read_u16().await.map_err(e)?;
            let name = String::from_utf8(name).map_err(|_| "SOCKS: bad host name".to_string())?;
            (name, port).into_tor_addr().map_err(|e| e.to_string())
        }
        _ => Err("SOCKS: unknown address type".into()),
    };
    let stream = match target {
        Ok(t) => tor.connect_with_prefs(t, &prefs).await.map_err(|e| e.to_string()),
        Err(err) => Err(err),
    };
    let mut stream = match stream {
        Ok(st) => st,
        Err(err) => {
            let _ = s.write_all(&[5, 4, 0, 1, 0, 0, 0, 0, 0, 0]).await; // host unreachable
            return Err(err);
        }
    };
    // which circuit and exit, to see that each connection has its own
    if let Some(t) = stream.client_stream_ctrl().and_then(|c| tor_proto::client::stream::ClientStreamCtrl::tunnel(&**c)) {
        let exit = t.last_hop_info().ok().flatten().map(|h| h.to_string()).unwrap_or_default();
        eprintln!("tor: circuit {} exit {exit}", t.unique_id());
    }
    s.write_all(&[5, 0, 0, 1, 0, 0, 0, 0, 0, 0]).await.map_err(e)?;
    let _ = tokio::io::copy_bidirectional(&mut s, &mut stream).await;
    Ok(())
}
