//! Process single-instance guard matching the C++ platform contracts.

#[cfg(not(target_os = "windows"))]
use crate::config::runtime_directory;
#[cfg(not(target_os = "windows"))]
use fs2::FileExt as _;
#[cfg(not(target_os = "windows"))]
use std::fs::{self, File, OpenOptions};
#[cfg(not(target_os = "windows"))]
use std::os::unix::fs::{MetadataExt as _, PermissionsExt as _};
#[cfg(target_os = "windows")]
use std::io::{BufRead as _, BufReader};
#[cfg(target_os = "windows")]
use std::process::{Child, ChildStdin, Command, Stdio};

#[cfg(target_os = "windows")]
const WINDOWS_MUTEX_NAME: &str = r"Local\NSOAlbumSync_SingleInstance_Mutex_f8bb0128";

#[cfg(target_os = "windows")]
pub struct SingleInstance {
    child: Child,
    stdin: Option<ChildStdin>,
}

#[cfg(not(target_os = "windows"))]
pub struct SingleInstance {
    file: File,
}

impl SingleInstance {
    pub fn acquire() -> anyhow::Result<Option<Self>> {
        #[cfg(target_os = "windows")]
        {
            acquire_windows()
        }
        #[cfg(not(target_os = "windows"))]
        {
            acquire_unix()
        }
    }
}

#[cfg(target_os = "windows")]
fn acquire_windows() -> anyhow::Result<Option<SingleInstance>> {
    use std::os::windows::process::CommandExt as _;

    // The C++ reference calls CreateMutexW with this exact Local\ name and
    // treats ERROR_ALREADY_EXISTS as the secondary-instance signal. Keep the
    // exact kernel-object identity without adding unsafe Rust by letting the
    // in-box .NET runtime own the handle for the lifetime of this process.
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$createdNew = $false
$mutex = [System.Threading.Mutex]::new(
    $false,
    'Local\NSOAlbumSync_SingleInstance_Mutex_f8bb0128',
    [ref]$createdNew
)
if (-not $createdNew) {
    [Console]::Out.WriteLine('exists')
    [Console]::Out.Flush()
    $mutex.Dispose()
    exit 0
}
[Console]::Out.WriteLine('acquired')
[Console]::Out.Flush()
[Console]::In.ReadToEnd() > $null
$mutex.Dispose()
"#;

    let mut child = Command::new("powershell.exe")
        .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .creation_flags(CREATE_NO_WINDOW)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()?;
    let stdin = child
        .stdin
        .take()
        .ok_or_else(|| anyhow::anyhow!("single-instance mutex helper has no stdin"))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| anyhow::anyhow!("single-instance mutex helper has no stdout"))?;
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    reader.read_line(&mut line)?;
    match line.trim() {
        "acquired" => Ok(Some(SingleInstance {
            child,
            stdin: Some(stdin),
        })),
        "exists" => {
            drop(stdin);
            let _ = child.wait();
            Ok(None)
        }
        _ => {
            drop(stdin);
            let status = child.wait()?;
            anyhow::bail!(
                "could not acquire Windows single-instance mutex {WINDOWS_MUTEX_NAME} (helper status {status})"
            )
        }
    }
}

#[cfg(not(target_os = "windows"))]
fn acquire_unix() -> anyhow::Result<Option<SingleInstance>> {
    let directory = runtime_directory()?;
    let path = directory.join("instance.lock");

    if let Ok(metadata) = fs::symlink_metadata(&path) {
        anyhow::ensure!(
            !metadata.file_type().is_symlink(),
            "single-instance lock path is a symlink"
        );
        anyhow::ensure!(metadata.is_file(), "single-instance lock path is not a regular file");
    }

    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(&path)?;
    let metadata = file.metadata()?;
    let directory_uid = fs::metadata(&directory)?.uid();
    anyhow::ensure!(metadata.is_file(), "single-instance lock is not a regular file");
    anyhow::ensure!(
        metadata.uid() == directory_uid,
        "single-instance lock is not owned by the current runtime-directory owner"
    );
    if metadata.mode() & 0o077 != 0 {
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
    }

    match file.try_lock_exclusive() {
        Ok(()) => Ok(Some(SingleInstance { file })),
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => Ok(None),
        Err(error) => Err(error.into()),
    }
}

#[cfg(target_os = "windows")]
impl Drop for SingleInstance {
    fn drop(&mut self) {
        // Closing stdin releases the helper's ReadToEnd wait, which disposes
        // the named mutex handle immediately. Fall back to terminating it only
        // if normal shutdown does not complete.
        drop(self.stdin.take());
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.wait();
        }
    }
}

#[cfg(not(target_os = "windows"))]
impl Drop for SingleInstance {
    fn drop(&mut self) {
        let _ = fs2::FileExt::unlock(&self.file);
    }
}
