using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace NsoAlbumSync;

#region Main Entry Point
internal static class Program
{
    private const string AppMutexName = "NSOAlbumSync_SingleInstance_Mutex_f8bb0128";

    [STAThread]
    private static void Main()
    {
        using var mutex = new Mutex(true, AppMutexName, out bool isNewInstance);
        if (!isNewInstance)
        {
            MessageBox.Show(
                "NSO Album Sync is already running in your System Tray (bottom-right of the taskbar).",
                "NSO Album Sync",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
            return;
        }

        ApplicationConfiguration.Initialize();
        Application.Run(new NsoTrayAppContext());
    }
}
#endregion

#region System Tray Application Context
public class NsoTrayAppContext : ApplicationContext
{
    private readonly NotifyIcon _trayIcon;
    private readonly ContextMenuStrip _contextMenu;
    private readonly System.Windows.Forms.Timer _syncTimer;
    private readonly ConfigManager _configManager;
    private readonly SyncEngine _syncEngine;
    private readonly NintendoAuthManager _authManager;

    private ToolStripMenuItem _statusItem = null!;
    private ToolStripMenuItem _lastSyncItem = null!;
    private ToolStripMenuItem _syncNowItem = null!;
    private ToolStripMenuItem _autoSyncItem = null!;
    private ToolStripMenuItem _startOnBootItem = null!;
    private ToolStripMenuItem _accountItem = null!;

    private bool _isSyncing = false;

    public NsoTrayAppContext()
    {
        _configManager = new ConfigManager();
        _authManager = new NintendoAuthManager();
        _syncEngine = new SyncEngine(_configManager, _authManager);

        _contextMenu = new ContextMenuStrip();
        BuildContextMenu();

        _trayIcon = new NotifyIcon
        {
            Icon = IconGenerator.CreateAlbumIcon(),
            ContextMenuStrip = _contextMenu,
            Text = "NSO Album Sync (Nintendo Switch)",
            Visible = true
        };

        _trayIcon.DoubleClick += (s, e) => OpenAlbumFolder();

        // Hourly background timer (3600 seconds)
        _syncTimer = new System.Windows.Forms.Timer
        {
            Interval = Math.Max(60_000, _configManager.Config.SyncIntervalMinutes * 60 * 1000)
        };
        _syncTimer.Tick += async (s, e) => await OnTimerTickAsync();

        UpdateMenuState();

        // Prompt initial onboarding if not logged in, or start immediate sync
        _ = InitializeAppAsync();
    }

    private void BuildContextMenu()
    {
        _contextMenu.Items.Clear();

        _statusItem = new ToolStripMenuItem("● NSO Album Sync") { Enabled = false, Font = new Font(_contextMenu.Font, FontStyle.Bold) };
        _lastSyncItem = new ToolStripMenuItem("Last sync: Never") { Enabled = false };

        var syncSeparator = new ToolStripSeparator();

        _syncNowItem = new ToolStripMenuItem("🔄 Sync Now", null, async (s, e) => await TriggerSyncAsync());
        _autoSyncItem = new ToolStripMenuItem("⏱ Auto-Sync (Hourly)", null, (s, e) => ToggleAutoSync())
        {
            CheckOnClick = true,
            Checked = _configManager.Config.AutoSyncEnabled
        };

        var folderSeparator = new ToolStripSeparator();

        var selectFolderItem = new ToolStripMenuItem("📁 Select Folder...", null, (s, e) => ChooseFolder());
        var openFolderItem = new ToolStripMenuItem("📂 Open Album Folder", null, (s, e) => OpenAlbumFolder());

        var systemSeparator = new ToolStripSeparator();

        _startOnBootItem = new ToolStripMenuItem("🚀 Start on Boot", null, (s, e) => ToggleStartOnBoot())
        {
            CheckOnClick = true,
            Checked = StartupHelper.IsRunAtStartupEnabled()
        };

        _accountItem = new ToolStripMenuItem("🔑 Sign In...", null, async (s, e) => await HandleAccountActionAsync());

        var exitSeparator = new ToolStripSeparator();

        var exitItem = new ToolStripMenuItem("❌ Exit", null, (s, e) => ExitApp());

        _contextMenu.Items.AddRange(new ToolStripItem[]
        {
            _statusItem,
            _lastSyncItem,
            syncSeparator,
            _syncNowItem,
            _autoSyncItem,
            folderSeparator,
            selectFolderItem,
            openFolderItem,
            systemSeparator,
            _startOnBootItem,
            _accountItem,
            exitSeparator,
            exitItem
        });
    }

    private void UpdateMenuState()
    {
        bool hasSession = !string.IsNullOrEmpty(_configManager.Config.SessionToken);
        string userName = _configManager.Config.UserNickname;

        if (_isSyncing)
        {
            _statusItem.Text = "● Syncing Album...";
            _trayIcon.Text = "NSO Album Sync: Syncing in progress...";
        }
        else if (hasSession)
        {
            _statusItem.Text = string.IsNullOrEmpty(userName) ? "● Connected" : $"● Connected ({userName})";
            _trayIcon.Text = $"NSO Album Sync ({userName ?? "Connected"})";
        }
        else
        {
            _statusItem.Text = "● Not Signed In";
            _trayIcon.Text = "NSO Album Sync (Click to Sign In)";
        }

        if (_configManager.Config.LastSyncTime.HasValue)
        {
            _lastSyncItem.Text = $"Last sync: {_configManager.Config.LastSyncTime.Value.ToLocalTime():HH:mm (yyyy-MM-dd)}";
        }
        else
        {
            _lastSyncItem.Text = "Last sync: Never";
        }

        _syncNowItem.Enabled = hasSession && !_isSyncing;
        _autoSyncItem.Checked = _configManager.Config.AutoSyncEnabled;
        _startOnBootItem.Checked = StartupHelper.IsRunAtStartupEnabled();
        _accountItem.Text = hasSession ? "🚪 Sign Out" : "🔑 Sign In to Nintendo Account...";
    }

    private async Task InitializeAppAsync()
    {
        if (string.IsNullOrEmpty(_configManager.Config.SessionToken))
        {
            _trayIcon.ShowBalloonTip(
                4000,
                "NSO Album Sync",
                "Welcome! Please sign in to your Nintendo Account to start auto-syncing your Switch album.",
                ToolTipIcon.Info);

            await PromptSignInDialogAsync();
        }
        else
        {
            if (_configManager.Config.AutoSyncEnabled)
            {
                _syncTimer.Start();
            }
            await TriggerSyncAsync();
        }
    }

