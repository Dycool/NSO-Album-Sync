//! Blocking HTTPS client with bounded response bodies and explicit redirect policy.

use anyhow::Context as _;
use reqwest::blocking::{Client, Response};
use reqwest::header::{HeaderMap, HeaderName, HeaderValue, SET_COOKIE};
use reqwest::{Method, Proxy, redirect::Policy};
use std::collections::BTreeMap;
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
        self.request(Method::GET, url, headers, None, timeout_seconds, max_bytes, true)
    }

    pub fn get_no_redirect(&self, url: &str, headers: &[String], timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        self.request(Method::GET, url, headers, None, timeout_seconds, max_bytes, false)
    }

    pub fn post_json(&self, url: &str, body: &serde_json::Value, headers: &[String], timeout_seconds: u64) -> anyhow::Result<HttpResponse> {
        let mut all = headers.to_vec();
        all.push("Content-Type: application/json".to_owned());
        self.request(Method::POST, url, &all, Some(serde_json::to_vec(body)?), timeout_seconds, DEFAULT_GET_RESPONSE_LIMIT, true)
    }

    pub fn post_form(&self, url: &str, fields: &[(&str, &str)], headers: &[String], timeout_seconds: u64) -> anyhow::Result<HttpResponse> {
        let body = fields.iter().map(|(key, value)| {
            format!("{}={}", percent_encoding::utf8_percent_encode(key, percent_encoding::NON_ALPHANUMERIC), percent_encoding::utf8_percent_encode(value, percent_encoding::NON_ALPHANUMERIC))
        }).collect::<Vec<_>>().join("&");
        let mut all = headers.to_vec();
        all.push("Content-Type: application/x-www-form-urlencoded".to_owned());
        self.request(Method::POST, url, &all, Some(body.into_bytes()), timeout_seconds, DEFAULT_GET_RESPONSE_LIMIT, true)
    }

    pub fn post_text(&self, url: &str, body: &str, headers: &[String], content_type: &str, timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        let mut all = headers.to_vec();
        if !content_type.is_empty() { all.push(format!("Content-Type: {content_type}")); }
        self.request(Method::POST, url, &all, Some(body.as_bytes().to_vec()), timeout_seconds, max_bytes, true)
    }

    pub fn post_bytes(&self, url: &str, body: &[u8], headers: &[String], timeout_seconds: u64, max_bytes: usize) -> anyhow::Result<HttpResponse> {
        self.request(Method::POST, url, headers, Some(body.to_vec()), timeout_seconds, max_bytes, true)
    }

    fn request(&self, method: Method, url: &str, headers: &[String], body: Option<Vec<u8>>, timeout_seconds: u64, max_bytes: usize, follow_redirects: bool) -> anyhow::Result<HttpResponse> {
        let parsed = url::Url::parse(url).context("invalid URL")?;
        anyhow::ensure!(matches!(parsed.scheme(), "http" | "https"), "unsupported URL scheme");
        let client = self.build_client(follow_redirects)?;
        let header_map = parse_headers(headers)?;
        let mut request = client.request(method, parsed).headers(header_map).timeout(Duration::from_secs(timeout_seconds.max(1)));
        if let Some(bytes) = body { request = request.body(bytes); }
        let response = request.send().context("HTTP request failed")?;
        collect_response(response, max_bytes)
    }

    fn build_client(&self, follow_redirects: bool) -> anyhow::Result<Client> {
        let mut builder = Client::builder()
            .user_agent("nso-album-sync/2.0.0 (+https://github.com/Dycool/NSO-Album-Sync)")
            .redirect(if follow_redirects { Policy::limited(8) } else { Policy::none() });
        let proxy = self.proxy_url();
        if !proxy.trim().is_empty() { builder = builder.proxy(Proxy::all(&proxy)?); }
        Ok(builder.build()?)
    }
}

fn parse_headers(headers: &[String]) -> anyhow::Result<HeaderMap> {
    let mut map = HeaderMap::new();
    for line in headers {
        let Some((name, value)) = line.split_once(':') else { anyhow::bail!("invalid HTTP header"); };
        let name = HeaderName::from_bytes(name.trim().as_bytes())?;
        let value = HeaderValue::from_str(value.trim())?;
        map.append(name, value);
    }
    Ok(map)
}

fn collect_response(mut response: Response, max_bytes: usize) -> anyhow::Result<HttpResponse> {
    if let Some(length) = response.content_length() {
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
        anyhow::ensure!(body.len().saturating_add(read) <= max_bytes, "HTTP response exceeds safety limit");
        body.extend_from_slice(&chunk[..read]);
    }
    Ok(HttpResponse { status, body, headers })
}
