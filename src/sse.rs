//! Bounded blocking Server-Sent Events reader used by Zelda Notes.

use reqwest::Proxy;
use reqwest::blocking::Client;
use reqwest::header::{HeaderMap, HeaderName, HeaderValue};
use std::io::{BufRead as _, BufReader};
use std::time::Duration;

#[derive(Debug, Clone, Default)]
pub struct ServerSentEvent { data: String }
impl ServerSentEvent { pub fn data(&self) -> &str { &self.data } }

#[derive(Debug, Clone, Default)]
pub struct SseResponse { status: u16 }
impl SseResponse { pub fn status(&self) -> u16 { self.status } }

pub struct SseClient { proxy_url: String }
impl SseClient {
    pub fn new(proxy_url: String) -> Self { Self { proxy_url } }

    pub fn stream<F, S>(&self, url: &str, headers: &[String], mut on_event: F, should_stop: S, read_timeout_seconds: u64, max_event_bytes: usize) -> anyhow::Result<SseResponse>
    where
        F: FnMut(&ServerSentEvent) -> bool,
        S: Fn() -> bool,
    {
        let mut builder = Client::builder().read_timeout(Duration::from_secs(read_timeout_seconds.max(1))).redirect(reqwest::redirect::Policy::limited(4));
        if !self.proxy_url.trim().is_empty() { builder = builder.proxy(Proxy::all(&self.proxy_url)?); }
        let client = builder.build()?;
        let mut header_map = HeaderMap::new();
        for line in headers {
            let Some((name, value)) = line.split_once(':') else { anyhow::bail!("invalid SSE header"); };
            header_map.append(HeaderName::from_bytes(name.trim().as_bytes())?, HeaderValue::from_str(value.trim())?);
        }
        let response = client.get(url).headers(header_map).send()?;
        let status = response.status().as_u16();
        if status / 100 != 2 { return Ok(SseResponse { status }); }
        let mut reader = BufReader::new(response);
        let mut line = String::new();
        let mut data = String::new();
        loop {
            if should_stop() { break; }
            line.clear();
            match reader.read_line(&mut line) {
                Ok(0) => break,
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::TimedOut || error.kind() == std::io::ErrorKind::WouldBlock => {
                    if should_stop() { break; }
                    continue;
                }
                Err(error) => return Err(error.into()),
            }
            let normalized = line.trim_end_matches(['\r', '\n']);
            if normalized.is_empty() {
                if !data.is_empty() {
                    if data.ends_with('\n') { data.pop(); }
                    let event = ServerSentEvent { data: std::mem::take(&mut data) };
                    if !on_event(&event) { break; }
                }
                continue;
            }
            if normalized.starts_with(':') { continue; }
            if let Some(value) = normalized.strip_prefix("data:") {
                let value = value.strip_prefix(' ').unwrap_or(value);
                anyhow::ensure!(data.len().saturating_add(value.len()).saturating_add(1) <= max_event_bytes, "SSE event exceeded safety limit");
                data.push_str(value);
                data.push('\n');
            }
        }
        Ok(SseResponse { status })
    }
}