    private async Task OnTimerTickAsync()
    {
        if (_configManager.Config.AutoSyncEnabled && !string.IsNullOrEmpty(_configManager.Config.SessionToken) && !_isSyncing)
        {
            await TriggerSyncAsync(isBackground: true);
        }
    }

    public async Task TriggerSyncAsync(bool isBackground = false)
    {
        if (_isSyncing || string.IsNullOrEmpty(_configManager.Config.SessionToken))
            return;

        _isSyncing = true;
        UpdateMenuState();

        try
        {
            var result = await _syncEngine.SyncAlbumAsync();
            _configManager.Config.LastSyncTime = DateTime.UtcNow;
            _configManager.Save();

            if (result.NewDownloads > 0)
            {
                _trayIcon.ShowBalloonTip(
                    4000,
                    "NSO Album Sync",
                    $"Synced {result.NewDownloads} new {(result.NewDownloads == 1 ? "capture" : "captures")} to your album folder!",
                    ToolTipIcon.Info);
            }
            else if (!isBackground)
            {
                _trayIcon.ShowBalloonTip(
                    3000,
                    "NSO Album Sync",
                    "Album is up to date. No new captures found.",
                    ToolTipIcon.Info);
            }
        }
        catch (Exception ex)
        {
            if (!isBackground)
            {
                MessageBox.Show(
                    $"Album sync encountered an error:\n\n{ex.Message}",
                    "NSO Album Sync Error",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning);
            }
            _trayIcon.ShowBalloonTip(
                4000,
                "NSO Album Sync Error",
                $"Sync failed: {ex.Message}",
                ToolTipIcon.Error);
        }
        finally
        {
            _isSyncing = false;
            UpdateMenuState();
        }
    }

    private void ToggleAutoSync()
    {
        _configManager.Config.AutoSyncEnabled = _autoSyncItem.Checked;
        _configManager.Save();

        if (_configManager.Config.AutoSyncEnabled)
        {
            _syncTimer.Start();
            _trayIcon.ShowBalloonTip(2000, "NSO Album Sync", "Auto-sync enabled (refreshes every hour).", ToolTipIcon.Info);
        }
        else
        {
            _syncTimer.Stop();
            _trayIcon.ShowBalloonTip(2000, "NSO Album Sync", "Auto-sync disabled.", ToolTipIcon.Info);
        }
        UpdateMenuState();
    }

    private void ToggleStartOnBoot()
    {
        bool enable = _startOnBootItem.Checked;
        StartupHelper.SetRunAtStartup(enable);
        UpdateMenuState();
    }

    private void ChooseFolder()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select destination folder for Nintendo Switch Album captures:",
            UseDescriptionForTitle = true,
            SelectedPath = _configManager.Config.DestinationFolder,
            ShowNewFolderButton = true
        };

        if (dialog.ShowDialog() == DialogResult.OK && !string.IsNullOrWhiteSpace(dialog.SelectedPath))
        {
            _configManager.Config.DestinationFolder = dialog.SelectedPath;
            _configManager.Save();
            _trayIcon.ShowBalloonTip(3000, "NSO Album Sync", $"Album save folder updated to:\n{dialog.SelectedPath}", ToolTipIcon.Info);
        }
    }

    private void OpenAlbumFolder()
    {
        string folder = _configManager.Config.DestinationFolder;
        if (!Directory.Exists(folder))
        {
            Directory.CreateDirectory(folder);
        }

        string albumSubdir = Path.Combine(folder, "Album");
        string targetToOpen = Directory.Exists(albumSubdir) ? albumSubdir : folder;

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = targetToOpen,
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Could not open folder: {ex.Message}", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private async Task HandleAccountActionAsync()
    {
        if (!string.IsNullOrEmpty(_configManager.Config.SessionToken))
        {
            var confirm = MessageBox.Show(
                "Are you sure you want to sign out from your Nintendo Account?",
                "Sign Out - NSO Album Sync",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question);

            if (confirm == DialogResult.Yes)
            {
                _configManager.Config.SessionToken = "";
                _configManager.Config.UserNickname = "";
                _configManager.Save();
                _syncTimer.Stop();
                UpdateMenuState();
                _trayIcon.ShowBalloonTip(3000, "NSO Album Sync", "Signed out successfully.", ToolTipIcon.Info);
            }
        }
        else
        {
            await PromptSignInDialogAsync();
        }
    }

    private async Task PromptSignInDialogAsync()
    {
        using var loginForm = new SignInForm(_authManager);
        if (loginForm.ShowDialog() == DialogResult.OK && !string.IsNullOrEmpty(loginForm.SessionToken))
        {
            _configManager.Config.SessionToken = loginForm.SessionToken;
            _configManager.Config.UserNickname = loginForm.UserNickname;
            _configManager.Save();

            UpdateMenuState();
            if (_configManager.Config.AutoSyncEnabled)
            {
                _syncTimer.Start();
            }

            _trayIcon.ShowBalloonTip(
                4000,
                "NSO Album Sync",
                $"Signed in as {loginForm.UserNickname}! Starting your first album sync...",
                ToolTipIcon.Info);

            await TriggerSyncAsync();
        }
    }

    private void ExitApp()
    {
        _syncTimer.Stop();
        _trayIcon.Visible = false;
        _trayIcon.Dispose();
        Application.Exit();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _syncTimer.Dispose();
            _trayIcon.Dispose();
            _contextMenu.Dispose();
        }
        base.Dispose(disposing);
    }
}
#endregion

#region Sign-In Interactive Dialog
public class SignInForm : Form
{
    private readonly NintendoAuthManager _authManager;
    private TextBox _linkInput = null!;
    private Button _openBrowserBtn = null!;
    private Button _signInBtn = null!;
    private Label _statusLabel = null!;

    public string SessionToken { get; private set; } = "";
    public string UserNickname { get; private set; } = "";

    public SignInForm(NintendoAuthManager authManager)
    {
        _authManager = authManager;
        InitializeComponents();
    }

