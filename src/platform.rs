//! Cross-platform desktop helpers that preserve the C++ reference behavior
//! without using unsafe Rust or direct FFI.

use rfd::{FileDialog, MessageButtons, MessageDialog, MessageDialogResult, MessageLevel};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
#[cfg(target_os = "windows")]
use std::sync::OnceLock;

const NXAPI_DISCLOSURE_TITLE: &str = "Third-Party Service Disclosure";
const NXAPI_SOURCE_URL: &str = "https://github.com/samuelthomas2774/nxapi-znca-api";

pub fn choose_folder(initial: &Path) -> Option<PathBuf> {
    FileDialog::new()
        .set_title("Choose Album Folder")
        .set_directory(initial)
        .pick_folder()
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
    native_confirm(title, message).unwrap_or_else(|| {
        matches!(
            MessageDialog::new()
                .set_title(title)
                .set_description(message)
                .set_level(MessageLevel::Warning)
                .set_buttons(MessageButtons::YesNo)
                .show(),
            MessageDialogResult::Yes
        )
    })
}

pub fn prompt(title: &str, message: &str, initial: &str) -> String {
    native_prompt(title, message, initial).unwrap_or_else(|| initial.to_owned())
}

#[cfg(target_os = "windows")]
fn native_confirm(title: &str, message: &str) -> Option<bool> {
    if title == NXAPI_DISCLOSURE_TITLE {
        return windows_disclosure(message);
    }

    const SCRIPT: &str = r#"& {
param($title, $message)
Add-Type -AssemblyName System.Windows.Forms
$result = [System.Windows.Forms.MessageBox]::Show(
    $message,
    $title,
    [System.Windows.Forms.MessageBoxButtons]::YesNo,
    [System.Windows.Forms.MessageBoxIcon]::Information,
    [System.Windows.Forms.MessageBoxDefaultButton]::Button2)
if ($result -eq [System.Windows.Forms.DialogResult]::Yes) { exit 0 }
exit 2
}"#;

    let status = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
            title,
            message,
        ])
        .status()
        .ok()?;
    Some(status.success())
}

#[cfg(target_os = "windows")]
fn windows_disclosure(message: &str) -> Option<bool> {
    const SCRIPT: &str = r#"& {
param($message, $source, $iconPath)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Third-Party Service Disclosure'
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.ShowInTaskbar = $true
if ($iconPath -and (Test-Path -LiteralPath $iconPath)) {
    try { $form.Icon = New-Object System.Drawing.Icon($iconPath) } catch {}
}
$font = [System.Windows.Forms.SystemInformation]::MenuFont
$measure = [System.Windows.Forms.TextRenderer]::MeasureText(
    $message,
    $font,
    (New-Object System.Drawing.Size(508, 1000)),
    [System.Windows.Forms.TextFormatFlags]::WordBreak)
$textHeight = [Math]::Max(1, $measure.Height)
$buttonY = 16 + $textHeight + 14
$form.ClientSize = New-Object System.Drawing.Size(540, ($buttonY + 28 + 16))
$label = New-Object System.Windows.Forms.Label
$label.Text = $message
$label.Font = $font
$label.Location = New-Object System.Drawing.Point(16, 16)
$label.Size = New-Object System.Drawing.Size(508, ($textHeight + 2))
$label.AutoSize = $false
$sourceButton = New-Object System.Windows.Forms.Button
$sourceButton.Text = 'View Source'
$sourceButton.Location = New-Object System.Drawing.Point(256, $buttonY)
$sourceButton.Size = New-Object System.Drawing.Size(96, 28)
$continue = New-Object System.Windows.Forms.Button
$continue.Text = 'Continue'
$continue.Location = New-Object System.Drawing.Point(360, $buttonY)
$continue.Size = New-Object System.Drawing.Size(82, 28)
$continue.DialogResult = [System.Windows.Forms.DialogResult]::OK
$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = 'Cancel'
$cancel.Location = New-Object System.Drawing.Point(450, $buttonY)
$cancel.Size = New-Object System.Drawing.Size(82, 28)
$cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$sourceButton.Add_Click({ Start-Process $source })
$form.Controls.AddRange(@($label, $sourceButton, $continue, $cancel))
$form.AcceptButton = $continue
$form.CancelButton = $cancel
$form.Add_Shown({ $form.Activate() })
if ($form.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { exit 0 }
exit 2
}"#;

    let icon = windows_dialog_icon_path()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_default();
    let status = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
            message,
            NXAPI_SOURCE_URL,
            &icon,
        ])
        .status()
        .ok()?;
    Some(status.success())
}

