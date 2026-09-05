//! Bounded blocking Server-Sent Events reader used by Zelda Notes.

use std::io::Read as _;
use std::time::Duration;
use ureq::{Agent, Proxy};

const READ_POLL_SECONDS: u64 = 1;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ServerSentEvent {
    event: String,
    data: String,
    id: String,
    retry_milliseconds: i64,
}

impl ServerSentEvent {
    pub fn event(&self) -> &str { &self.event }
    pub fn data(&self) -> &str { &self.data }
    pub fn id(&self) -> &str { &self.id }
    pub fn retry_milliseconds(&self) -> i64 { self.retry_milliseconds }
}

#[derive(Debug, Clone, Default)]
pub struct SseResponse {
    status: u16,
}

impl SseResponse {
    pub fn status(&self) -> u16 { self.status }
}

struct ServerSentEventParser {
    max_event_bytes: usize,
    line: String,
    data: String,
    event: String,
    last_event_id: String,
    retry_milliseconds: i64,
    pending_cr: bool,
}

impl ServerSentEventParser {
    fn new(max_event_bytes: usize) -> anyhow::Result<Self> {
        anyhow::ensure!(max_event_bytes > 0, "SSE event size limit must be greater than zero");
        Ok(Self {
            max_event_bytes,
            line: String::new(),
            data: String::new(),
            event: String::new(),
            last_event_id: String::new(),
            retry_milliseconds: -1,
            pending_cr: false,
        })
    }

    fn feed<F>(&mut self, bytes: &[u8], on_event: &mut F) -> anyhow::Result<bool>
    where
        F: FnMut(&ServerSentEvent) -> bool,
    {
        for &byte in bytes {
            let character = char::from(byte);
            if self.pending_cr {
                self.pending_cr = false;
                if !self.process_line(on_event)? {
                    return Ok(false);
                }
                if character == '\n' {
                    continue;
                }
            }

            if character == '\r' {
                self.pending_cr = true;
            } else if character == '\n' {
                if !self.process_line(on_event)? {
                    return Ok(false);
                }
            } else {
                self.line.push(character);
                self.enforce_limit(self.line.len())?;
            }
        }
        Ok(true)
    }

    fn finish<F>(&mut self, on_event: &mut F) -> anyhow::Result<bool>
    where
        F: FnMut(&ServerSentEvent) -> bool,
    {
        if self.pending_cr {
            self.pending_cr = false;
            if !self.process_line(on_event)? {
                return Ok(false);
            }
        } else if !self.line.is_empty() && !self.process_line(on_event)? {
            return Ok(false);
        }
        self.line.clear();
        self.reset_event_fields();
        Ok(true)
    }

    fn enforce_limit(&self, pending_size: usize) -> anyhow::Result<()> {
        anyhow::ensure!(
            pending_size <= self.max_event_bytes
                && self.data.len() <= self.max_event_bytes.saturating_sub(pending_size),
            "SSE event exceeded configured size limit"
        );
        Ok(())
    }