    private void InitializeComponents()
    {
        Text = "Nintendo Account Sign-In - NSO Album Sync";
        Icon = IconGenerator.CreateAlbumIcon();
        Size = new Size(540, 360);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(246, 248, 250);
        Font = new Font("Segoe UI", 9.5f, FontStyle.Regular);

        var header = new Label
        {
            Text = "Nintendo Switch Online Album Sync",
            Font = new Font("Segoe UI", 12f, FontStyle.Bold),
            ForeColor = Color.FromArgb(220, 20, 60),
            Location = new Point(24, 18),
            AutoSize = true
        };

        var instructions = new Label
        {
            Text = "1. Click the button below to open Nintendo's official sign-in page in your browser.\n" +
                   "2. Log in and right-click / copy the link on 'Select this person' (or copy redirect URL).\n" +
                   "3. Paste the copied link in the box below and click 'Sign In & Connect'.",
            Location = new Point(24, 52),
            Size = new Size(480, 60),
            ForeColor = Color.FromArgb(40, 40, 40)
        };

        _openBrowserBtn = new Button
        {
            Text = "🌐 1. Open Nintendo Sign-In Page",
            Location = new Point(24, 122),
            Size = new Size(475, 36),
            BackColor = Color.FromArgb(230, 0, 18),
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat,
            Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
            Cursor = Cursors.Hand
        };
        _openBrowserBtn.FlatAppearance.BorderSize = 0;
        _openBrowserBtn.Click += (s, e) => OpenBrowserOAuth();

        var inputLabel = new Label
        {
            Text = "2. Paste redirect link or code here:",
            Location = new Point(24, 172),
            AutoSize = true,
            ForeColor = Color.FromArgb(60, 60, 60),
            Font = new Font("Segoe UI", 9f, FontStyle.Bold)
        };

        _linkInput = new TextBox
        {
            Location = new Point(24, 194),
            Size = new Size(475, 26),
            PlaceholderText = "npf71b963c1b7b6d119://auth#session_token_code=..."
        };
        _linkInput.TextChanged += (s, e) => _signInBtn.Enabled = !string.IsNullOrWhiteSpace(_linkInput.Text);

        _statusLabel = new Label
        {
            Text = "",
            Location = new Point(24, 230),
            Size = new Size(475, 20),
            ForeColor = Color.FromArgb(100, 100, 100)
        };

        _signInBtn = new Button
        {
            Text = "✅ Sign In & Connect",
            Location = new Point(24, 258),
            Size = new Size(475, 38),
            BackColor = Color.FromArgb(46, 150, 234),
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat,
            Font = new Font("Segoe UI", 10f, FontStyle.Bold),
            Enabled = false,
            Cursor = Cursors.Hand
        };
        _signInBtn.FlatAppearance.BorderSize = 0;
        _signInBtn.Click += async (s, e) => await ExecuteSignInAsync();

        Controls.AddRange(new Control[]
        {
            header,
            instructions,
            _openBrowserBtn,
            inputLabel,
            _linkInput,
            _statusLabel,
            _signInBtn
        });
    }

    private void OpenBrowserOAuth()
    {
        try
        {
            string url = _authManager.GetAuthorizeUrl();
            Process.Start(new ProcessStartInfo
            {
                FileName = url,
                UseShellExecute = true
            });
            _statusLabel.Text = "Browser opened. After logging in, paste the link above.";
            _statusLabel.ForeColor = Color.FromArgb(0, 120, 215);
            _linkInput.Focus();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Could not open browser: {ex.Message}", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private async Task ExecuteSignInAsync()
    {
        string input = _linkInput.Text.Trim();
        if (string.IsNullOrEmpty(input))
            return;

        _signInBtn.Enabled = false;
        _openBrowserBtn.Enabled = false;
        _linkInput.Enabled = false;
        _statusLabel.Text = "Authenticating with Nintendo...";
        _statusLabel.ForeColor = Color.FromArgb(0, 120, 215);

        try
        {
            var authResult = await _authManager.CompleteLoginFromInputAsync(input);
            SessionToken = authResult.SessionToken;
            UserNickname = authResult.UserNickname;

            DialogResult = DialogResult.OK;
            Close();
        }
        catch (Exception ex)
        {
            _statusLabel.Text = $"Authentication failed: {ex.Message}";
            _statusLabel.ForeColor = Color.Crimson;
            MessageBox.Show(
                $"Sign-In Failed:\n\n{ex.Message}\n\nPlease click 'Open Nintendo Sign-In Page' again and copy the new redirect link.",
                "Authentication Error",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            _signInBtn.Enabled = true;
            _openBrowserBtn.Enabled = true;
            _linkInput.Enabled = true;
        }
    }
}
#endregion

#region Sync Engine & USB File Generator
public class SyncEngine
{
    private readonly ConfigManager _config;
    private readonly NintendoAuthManager _auth;
    private readonly NxapiClient _nxapi;
    private readonly CoralClient _coral;
    private readonly HttpClient _http;

    public SyncEngine(ConfigManager config, NintendoAuthManager auth)
    {
        _config = config;
        _auth = auth;
        _nxapi = new NxapiClient();
        _coral = new CoralClient(_auth, _nxapi);
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(60) };
    }

    public async Task<SyncResult> SyncAlbumAsync()
    {
        string sessionToken = _config.Config.SessionToken;
        if (string.IsNullOrEmpty(sessionToken))
            throw new InvalidOperationException("Not signed in to Nintendo Account.");

        // 1. Fetch live media captures from Coral API
        var mediaItems = await _coral.GetMediaListAsync(sessionToken);

        string destinationRoot = _config.Config.DestinationFolder;
        if (string.IsNullOrWhiteSpace(destinationRoot))
        {
            destinationRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "Nintendo Switch Album");
        }

        Directory.CreateDirectory(destinationRoot);

        // 2. Load sync ledger
        var ledger = _config.LoadSyncLedger();
        int newDownloaded = 0;

        foreach (var item in mediaItems)
        {
            string mediaKey = string.IsNullOrEmpty(item.Id) ? item.ContentUri : item.Id;
            string relativePath = GetSwitchUsbPath(item);
            string fullPath = Path.Combine(destinationRoot, relativePath);

            // Check if already downloaded and file exists
            if (ledger.Contains(mediaKey) && File.Exists(fullPath))
            {
                continue;
            }

            // Ensure destination directory exists
            string? parentDir = Path.GetDirectoryName(fullPath);
            if (!string.IsNullOrEmpty(parentDir))
            {
                Directory.CreateDirectory(parentDir);
            }

            // 3. Download media stream directly from Nintendo CDN
            using (var response = await _http.GetAsync(item.ContentUri, HttpCompletionOption.ResponseHeadersRead))
            {
                response.EnsureSuccessStatusCode();
                using var fs = new FileStream(fullPath, FileMode.Create, FileAccess.Write, FileShare.None);
                await response.Content.CopyToAsync(fs);
            }

            // 4. Preserve original capture timestamps on local filesystem
            long tsSeconds = item.CapturedAt > 0 ? item.CapturedAt : item.UploadedAt;
            if (tsSeconds > 0)
            {
                long ms = tsSeconds > 10_000_000_000 ? tsSeconds : tsSeconds * 1000;
                var captureTime = DateTimeOffset.FromUnixTimeMilliseconds(ms).LocalDateTime;
                try
                {
                    File.SetCreationTime(fullPath, captureTime);
                    File.SetLastWriteTime(fullPath, captureTime);
                }
                catch { }
            }

            ledger.Add(mediaKey);
            newDownloaded++;
        }

        _config.SaveSyncLedger(ledger);
        return new SyncResult { TotalFound = mediaItems.Count, NewDownloads = newDownloaded };
    }

    /// <summary>
    /// Exact official Nintendo Switch / Switch 2 PC USB transfer structure:
    /// Album/<Game Name>/YYYYMMDDHHMMSS00_c.jpg (or .mp4)
    /// </summary>
    private static string GetSwitchUsbPath(MediaItem item)
    {
        long timestamp = item.CapturedAt > 0 ? item.CapturedAt : item.UploadedAt;
        if (timestamp <= 0) timestamp = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        long ms = timestamp > 10_000_000_000 ? timestamp : timestamp * 1000;

        var d = DateTimeOffset.FromUnixTimeMilliseconds(ms).LocalDateTime;
        string yyyy = d.Year.ToString("D4");
        string mm = d.Month.ToString("D2");
        string dd = d.Day.ToString("D2");
        string hh = d.Hour.ToString("D2");
        string min = d.Minute.ToString("D2");
        string sec = d.Second.ToString("D2");

        string timePrefix = $"{yyyy}{mm}{dd}{hh}{min}{sec}00";
        string ext = string.Equals(item.Type, "video", StringComparison.OrdinalIgnoreCase) ? "mp4" : "jpg";
        string folder = SanitizeFolderName(item.AppName);
        string filename = $"{timePrefix}_c.{ext}";

        return Path.Combine("Album", folder, filename);
    }

    private static string SanitizeFolderName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return "Other";
        var invalid = Path.GetInvalidFileNameChars();
        var sb = new StringBuilder();
        foreach (char c in name)
        {
            if (!invalid.Contains(c) && c != '<' && c != '>' && c != ':' && c != '"' && c != '/' && c != '\\' && c != '|' && c != '?' && c != '*')
            {
                sb.Append(c);
            }
        }
        string clean = sb.ToString().Trim();
        return string.IsNullOrWhiteSpace(clean) ? "Other" : clean;
    }
}

