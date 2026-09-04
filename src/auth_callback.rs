//! Custom-protocol callback registration and single-instance callback hand-off.

use crate::config::runtime_directory;
use atomic_write_file::AtomicWriteFile;
use std::fs;
use std::io::Write as _;
use std::path::PathBuf;
use std::process::Command;

pub const NINTENDO_CALLBACK_SCHEME: &str = "npf71b963c1b7b6d119";
pub const NINTENDO_REDIRECT_URI: &str = "npf71b963c1b7b6d119://auth";

pub fn is_nintendo_auth_callback(input: &str) -> bool {
    let Ok(url) = url::Url::parse(input.trim()) else { return false; };
    url.scheme().eq_ignore_ascii_case(NINTENDO_CALLBACK_SCHEME)
        && url.host_str().is_some_and(|host| host.eq_ignore_ascii_case("auth"))
}

pub fn callback_from_args(args: &[String]) -> Option<String> {
    args.iter().find(|arg| is_nintendo_auth_callback(arg)).cloned()
}

pub fn register_protocol() -> anyhow::Result<()> {
    let executable = std::env::current_exe()?;
    #[cfg(target_os = "windows")]
    {
        let scheme_key = format!(r"HKCU\Software\Classes\{NINTENDO_CALLBACK_SCHEME}");
        run_reg(&["add", &scheme_key, "/ve", "/d", "URL:Nintendo Account callback", "/f"])?;
        run_reg(&["add", &scheme_key, "/v", "URL Protocol", "/d", "", "/f"])?;
        let command_key = format!(r"{scheme_key}\shell\open\command");
        let command = format!("\"{}\" \"%1\"", executable.display());
        run_reg(&["add", &command_key, "/ve", "/d", &command, "/f"])?;
    }
    #[cfg(target_os = "linux")]
    {
        let home = std::env::var_os("HOME").ok_or_else(|| anyhow::anyhow!("HOME is unavailable"))?;
        let applications = PathBuf::from(home).join(".local/share/applications");
        fs::create_dir_all(&applications)?;
        let desktop = applications.join("nso-album-sync.desktop");
        let escaped = executable.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"");
        let contents = format!(
            "[Desktop Entry]\nType=Application\nName=NSO Album Sync\nExec=\"{escaped}\" %u\nTerminal=false\nNoDisplay=true\nMimeType=x-scheme-handler/{NINTENDO_CALLBACK_SCHEME};\n"
        );
        fs::write(&desktop, contents)?;
        let _ = Command::new("xdg-mime")
            .args(["default", "nso-album-sync.desktop", &format!("x-scheme-handler/{NINTENDO_CALLBACK_SCHEME}")])
            .status();
    }
    #[cfg(target_os = "macos")]
    {
        // macOS registers the scheme from the bundled Info.plist. Cargo bundle
        // metadata in Cargo.toml is the source of truth; no LaunchServices FFI is needed.
        let _ = executable;
    }
    Ok(())
}

#[cfg(target_os = "windows")]
fn run_reg(arguments: &[&str]) -> anyhow::Result<()> {
    let status = Command::new("reg.exe").args(arguments).status()?;
    anyhow::ensure!(status.success(), "failed to register Nintendo callback protocol");
    Ok(())
}

pub fn publish_callback(callback: &str) -> anyhow::Result<()> {
    anyhow::ensure!(is_nintendo_auth_callback(callback), "refusing invalid callback URL");
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
    if !path.exists() { return Ok(None); }
    let value = fs::read_to_string(&path)?;
    let _ = fs::remove_file(&path);
    let value = value.trim().to_owned();
    if is_nintendo_auth_callback(&value) { Ok(Some(value)) } else { Ok(None) }
}

fn callback_path() -> anyhow::Result<PathBuf> { Ok(runtime_directory()?.join("auth-callback.txt")) }

fn make_private(path: &std::path::Path) -> anyhow::Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
    }
    Ok(())
}
