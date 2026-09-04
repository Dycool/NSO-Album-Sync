//! Custom-protocol callback registration and single-instance callback hand-off.

use crate::config::runtime_directory;
use atomic_write_file::AtomicWriteFile;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
#[cfg(any(target_os = "windows", target_os = "linux"))]
use std::process::Command;
#[cfg(target_os = "windows")]
use std::sync::atomic::{AtomicBool, Ordering};

pub const NINTENDO_CALLBACK_SCHEME: &str = "npf71b963c1b7b6d119";
pub const NINTENDO_REDIRECT_URI: &str = "npf71b963c1b7b6d119://auth";

#[cfg(target_os = "windows")]
const WINDOWS_HANDLER_DESCRIPTION: &str = "URL:NSO Album Sync Nintendo Account callback";
#[cfg(target_os = "windows")]
static WINDOWS_HANDLER_OWNED: AtomicBool = AtomicBool::new(false);

#[cfg(target_os = "linux")]
const LINUX_DESKTOP_ID: &str = "nso-album-sync-auth.desktop";
#[cfg(target_os = "linux")]
const LINUX_MIME_TYPE: &str = "x-scheme-handler/npf71b963c1b7b6d119";

pub fn is_nintendo_auth_callback(input: &str) -> bool {
    let Ok(url) = url::Url::parse(input.trim()) else {
        return false;
    };
    url.scheme().eq_ignore_ascii_case(NINTENDO_CALLBACK_SCHEME)
        && url
            .host_str()
            .is_some_and(|host| host.eq_ignore_ascii_case("auth"))
}

pub fn callback_from_args(args: &[String]) -> Option<String> {
    args.iter()
        .find(|arg| is_nintendo_auth_callback(arg))
        .cloned()
}

/// Register the Nintendo callback protocol for the active sign-in flow.
///
/// This intentionally mirrors the C++ reference: Windows/Linux refuse to take
/// the scheme from another application instead of silently overwriting it.
pub fn register_protocol() -> anyhow::Result<()> {
    #[cfg(target_os = "windows")]
    register_protocol_windows()?;
    #[cfg(target_os = "linux")]
    register_protocol_linux()?;
    #[cfg(target_os = "macos")]
    {
        // The scheme is declared by the application bundle's Info.plist.
    }
    Ok(())
}

pub fn unregister_protocol() {
    #[cfg(target_os = "windows")]
    unregister_protocol_windows();
    #[cfg(target_os = "linux")]
    {
        // Deliberately a no-op, matching the C++ Linux implementation.
    }
    #[cfg(target_os = "macos")]
    {
        // Bundle protocol registration is managed by Launch Services.
    }
}

#[cfg(target_os = "windows")]
fn register_protocol_windows() -> anyhow::Result<()> {
    let desired = windows_desired_command()?;
    if windows_current_command()
        .is_some_and(|current| current.eq_ignore_ascii_case(&desired))
    {
        WINDOWS_HANDLER_OWNED.store(true, Ordering::Release);
        return Ok(());
    }

    let marker = windows_user_handler_marker();
    let handler_exists = windows_current_command().is_some();
    if handler_exists && marker.as_deref() != Some(WINDOWS_HANDLER_DESCRIPTION) {
        anyhow::bail!("Nintendo callback protocol is already owned by another application");
    }

    let scheme_key = format!(r"HKCU\Software\Classes\{NINTENDO_CALLBACK_SCHEME}");
    if marker.as_deref() == Some(WINDOWS_HANDLER_DESCRIPTION) {
        let _ = run_reg_status(&["delete", &scheme_key, "/f"]);
    }

    run_reg(&[
        "add",
        &scheme_key,
        "/ve",
        "/d",
        WINDOWS_HANDLER_DESCRIPTION,
        "/f",
    ])?;
    run_reg(&[
        "add",
        &scheme_key,
        "/v",
        "URL Protocol",
        "/d",
        "",
        "/f",
    ])?;
    let command_key = format!(r"{scheme_key}\shell\open\command");
    if let Err(error) = run_reg(&["add", &command_key, "/ve", "/d", &desired, "/f"]) {
        let _ = run_reg_status(&["delete", &scheme_key, "/f"]);
        return Err(error);
    }

    WINDOWS_HANDLER_OWNED.store(true, Ordering::Release);
    Ok(())
}

#[cfg(target_os = "windows")]
fn unregister_protocol_windows() {
    if !WINDOWS_HANDLER_OWNED.load(Ordering::Acquire) {
        return;
    }
    let Ok(desired) = windows_desired_command() else {
        return;
    };
    if !windows_current_command().is_some_and(|current| current.eq_ignore_ascii_case(&desired)) {
        return;
    }
    let scheme_key = format!(r"HKCU\Software\Classes\{NINTENDO_CALLBACK_SCHEME}");
    if run_reg_status(&["delete", &scheme_key, "/f"]) {
        WINDOWS_HANDLER_OWNED.store(false, Ordering::Release);
    }
}