public class SyncResult
{
    public int TotalFound { get; set; }
    public int NewDownloads { get; set; }
}
#endregion

#region Nintendo Authentication & Coral Protocol Layer
public class NintendoAuthManager
{
    private const string ClientId = "71b963c1b7b6d119";
    private const string RedirectUri = "npf71b963c1b7b6d119://auth";
    private const string Scope = "openid user user.birthday user.screenName";

    private readonly HttpClient _http;
    private string? _currentVerifier;

    public NintendoAuthManager()
    {
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
    }

    public string GetAuthorizeUrl()
    {
        var pkce = GeneratePkce();
        _currentVerifier = pkce.Verifier;

        var query = new Dictionary<string, string>
        {
            ["state"] = pkce.State,
            ["redirect_uri"] = RedirectUri,
            ["client_id"] = ClientId,
            ["scope"] = Scope,
            ["response_type"] = "session_token_code",
            ["session_token_code_challenge"] = pkce.Challenge,
            ["session_token_code_challenge_method"] = "S256",
            ["theme"] = "login_form"
        };

        var queryString = string.Join("&", query.Select(kvp => $"{Uri.EscapeDataString(kvp.Key)}={Uri.EscapeDataString(kvp.Value)}"));
        return $"https://accounts.nintendo.com/connect/1.0.0/authorize?{queryString}";
    }

    public async Task<AuthResult> CompleteLoginFromInputAsync(string input)
    {
        string code = input;
        if (input.Contains("session_token_code="))
        {
            int idx = input.IndexOf("session_token_code=", StringComparison.Ordinal);
            string fragment = input.Substring(idx + "session_token_code=".Length);
            int ampIdx = fragment.IndexOf('&');
            code = ampIdx > 0 ? fragment.Substring(0, ampIdx) : fragment;
        }

        string verifier = _currentVerifier ?? "";
        if (string.IsNullOrEmpty(verifier))
            throw new InvalidOperationException("PKCE verifier expired. Please click 'Open Nintendo Sign-In Page' again.");

        // Step 1: Exchange session_token_code for long-lived session_token
        string sessionToken = await ExchangeCodeForSessionTokenAsync(code, verifier);

        // Step 2: Exchange session_token for id_token & access_token
        var tokens = await ExchangeSessionTokenForTokensAsync(sessionToken);

        // Step 3: Fetch profile nickname
        string nickname = "Nintendo Switch Player";
        try
        {
            var profile = await FetchUserProfileAsync(tokens.AccessToken);
            if (!string.IsNullOrEmpty(profile.Nickname))
                nickname = profile.Nickname;
        }
        catch { }

        return new AuthResult
        {
            SessionToken = sessionToken,
            IdToken = tokens.IdToken,
            AccessToken = tokens.AccessToken,
            UserNickname = nickname
        };
    }