#[cfg(target_os = "linux")]
fn native_confirm(title: &str, message: &str) -> Option<bool> {
    let disclosure = title == NXAPI_DISCLOSURE_TITLE;
    loop {
        let mut command = Command::new("zenity");
        command.args(["--question", "--title", title, "--text", message]);
        if disclosure {
            command.args([
                "--ok-label=Continue",
                "--cancel-label=Cancel",
                "--extra-button=View Source",
            ]);
        }
        let output = command.output().ok()?;
        if output.status.success() {
            return Some(true);
        }
        let answer = trim_command_output(&output.stdout);
        if disclosure && answer == "View Source" {
            let _ = open::that(NXAPI_SOURCE_URL);
            continue;
        }
        return Some(false);
    }
}

#[cfg(target_os = "macos")]
fn native_confirm(title: &str, message: &str) -> Option<bool> {
    let disclosure = title == NXAPI_DISCLOSURE_TITLE;
    const DISCLOSURE_SCRIPT: &str = r#"on run argv
set dialogTitle to item 1 of argv
set dialogMessage to item 2 of argv
set sourceUrl to item 3 of argv
repeat
    try
        set answer to display dialog dialogMessage with title dialogTitle buttons {"View Source", "Cancel", "Continue"} default button "Continue" cancel button "Cancel"
        if button returned of answer is "View Source" then
            open location sourceUrl
        else
            return "continue"
        end if
    on error number -128
        return "cancel"
    end try
end repeat
end run"#;
    const GENERAL_SCRIPT: &str = r#"on run argv
try
    set answer to display dialog (item 2 of argv) with title (item 1 of argv) buttons {"Cancel", "Confirm"} default button "Confirm" cancel button "Cancel" with icon caution
    if button returned of answer is "Confirm" then return "confirm"
on error number -128
end try
return "cancel"
end run"#;

    let mut command = Command::new("osascript");
    if disclosure {
        command.args([
            "-e",
            DISCLOSURE_SCRIPT,
            "--",
            title,
            message,
            NXAPI_SOURCE_URL,
        ]);
    } else {
        command.args(["-e", GENERAL_SCRIPT, "--", title, message]);
    }
    let output = command.output().ok()?;
    if !output.status.success() {
        return Some(false);
    }
    Some(matches!(
        trim_command_output(&output.stdout).as_str(),
        "continue" | "confirm"
    ))
}

#[cfg(not(any(target_os = "windows", target_os = "linux", target_os = "macos")))]
fn native_confirm(_title: &str, _message: &str) -> Option<bool> {
    None
}

#[cfg(target_os = "linux")]
fn native_prompt(title: &str, message: &str, initial: &str) -> Option<String> {
    let output = Command::new("zenity")
        .args([
            "--entry",
            "--title",
            title,
            "--text",
            message,
            "--entry-text",
            initial,
            "--ok-label=Save",
            "--cancel-label=Cancel",
        ])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .or_else(|| {
            Command::new("kdialog")
                .args(["--title", title, "--inputbox", message, initial])
                .output()
                .ok()
                .filter(|output| output.status.success())
        })?;
    Some(trim_command_output(&output.stdout))
}

#[cfg(target_os = "macos")]
fn native_prompt(title: &str, message: &str, initial: &str) -> Option<String> {
    let output = Command::new("osascript")
        .args([
            "-e",
            "on run argv",
            "-e",
            "set answer to display dialog (item 2 of argv) with title (item 1 of argv) default answer (item 3 of argv) buttons {\"Cancel\", \"Save\"} default button \"Save\" cancel button \"Cancel\"",
            "-e",
            "return text returned of answer",
            "-e",
            "end run",
            "--",
            title,
            message,
            initial,
        ])
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| trim_command_output(&output.stdout))
}

