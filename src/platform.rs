//! Cross-platform desktop helpers implemented through safe Rust crates and child processes.

use auto_launcher::{AutoLaunch, AutoLaunchBuilder, LinuxLaunchMode, MacOSLaunchMode, WindowsEnableMode};
use rfd::{FileDialog, MessageButtons, MessageDialog, MessageLevel};
use std::path::{Path, PathBuf};

pub fn choose_folder(initial: &Path) -> Option<PathBuf> {
    FileDialog::new().set_directory(initial).pick_folder()
}

pub fn open_folder(path: &Path) -> anyhow::Result<()> {
    open::that(path)?;
    Ok(())
}

pub fn show_error(title: &str, message: &str) {
    let _ = MessageDialog::new()
        .set_title(title)
        .set_description(message)
        .set_level(MessageLevel::Error)
        .set_buttons(MessageButtons::Ok)
        .show();
}

pub fn show_info(title: &str, message: &str) {
    let _ = MessageDialog::new()
        .set_title(title)
        .set_description(message)
        .set_level(MessageLevel::Info)
        .set_buttons(MessageButtons::Ok)
        .show();
}

pub fn notify(title: &str, message: &str) {
    show_info(title, message);
}

pub fn start_on_boot_enabled() -> bool {
    startup_launcher()
        .and_then(|launcher| launcher.is_enabled().map_err(Into::into))
        .unwrap_or(false)
}

pub fn set_start_on_boot(enabled: bool) -> anyhow::Result<()> {
    let launcher = startup_launcher()?;
    if enabled {
        launcher.enable()?;
    } else {
        launcher.disable()?;
    }
    Ok(())
}

fn startup_launcher() -> anyhow::Result<AutoLaunch> {
    let executable = std::env::current_exe()?;
    let path = executable.to_string_lossy();
    let mut builder = AutoLaunchBuilder::new();
    builder
        .set_app_name("NSO Album Sync")
        .set_app_path(path.as_ref())
        .set_args(&["--background"])
        .set_macos_launch_mode(MacOSLaunchMode::LaunchAgentUser)
        .set_windows_enable_mode(WindowsEnableMode::CurrentUser)
        .set_linux_launch_mode(LinuxLaunchMode::XdgAutostart);
    Ok(builder.build()?)
}