    public async Task<TokenResponse> ExchangeSessionTokenForTokensAsync(string sessionToken)
    {
        var body = new
        {
            client_id = ClientId,
            session_token = sessionToken,
            grant_type = "urn:ietf:params:oauth:grant-type:jwt-bearer-session-token"
        };

        using var request = new HttpRequestMessage(HttpMethod.Post, "https://accounts.nintendo.com/connect/1.0.0/api/token")
        {
            Content = new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json")
        };
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        request.Headers.UserAgent.ParseAdd("Dalvik/2.1.0 (Linux; U; Android 12)");

        using var response = await _http.SendAsync(request);
        string json = await response.Content.ReadAsStringAsync();
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"Session token exchange failed (HTTP {response.StatusCode}): {json}");
        }

        var doc = JsonDocument.Parse(json);
        return new TokenResponse
        {
            IdToken = doc.RootElement.GetProperty("id_token").GetString() ?? "",
            AccessToken = doc.RootElement.GetProperty("access_token").GetString() ?? "",
            ExpiresIn = doc.RootElement.TryGetProperty("expires_in", out var exp) ? exp.GetInt32() : 900
        };
    }

    public async Task<UserProfile> FetchUserProfileAsync(string accessToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, "https://api.accounts.nintendo.com/2.0.0/users/me");
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        request.Headers.Add("Accept-Language", "en-GB");
        request.Headers.UserAgent.ParseAdd("NASDKAPI; Android");

        using var response = await _http.SendAsync(request);
        string json = await response.Content.ReadAsStringAsync();
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException($"Failed to fetch Nintendo Account profile: {json}");

        var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;

        return new UserProfile
        {
            Id = root.TryGetProperty("id", out var idProp) ? idProp.GetString() ?? "" : "",
            Nickname = root.TryGetProperty("nickname", out var nickProp) ? nickProp.GetString() ?? "" : "",
            Birthday = root.TryGetProperty("birthday", out var bdayProp) ? bdayProp.GetString() ?? "1995-01-01" : "1995-01-01",
            Country = root.TryGetProperty("country", out var ctryProp) ? ctryProp.GetString() ?? "US" : "US",
            Language = root.TryGetProperty("language", out var langProp) ? langProp.GetString() ?? "en-GB" : "en-GB"
        };
    }

    private async Task<string> ExchangeCodeForSessionTokenAsync(string code, string verifier)
    {
        var form = new Dictionary<string, string>
        {
            ["client_id"] = ClientId,
            ["session_token_code"] = code,
            ["session_token_code_verifier"] = verifier
        };

        using var request = new HttpRequestMessage(HttpMethod.Post, "https://accounts.nintendo.com/connect/1.0.0/api/session_token")
        {
            Content = new FormUrlEncodedContent(form)
        };
        request.Headers.UserAgent.ParseAdd("NASDKAPI; Android");

        using var response = await _http.SendAsync(request);
        string json = await response.Content.ReadAsStringAsync();
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException($"Invalid session code ({response.StatusCode}): {json}");

        var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("session_token", out var tokenProp))
        {
            return tokenProp.GetString() ?? "";
        }
        throw new InvalidOperationException($"Missing session_token in response: {json}");
    }

    private static (string State, string Verifier, string Challenge) GeneratePkce()
    {
        byte[] stateBytes = RandomNumberGenerator.GetBytes(36);
        byte[] verifierBytes = RandomNumberGenerator.GetBytes(32);

        string state = Base64Url(stateBytes);
        string verifier = Base64Url(verifierBytes);

        using var sha256 = SHA256.Create();
        byte[] challengeBytes = sha256.ComputeHash(Encoding.ASCII.GetBytes(verifier));
        string challenge = Base64Url(challengeBytes);

        return (state, verifier, challenge);
    }

    private static string Base64Url(byte[] bytes) =>
        Convert.ToBase64String(bytes).Replace("+", "-").Replace("/", "_").TrimEnd('=');
}

public class AuthResult
{
    public string SessionToken { get; set; } = "";
    public string IdToken { get; set; } = "";
    public string AccessToken { get; set; } = "";
    public string UserNickname { get; set; } = "";
}

public class TokenResponse
{
    public string IdToken { get; set; } = "";
    public string AccessToken { get; set; } = "";
    public int ExpiresIn { get; set; }
}

public class UserProfile
{
    public string Id { get; set; } = "";
    public string Nickname { get; set; } = "";
    public string Birthday { get; set; } = "";
    public string Country { get; set; } = "";
    public string Language { get; set; } = "";
}
#endregion

#region nxapi ZNCA Client (strictly respecting maintainer guidelines)
public class NxapiClient
{
    private const string NxapiZncaBase = "https://nxapi-znca-api.fancy.org.uk/api/znca";
    private const string NxapiAuthClientId = "JGN1is1KSmRMOL-g4qmgZA";
    private const string NxapiClientVersion = "w8zSLBsxR7rVoGJA";
    private const string UserAgent = "nso-album-sync/1.0 (+https://github.com/dycool/nso-album-sync)";

    private readonly HttpClient _http;
    private string? _cachedNsoVersion;
    private DateTime _nsoVersionExpiresAt = DateTime.MinValue;

    private string? _cachedNxapiToken;
    private DateTime _nxapiTokenExpiresAt = DateTime.MinValue;

    public NxapiClient()
    {
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
    }

    /// <summary>
    /// Discovers NSO app version dynamically from /config and caches for 6 hours (rule #7 in guidelines)
    /// </summary>
    public async Task<string> GetNsoVersionAsync()
    {
        if (!string.IsNullOrEmpty(_cachedNsoVersion) && DateTime.UtcNow < _nsoVersionExpiresAt)
            return _cachedNsoVersion;

        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, $"{NxapiZncaBase}/config");
            req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
            req.Headers.UserAgent.ParseAdd(UserAgent);

