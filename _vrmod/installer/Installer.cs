// TWFC VR mod installer (Windows Forms, .NET Framework 4 - present on every Windows 10/11).
// Built by build_installer.bat; the mod's files travel inside the exe as resources.
// It only reads the game's own Binaries folder: the VR copy lives in BinariesVR next to it.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("TWFC VR mod setup")]
[assembly: AssemblyProduct("Transformers: War for Cybertron VR mod")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

static class Setup
{
    public const string Version = "1.0";
    public const string KnownExeMd5 = "69acde90af160650ee5f60fff2f57d50";

    // Game files the VR copy needs (copied from Binaries; D3D9.dll is ours - dgVoodoo)
    static readonly string[] GameFiles = { "TWFC.exe", "PhysXExtensions.dll", "binkw32.dll", "cudart.dll", "fmodex.dll", "nvtt.dll" };
    // Resource name -> path relative to the game folder
    static readonly string[,] Payload = {
        { "xinput1_3.dll",     @"BinariesVR\xinput1_3.dll" },
        { "openxr_loader.dll", @"BinariesVR\openxr_loader.dll" },
        { "D3D9.dll",          @"BinariesVR\D3D9.dll" },
        { "dgVoodoo.conf",     @"BinariesVR\dgVoodoo.conf" },
        { "launch_en.bat",     @"Launch VR.bat" },
        { "launch_ru.bat",     "\u0417\u0430\u043f\u0443\u0441\u043a VR.bat" },
        { "readme_en.txt",     @"VR mod - instructions.txt" },
        { "readme_ru.txt",     "VR-\u043c\u043e\u0434 \u2014 \u0438\u043d\u0441\u0442\u0440\u0443\u043a\u0446\u0438\u044f.txt" },
        { "controls_en.png",   @"VR controls.png" },
        { "controls_ru.png",   "VR \u0443\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435.png" },
    };
    // Files the mod writes while it runs (removed on uninstall)
    static readonly string[] RunFiles = { "twfcvr.ini", "twfcvr.log", "twfcvr_2.log", "twfcvr_headset.txt", "TWFC.exe.noLAA" };

    public static bool Ru;
    public static string T(string en, string ru) { return Ru ? ru : en; }

    // Silent mode: TWFC-VR-Setup.exe /install|/uninstall "<game folder>" [/reset] [/shortcut] [/log:<file>]
    // (exit code 0 = done, 1 = failed)
    [STAThread]
    static int Main(string[] args)
    {
        string lang = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
        Ru = lang == "ru" || lang == "uk" || lang == "be";
        if (args.Length >= 2 && (args[0] == "/install" || args[0] == "/uninstall")) {
            string logFile = null;
            foreach (var a in args) if (a.StartsWith("/log:")) logFile = a.Substring(5);
            var sb = new StringBuilder();
            Action<string> log = s => sb.AppendLine(s);
            int rc = 0;
            try {
                string game = Path.GetFullPath(args[1]);
                if (!IsGameDir(game)) throw new DirectoryNotFoundException("no Binaries\\TWFC.exe in " + game);
                if (args[0] == "/install") Install(game, Array.IndexOf(args, "/reset") >= 0, Array.IndexOf(args, "/shortcut") >= 0, log);
                else Uninstall(game, log);
            } catch (Exception e) { log("ERROR: " + e.Message); rc = 1; }
            if (logFile != null) File.WriteAllText(logFile, sb.ToString(), Encoding.UTF8);
            return rc;
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
        return 0;
    }

    // ---- finding the game ----
    public static bool IsGameDir(string d)
    {
        try { return !string.IsNullOrEmpty(d) && File.Exists(Path.Combine(d, @"Binaries\TWFC.exe")); } catch { return false; }
    }

    public static string FindGame()
    {
        var c = new List<string>();
        string self = Path.GetDirectoryName(Application.ExecutablePath);
        c.Add(self); c.Add(Path.GetDirectoryName(self) ?? "");
        // Steam libraries
        foreach (var root in new[] { Registry.CurrentUser, Registry.LocalMachine })
            foreach (var key in new[] { @"Software\Valve\Steam", @"Software\WOW6432Node\Valve\Steam" })
                try {
                    using (var k = root.OpenSubKey(key)) {
                        if (k == null) continue;
                        foreach (var v in new[] { "SteamPath", "InstallPath" }) {
                            var p = k.GetValue(v) as string;
                            if (string.IsNullOrEmpty(p)) continue;
                            p = p.Replace('/', '\\');
                            AddSteamLib(c, p);
                            var vdf = Path.Combine(p, @"steamapps\libraryfolders.vdf");
                            if (File.Exists(vdf))
                                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                                    AddSteamLib(c, m.Groups[1].Value.Replace(@"\\", @"\"));
                        }
                    }
                } catch { }
        // Uninstall entries (retail / other stores)
        foreach (var root in new[] { Registry.LocalMachine, Registry.CurrentUser })
            foreach (var key in new[] { @"Software\Microsoft\Windows\CurrentVersion\Uninstall", @"Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall" })
                try {
                    using (var k = root.OpenSubKey(key)) {
                        if (k == null) continue;
                        foreach (var n in k.GetSubKeyNames())
                            using (var s = k.OpenSubKey(n)) {
                                var name = s == null ? null : s.GetValue("DisplayName") as string;
                                if (name == null || name.IndexOf("Cybertron", StringComparison.OrdinalIgnoreCase) < 0) continue;
                                var loc = s.GetValue("InstallLocation") as string;
                                if (!string.IsNullOrEmpty(loc)) c.Add(loc.Trim('"'));
                            }
                    }
                } catch { }
        // Common folders on every drive
        foreach (var drive in Directory.GetLogicalDrives())
            foreach (var sub in new[] { "Games", @"Program Files (x86)", "Program Files", "" })
                foreach (var name in new[] { "Transformers - War for Cybertron", "Transformers War for Cybertron" })
                    c.Add(Path.Combine(Path.Combine(drive, sub), name));
        foreach (var d in c) if (IsGameDir(d)) return Path.GetFullPath(d);
        return "";
    }

    static void AddSteamLib(List<string> c, string lib)
    {
        try {
            var common = Path.Combine(lib, @"steamapps\common");
            if (!Directory.Exists(common)) return;
            foreach (var d in Directory.GetDirectories(common))
                if (d.IndexOf("Cybertron", StringComparison.OrdinalIgnoreCase) >= 0) c.Add(d);
        } catch { }
    }

    // ---- install / uninstall ----
    static byte[] Res(string name)
    {
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name)) {
            if (s == null) return null;
            var b = new byte[s.Length]; int o = 0;
            while (o < b.Length) { int r = s.Read(b, o, b.Length - o); if (r <= 0) break; o += r; }
            return b;
        }
    }

    public static string Md5(string path)
    {
        using (var md5 = MD5.Create()) using (var f = File.OpenRead(path))
            return BitConverter.ToString(md5.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
    }

    // Sets IMAGE_FILE_LARGE_ADDRESS_AWARE: the 32-bit game may use 4 GB instead of 2 (VR needs it)
    static bool SetLargeAddressAware(string exe)
    {
        using (var f = new FileStream(exe, FileMode.Open, FileAccess.ReadWrite)) {
            var br = new BinaryReader(f);
            f.Position = 0x3C; int pe = br.ReadInt32();
            f.Position = pe; if (br.ReadUInt32() != 0x00004550) throw new InvalidDataException("TWFC.exe: not a PE file");
            f.Position = pe + 4 + 18; ushort ch = br.ReadUInt16();
            if ((ch & 0x20) != 0) return false;
            var bw = new BinaryWriter(f);
            f.Position = pe + 4 + 18; bw.Write((ushort)(ch | 0x20)); bw.Flush();
            // the header checksum (as CheckSumMappedFile computes it), so the file is a valid image
            int sumAt = pe + 24 + 64;
            f.Position = 0;
            var data = new byte[f.Length]; int o = 0;
            while (o < data.Length) { int r = f.Read(data, o, data.Length - o); if (r <= 0) break; o += r; }
            data[sumAt] = data[sumAt + 1] = data[sumAt + 2] = data[sumAt + 3] = 0;
            ulong sum = 0;
            for (int i = 0; i + 1 < data.Length; i += 2) { sum += (uint)(data[i] | data[i + 1] << 8); sum = (sum & 0xFFFF) + (sum >> 16); }
            if ((data.Length & 1) != 0) { sum += data[data.Length - 1]; sum = (sum & 0xFFFF) + (sum >> 16); }
            sum = (sum & 0xFFFF) + (sum >> 16);
            f.Position = sumAt; bw.Write((uint)(sum + (ulong)data.Length)); bw.Flush();
            return true;
        }
    }

    public static void Install(string game, bool resetSettings, bool shortcut, Action<string> log)
    {
        string bin = Path.Combine(game, "Binaries"), vr = Path.Combine(game, "BinariesVR");
        Directory.CreateDirectory(vr);
        foreach (var f in GameFiles) {
            string src = Path.Combine(bin, f);
            if (!File.Exists(src)) throw new FileNotFoundException(T("Game file not found: ", "\u041d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d \u0444\u0430\u0439\u043b \u0438\u0433\u0440\u044b: ") + src);
            File.Copy(src, Path.Combine(vr, f), true);
            log("Binaries\\" + f + " -> BinariesVR");
        }
        string exe = Path.Combine(vr, "TWFC.exe");
        File.Copy(exe, exe + ".noLAA", true);
        log(SetLargeAddressAware(exe) ? T("TWFC.exe (VR copy): 4 GB memory flag set", "TWFC.exe (\u043a\u043e\u043f\u0438\u044f \u0434\u043b\u044f VR): \u0432\u043a\u043b\u044e\u0447\u0451\u043d \u0444\u043b\u0430\u0433 \u043f\u0430\u043c\u044f\u0442\u0438 4 \u0413\u0411")
                                      : T("TWFC.exe: 4 GB memory flag already set", "TWFC.exe: \u0444\u043b\u0430\u0433 \u043f\u0430\u043c\u044f\u0442\u0438 4 \u0413\u0411 \u0443\u0436\u0435 \u0441\u0442\u043e\u0438\u0442"));
        for (int i = 0; i < Payload.GetLength(0); i++) {
            var data = Res(Payload[i, 0]);
            if (data == null) continue;
            File.WriteAllBytes(Path.Combine(game, Payload[i, 1]), data);
            log(Payload[i, 1]);
        }
        string ini = Path.Combine(vr, "twfcvr.ini");
        if (File.Exists(ini)) {
            if (resetSettings) {
                File.Copy(ini, ini + ".bak", true); File.Delete(ini);
                log(T("Settings reset (old ones: BinariesVR\\twfcvr.ini.bak)", "\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u0441\u0431\u0440\u043e\u0448\u0435\u043d\u044b (\u0441\u0442\u0430\u0440\u044b\u0435: BinariesVR\\twfcvr.ini.bak)"));
            } else log(T("Your settings (twfcvr.ini) kept", "\u0412\u0430\u0448\u0438 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 (twfcvr.ini) \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u044b"));
        }
        if (shortcut) { CreateShortcut(ShortcutPath(), exe, vr); log(T("Desktop shortcut created", "\u042f\u0440\u043b\u044b\u043a \u043d\u0430 \u0440\u0430\u0431\u043e\u0447\u0435\u043c \u0441\u0442\u043e\u043b\u0435 \u0441\u043e\u0437\u0434\u0430\u043d")); }
    }

    public static void Uninstall(string game, Action<string> log)
    {
        string vr = Path.Combine(game, "BinariesVR");
        var names = new List<string>();
        foreach (var f in GameFiles) names.Add(Path.Combine(vr, f));
        foreach (var f in RunFiles) names.Add(Path.Combine(vr, f));
        names.Add(Path.Combine(vr, "twfcvr.ini.bak"));
        for (int i = 0; i < Payload.GetLength(0); i++) names.Add(Path.Combine(game, Payload[i, 1]));
        foreach (var n in names)
            if (File.Exists(n)) { File.Delete(n); log(T("deleted ", "\u0443\u0434\u0430\u043b\u0451\u043d ") + n.Substring(game.Length).TrimStart('\\')); }
        try { if (Directory.Exists(vr) && Directory.GetFileSystemEntries(vr).Length == 0) Directory.Delete(vr); } catch { }
        if (File.Exists(ShortcutPath())) { File.Delete(ShortcutPath()); log(T("Desktop shortcut deleted", "\u042f\u0440\u043b\u044b\u043a \u0443\u0434\u0430\u043b\u0451\u043d")); }
    }

    static string ShortcutPath()
    {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Transformers WFC VR.lnk");
    }

    static void CreateShortcut(string lnk, string target, string workDir)
    {
        var t = Type.GetTypeFromProgID("WScript.Shell");
        object sh = Activator.CreateInstance(t);
        object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, sh, new object[] { lnk });
        var st = sc.GetType();
        st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
        st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { workDir });
        st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { "Transformers: War for Cybertron VR" });
        st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
    }

    public static bool GameRunning()
    {
        try { return Process.GetProcessesByName("TWFC").Length > 0; } catch { return false; }
    }
}