    fn process_line<F>(&mut self, on_event: &mut F) -> anyhow::Result<bool>
    where
        F: FnMut(&ServerSentEvent) -> bool,
    {
        let line = std::mem::take(&mut self.line);
        if line.is_empty() {
            if self.data.is_empty() {
                self.reset_event_fields();
                return Ok(true);
            }
            if self.data.ends_with('\n') {
                self.data.pop();
            }
            let parsed = ServerSentEvent {
                event: if self.event.is_empty() {
                    "message".to_owned()
                } else {
                    std::mem::take(&mut self.event)
                },
                data: std::mem::take(&mut self.data),
                id: self.last_event_id.clone(),
                retry_milliseconds: self.retry_milliseconds,
            };
            self.reset_event_fields();
            return Ok(on_event(&parsed));
        }

        if line.starts_with(':') {
            return Ok(true);
        }
        let (field, mut value) = match line.split_once(':') {
            Some((field, value)) => (field, value),
            None => (line.as_str(), ""),
        };
        if let Some(stripped) = value.strip_prefix(' ') {
            value = stripped;
        }

        match field {
            "data" => {
                self.enforce_limit(value.len().saturating_add(1))?;
                self.data.push_str(value);
                self.data.push('\n');
            }
            "event" => {
                self.enforce_limit(value.len())?;
                self.event = value.to_owned();
            }
            "id" if !value.contains('\0') => {
                self.enforce_limit(value.len())?;
                self.last_event_id = value.to_owned();
            }
            "retry" if !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()) => {
                if let Ok(retry) = value.parse::<i64>() {
                    self.retry_milliseconds = retry;
                }
            }
            _ => {}
        }
        Ok(true)
    }

    fn reset_event_fields(&mut self) {
        self.data.clear();
        self.event.clear();
        self.retry_milliseconds = -1;
    }
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
        connect_timeout_seconds: u64,
        max_event_bytes: usize,
    ) -> anyhow::Result<SseResponse>
    where
        F: FnMut(&ServerSentEvent) -> bool,
        S: Fn() -> bool,
    {
        let connect_timeout = Duration::from_secs(connect_timeout_seconds.clamp(1, 24 * 60 * 60));
        let read_poll = Duration::from_secs(READ_POLL_SECONDS);
        let mut config = Agent::config_builder()
            .http_status_as_error(false)
            .max_redirects(4)
            .timeout_connect(Some(connect_timeout))
            .timeout_recv_response(Some(connect_timeout))
            .timeout_recv_body(Some(read_poll));

        if !self.proxy_url.trim().is_empty() {
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
            anyhow::ensure!(!value.contains(['\r', '\n']), "invalid SSE header value");
            request = request.header(name, value);
        }

        let response = request.call()?;
        let status = response.status().as_u16();
        if status / 100 != 2 {
            return Ok(SseResponse { status });
        }

        let (_, body) = response.into_parts();
        let mut reader = body.into_reader();
        let mut parser = ServerSentEventParser::new(max_event_bytes)?;
        let mut buffer = [0_u8; 16 * 1024];

        loop {
            if should_stop() {
                break;
            }
            match reader.read(&mut buffer) {
                Ok(0) => {
                    let _ = parser.finish(&mut on_event)?;
                    break;
                }
                Ok(read) => {
                    if !parser.feed(&buffer[..read], &mut on_event)? {
                        break;
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
                    ) =>
                {
                    if should_stop() {
                        break;
                    }
                }
                Err(error) => return Err(error.into()),
            }
        }

        Ok(SseResponse { status })
    }
}

#[cfg(test)]
mod tests {
    use super::ServerSentEventParser;

    #[test]
    fn parser_matches_event_id_retry_and_crlf_semantics() {
        let mut parser = ServerSentEventParser::new(1024).expect("parser");
        let mut events = Vec::new();
        let mut collect = |event: &super::ServerSentEvent| {
            events.push(event.clone());
            true
        };
        parser.feed(b"id: 7\r", &mut collect).expect("feed");
        parser.feed(b"\nevent: map\r\ndata: first\r\ndata: second\r\nretry: 1500\r\n\r\n", &mut collect).expect("feed");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].id(), "7");
        assert_eq!(events[0].event(), "map");
        assert_eq!(events[0].data(), "first\nsecond");
        assert_eq!(events[0].retry_milliseconds(), 1500);
    }

    #[test]
    fn last_event_id_persists_but_retry_does_not() {
        let mut parser = ServerSentEventParser::new(1024).expect("parser");
        let mut events = Vec::new();
        let mut collect = |event: &super::ServerSentEvent| {
            events.push(event.clone());
            true
        };
        parser.feed(b"id: abc\ndata: one\n\ndata: two\n\n", &mut collect).expect("feed");
        assert_eq!(events.len(), 2);
        assert_eq!(events[0].id(), "abc");
        assert_eq!(events[1].id(), "abc");
        assert_eq!(events[1].event(), "message");
        assert_eq!(events[1].retry_milliseconds(), -1);
    }

    #[test]
    fn invalid_id_with_nul_is_ignored() {
        let mut parser = ServerSentEventParser::new(1024).expect("parser");
        let mut events = Vec::new();
        let mut collect = |event: &super::ServerSentEvent| {
            events.push(event.clone());
            true
        };
        parser.feed(b"id: good\ndata: one\n\nid: bad\0id\ndata: two\n\n", &mut collect).expect("feed");
        assert_eq!(events[1].id(), "good");
    }
}
