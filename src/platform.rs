//! Cross-platform desktop helpers implemented through safe Rust crates and child processes.

use auto_launcher::{
    AutoLaunch, AutoLaunchBuilder, LinuxLaunchMode, MacOSLaunchMode, WindowsEnableMode,
};
use rfd::{
    FileDialog, MessageButtons, MessageDialog, MessageDialogResult, MessageLevel,
};
use std::path::{Path, PathBuf};
use std::process::Command;

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

pub fn confirm(title: &str, message: &str) -> bool {
    matches!(
        MessageDialog::new()
            .set_title(title)
            .set_description(message)
            .set_level(MessageLevel::Warning)
            .set_buttons(MessageButtons::YesNo)
            .show(),
        MessageDialogResult::Yes
    )
}

pub fn notify(title: &str, message: &str) {
    if !send_native_notification(title, message) {
        show_info(title, message);
    }
}

#[cfg(target_os = "linux")]
fn send_native_notification(title: &str, message: &str) -> bool {
    Command::new("notify-send")
        .arg("--app-name=NSO Album Sync")
        .arg(title)
        .arg(message)
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(target_os = "macos")]
fn send_native_notification(title: &str, message: &str) -> bool {
    Command::new("osascript")
        .args([
            "-e",
            "on run argv",
            "-e",
            "display notification (item 2 of argv) with title (item 1 of argv)",
            "-e",
            "end run",
            "--",
            title,
            message,
        ])
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(target_os = "windows")]
fn send_native_notification(title: &str, message: &str) -> bool {
    const SCRIPT: &str = r#"& {
param($title, $message)
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime] > $null
$template = [Windows.UI.Notifications.ToastTemplateType]::ToastText02
$xml = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent($template)
$text = $xml.GetElementsByTagName('text')
$text.Item(0).AppendChild($xml.CreateTextNode($title)) > $null
$text.Item(1).AppendChild($xml.CreateTextNode($message)) > $null
$toast = New-Object Windows.UI.Notifications.ToastNotification $xml
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('NSO Album Sync').Show($toast)
}"#;

    Command::new("powershell.exe")
        .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", SCRIPT, title, message])
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
fn send_native_notification(_title: &str, _message: &str) -> bool {
    false
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
