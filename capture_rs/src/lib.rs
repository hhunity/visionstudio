use std::collections::VecDeque;
use std::ffi::{CStr, CString};
use std::io::{BufRead, BufReader, Write};
use std::net::{Shutdown, TcpStream};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::{Arc, Condvar, Mutex};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;

// ---------------------------------------------------------------------------
// State constants (match capture_rs.h)
// ---------------------------------------------------------------------------

pub const STATE_DISCONNECTED: u32 = 0;
pub const STATE_CONNECTING: u32 = 1;
pub const STATE_CONNECTED: u32 = 2;
pub const STATE_ERROR: u32 = 3;

pub const EVENT_DISCONNECTED: u32 = 0;
pub const EVENT_ERROR: u32 = 1;
pub const EVENT_CAPTURE_DONE: u32 = 2;

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

#[derive(Clone)]
struct Config {
    host: String,
    port: u16,
    connect_path: String,
    start_path: String,
    stop_path: String,
    disconnect_path: String,
    sse_path: String,
    timeout_ms: u64,
}

impl Config {
    fn base_url(&self) -> String {
        format!("http://{}:{}", self.host, self.port)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Cmd {
    Connect,
    StartCapture,
    StopCapture,
    Disconnect,
}

#[derive(Clone)]
struct Event {
    kind: u32,
    path: String,
    message: String,
}

// ---------------------------------------------------------------------------
// Shared state — accessed by all threads via Arc
// ---------------------------------------------------------------------------

struct Shared {
    cfg: Config,

    cmd_queue: Mutex<VecDeque<Cmd>>,
    cmd_cv: Condvar,
    shutdown: AtomicBool,

    // worker waits on this after spawning the SSE thread
    sse_ready: Mutex<(bool, bool)>, // (signaled, ok)
    sse_ready_cv: Condvar,

    // held so interrupt_sse() can call shutdown() on the live socket
    sse_tcp: Mutex<Option<TcpStream>>,
    sse_interrupted: AtomicBool,

    sse_state: AtomicU32,

    events: Mutex<VecDeque<Event>>,
    last_error: Mutex<String>,
    logger: Mutex<Option<Box<dyn Fn(&str) + Send + Sync>>>,
}

impl Shared {
    fn log(&self, msg: &str) {
        if let Ok(g) = self.logger.lock() {
            if let Some(f) = &*g {
                f(msg);
            }
        }
    }

    fn set_error(&self, msg: String) {
        *self.last_error.lock().unwrap() = msg;
    }

    fn get_error(&self) -> String {
        self.last_error.lock().unwrap().clone()
    }

    fn push_event(&self, ev: Event) {
        self.events.lock().unwrap().push_back(ev);
    }

    fn poll_event(&self) -> Option<Event> {
        self.events.lock().ok()?.pop_front()
    }

    fn signal_sse_ready(&self, ok: bool) {
        *self.sse_ready.lock().unwrap() = (true, ok);
        self.sse_ready_cv.notify_one();
    }

    // Safe to call from any thread; sets flag first to handle pre-store races.
    fn interrupt_sse(&self) {
        self.sse_interrupted.store(true, Ordering::Release);
        if let Ok(g) = self.sse_tcp.lock() {
            if let Some(s) = &*g {
                let _ = s.shutdown(Shutdown::Both);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SSE thread — raw TCP so we can call shutdown() to interrupt
// ---------------------------------------------------------------------------

fn run_sse(shared: Arc<Shared>) {
    let cfg = &shared.cfg;
    let addr = format!("{}:{}", cfg.host, cfg.port);
    let log_url = format!("{}:{}{}", cfg.host, cfg.port, cfg.sse_path);

    shared.log(&format!("[sse] GET {}", log_url));

    let stream = match TcpStream::connect(&addr) {
        Ok(s) => s,
        Err(e) => {
            shared.set_error(format!("SSE: connect failed: {}", e));
            shared.sse_state.store(STATE_ERROR, Ordering::Release);
            shared.signal_sse_ready(false);
            return;
        }
    };

    // Clone for the interrupt handle; original is used for read/write below.
    let interrupt_clone = match stream.try_clone() {
        Ok(s) => s,
        Err(e) => {
            shared.set_error(format!("SSE: clone failed: {}", e));
            shared.signal_sse_ready(false);
            return;
        }
    };

    {
        let mut g = shared.sse_tcp.lock().unwrap();
        *g = Some(interrupt_clone);
        // If interrupt arrived before we stored the handle, shut down now.
        if shared.sse_interrupted.load(Ordering::Acquire) {
            g.as_ref().unwrap().shutdown(Shutdown::Both).ok();
        }
    }

    let request = format!(
        "GET {} HTTP/1.1\r\nHost: {}:{}\r\nAccept: text/event-stream\r\n\
         Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n",
        cfg.sse_path, cfg.host, cfg.port
    );

    if let Err(e) = (&stream).write_all(request.as_bytes()) {
        shared.set_error(format!("SSE: write failed: {}", e));
        shared.sse_state.store(STATE_ERROR, Ordering::Release);
        shared.signal_sse_ready(false);
        return;
    }

    let mut reader = BufReader::new(&stream);
    let mut line = String::new();

    // Status line
    line.clear();
    if reader.read_line(&mut line).unwrap_or(0) == 0 {
        shared.set_error("SSE: empty response".to_string());
        shared.sse_state.store(STATE_ERROR, Ordering::Release);
        shared.signal_sse_ready(false);
        return;
    }

    let status = line.split_whitespace()
        .nth(1)
        .and_then(|s| s.parse::<u32>().ok())
        .unwrap_or(0);

    shared.log(&format!("[sse] GET {} -> {}", log_url, status));

    if !(200..300).contains(&status) {
        shared.set_error(format!("SSE: HTTP {}", status));
        shared.sse_state.store(STATE_ERROR, Ordering::Release);
        shared.signal_sse_ready(false);
        return;
    }

    // Skip headers (blank line terminates)
    loop {
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => {
                shared.signal_sse_ready(false);
                return;
            }
            Ok(_) => {}
        }
        if line.trim().is_empty() {
            break;
        }
    }

    shared.signal_sse_ready(true);

    // Read SSE event lines
    let mut cur_event = String::new();
    let mut cur_data = String::new();

    loop {
        if shared.shutdown.load(Ordering::Acquire) || shared.sse_interrupted.load(Ordering::Acquire) {
            break;
        }

        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }

        let trimmed = line.trim_end_matches(|c| c == '\r' || c == '\n');
        shared.log(&format!("[sse] << {}", trimmed));

        if trimmed.is_empty() {
            if !cur_event.is_empty() || !cur_data.is_empty() {
                dispatch_event(&shared, &cur_event, &cur_data);
                cur_event.clear();
                cur_data.clear();
            }
        } else if let Some(rest) = trimmed.strip_prefix("event:") {
            cur_event = rest.trim().to_string();
        } else if let Some(rest) = trimmed.strip_prefix("data:") {
            cur_data = rest.trim().to_string();
        }
    }

    { *shared.sse_tcp.lock().unwrap() = None; }

    shared.log(&format!("[sse] GET {} closed", log_url));
    let s = shared.sse_state.load(Ordering::Acquire);
    if s != STATE_ERROR && s != STATE_DISCONNECTED {
        shared.sse_state.store(STATE_DISCONNECTED, Ordering::Release);
    }
}

fn dispatch_event(shared: &Shared, event_type: &str, data: &str) {
    let get_str = |key: &str| -> String {
        serde_json::from_str::<serde_json::Value>(data).ok()
            .and_then(|v| v.get(key)?.as_str().map(str::to_string))
            .unwrap_or_default()
    };

    match event_type {
        "disconnected" => shared.push_event(Event { kind: EVENT_DISCONNECTED, path: String::new(), message: String::new() }),
        "error" => shared.push_event(Event { kind: EVENT_ERROR, path: String::new(), message: get_str("message") }),
        "capture_done" => {
            let path = get_str("path");
            if !path.is_empty() {
                shared.push_event(Event { kind: EVENT_CAPTURE_DONE, path, message: String::new() });
            }
        }
        _ => {}
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers — ureq for short-lived POST/PUT requests
// ---------------------------------------------------------------------------

fn make_agent(cfg: &Config) -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout(Duration::from_millis(cfg.timeout_ms))
        .build()
}

fn do_simple_post(shared: &Shared, path: &str, label: &str) -> bool {
    let url = format!("{}{}", shared.cfg.base_url(), path);
    shared.log(&format!("[http] POST {}", url));
    match make_agent(&shared.cfg).post(&url).call() {
        Ok(resp) => {
            shared.log(&format!("[http] POST {} -> {}", url, resp.status()));
            if resp.status() < 300 { return true; }
            shared.set_error(format!("{}: HTTP {}", label, resp.status()));
            false
        }
        Err(ureq::Error::Status(code, _)) => {
            shared.log(&format!("[http] POST {} -> {}", url, code));
            shared.set_error(format!("{}: HTTP {}", label, code));
            false
        }
        Err(e) => {
            shared.log(&format!("[http] POST {} -> error: {}", url, e));
            shared.set_error(format!("{}: POST failed: {}", label, e));
            false
        }
    }
}

fn do_connect_put(shared: &Shared) -> bool {
    let cfg = &shared.cfg;
    let url = format!("{}{}", cfg.base_url(), cfg.connect_path);
    shared.log(&format!("[http] PUT {}", url));

    let meta = serde_json::json!({
        "host": cfg.host, "port": cfg.port,
        "connect_path": cfg.connect_path, "start_path": cfg.start_path,
        "stop_path": cfg.stop_path, "disconnect_path": cfg.disconnect_path,
        "sse_path": cfg.sse_path, "timeout_ms": cfg.timeout_ms,
    });

    let boundary = "----CaptureBoundary";
    let mut body = String::new();
    body.push_str(&format!("--{}\r\n", boundary));
    body.push_str("Content-Disposition: form-data; name=\"config\"\r\n");
    body.push_str("Content-Type: application/json\r\n\r\n");
    body.push_str(&meta.to_string());
    body.push_str("\r\n");
    body.push_str(&format!("--{}--\r\n", boundary));

    let ct = format!("multipart/form-data; boundary={}", boundary);
    match make_agent(cfg).put(&url).set("Content-Type", &ct).send_bytes(body.as_bytes()) {
        Ok(resp) => {
            shared.log(&format!("[http] PUT {} -> {}", url, resp.status()));
            if resp.status() < 300 { return true; }
            shared.set_error(format!("connect: HTTP {}", resp.status()));
            false
        }
        Err(ureq::Error::Status(code, _)) => {
            shared.log(&format!("[http] PUT {} -> {}", url, code));
            shared.set_error(format!("connect: HTTP {}", code));
            false
        }
        Err(e) => {
            shared.log(&format!("[http] PUT {} -> error: {}", url, e));
            shared.set_error(format!("connect: PUT failed: {}", e));
            false
        }
    }
}

// ---------------------------------------------------------------------------
// Worker thread — serialises all commands
// ---------------------------------------------------------------------------

fn worker_thread(shared: Arc<Shared>, sse_handle: Arc<Mutex<Option<JoinHandle<()>>>>) {
    loop {
        let cmd = {
            let mut q = shared.cmd_queue.lock().unwrap();
            loop {
                if shared.shutdown.load(Ordering::Acquire) { return; }
                if let Some(c) = q.pop_front() { break c; }
                q = shared.cmd_cv.wait(q).unwrap();
            }
        };

        let state = shared.sse_state.load(Ordering::Acquire);

        match cmd {
            Cmd::Connect => {
                if state != STATE_DISCONNECTED { continue; }

                *shared.sse_ready.lock().unwrap() = (false, false);
                shared.sse_state.store(STATE_CONNECTING, Ordering::Release);
                shared.sse_interrupted.store(false, Ordering::Release);

                if let Some(h) = sse_handle.lock().unwrap().take() {
                    h.join().ok();
                }

                let shared2 = shared.clone();
                *sse_handle.lock().unwrap() = Some(thread::spawn(move || run_sse(shared2)));

                let ok = {
                    let g = shared.sse_ready.lock().unwrap();
                    shared.sse_ready_cv.wait_while(g, |state| {
                        !state.0 && !shared.shutdown.load(Ordering::Acquire)
                    }).unwrap().1
                };

                if shared.shutdown.load(Ordering::Acquire) || !ok { continue; }

                if do_connect_put(&shared) {
                    shared.sse_state.store(STATE_CONNECTED, Ordering::Release);
                } else {
                    shared.sse_state.store(STATE_ERROR, Ordering::Release);
                }
            }

            Cmd::StartCapture => {
                if state != STATE_CONNECTED { continue; }
                if !do_simple_post(&shared, &shared.cfg.start_path.clone(), "start") {
                    shared.push_event(Event { kind: EVENT_ERROR, path: String::new(), message: shared.get_error() });
                }
            }

            Cmd::StopCapture => {
                if state != STATE_CONNECTED { continue; }
                if !do_simple_post(&shared, &shared.cfg.stop_path.clone(), "stop") {
                    shared.push_event(Event { kind: EVENT_ERROR, path: String::new(), message: shared.get_error() });
                }
            }

            Cmd::Disconnect => {
                if state == STATE_DISCONNECTED { continue; }
                shared.interrupt_sse();
                // Fire-and-forget; ignore errors (server may already be gone)
                do_simple_post(&shared, &shared.cfg.disconnect_path.clone(), "disconnect");
                shared.sse_state.store(STATE_DISCONNECTED, Ordering::Release);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public Rust API
// ---------------------------------------------------------------------------

pub struct CaptureClient {
    shared: Arc<Shared>,
    worker_handle: Option<JoinHandle<()>>,
    sse_handle: Arc<Mutex<Option<JoinHandle<()>>>>,
}

impl CaptureClient {
    pub(crate) fn new(cfg: Config) -> Self {
        let shared = Arc::new(Shared {
            cfg,
            cmd_queue: Mutex::new(VecDeque::new()),
            cmd_cv: Condvar::new(),
            shutdown: AtomicBool::new(false),
            sse_ready: Mutex::new((false, false)),
            sse_ready_cv: Condvar::new(),
            sse_tcp: Mutex::new(None),
            sse_interrupted: AtomicBool::new(false),
            sse_state: AtomicU32::new(STATE_DISCONNECTED),
            events: Mutex::new(VecDeque::new()),
            last_error: Mutex::new(String::new()),
            logger: Mutex::new(None),
        });

        let sse_handle = Arc::new(Mutex::new(None::<JoinHandle<()>>));
        let shared2 = shared.clone();
        let sse_handle2 = sse_handle.clone();
        let worker_handle = thread::spawn(move || worker_thread(shared2, sse_handle2));

        CaptureClient { shared, worker_handle: Some(worker_handle), sse_handle }
    }

    fn push(&self, cmd: Cmd) {
        self.shared.cmd_queue.lock().unwrap().push_back(cmd);
        self.shared.cmd_cv.notify_one();
    }

    pub fn connect(&self)       { self.push(Cmd::Connect); }
    pub fn disconnect(&self)    { self.push(Cmd::Disconnect); self.shared.interrupt_sse(); }
    pub fn start_capture(&self) { self.push(Cmd::StartCapture); }
    pub fn stop_capture(&self)  { self.push(Cmd::StopCapture); }

    pub fn get_state(&self) -> u32 { self.shared.sse_state.load(Ordering::Acquire) }
    pub fn get_last_error(&self) -> String { self.shared.get_error() }
    pub(crate) fn poll_event_raw(&self) -> Option<Event> { self.shared.poll_event() }

    pub fn set_logger(&self, f: impl Fn(&str) + Send + Sync + 'static) {
        *self.shared.logger.lock().unwrap() = Some(Box::new(f));
    }
}

impl Drop for CaptureClient {
    fn drop(&mut self) {
        self.shared.shutdown.store(true, Ordering::Release);
        self.shared.cmd_cv.notify_all();
        self.shared.signal_sse_ready(false);
        self.shared.interrupt_sse();

        if let Some(h) = self.sse_handle.lock().unwrap().take() {
            h.join().ok();
        }
        if let Some(h) = self.worker_handle.take() {
            h.join().ok();
        }
    }
}

// ---------------------------------------------------------------------------
// C FFI
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct CCaptureEvent {
    pub kind: c_int,
    pub path: [c_char; 512],
    pub message: [c_char; 512],
}

fn write_cstr(s: &str, buf: &mut [c_char]) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(buf.len() - 1);
    for (i, &b) in bytes[..n].iter().enumerate() {
        buf[i] = b as c_char;
    }
    buf[n] = 0;
}

unsafe fn ptr_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() { return String::new(); }
    CStr::from_ptr(ptr).to_string_lossy().into_owned()
}

#[no_mangle]
pub extern "C" fn capture_client_new(
    host: *const c_char,
    port: c_int,
    connect_path: *const c_char,
    start_path: *const c_char,
    stop_path: *const c_char,
    disconnect_path: *const c_char,
    sse_path: *const c_char,
    timeout_ms: c_int,
) -> *mut CaptureClient {
    let cfg = unsafe {
        Config {
            host: ptr_to_string(host),
            port: port as u16,
            connect_path: ptr_to_string(connect_path),
            start_path: ptr_to_string(start_path),
            stop_path: ptr_to_string(stop_path),
            disconnect_path: ptr_to_string(disconnect_path),
            sse_path: ptr_to_string(sse_path),
            timeout_ms: timeout_ms as u64,
        }
    };
    Box::into_raw(Box::new(CaptureClient::new(cfg)))
}

#[no_mangle]
pub extern "C" fn capture_client_free(client: *mut CaptureClient) {
    if !client.is_null() {
        drop(unsafe { Box::from_raw(client) });
    }
}

#[no_mangle]
pub extern "C" fn capture_client_connect(client: *mut CaptureClient) {
    if let Some(c) = unsafe { client.as_ref() } { c.connect(); }
}

#[no_mangle]
pub extern "C" fn capture_client_disconnect(client: *mut CaptureClient) {
    if let Some(c) = unsafe { client.as_ref() } { c.disconnect(); }
}

#[no_mangle]
pub extern "C" fn capture_client_start_capture(client: *mut CaptureClient) {
    if let Some(c) = unsafe { client.as_ref() } { c.start_capture(); }
}

#[no_mangle]
pub extern "C" fn capture_client_stop_capture(client: *mut CaptureClient) {
    if let Some(c) = unsafe { client.as_ref() } { c.stop_capture(); }
}

#[no_mangle]
pub extern "C" fn capture_client_get_state(client: *const CaptureClient) -> c_int {
    unsafe { client.as_ref() }.map(|c| c.get_state() as c_int).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn capture_client_poll_event(
    client: *mut CaptureClient,
    out: *mut CCaptureEvent,
) -> c_int {
    let c = match unsafe { client.as_ref() } { Some(c) => c, None => return 0 };
    match c.poll_event_raw() {
        None => 0,
        Some(ev) => {
            if !out.is_null() {
                let out = unsafe { &mut *out };
                out.kind = ev.kind as c_int;
                write_cstr(&ev.path, &mut out.path);
                write_cstr(&ev.message, &mut out.message);
            }
            1
        }
    }
}

#[no_mangle]
pub extern "C" fn capture_client_get_last_error(
    client: *const CaptureClient,
    buf: *mut c_char,
    buf_len: c_int,
) {
    if buf.is_null() || buf_len <= 0 { return; }
    let err = unsafe { client.as_ref() }.map(|c| c.get_last_error()).unwrap_or_default();
    let slice = unsafe { std::slice::from_raw_parts_mut(buf, buf_len as usize) };
    write_cstr(&err, slice);
}

// Logger: fn(msg, userdata).  userdata lifetime is the caller's responsibility.
type CLogFn = unsafe extern "C" fn(*const c_char, *mut c_void);

#[no_mangle]
pub extern "C" fn capture_client_set_logger(
    client: *mut CaptureClient,
    log_fn: Option<CLogFn>,
    userdata: *mut c_void,
) {
    let c = match unsafe { client.as_ref() } { Some(c) => c, None => return };
    match log_fn {
        None => { *c.shared.logger.lock().unwrap() = None; }
        Some(f) => {
            let ud = userdata as usize; // make Send by erasing the pointer type
            c.set_logger(move |msg| {
                if let Ok(s) = CString::new(msg) {
                    unsafe { f(s.as_ptr(), ud as *mut c_void) };
                }
            });
        }
    }
}