#[cfg(target_os = "windows")]
fn native_prompt(title: &str, message: &str, initial: &str) -> Option<String> {
    const SCRIPT: &str = r#"& {
param($title, $message, $initial, $iconPath)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object System.Windows.Forms.Form
$form.Text = $title
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(540, 150)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
if ($iconPath -and (Test-Path -LiteralPath $iconPath)) {
    try { $form.Icon = New-Object System.Drawing.Icon($iconPath) } catch {}
}
$label = New-Object System.Windows.Forms.Label
$label.Text = $message
$label.Location = New-Object System.Drawing.Point(18, 17)
$label.Size = New-Object System.Drawing.Size(504, 36)
$text = New-Object System.Windows.Forms.TextBox
$text.Text = $initial
$text.Location = New-Object System.Drawing.Point(18, 63)
$text.Size = New-Object System.Drawing.Size(504, 27)
$save = New-Object System.Windows.Forms.Button
$save.Text = 'Save'
$save.Location = New-Object System.Drawing.Point(354, 105)
$save.Size = New-Object System.Drawing.Size(80, 28)
$save.DialogResult = [System.Windows.Forms.DialogResult]::OK
$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = 'Cancel'
$cancel.Location = New-Object System.Drawing.Point(442, 105)
$cancel.Size = New-Object System.Drawing.Size(80, 28)
$cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.Controls.AddRange(@($label, $text, $save, $cancel))
$form.AcceptButton = $save
$form.CancelButton = $cancel
$form.Add_Shown({ $text.Focus(); $text.SelectAll() })
if ($form.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
    [Console]::Out.Write($text.Text)
    exit 0
}
exit 2
}"#;

    let icon = windows_dialog_icon_path()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_default();
    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
            title,
            message,
            initial,
            &icon,
        ])
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).into_owned())
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
fn native_prompt(_title: &str, _message: &str, _initial: &str) -> Option<String> {
    None
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
fn trim_command_output(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes)
        .trim_end_matches(['\r', '\n'])
        .to_owned()
}

pub fn notify(title: &str, message: &str) {
    if !send_native_notification(title, message) {
        show_info(title, message);
    }
}

#[cfg(target_os = "linux")]
fn send_native_notification(title: &str, message: &str) -> bool {
    Command::new("notify-send")
        .args([
            "--app-name=NSO Album Sync",
            "--icon=applications-games",
            title,
            message,
        ])
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
            "display notification (item 2 of argv) with title (item 1 of argv) sound name \"default\"",
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
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
            title,
            message,
        ])
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(not(any(target_os = "linux", target_os = "macos", target_os = "windows")))]
fn send_native_notification(_title: &str, _message: &str) -> bool {
    false
}

pub fn start_on_boot_enabled() -> bool {
    native_start_on_boot_enabled()
}

pub fn set_start_on_boot(enabled: bool) -> anyhow::Result<()> {
    native_set_start_on_boot(enabled)
}