#[cfg(target_os = "windows")]
fn windows_desired_command() -> anyhow::Result<String> {
    let executable = std::env::current_exe()?;
    Ok(format!("\"{}\" \"%1\"", executable.display()))
}

#[cfg(target_os = "windows")]
fn windows_current_command() -> Option<String> {
    let key = format!(r"HKCR\{NINTENDO_CALLBACK_SCHEME}\shell\open\command");
    query_reg_default(&key)
}

#[cfg(target_os = "windows")]
fn windows_user_handler_marker() -> Option<String> {
    let key = format!(r"HKCU\Software\Classes\{NINTENDO_CALLBACK_SCHEME}");
    query_reg_default(&key)
}

#[cfg(target_os = "windows")]
fn query_reg_default(key: &str) -> Option<String> {
    let output = Command::new("reg.exe")
        .args(["query", key, "/ve"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    text.lines().find_map(|line| {
        let (_, value) = line.split_once("REG_SZ")?;
        Some(value.trim().to_owned())
    })
}

#[cfg(target_os = "windows")]
fn run_reg(arguments: &[&str]) -> anyhow::Result<()> {
    anyhow::ensure!(
        run_reg_status(arguments),
        "failed to register Nintendo callback protocol"
    );
    Ok(())
}

#[cfg(target_os = "windows")]
fn run_reg_status(arguments: &[&str]) -> bool {
    Command::new("reg.exe")
        .args(arguments)
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(target_os = "linux")]
fn register_protocol_linux() -> anyhow::Result<()> {
    let current = Command::new("xdg-mime")
        .args(["query", "default", LINUX_MIME_TYPE])
        .output()?;
    anyhow::ensure!(current.status.success(), "could not query Nintendo callback protocol");
    let current = String::from_utf8_lossy(&current.stdout).trim().to_owned();
    anyhow::ensure!(
        current.is_empty() || current == LINUX_DESKTOP_ID,
        "Nintendo callback protocol is already owned by another application"
    );

    let home = std::env::var_os("HOME").ok_or_else(|| anyhow::anyhow!("HOME is unavailable"))?;
    let applications = PathBuf::from(home).join(".local/share/applications");
    fs::create_dir_all(&applications)?;
    let desktop = applications.join(LINUX_DESKTOP_ID);
    let executable = linux_executable_path()?;
    let contents = format!(
        "[Desktop Entry]\nType=Application\nName=NSO Album Sync Nintendo Account Sign-In\nExec={} %u\nNoDisplay=true\nTerminal=false\nMimeType={LINUX_MIME_TYPE};\n",
        desktop_quote(&executable)
    );
    fs::write(&desktop, contents)?;

    if current != LINUX_DESKTOP_ID {
        let status = Command::new("xdg-mime")
            .args(["default", LINUX_DESKTOP_ID, LINUX_MIME_TYPE])
            .status()?;
        anyhow::ensure!(status.success(), "could not register Nintendo callback protocol");
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn linux_executable_path() -> anyhow::Result<String> {
    if let Some(appimage) = std::env::var_os("APPIMAGE")
        && !appimage.is_empty()
    {
        return Ok(appimage.to_string_lossy().into_owned());
    }
    Ok(std::fs::read_link("/proc/self/exe")?
        .to_string_lossy()
        .into_owned())
}

#[cfg(target_os = "linux")]
fn desktop_quote(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len() + 2);
    escaped.push('"');
    for character in value.chars() {
        if matches!(character, '\\' | '"' | '`' | '$') {
            escaped.push('\\');
        }
        escaped.push(character);
    }
    escaped.push('"');
    escaped
}

pub fn publish_callback(callback: &str) -> anyhow::Result<()> {
    anyhow::ensure!(
        is_nintendo_auth_callback(callback),
        "refusing invalid callback URL"
    );
    let path = callback_path()?;
    let mut file = AtomicWriteFile::open(&path)?;
    file.write_all(callback.trim().as_bytes())?;
    file.flush()?;
    file.commit()?;
    make_private(&path)?;
    Ok(())
}

pub fn take_callback() -> anyhow::Result<Option<String>> {
    let path = callback_path()?;
    if !path.exists() {
        return Ok(None);
    }
    let value = fs::read_to_string(&path)?;
    let _ = fs::remove_file(&path);
    let value = value.trim().to_owned();
    if is_nintendo_auth_callback(&value) {
        Ok(Some(value))
    } else {
        Ok(None)
    }
}

pub fn clear_callback() -> anyhow::Result<()> {
    let path = callback_path()?;
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
    }
}

fn callback_path() -> anyhow::Result<PathBuf> {
    Ok(runtime_directory()?.join("auth-callback.txt"))
}

fn make_private(path: &Path) -> anyhow::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    }
    #[cfg(not(unix))]
    {
        let _ = path;
    }
    Ok(())
}