            using var resp = await _http.SendAsync(req);
            if (resp.IsSuccessStatusCode)
            {
                string json = await resp.Content.ReadAsStringAsync();
                var doc = JsonDocument.Parse(json);
                if (doc.RootElement.TryGetProperty("nso_version", out var versionProp))
                {
                    _cachedNsoVersion = versionProp.GetString() ?? "3.4.0";
                    _nsoVersionExpiresAt = DateTime.UtcNow.AddHours(6);
                    return _cachedNsoVersion;
                }
            }
        }
        catch { }

        _cachedNsoVersion = "3.4.0";
        _nsoVersionExpiresAt = DateTime.UtcNow.AddHours(1);
        return _cachedNsoVersion;
    }

    /// <summary>
    /// Gets or refreshes cached nxapi-auth client token
    /// </summary>
    public async Task<string> GetNxapiAuthTokenAsync()
    {
        if (!string.IsNullOrEmpty(_cachedNxapiToken) && DateTime.UtcNow < _nxapiTokenExpiresAt)
            return _cachedNxapiToken;

        try
        {
            var form = new Dictionary<string, string>
            {
                ["grant_type"] = "client_credentials",
                ["client_id"] = NxapiAuthClientId,
                ["scope"] = "ca:gf ca:er ca:dr"
            };

            using var req = new HttpRequestMessage(HttpMethod.Post, "https://nxapi-auth.fancy.org.uk/api/oauth/token")
            {
                Content = new FormUrlEncodedContent(form)
            };
            req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
            req.Headers.UserAgent.ParseAdd(UserAgent);

            using var resp = await _http.SendAsync(req);
            if (resp.IsSuccessStatusCode)
            {
                string json = await resp.Content.ReadAsStringAsync();
                var doc = JsonDocument.Parse(json);
                if (doc.RootElement.TryGetProperty("access_token", out var tokenProp))
                {
                    _cachedNxapiToken = tokenProp.GetString();
                    int exp = doc.RootElement.TryGetProperty("expires_in", out var expProp) ? expProp.GetInt32() : 300;
                    _nxapiTokenExpiresAt = DateTime.UtcNow.AddSeconds(Math.Max(30, exp - 30));
                    return _cachedNxapiToken ?? "";
                }
            }
        }
        catch { }

        return "";
    }

    /// <summary>
    /// Single round-trip /f with encrypt_token_request (Guideline §2.5)
    /// </summary>
    public async Task<byte[]> GenerateEncryptedLoginBodyAsync(string idToken, UserProfile profile)
    {
        string nsoVersion = await GetNsoVersionAsync();
        string authToken = await GetNxapiAuthTokenAsync();

        var payload = new
        {
            token = idToken,
            hash_method = "1",
            encrypt_token_request = new
            {
                url = "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login",
                parameter = new
                {
                    naIdToken = idToken,
                    naBirthday = profile.Birthday,
                    naCountry = profile.Country,
                    language = profile.Language,
                    f = "",
                    requestId = "",
                    timestamp = 0 // numeric 0 per §4 item 1
                }
            }
        };

        using var req = new HttpRequestMessage(HttpMethod.Post, $"{NxapiZncaBase}/f")
        {
            Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json")
        };
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        req.Headers.Add("X-znca-Platform", "Android");
        req.Headers.Add("X-znca-Version", nsoVersion);
        req.Headers.Add("X-znca-Client-Version", NxapiClientVersion);
        req.Headers.UserAgent.ParseAdd(UserAgent);
        if (!string.IsNullOrEmpty(authToken))
        {
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", authToken);
        }

        using var resp = await _http.SendAsync(req);
        string json = await resp.Content.ReadAsStringAsync();
        if (!resp.IsSuccessStatusCode)
            throw new InvalidOperationException($"nxapi /f failed ({resp.StatusCode}): {json}");

        var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("encrypted_token_request", out var encProp))
        {
            string encBase64 = encProp.GetString() ?? "";
            return Convert.FromBase64String(encBase64);
        }
        throw new InvalidOperationException($"Missing encrypted_token_request in nxapi /f: {json}");
    }

    /// <summary>
    /// Encrypts authenticated Coral API request body via /encrypt-request (Strict Accept: application/json)
    /// </summary>
    public async Task<byte[]> EncryptCoralRequestBodyAsync(string coralUrl, string coralAccessToken, string jsonBody)
    {
        string nsoVersion = await GetNsoVersionAsync();
        string authToken = await GetNxapiAuthTokenAsync();

        var payload = new
        {
            url = coralUrl,
            token = coralAccessToken,
            data = jsonBody
        };

        using var req = new HttpRequestMessage(HttpMethod.Post, $"{NxapiZncaBase}/encrypt-request")
        {
            Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json")
        };
        // MUST be application/json to avoid MessagePack response (Guideline §2.8)
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        req.Headers.Add("X-znca-Platform", "Android");
        req.Headers.Add("X-znca-Version", nsoVersion);
        req.Headers.Add("X-znca-Client-Version", NxapiClientVersion);
        req.Headers.UserAgent.ParseAdd(UserAgent);
        if (!string.IsNullOrEmpty(authToken))
        {
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", authToken);
        }

        using var resp = await _http.SendAsync(req);
        string json = await resp.Content.ReadAsStringAsync();
        if (!resp.IsSuccessStatusCode)
            throw new InvalidOperationException($"nxapi /encrypt-request failed ({resp.StatusCode}): {json}");

        var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("data", out var dataProp))
        {
            string b64url = dataProp.GetString() ?? "";
            string b64 = b64url.Replace("-", "+").Replace("_", "/");
            while (b64.Length % 4 != 0) b64 += "=";
            return Convert.FromBase64String(b64);
        }
        throw new InvalidOperationException($"Missing data in nxapi /encrypt-request: {json}");
    }

    /// <summary>
    /// Decrypts Coral response body via /decrypt-response (Strict Accept: text/plain)
    /// </summary>
    public async Task<string> DecryptCoralResponseAsync(byte[] encryptedBytes)
    {
        string nsoVersion = await GetNsoVersionAsync();
        string authToken = await GetNxapiAuthTokenAsync();

        var payload = new
        {
            data = Convert.ToBase64String(encryptedBytes)
        };

        using var req = new HttpRequestMessage(HttpMethod.Post, $"{NxapiZncaBase}/decrypt-response")
        {
            Content = new StringContent(JsonSerializer.Serialize(payload), Encoding.UTF8, "application/json")
        };
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("text/plain"));
        req.Headers.Add("X-znca-Platform", "Android");
        req.Headers.Add("X-znca-Version", nsoVersion);
        req.Headers.Add("X-znca-Client-Version", NxapiClientVersion);
        req.Headers.UserAgent.ParseAdd(UserAgent);
        if (!string.IsNullOrEmpty(authToken))
        {
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", authToken);
        }

        using var resp = await _http.SendAsync(req);
        string text = await resp.Content.ReadAsStringAsync();
        if (!resp.IsSuccessStatusCode)
            throw new InvalidOperationException($"nxapi /decrypt-response failed ({resp.StatusCode}): {text}");

        return text;
    }
}
#endregion

#region Coral API Client
public class CoralClient
{
    private readonly NintendoAuthManager _auth;
    private readonly NxapiClient _nxapi;
    private readonly HttpClient _http;

    private string? _cachedCoralAccessToken;
    private DateTime _coralTokenExpiresAt = DateTime.MinValue;

    public CoralClient(NintendoAuthManager auth, NxapiClient nxapi)
    {
        _auth = auth;
        _nxapi = nxapi;
        _http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
    }

    public async Task<string> EnsureCoralSessionAsync(string sessionToken)
    {
        // 2-hour session caching: Never hammer login if valid (Guideline §4 item 5)
        if (!string.IsNullOrEmpty(_cachedCoralAccessToken) && DateTime.UtcNow < _coralTokenExpiresAt)
            return _cachedCoralAccessToken;

        // 1. Refresh Nintendo Account id_token & user profile
        var tokens = await _auth.ExchangeSessionTokenForTokensAsync(sessionToken);
        var profile = await _auth.FetchUserProfileAsync(tokens.AccessToken);

        // 2. Generate encrypted login body via single /f call
        byte[] encryptedLoginBody = await _nxapi.GenerateEncryptedLoginBodyAsync(tokens.IdToken, profile);

        // 3. POST /v4/Account/Login to Coral
        string nsoVersion = await _nxapi.GetNsoVersionAsync();
        using var req = new HttpRequestMessage(HttpMethod.Post, "https://api-lp1.znc.srv.nintendo.net/v4/Account/Login")
        {
            Content = new ByteArrayContent(encryptedLoginBody)
        };
        req.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        req.Headers.Add("X-Platform", "Android");
        req.Headers.Add("X-ProductVersion", nsoVersion);
        req.Headers.UserAgent.ParseAdd($"com.nintendo.znca/{nsoVersion}(Android/12)");

        using var resp = await _http.SendAsync(req);
        byte[] respBytes = await resp.Content.ReadAsByteArrayAsync();

        // 4. Decrypt response
        string decryptedJson = await _nxapi.DecryptCoralResponseAsync(respBytes);
        var doc = JsonDocument.Parse(decryptedJson);

        if (doc.RootElement.TryGetProperty("result", out var resultProp) &&
            resultProp.TryGetProperty("webApiServerCredential", out var credProp) &&
            credProp.TryGetProperty("accessToken", out var accessProp))
        {
            _cachedCoralAccessToken = accessProp.GetString();
            int exp = credProp.TryGetProperty("expiresIn", out var expProp) ? expProp.GetInt32() : 7200;
            // Cache token safely with a 5-minute buffer
            _coralTokenExpiresAt = DateTime.UtcNow.AddSeconds(Math.Max(300, exp - 300));
            return _cachedCoralAccessToken ?? "";
        }

        throw new InvalidOperationException($"Coral login failed: {decryptedJson}");
    }