#[cfg(target_os = "windows")]
fn native_start_on_boot_enabled() -> bool {
    Command::new("reg.exe")
        .args([
            "query",
            r"HKCU\Software\Microsoft\Windows\CurrentVersion\Run",
            "/v",
            "NSO Album Sync",
        ])
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(target_os = "windows")]
fn native_set_start_on_boot(enabled: bool) -> anyhow::Result<()> {
    let key = r"HKCU\Software\Microsoft\Windows\CurrentVersion\Run";
    let status = if enabled {
        let executable = std::env::current_exe()?;
        let value = format!("\"{}\"", executable.display());
        Command::new("reg.exe")
            .args([
                "add",
                key,
                "/v",
                "NSO Album Sync",
                "/t",
                "REG_SZ",
                "/d",
                &value,
                "/f",
            ])
            .status()?
    } else {
        Command::new("reg.exe")
            .args(["delete", key, "/v", "NSO Album Sync", "/f"])
            .status()?
    };
    if enabled {
        anyhow::ensure!(status.success(), "could not update Start on Boot registry value");
    } else if !status.success() && native_start_on_boot_enabled() {
        anyhow::bail!("could not remove Start on Boot registry value");
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn native_start_on_boot_enabled() -> bool {
    linux_autostart_file().is_some_and(|path| path.exists())
}

#[cfg(target_os = "linux")]
fn native_set_start_on_boot(enabled: bool) -> anyhow::Result<()> {
    let Some(path) = linux_autostart_file() else {
        return Ok(());
    };
    if !enabled {
        match fs::remove_file(path) {
            Ok(()) => return Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(error) => return Err(error.into()),
        }
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let executable = linux_executable_path();
    let escaped = desktop_escape(&executable);
    fs::write(
        path,
        format!(
            "[Desktop Entry]\nType=Application\nName=NSO Album Sync\nComment=Sync Nintendo Switch Online album captures\nExec=\"{escaped}\"\nIcon=applications-games\nTerminal=false\nX-GNOME-Autostart-enabled=true\n"
        ),
    )?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn linux_autostart_file() -> Option<PathBuf> {
    let home = std::env::var_os("HOME")?;
    Some(
        PathBuf::from(home)
            .join(".config")
            .join("autostart")
            .join("nso-album-sync.desktop"),
    )
}

#[cfg(target_os = "linux")]
fn linux_executable_path() -> String {
    if let Some(appimage) = std::env::var_os("APPIMAGE")
        && !appimage.is_empty()
    {
        return appimage.to_string_lossy().into_owned();
    }
    fs::read_link("/proc/self/exe")
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "nso-album-sync".to_owned())
}

#[cfg(target_os = "linux")]
fn desktop_escape(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len() + 8);
    for character in value.chars() {
        if matches!(character, '\\' | '"' | '`' | '$') {
            escaped.push('\\');
        }
        escaped.push(character);
    }
    escaped
}

#[cfg(target_os = "macos")]
fn native_start_on_boot_enabled() -> bool {
    macos_launch_agent_file().is_some_and(|path| path.exists())
}

#[cfg(target_os = "macos")]
fn native_set_start_on_boot(enabled: bool) -> anyhow::Result<()> {
    let Some(path) = macos_launch_agent_file() else {
        return Ok(());
    };
    if !enabled {
        match fs::remove_file(path) {
            Ok(()) => return Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(error) => return Err(error.into()),
        }
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let executable = std::env::current_exe()?.to_string_lossy().into_owned();
    let executable = xml_escape(executable);
    fs::write(
        path,
        format!(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n  <key>Label</key>\n  <string>org.dycool.nso-album-sync</string>\n  <key>ProgramArguments</key>\n  <array><string>{executable}</string></array>\n  <key>RunAtLoad</key>\n  <true/>\n</dict>\n</plist>\n"
        ),
    )?;
    Ok(())
}

#[cfg(target_os = "macos")]
fn macos_launch_agent_file() -> Option<PathBuf> {
    let home = std::env::var_os("HOME")?;
    Some(
        PathBuf::from(home)
            .join("Library")
            .join("LaunchAgents")
            .join("org.dycool.nso-album-sync.plist"),
    )
}

#[cfg(target_os = "macos")]
fn xml_escape(mut value: String) -> String {
    for (from, to) in [
        ("&", "&amp;"),
        ("<", "&lt;"),
        (">", "&gt;"),
        ("\"", "&quot;"),
        ("'", "&apos;"),
    ] {
        value = value.replace(from, to);
    }
    value
}

#[cfg(not(any(target_os = "windows", target_os = "linux", target_os = "macos")))]
fn native_start_on_boot_enabled() -> bool {
    false
}

#[cfg(not(any(target_os = "windows", target_os = "linux", target_os = "macos")))]
fn native_set_start_on_boot(_enabled: bool) -> anyhow::Result<()> {
    Ok(())
}

#[cfg(target_os = "windows")]
fn windows_dialog_icon_path() -> Option<&'static Path> {
    static ICON: OnceLock<PathBuf> = OnceLock::new();
    let path = ICON.get_or_init(|| {
        let path = std::env::temp_dir().join("nso-album-sync-dialog.ico");
        if fs::write(&path, include_bytes!("../app.ico")).is_err() {
            return PathBuf::new();
        }
        path
    });
    (!path.as_os_str().is_empty()).then_some(path.as_path())
}
