//! Blocking HTTP(S) client with C++-compatible transport defaults.

use anyhow::Context as _;
use reqwest::blocking::{Client, Response};
use reqwest::header::{ACCEPT_ENCODING, CONNECTION, HeaderMap, HeaderName, HeaderValue, SET_COOKIE};
use reqwest::{Method, Proxy, redirect::Policy};
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::Read as _;
use std::sync::{Arc, RwLock};
use std::time::Duration;

pub const DEFAULT_GET_RESPONSE_LIMIT: usize = 256 * 1024 * 1024;

#[derive(Debug, Clone, Default)]
pub struct HttpResponse {
    status: u16,
    body: Vec<u8>,
    headers: BTreeMap<String, String>,
}

impl HttpResponse {
    pub fn status(&self) -> u16 { self.status }
    pub fn body(&self) -> &[u8] { &self.body }
    pub fn text(&self) -> String { String::from_utf8_lossy(&self.body).into_owned() }
    pub fn header(&self, key: &str) -> Option<&str> { self.headers.get(&key.to_ascii_lowercase()).map(String::as_str) }
}

#[derive(Debug, Clone, Copy)]
struct RequestOptions {
    timeout_seconds: u64,
    max_bytes: usize,
    follow_redirects: bool,
}

impl RequestOptions {
    fn bounded(timeout_seconds: u64, max_bytes: usize) -> Self {
        Self { timeout_seconds, max_bytes, follow_redirects: cfg!(target_os = "windows") }
    }

    fn no_redirect(timeout_seconds: u64, max_bytes: usize) -> Self {
        Self { timeout_seconds, max_bytes, follow_redirects: false }
    }
}

#[derive(Clone, Default)]
pub struct HttpClient {
    proxy_url: Arc<RwLock<String>>,
}

impl HttpClient {
    pub fn new(proxy_url: impl Into<String>) -> Self {
        Self { proxy_url: Arc::new(RwLock::new(proxy_url.into())) }
    }

    pub fn set_proxy(&self, proxy_url: impl Into<String>) {
        *self.proxy_url.write().unwrap_or_else(|poisoned| poisoned.into_inner()) = proxy_url.into();
    }

    pub fn proxy_url(&self) -> String {
        self.proxy_url.read().unwrap_or_else(|poisoned| poisoned.into_inner()).clone()
    }

    pub fn get(&self, url: &str, headers: &[String], timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        self.request(Method::GET, url, headers, None, RequestOptions::bounded(timeout_seconds, max_bytes))
    }

    pub fn get_no_redirect(&self, url: &str, headers: &[String], timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        self.request(Method::GET, url, headers, None, RequestOptions::no_redirect(timeout_seconds, max_bytes))
    }

    pub fn post_json(&self, url: &str, body: &serde_json::Value, headers: &[String], timeout_seconds: u64) -> anyhow::Result<HttpResponse> {
        let mut all = headers.to_vec();
        all.push("Content-Type: application/json".to_owned());
        self.request(Method::POST, url, &all, Some(serde_json::to_vec(body)?), RequestOptions::bounded(timeout_seconds, DEFAULT_GET_RESPONSE_LIMIT))
    }

    pub fn post_form(&self, url: &str, fields: &[(&str, &str)], headers: &[String], timeout_seconds: u64) -> anyhow::Result<HttpResponse> {
        let body = fields
            .iter()
            .map(|(key, value)| format!("{}={}", form_component_encode(key), form_component_encode(value)))
            .collect::<Vec<_>>()
            .join("&");
        let mut all = headers.to_vec();
        all.push("Content-Type: application/x-www-form-urlencoded".to_owned());
        self.request(Method::POST, url, &all, Some(body.into_bytes()), RequestOptions::bounded(timeout_seconds, DEFAULT_GET_RESPONSE_LIMIT))
    }

    pub fn post_text(&self, url: &str, body: &str, headers: &[String], content_type: &str, timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        let mut all = headers.to_vec();
        if !content_type.is_empty() { all.push(format!("Content-Type: {content_type}")); }
        self.request(Method::POST, url, &all, Some(body.as_bytes().to_vec()), RequestOptions::bounded(timeout_seconds, max_bytes))
    }

    pub fn post_bytes(&self, url: &str, body: &[u8], headers: &[String], timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        let mut all = headers.to_vec();
        all.push("Content-Type: application/octet-stream".to_owned());
        self.request(Method::POST, url, &all, Some(body.to_vec()), RequestOptions::bounded(timeout_seconds, max_bytes))
    }