    public async Task<List<MediaItem>> GetMediaListAsync(string sessionToken)
    {
        string coralToken = await EnsureCoralSessionAsync(sessionToken);
        string nsoVersion = await _nxapi.GetNsoVersionAsync();

        const string mediaUrl = "https://api-lp1.znc.srv.nintendo.net/v4/Media/List";
        string jsonBody = "{\"parameter\":{}}";

        // Encrypt request body
        byte[] encryptedBody = await _nxapi.EncryptCoralRequestBodyAsync(mediaUrl, coralToken, jsonBody);

        using var req = new HttpRequestMessage(HttpMethod.Post, mediaUrl)
        {
            Content = new ByteArrayContent(encryptedBody)
        };
        req.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", coralToken);
        req.Headers.UserAgent.ParseAdd($"com.nintendo.znca/{nsoVersion}(Android/12)");

        using var resp = await _http.SendAsync(req);
        byte[] respBytes = await resp.Content.ReadAsByteArrayAsync();

        string decryptedJson = await _nxapi.DecryptCoralResponseAsync(respBytes);
        var doc = JsonDocument.Parse(decryptedJson);

        var list = new List<MediaItem>();
        if (doc.RootElement.TryGetProperty("result", out var resultProp) &&
            resultProp.TryGetProperty("media", out var mediaArr) &&
            mediaArr.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in mediaArr.EnumerateArray())
            {
                list.Add(new MediaItem
                {
                    Id = item.TryGetProperty("id", out var id) ? id.GetString() ?? "" : "",
                    AppName = item.TryGetProperty("appName", out var app) ? app.GetString() ?? "Nintendo Switch" : "Nintendo Switch",
                    Type = item.TryGetProperty("type", out var type) ? type.GetString() ?? "image" : "image",
                    CapturedAt = item.TryGetProperty("capturedAt", out var cap) ? cap.GetInt64() : 0,
                    UploadedAt = item.TryGetProperty("uploadedAt", out var up) ? up.GetInt64() : 0,
                    ExpiresAt = item.TryGetProperty("expiresAt", out var exp) ? exp.GetInt64() : 0,
                    ContentUri = item.TryGetProperty("contentUri", out var uri) ? uri.GetString() ?? "" : "",
                    ThumbnailUri = item.TryGetProperty("thumbnailUri", out var thumb) ? thumb.GetString() ?? "" : ""
                });
            }
        }

        return list;
    }
}

public class MediaItem
{
    public string Id { get; set; } = "";
    public string AppName { get; set; } = "";
    public string Type { get; set; } = "";
    public long CapturedAt { get; set; }
    public long UploadedAt { get; set; }
    public long ExpiresAt { get; set; }
    public string ContentUri { get; set; } = "";
    public string ThumbnailUri { get; set; } = "";
}
#endregion

#region Configuration & State Persistence
public class AppConfig
{
    public string SessionToken { get; set; } = "";
    public string UserNickname { get; set; } = "";
    public string DestinationFolder { get; set; } = "";
    public bool AutoSyncEnabled { get; set; } = true;
    public int SyncIntervalMinutes { get; set; } = 60;
    public DateTime? LastSyncTime { get; set; }
}

public class ConfigManager
{
    private readonly string _configDir;
    private readonly string _configPath;
    private readonly string _ledgerPath;

    public AppConfig Config { get; private set; }

    public ConfigManager()
    {
        _configDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "NSOAlbumSync");
        Directory.CreateDirectory(_configDir);

        _configPath = Path.Combine(_configDir, "config.json");
        _ledgerPath = Path.Combine(_configDir, "sync_ledger.json");

        Config = LoadConfig();
    }

    private AppConfig LoadConfig()
    {
        if (File.Exists(_configPath))
        {
            try
            {
                string json = File.ReadAllText(_configPath);
                var loaded = JsonSerializer.Deserialize<AppConfig>(json);
                if (loaded != null)
                {
                    if (string.IsNullOrWhiteSpace(loaded.DestinationFolder))
                    {
                        loaded.DestinationFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "Nintendo Switch Album");
                    }
                    return loaded;
                }
            }
            catch { }
        }

        return new AppConfig
        {
            DestinationFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "Nintendo Switch Album"),
            AutoSyncEnabled = true,
            SyncIntervalMinutes = 60
        };
    }

    public void Save()
    {
        try
        {
            string json = JsonSerializer.Serialize(Config, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(_configPath, json);
        }
        catch { }
    }

    public HashSet<string> LoadSyncLedger()
    {
        if (File.Exists(_ledgerPath))
        {
            try
            {
                string json = File.ReadAllText(_ledgerPath);
                var list = JsonSerializer.Deserialize<List<string>>(json);
                if (list != null) return new HashSet<string>(list);
            }
            catch { }
        }
        return new HashSet<string>();
    }

    public void SaveSyncLedger(HashSet<string> ledger)
    {
        try
        {
            string json = JsonSerializer.Serialize(ledger.ToList(), new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(_ledgerPath, json);
        }
        catch { }
    }
}
#endregion

#region Helpers & Windows Registry Startup
public static class StartupHelper
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string AppName = "NSOAlbumSync";

    public static bool IsRunAtStartupEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false);
            return key?.GetValue(AppName) != null;
        }
        catch
        {
            return false;
        }
    }

    public static void SetRunAtStartup(bool enable)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, true);
            if (key == null) return;

            if (enable)
            {
                string exePath = Environment.ProcessPath ?? Application.ExecutablePath;
                key.SetValue(AppName, $"\"{exePath}\"");
            }
            else
            {
                key.DeleteValue(AppName, false);
            }
        }
        catch { }
    }
}

