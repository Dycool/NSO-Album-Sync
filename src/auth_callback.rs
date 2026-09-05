//! Custom-protocol callback registration and single-instance callback hand-off.

use crate::config::runtime_directory;
use atomic_write_file::AtomicWriteFile;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
#[cfg(any(target_os = "windows", target_os = "linux", target_os = "macos"))]
use std::process::Command;
#[cfg(target_os = "windows")]
use std::sync::atomic::{AtomicBool, Ordering};

pub const NINTENDO_CALLBACK_SCHEME: &str = "npf71b963c1b7b6d119";
pub const NINTENDO_REDIRECT_URI: &str = "npf71b963c1b7b6d119://auth";
const MAX_CALLBACK_BYTES: usize = 16 * 1024;

#[cfg(target_os = "windows")]
const WINDOWS_HANDLER_DESCRIPTION: &str = "URL:NSO Album Sync Nintendo Account callback";
#[cfg(target_os = "windows")]
static WINDOWS_HANDLER_OWNED: AtomicBool = AtomicBool::new(false);

#[cfg(target_os = "linux")]
const LINUX_DESKTOP_ID: &str = "nso-album-sync-auth.desktop";
#[cfg(target_os = "linux")]
const LINUX_MIME_TYPE: &str = "x-scheme-handler/npf71b963c1b7b6d119";

pub fn is_nintendo_auth_callback(input: &str) -> bool {
    let value = input.trim();
    if value.len() < NINTENDO_REDIRECT_URI.len()
        || !value[..NINTENDO_REDIRECT_URI.len()].eq_ignore_ascii_case(NINTENDO_REDIRECT_URI)
    {
        return false;
    }

    let mut suffix = &value[NINTENDO_REDIRECT_URI.len()..];
    if suffix.is_empty() {
        return true;
    }
    if let Some(rest) = suffix.strip_prefix('/') {
        suffix = rest;
        if suffix.is_empty() {
            return true;
        }
    }
    matches!(suffix.as_bytes().first(), Some(b'#' | b'?'))
}

pub fn callback_from_args(args: &[String]) -> Option<String> {
    args.iter()
        .find(|arg| is_nintendo_auth_callback(arg))
        .cloned()
}

/// Register the Nintendo callback protocol for the active sign-in flow.
pub fn register_protocol() -> anyhow::Result<()> {
    #[cfg(target_os = "windows")]
    register_protocol_windows()?;
    #[cfg(target_os = "linux")]
    register_protocol_linux()?;
    #[cfg(target_os = "macos")]
    register_protocol_macos();
    Ok(())
}

pub fn unregister_protocol() {
    #[cfg(target_os = "windows")]
    unregister_protocol_windows();
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    {
        // Deliberately a no-op, matching the C++ Linux/macOS implementations.
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

#[cfg(target_os = "macos")]
fn register_protocol_macos() {
    // Info.plist declares the scheme. Force-register the containing app bundle
    // with Launch Services so moved/ad-hoc builds are rediscovered, mirroring
    // the C++ LSRegisterURL call without FFI in this crate.
    let Ok(executable) = std::env::current_exe() else {
        return;
    };
    let Some(bundle) = executable
        .ancestors()
        .find(|path| path.extension().is_some_and(|extension| extension == "app"))
    else {
        return;
    };
    let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister";
    let _ = Command::new(lsregister).args(["-f"]).arg(bundle).status();
}

pub fn publish_callback(callback: &str) -> anyhow::Result<()> {
    let callback = callback.trim();
    anyhow::ensure!(
        is_nintendo_auth_callback(callback),
        "refusing invalid callback URL"
    );
    anyhow::ensure!(callback.len() <= MAX_CALLBACK_BYTES, "callback URL is too large");
    let path = callback_path()?;
    let mut file = AtomicWriteFile::open(&path)?;
    file.write_all(callback.as_bytes())?;
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

    let metadata = fs::symlink_metadata(&path)?;
    if !metadata.file_type().is_file() || metadata.len() > MAX_CALLBACK_BYTES as u64 {
        let _ = fs::remove_file(&path);
        return Ok(None);
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt as _;
        if metadata.uid() != unsafe_free_current_uid()
            || metadata.mode() & 0o077 != 0
        {
            let _ = fs::remove_file(&path);
            return Ok(None);
        }
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

#[cfg(unix)]
fn unsafe_free_current_uid() -> u32 {
    // `id -u` preserves the ownership check from the C++ implementation while
    // keeping this crate free of libc FFI and unsafe code.
    Command::new("id")
        .arg("-u")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .and_then(|value| value.trim().parse::<u32>().ok())
        .unwrap_or(u32::MAX)
}

pub fn clear_callback() -> anyhow::Result<()> {
    let directory = runtime_directory()?;
    let path = directory.join("auth-callback.txt");
    match fs::remove_file(&path) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(error.into()),
    }

    if let Ok(entries) = fs::read_dir(&directory) {
        for entry in entries.flatten() {
            let name = entry.file_name();
            if name.to_string_lossy().starts_with("auth-callback.tmp.") {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
    Ok(())
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

#[cfg(test)]
mod tests {
    use super::is_nintendo_auth_callback;

    #[test]
    fn accepts_only_reference_callback_shapes() {
        assert!(is_nintendo_auth_callback("npf71b963c1b7b6d119://auth"));
        assert!(is_nintendo_auth_callback("npf71b963c1b7b6d119://auth/#state=x"));
        assert!(is_nintendo_auth_callback("NPF71B963C1B7B6D119://AUTH?state=x"));
        assert!(!is_nintendo_auth_callback("npf71b963c1b7b6d119://auth/evil"));
        assert!(!is_nintendo_auth_callback("https://example.com/"));
    }
}