    fn request(&self, method: Method, url: &str, headers: &[String], body: Option<Vec<u8>>, options: RequestOptions) -> anyhow::Result<HttpResponse> {
        let parsed = url::Url::parse(url).context("invalid URL")?;
        anyhow::ensure!(matches!(parsed.scheme(), "http" | "https"), "unsupported URL scheme");
        let client = self.build_client(options.follow_redirects)?;
        let header_map = parse_headers(headers)?;
        let mut request = client.request(method, parsed).headers(header_map).timeout(Duration::from_secs(options.timeout_seconds.max(1)));
        if let Some(bytes) = body { request = request.body(bytes); }
        let response = request.send().context("HTTP request failed")?;
        collect_response(response, options.max_bytes)
    }

    fn build_client(&self, follow_redirects: bool) -> anyhow::Result<Client> {
        let mut builder = Client::builder().redirect(redirect_policy(follow_redirects));
        #[cfg(not(target_os = "windows"))]
        {
            builder = builder.http1_only();
        }
        #[cfg(target_os = "windows")]
        {
            builder = builder.user_agent("NSO Album Sync/2.0");
        }
        let proxy = self.proxy_url();
        if !proxy.trim().is_empty() {
            let parsed = url::Url::parse(&proxy).context("invalid proxy URL")?;
            anyhow::ensure!(parsed.scheme() == "http", "Only http:// proxies are supported");
            builder = builder.proxy(Proxy::all(parsed.as_str())?);
        }
        Ok(builder.build()?)
    }
}

fn form_component_encode(value: &str) -> String {
    let mut encoded = String::with_capacity(value.len());
    for byte in value.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            encoded.push(char::from(byte));
        } else {
            let _ = write!(encoded, "%{byte:02X}");
        }
    }
    encoded
}

fn redirect_policy(follow_redirects: bool) -> Policy {
    if !follow_redirects {
        return Policy::none();
    }
    #[cfg(target_os = "windows")]
    {
        Policy::custom(|attempt| {
            if attempt.previous().len() >= 10 {
                return attempt.error("too many redirects");
            }
            if attempt
                .previous()
                .last()
                .is_some_and(|previous| previous.scheme() == "https")
                && attempt.url().scheme() == "http"
            {
                return attempt.stop();
            }
            attempt.follow()
        })
    }
    #[cfg(not(target_os = "windows"))]
    {
        Policy::none()
    }
}

fn parse_headers(headers: &[String]) -> anyhow::Result<HeaderMap> {
    let mut map = HeaderMap::new();
    map.insert(CONNECTION, HeaderValue::from_static("close"));
    map.insert(ACCEPT_ENCODING, HeaderValue::from_static("identity"));
    for line in headers {
        let Some((name, value)) = line.split_once(':') else { anyhow::bail!("invalid HTTP header"); };
        let name = HeaderName::from_bytes(name.trim().as_bytes())?;
        let value = HeaderValue::from_str(value.trim())?;
        map.append(name, value);
    }
    Ok(map)
}

fn collect_response(mut response: Response, max_bytes: usize) -> anyhow::Result<HttpResponse> {
    if max_bytes != 0
        && let Some(length) = response.content_length()
    {
        anyhow::ensure!(length <= max_bytes as u64, "HTTP response exceeds safety limit");
    }
    let status = response.status().as_u16();
    let mut headers = BTreeMap::new();
    for name in response.headers().keys() {
        if *name == SET_COOKIE { continue; }
        if let Some(value) = response.headers().get(name).and_then(|v| v.to_str().ok()) {
            headers.insert(name.as_str().to_ascii_lowercase(), value.to_owned());
        }
    }
    let set_cookies = response.headers().get_all(SET_COOKIE).iter().filter_map(|value| value.to_str().ok()).collect::<Vec<_>>().join("\n");
    if !set_cookies.is_empty() { headers.insert("set-cookie".to_owned(), set_cookies); }

    let mut body = Vec::new();
    let mut chunk = [0_u8; 32 * 1024];
    loop {
        let read = response.read(&mut chunk)?;
        if read == 0 { break; }
        if max_bytes != 0 {
            anyhow::ensure!(body.len().saturating_add(read) <= max_bytes, "HTTP response exceeds safety limit");
        }
        body.extend_from_slice(&chunk[..read]);
    }
    Ok(HttpResponse { status, body, headers })
}

#[cfg(test)]
mod tests {
    use super::form_component_encode;

    #[test]
    fn form_encoding_matches_reference_unreserved_set() {
        assert_eq!(form_component_encode("A-z_~.123"), "A-z_~.123");
        assert_eq!(form_component_encode("npf://auth?x=a+b"), "npf%3A%2F%2Fauth%3Fx%3Da%2Bb");
        assert_eq!(form_component_encode("é"), "%C3%A9");
    }
}
