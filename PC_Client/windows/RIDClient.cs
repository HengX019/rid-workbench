using System;
using System.Diagnostics;
using System.Drawing;
using System.Management;
using System.Threading;
using System.Windows.Forms;

namespace RidClient {
    static class Program {
        [STAThread] static void Main(string[] args) {
            bool created;
            using (var mutex = new Mutex(true, "Local\\RIDWorkbenchClient", out created)) {
                if (!created) return;
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm(Array.IndexOf(args, "--background") >= 0));
            }
        }
    }
    class MainForm : Form {
        const string BoardUrl = "http://192.168.4.1/";
        readonly Label status;
        readonly System.Windows.Forms.Timer timer;
        readonly NotifyIcon tray;
        bool previous, first = true, checking;
        public MainForm(bool background) {
            Text = "RID 工作台"; Width = 590; Height = 265; StartPosition = FormStartPosition.CenterScreen;
            var heading = new Label { Text = "RID 工作台 · 电脑端", Font = new Font(SystemFonts.DefaultFont.FontFamily, 16), AutoSize = true, Location = new Point(22, 20) };
            status = new Label { Text = "正在检查 USB 设备…", AutoSize = false, Size = new Size(530, 42), Location = new Point(25, 65) };
            var instructions = new Label { Text = "请将电脑连接到设备 Wi-Fi 热点，然后打开设备页面。", AutoSize = false, Size = new Size(530, 30), Location = new Point(25, 110) };
            var open = new Button { Text = "打开设备页面", Size = new Size(130, 32), Location = new Point(25, 155) };
            var exit = new Button { Text = "退出监听", Size = new Size(110, 32), Location = new Point(165, 155) };
            open.Click += delegate { OpenPage(); };
            exit.Click += delegate { ExitClient(); };
            Controls.AddRange(new Control[] { heading, status, instructions, open, exit });
            var trayMenu = new ContextMenuStrip();
            trayMenu.Items.Add("打开 RID 工作台", null, delegate { ShowClient(); });
            trayMenu.Items.Add("打开设备页面", null, delegate { OpenPage(); });
            trayMenu.Items.Add("退出监听", null, delegate { ExitClient(); });
            tray = new NotifyIcon { Icon = SystemIcons.Application, Text = "RID 工作台", ContextMenuStrip = trayMenu, Visible = true };
            tray.DoubleClick += delegate { ShowClient(); };
            FormClosing += delegate(object sender, FormClosingEventArgs e) { if (e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); } };
            Shown += delegate { if (background) Hide(); };
            timer = new System.Windows.Forms.Timer(); timer.Interval = 3000; timer.Tick += delegate { CheckUsb(background); }; timer.Start();
            CheckUsb(background);
        }
        void ShowClient() { Show(); WindowState = FormWindowState.Normal; Activate(); }
        void ExitClient() { timer.Stop(); tray.Visible = false; tray.Dispose(); Application.Exit(); }
        void OpenPage() {
            try { Process.Start(new ProcessStartInfo(BoardUrl) { UseShellExecute = true }); }
            catch (Exception ex) { MessageBox.Show("无法打开浏览器：" + ex.Message, "RID 工作台"); }
        }
        void CheckUsb(bool background) {
            if (checking) return; checking = true;
            ThreadPool.QueueUserWorkItem(delegate {
                bool present = false; string error = null;
                try {
                    using (var search = new ManagementObjectSearcher("SELECT PNPDeviceID FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB%'") )
                    using (var devices = search.Get()) {
                        foreach (ManagementObject device in devices) {
                            using (device) { var id = Convert.ToString(device["PNPDeviceID"]); if (id.IndexOf("VID_303A&PID_1001", StringComparison.OrdinalIgnoreCase) >= 0) { present = true; break; } }
                        }
                    }
                } catch (Exception ex) { error = ex.Message; }
                if (IsDisposed || !IsHandleCreated) { checking = false; return; }
                try { BeginInvoke((MethodInvoker)delegate {
                    checking = false;
                    status.Text = error != null ? "USB 检查不可用：" + error : present ? "已检测到ESP32-S3 USB 设备。" : "等待ESP32-S3 USB 设备。";
                    if (error == null) {
                        if (present && !previous && (!first || background)) { ShowClient(); OpenPage(); }
                        previous = present; first = false;
                    }
                }); } catch (InvalidOperationException) { checking = false; }
            });
        }
    }
}
