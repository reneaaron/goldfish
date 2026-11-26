//! Webcam QR reader (Linux, V4L2). Capture happens in Rust, not in the
//! webview: WebKitGTK's getUserMedia is unreliable. YUYV frames carry the
//! grey image the QR decoder wants in every other byte, so no conversion.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::thread::JoinHandle;

use v4l::buffer::Type;
use v4l::io::traits::CaptureStream;
use v4l::prelude::*;
use v4l::video::Capture;
use v4l::FourCC;

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde::Serialize;

use crate::Res;

/// What the page shows of the camera: a half-size grey frame (base64, one
/// byte a pixel) and the outline of each QR code found in it, `read` once
/// its content was decoded.
#[derive(Serialize, Clone)]
pub struct Preview {
    pub width: usize,
    pub height: usize,
    pub grey: String,
    pub codes: Vec<Outline>,
}

#[derive(Serialize, Clone)]
pub struct Outline {
    pub corners: [(i32, i32); 4],
    pub read: bool,
}

/// About 10 previews a second: enough to aim the camera by, and cheap.
const PREVIEW_EVERY: std::time::Duration = std::time::Duration::from_millis(100);

pub struct Camera {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Camera {
    /// Opens `device` and reads QR codes until stopped. `on_text` gets each
    /// QR whose content differs from the last one; returning true stops.
    /// `on_preview` gets a picture of what the camera sees, ~10 times a second.
    pub fn start(
        device: &str,
        mut on_text: impl FnMut(String) -> bool + Send + 'static,
        mut on_preview: impl FnMut(Preview) + Send + 'static,
    ) -> Res<Camera> {
        let stop = Arc::new(AtomicBool::new(false));
        let (ready_tx, ready_rx) = mpsc::channel::<Res<()>>();
        let device = device.to_string();
        let stop_flag = stop.clone();
        let thread = std::thread::spawn(move || {
            let opened = (|| -> std::io::Result<_> {
                let dev = Device::with_path(&device)?;
                let mut fmt = dev.format()?;
                fmt.width = 640;
                fmt.height = 480;
                fmt.fourcc = FourCC::new(b"YUYV");
                let fmt = dev.set_format(&fmt)?;
                Ok((dev, fmt))
            })();
            let (dev, fmt) = match opened {
                Ok(v) => v,
                Err(e) => return drop(ready_tx.send(Err(format!("camera {device}: {e}")))),
            };
            if fmt.fourcc != FourCC::new(b"YUYV") {
                return drop(ready_tx.send(Err(format!("camera {device} won't give YUYV frames ({})", fmt.fourcc))));
            }
            let mut stream = match MmapStream::with_buffers(&dev, Type::VideoCapture, 4) {
                Ok(s) => s,
                Err(e) => return drop(ready_tx.send(Err(format!("camera {device}: {e}")))),
            };
            let _ = ready_tx.send(Ok(()));
            let (w, h, stride) = (fmt.width as usize, fmt.height as usize, fmt.stride as usize);
            let mut last = String::new();
            let mut last_preview = std::time::Instant::now() - PREVIEW_EVERY;
            while !stop_flag.load(Ordering::Relaxed) {
                let Ok((buf, _)) = stream.next() else { break };
                if buf.len() < h * stride {
                    continue;
                }
                let mut img = rqrr::PreparedImage::prepare_from_greyscale(w, h, |x, y| buf[y * stride + 2 * x]);
                let mut codes = vec![];
                let mut found = None;
                for grid in img.detect_grids() {
                    let decoded = grid.decode().ok().map(|(_, text)| text);
                    codes.push(Outline {
                        corners: grid.bounds.map(|p| (p.x / 2, p.y / 2)),
                        read: decoded.is_some(),
                    });
                    if let Some(text) = decoded {
                        if text != last && found.is_none() {
                            found = Some(text);
                        }
                    }
                }
                if last_preview.elapsed() >= PREVIEW_EVERY {
                    last_preview = std::time::Instant::now();
                    let (pw, ph) = (w / 2, h / 2);
                    let mut grey = Vec::with_capacity(pw * ph);
                    for y in 0..ph {
                        let row = &buf[2 * y * stride..];
                        grey.extend((0..pw).map(|x| row[4 * x]));
                    }
                    on_preview(Preview { width: pw, height: ph, grey: B64.encode(grey), codes });
                }
                if let Some(text) = found {
                    last = text.clone();
                    if on_text(text) {
                        return;
                    }
                }
            }
        });
        ready_rx.recv().map_err(|e| e.to_string())??;
        Ok(Camera { stop, thread: Some(thread) })
    }
}

impl Drop for Camera {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            if t.thread().id() != std::thread::current().id() {
                let _ = t.join();
            }
        }
    }
}
