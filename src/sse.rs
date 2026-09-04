//! Bounded blocking Server-Sent Events reader used by Zelda Notes.

use std::io::{BufRead as _, BufReader};
use std::time::Duration;
use ureq::{Agent, Proxy};

#[derive(Debug, Clone, Default)]
pub struct ServerSentEvent {
    data: String,
}

impl ServerSentEvent {
    pub fn data(&self) -> &str { &self.data }
}

#[derive(Debug, Clone, Default)]
pub struct SseResponse {
    status: u16,
}

impl SseResponse {
    pub fn status(&self) -> u16 { self.status }
}

pub struct SseClient {
    proxy_url: String,
}

impl SseClient {
    pub fn new(proxy_url: String) -> Self { Self { proxy_url } }

    pub fn stream<F, S>(
        &self,
        url: &str,
        headers: &[String],
        mut on_event: F,
        should_stop: S,
        read_timeout_seconds: u64,
        max_event_bytes: usize,
    ) -> anyhow::Result<SseResponse>
    where
        F: FnMut(&ServerSentEvent) -> bool,
        S: Fn() -> bool,
    {
        let timeout = Duration::from_secs(read_timeout_seconds.max(1));
        let mut config = Agent::config_builder()
            .https_only(true)
            .http_status_as_error(false)
            .max_redirects(4)
            .timeout_connect(Some(timeout))
            .timeout_recv_response(Some(timeout))
            .timeout_recv_body(Some(timeout));

        if !self.proxy_url.trim().is_empty() {
            // ureq SOCKS5 delegates DNS to the proxy by default, which is the
            // behavior denoted by the common `socks5h` spelling.
            let normalized_proxy = if let Some(rest) = self.proxy_url.strip_prefix("socks5h://") {
                format!("socks5://{rest}")
            } else {
                self.proxy_url.clone()
            };
            config = config.proxy(Some(Proxy::new(&normalized_proxy)?));
        }

        let agent: Agent = config.build().into();
        let mut request = agent.get(url);
        for line in headers {
            let Some((name, value)) = line.split_once(':') else {
                anyhow::bail!("invalid SSE header");
            };
            let name = name.trim();
            let value = value.trim();
            anyhow::ensure!(!name.is_empty(), "invalid SSE header name");
            anyhow::ensure!(
                !value.contains(['\r', '\n']),
                "invalid SSE header value"
            );
            request = request.header(name, value);
        }

        let response = request.call()?;
        let status = response.status().as_u16();
        if status / 100 != 2 {
            return Ok(SseResponse { status });
        }

        let (_, body) = response.into_parts();
        let mut reader = BufReader::new(body.into_reader());
        let mut line = String::new();
        let mut data = String::new();

        loop {
            if should_stop() {
                break;
            }

            line.clear();
            match reader.read_line(&mut line) {
                Ok(0) => break,
                Ok(_) => {}
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
                    ) =>
                {
                    if should_stop() {
                        break;
                    }
                    continue;
                }
                Err(error) => return Err(error.into()),
            }

            let normalized = line.trim_end_matches(|character| character == '\r' || character == '\n');
            if normalized.is_empty() {
                if !data.is_empty() {
                    if data.ends_with('\n') {
                        data.pop();
                    }
                    let event = ServerSentEvent {
                        data: std::mem::take(&mut data),
                    };
                    if !on_event(&event) {
                        break;
                    }
                }
                continue;
            }

            if normalized.starts_with(':') {
                continue;
            }

            if let Some(value) = normalized.strip_prefix("data:") {
                let value = value.strip_prefix(' ').unwrap_or(value);
                anyhow::ensure!(
                    data.len().saturating_add(value.len()).saturating_add(1) <= max_event_bytes,
                    "SSE event exceeded safety limit"
                );
                data.push_str(value);
                data.push('\n');
            }
        }

        Ok(SseResponse { status })
    }
}