class SetupForm : Form
{
    TextBox path = new TextBox(), logBox = new TextBox();
    CheckBox reset = new CheckBox(), shortcut = new CheckBox();
    Button install = new Button(), remove = new Button(), browse = new Button();

    static string T(string en, string ru) { return Setup.T(en, ru); }

    public SetupForm()
    {
        Text = T("TWFC VR mod setup ", "\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 VR-\u043c\u043e\u0434\u0430 TWFC ") + Setup.Version;
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(620, 430);
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        var title = new Label { Left = 12, Top = 10, Width = 596, Height = 22, Font = new Font("Segoe UI", 11f, FontStyle.Bold),
            Text = "Transformers: War for Cybertron \u2014 VR " + T("mod", "\u043c\u043e\u0434") + " " + Setup.Version };
        var info = new Label { Left = 12, Top = 36, Width = 596, Height = 48, Text = T(
            "Installs the mod into a BinariesVR folder next to the game's Binaries. The game's own files are not changed:\n" +
            "the normal game still starts as before. You need a PC VR headset with an OpenXR runtime\n" +
            "(SteamVR, Virtual Desktop, Oculus/Meta Link...).",
            "\u041c\u043e\u0434 \u0441\u0442\u0430\u0432\u0438\u0442\u0441\u044f \u0432 \u043f\u0430\u043f\u043a\u0443 BinariesVR \u0440\u044f\u0434\u043e\u043c \u0441 Binaries \u0438\u0433\u0440\u044b. \u0424\u0430\u0439\u043b\u044b \u0441\u0430\u043c\u043e\u0439 \u0438\u0433\u0440\u044b \u043d\u0435 \u043c\u0435\u043d\u044f\u044e\u0442\u0441\u044f:\n" +
            "\u043e\u0431\u044b\u0447\u043d\u0430\u044f \u0438\u0433\u0440\u0430 \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0435\u0442\u0441\u044f \u043a\u0430\u043a \u0440\u0430\u043d\u044c\u0448\u0435. \u041d\u0443\u0436\u0435\u043d \u041f\u041a VR-\u0448\u043b\u0435\u043c \u0441 OpenXR\n" +
            "(SteamVR, Virtual Desktop, Oculus/Meta Link...).") };
        var pl = new Label { Left = 12, Top = 92, Width = 596, Text = T("Game folder (the one with Binaries\\TWFC.exe):", "\u041f\u0430\u043f\u043a\u0430 \u0438\u0433\u0440\u044b (\u0442\u0430, \u0433\u0434\u0435 Binaries\\TWFC.exe):") };
        path.SetBounds(12, 114, 500, 24);
        browse.SetBounds(518, 113, 90, 26); browse.Text = T("Browse...", "\u041e\u0431\u0437\u043e\u0440...");
        reset.SetBounds(12, 146, 596, 22); reset.Text = T("Reset VR settings to the defaults (yours are kept as twfcvr.ini.bak)",
            "\u0421\u0431\u0440\u043e\u0441\u0438\u0442\u044c \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 VR \u043a \u0441\u0442\u0430\u043d\u0434\u0430\u0440\u0442\u043d\u044b\u043c (\u0432\u0430\u0448\u0438 \u043e\u0441\u0442\u0430\u043d\u0443\u0442\u0441\u044f \u043a\u0430\u043a twfcvr.ini.bak)");
        shortcut.SetBounds(12, 170, 596, 22); shortcut.Text = T("Desktop shortcut \"Transformers WFC VR\"", "\u042f\u0440\u043b\u044b\u043a \u00abTransformers WFC VR\u00bb \u043d\u0430 \u0440\u0430\u0431\u043e\u0447\u0435\u043c \u0441\u0442\u043e\u043b\u0435");
        shortcut.Checked = true;
        install.SetBounds(12, 200, 140, 32); install.Text = T("Install", "\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c");
        remove.SetBounds(160, 200, 140, 32); remove.Text = T("Uninstall", "\u0423\u0434\u0430\u043b\u0438\u0442\u044c \u043c\u043e\u0434");
        logBox.SetBounds(12, 242, 596, 176); logBox.Multiline = true; logBox.ReadOnly = true; logBox.ScrollBars = ScrollBars.Vertical;
        Controls.AddRange(new Control[] { title, info, pl, path, browse, reset, shortcut, install, remove, logBox });

        browse.Click += delegate {
            using (var d = new FolderBrowserDialog { Description = pl.Text, SelectedPath = path.Text })
                if (d.ShowDialog(this) == DialogResult.OK) { path.Text = d.SelectedPath; Refresh2(); }
        };
        path.TextChanged += delegate { Refresh2(); };
        install.Click += delegate { Run(true); };
        remove.Click += delegate { Run(false); };

        path.Text = Setup.FindGame();
        if (path.Text == "") Log(T("The game was not found automatically: choose its folder.", "\u0418\u0433\u0440\u0430 \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u0430 \u0430\u0432\u0442\u043e\u043c\u0430\u0442\u0438\u0447\u0435\u0441\u043a\u0438: \u0443\u043a\u0430\u0436\u0438\u0442\u0435 \u0435\u0451 \u043f\u0430\u043f\u043a\u0443."));
        Refresh2();
    }