public static class IconGenerator
{
    private static Icon? _cachedAlbumIcon;

    /// <summary>
    /// Returns the official Nintendo Switch Album Icon (32x32 / multi-size with rounded blue background and photo emblem)
    /// </summary>
    public static Icon CreateAlbumIcon()
    {
        if (_cachedAlbumIcon != null) return _cachedAlbumIcon;

        // 1. Try to load from Embedded Assembly Resource
        try
        {
            var asm = typeof(Program).Assembly;
            using var stream = asm.GetManifestResourceStream("NsoAlbumSync.app.ico") 
                             ?? asm.GetManifestResourceStream("app.ico");
            if (stream != null)
            {
                _cachedAlbumIcon = new Icon(stream);
                return _cachedAlbumIcon;
            }
        }
        catch { }

        // 2. Try to load from app.ico on disk or executable's embedded icon
        try
        {
            if (File.Exists("app.ico"))
            {
                _cachedAlbumIcon = new Icon("app.ico");
                return _cachedAlbumIcon;
            }
            string? exePath = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(exePath) && File.Exists(exePath))
            {
                var assoc = Icon.ExtractAssociatedIcon(exePath);
                if (assoc != null)
                {
                    _cachedAlbumIcon = assoc;
                    return _cachedAlbumIcon;
                }
            }
        }
        catch { }

        // 3. Render dynamic high-DPI official Switch Album Icon
        using var bitmap = DrawAlbumBitmap(32);
        IntPtr hIcon = bitmap.GetHicon();
        _cachedAlbumIcon = (Icon)Icon.FromHandle(hIcon).Clone();
        return _cachedAlbumIcon;
    }

    private const string NsoPath1Data = "M23.224,26.1H8.784C8.095,26.1 7.535,25.54 7.535,24.851V16.24C7.488,14.788 8.081,13.24 9.089,12.195C10.059,11.187 11.348,10.654 12.816,10.654C14.284,10.654 15.563,11.22 16.535,12.246C17.19,12.939 17.647,13.792 17.869,14.71C18.504,14.361 19.236,14.167 20.02,14.167C22.21,14.167 24.473,15.861 24.473,18.695V24.849C24.473,25.539 23.913,26.098 23.224,26.098V26.1ZM10.033,23.601H21.974V18.696C21.974,17.426 20.982,16.667 20.02,16.667C18.89,16.667 18.066,17.515 18.014,18.728C17.985,19.398 17.433,19.924 16.766,19.924C16.757,19.924 16.748,19.924 16.739,19.924C16.06,19.909 15.517,19.356 15.517,18.675V16.219C15.517,16.196 15.517,16.174 15.518,16.151C15.562,15.356 15.263,14.538 14.72,13.966C14.37,13.596 13.754,13.154 12.814,13.154C12.03,13.154 11.382,13.415 10.886,13.928C10.217,14.622 10.008,15.57 10.03,16.17C10.03,16.187 10.03,16.202 10.03,16.217V23.6L10.033,23.601Z";
    private const string NsoPath2Data = "M28.918,26.1H3.082C1.934,26.1 1,25.166 1,24.018V7.582C1,6.434 1.934,5.5 3.082,5.5H28.918C30.066,5.5 31,6.434 31,7.582V24.018C31,25.166 30.066,26.1 28.918,26.1ZM3.497,23.601H28.5V7.999H3.497V23.601Z";

    public static Bitmap DrawAlbumBitmap(int size)
    {
        var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.Clear(Color.Transparent);

        float scale = size / 32f;

        using var matrix = new Matrix();
        matrix.Scale(scale, scale);

        // 1. Official anti-transparency white plate inside frame (from iOS album_light_on.json BG_透過防止)
        float innerX = 3.5f * scale;
        float innerY = 8.0f * scale;
        float innerW = 25.0f * scale;
        float innerH = 15.6f * scale;
        using var whiteBrush = new SolidBrush(Color.FromArgb(252, 252, 252));
        g.FillRectangle(whiteBrush, innerX, innerY, innerW, innerH);

        // 2. Render official vector paths
        using var path1 = ParseSvgPath(NsoPath1Data);
        using var path2 = ParseSvgPath(NsoPath2Data);

        path1.Transform(matrix);
        path2.Transform(matrix);

        // Official Nintendo color from APK/IPA (#3571E9)
        Color nsoOfficialBlue = Color.FromArgb(53, 113, 233);
        using var blueBrush = new SolidBrush(nsoOfficialBlue);

        g.FillPath(blueBrush, path2);
        g.FillPath(blueBrush, path1);

        return bmp;
    }

    private static GraphicsPath ParseSvgPath(string pathData)
    {
        var path = new GraphicsPath { FillMode = FillMode.Winding };
        var matches = System.Text.RegularExpressions.Regex.Matches(pathData, @"([a-zA-Z])|([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)");

        char command = 'M';
        int i = 0;
        PointF currentPoint = PointF.Empty;
        PointF startPoint = PointF.Empty;

        while (i < matches.Count)
        {
            string token = matches[i].Value;
            if (char.IsLetter(token[0]))
            {
                command = token[0];
                i++;
            }

            switch (command)
            {
                case 'M':
                    {
                        float x = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float y = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        currentPoint = new PointF(x, y);
                        startPoint = currentPoint;
                        path.StartFigure();
                        command = 'L';
                    }
                    break;
                case 'L':
                    {
                        float x = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float y = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        var target = new PointF(x, y);
                        path.AddLine(currentPoint, target);
                        currentPoint = target;
                    }
                    break;
                case 'H':
                    {
                        float x = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        var target = new PointF(x, currentPoint.Y);
                        path.AddLine(currentPoint, target);
                        currentPoint = target;
                    }
                    break;
                case 'V':
                    {
                        float y = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        var target = new PointF(currentPoint.X, y);
                        path.AddLine(currentPoint, target);
                        currentPoint = target;
                    }
                    break;
                case 'C':
                    {
                        float x1 = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float y1 = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float x2 = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float y2 = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float x = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);
                        float y = float.Parse(matches[i++].Value, System.Globalization.CultureInfo.InvariantCulture);

                        var endPoint = new PointF(x, y);
                        path.AddBezier(currentPoint, new PointF(x1, y1), new PointF(x2, y2), endPoint);
                        currentPoint = endPoint;
                    }
                    break;
                case 'Z':
                case 'z':
                    path.CloseFigure();
                    currentPoint = startPoint;
                    break;
                default:
                    i++;
                    break;
            }
        }

        return path;
    }
}
#endregion