    void Refresh2()
    {
        bool ok = Setup.IsGameDir(path.Text);
        install.Enabled = ok;
        remove.Enabled = ok && File.Exists(Path.Combine(path.Text, @"BinariesVR\xinput1_3.dll"));
        reset.Enabled = ok && File.Exists(Path.Combine(path.Text, @"BinariesVR\twfcvr.ini"));
        if (!reset.Enabled) reset.Checked = false;
    }

    void Log(string s) { logBox.AppendText(s + "\r\n"); }

    void Run(bool doInstall)
    {
        string game = Path.GetFullPath(path.Text.Trim());
        if (Setup.GameRunning()) {
            MessageBox.Show(this, T("Close the game first.", "\u0421\u043d\u0430\u0447\u0430\u043b\u0430 \u0437\u0430\u043a\u0440\u043e\u0439\u0442\u0435 \u0438\u0433\u0440\u0443."), Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (doInstall) {
            string md5 = "";
            try { md5 = Setup.Md5(Path.Combine(game, @"Binaries\TWFC.exe")); } catch { }
            if (md5 != Setup.KnownExeMd5 &&
                MessageBox.Show(this, T("Binaries\\TWFC.exe is not the version the mod was made for (PC, patch with the 2016 exe).\n" +
                    "The mod finds the game's code by its addresses and may not work with it. Install anyway?",
                    "Binaries\\TWFC.exe \u2014 \u043d\u0435 \u0442\u0430 \u0432\u0435\u0440\u0441\u0438\u044f, \u043f\u043e\u0434 \u043a\u043e\u0442\u043e\u0440\u0443\u044e \u0441\u0434\u0435\u043b\u0430\u043d \u043c\u043e\u0434 (PC, exe 2016 \u0433\u043e\u0434\u0430).\n" +
                    "\u041c\u043e\u0434 \u0438\u0449\u0435\u0442 \u043a\u043e\u0434 \u0438\u0433\u0440\u044b \u043f\u043e \u0430\u0434\u0440\u0435\u0441\u0430\u043c \u0438 \u043c\u043e\u0436\u0435\u0442 \u0441 \u043d\u0435\u0439 \u043d\u0435 \u0440\u0430\u0431\u043e\u0442\u0430\u0442\u044c. \u0412\u0441\u0451 \u0440\u0430\u0432\u043d\u043e \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c?"),
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
        } else if (MessageBox.Show(this, T("Remove the VR mod (BinariesVR, its settings, the VR launchers and instructions)?\nThe game itself stays.",
                    "\u0423\u0434\u0430\u043b\u0438\u0442\u044c VR-\u043c\u043e\u0434 (BinariesVR, \u0435\u0433\u043e \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438, \u0437\u0430\u043f\u0443\u0441\u043a VR \u0438 \u0438\u043d\u0441\u0442\u0440\u0443\u043a\u0446\u0438\u0438)?\n\u0421\u0430\u043c\u0430 \u0438\u0433\u0440\u0430 \u043e\u0441\u0442\u0430\u043d\u0435\u0442\u0441\u044f."),
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

        logBox.Clear();
        Cursor = Cursors.WaitCursor;
        try {
            if (doInstall) {
                Setup.Install(game, reset.Checked, shortcut.Checked, Log);
                Log("");
                Log(T("Done. Start your OpenXR runtime (SteamVR / Virtual Desktop / Link), then \"Launch VR.bat\"",
                      "\u0413\u043e\u0442\u043e\u0432\u043e. \u0417\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u0435 OpenXR (SteamVR / Virtual Desktop / Link), \u0437\u0430\u0442\u0435\u043c \u00ab\u0417\u0430\u043f\u0443\u0441\u043a VR.bat\u00bb"));
                Log(T("or the desktop shortcut. Controls: \"VR controls.png\" and the instructions in the game folder.",
                      "\u0438\u043b\u0438 \u044f\u0440\u043b\u044b\u043a. \u0423\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435: \u00abVR \u0443\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435.png\u00bb \u0438 \u0438\u043d\u0441\u0442\u0440\u0443\u043a\u0446\u0438\u044f \u0432 \u043f\u0430\u043f\u043a\u0435 \u0438\u0433\u0440\u044b."));
            } else {
                Setup.Uninstall(game, Log);
                Log(T("The VR mod is removed.", "VR-\u043c\u043e\u0434 \u0443\u0434\u0430\u043b\u0451\u043d."));
            }
        } catch (UnauthorizedAccessException) {
            Log(T("No permission to write to the game folder.", "\u041d\u0435\u0442 \u043f\u0440\u0430\u0432 \u043d\u0430 \u0437\u0430\u043f\u0438\u0441\u044c \u0432 \u043f\u0430\u043f\u043a\u0443 \u0438\u0433\u0440\u044b."));
            if (MessageBox.Show(this, T("No permission to write to the game folder. Restart the setup as administrator?",
                    "\u041d\u0435\u0442 \u043f\u0440\u0430\u0432 \u043d\u0430 \u0437\u0430\u043f\u0438\u0441\u044c \u0432 \u043f\u0430\u043f\u043a\u0443 \u0438\u0433\u0440\u044b. \u041f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0443 \u043e\u0442 \u0438\u043c\u0435\u043d\u0438 \u0430\u0434\u043c\u0438\u043d\u0438\u0441\u0442\u0440\u0430\u0442\u043e\u0440\u0430?"),
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes) {
                try { Process.Start(new ProcessStartInfo(Application.ExecutablePath) { Verb = "runas", UseShellExecute = true }); Close(); } catch { }
            }
        } catch (Exception e) {
            Log(T("ERROR: ", "\u041e\u0428\u0418\u0411\u041a\u0410: ") + e.Message);
        } finally {
            Cursor = Cursors.Default;
            Refresh2();
        }
    }
}
