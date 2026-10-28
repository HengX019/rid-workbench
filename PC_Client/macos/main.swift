import AppKit
import WebKit
import IOKit

let boardURL = URL(string: "http://192.168.4.1/")!
let launchLabel = "com.ridworkbench.pcclient"

final class Client: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!
    var web: WKWebView!
    var status: NSTextField!
    var loginButton: NSButton!
    var timer: Timer?
    var wasPresent = false
    var firstPoll = true
    let backgroundStart = CommandLine.arguments.contains("--background")
    var agentPath: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(launchLabel).plist") }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let mainMenu = NSMenu()
        let appMenuItem = NSMenuItem(); mainMenu.addItem(appMenuItem)
        let appMenu = NSMenu(); appMenu.addItem(withTitle: "退出 RID工作台", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appMenuItem.submenu = appMenu; NSApp.mainMenu = mainMenu
        let rect = NSRect(x: 0, y: 0, width: 1120, height: 740)
        window = NSWindow(contentRect: rect, styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "RID 工作台"; window.center(); window.delegate = self
        let container = NSView(frame: rect); container.autoresizingMask = [.width, .height]; window.contentView = container
        let bar = NSView(frame: NSRect(x: 0, y: rect.height - 64, width: rect.width, height: 64))
        bar.autoresizingMask = [.width, .minYMargin]; container.addSubview(bar)
        status = NSTextField(labelWithString: "正在检查 USB 设备。请将电脑连接到设备的 Wi-Fi 热点。")
        status.frame = NSRect(x: 15, y: 36, width: 800, height: 20); status.autoresizingMask = [.width]; bar.addSubview(status)
        let open = NSButton(title: "打开设备页面", target: self, action: #selector(openPage))
        open.frame = NSRect(x: 12, y: 5, width: 125, height: 26); bar.addSubview(open)
        let browser = NSButton(title: "浏览器打开", target: self, action: #selector(openBrowser))
        browser.frame = NSRect(x: 145, y: 5, width: 115, height: 26); bar.addSubview(browser)
        loginButton = NSButton(title: "", target: self, action: #selector(toggleLogin))
        loginButton.frame = NSRect(x: 265, y: 5, width: 190, height: 26); bar.addSubview(loginButton); refreshLogin()
        let config = WKWebViewConfiguration()
        web = WKWebView(frame: NSRect(x: 0, y: 0, width: rect.width, height: rect.height - 64), configuration: config)
        web.autoresizingMask = [.width, .height]; container.addSubview(web)
        if !backgroundStart { showWindow(); openPage() }
        pollUSB(); timer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in self?.pollUSB() }
    }
    func showWindow() { window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true) }
    @objc func openPage() { web.load(URLRequest(url: boardURL, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 12)); showWindow() }
    @objc func openBrowser() { NSWorkspace.shared.open(boardURL) }
    func isBoardPresent() -> Bool {
        guard let matching = IOServiceMatching("IOUSBHostDevice") else { return false }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return false }
        defer { IOObjectRelease(iterator) }
        while case let device = IOIteratorNext(iterator), device != 0 {
            let value = IORegistryEntryCreateCFProperty(device, "idVendor" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
            let product = IORegistryEntryCreateCFProperty(device, "idProduct" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
            IOObjectRelease(device)
            if value?.intValue == 0x303a && product?.intValue == 0x1001 { return true }
        }
        return false
    }
    func pollUSB() {
        let present = isBoardPresent()
        status.stringValue = present ? "已检测到ESP32-S3 USB 设备。请将电脑连接设备 Wi-Fi，再打开设备页面。" : "等待ESP32-S3 USB 设备。设备页面通过设备 Wi-Fi 连接。"
        if present && !wasPresent && (!firstPoll || backgroundStart) { openPage() }
        wasPresent = present; firstPoll = false
    }
    func refreshLogin() { loginButton.title = FileManager.default.fileExists(atPath: agentPath.path) ? "关闭登录后自动监听" : "开启登录后自动监听" }
    @objc func toggleLogin() {
        do {
            let fm = FileManager.default
            if fm.fileExists(atPath: agentPath.path) {
                try fm.removeItem(at: agentPath)
            } else {
                try fm.createDirectory(at: agentPath.deletingLastPathComponent(), withIntermediateDirectories: true)
                let executable = Bundle.main.executableURL!.path
                let data = try PropertyListSerialization.data(fromPropertyList: ["Label": launchLabel, "ProgramArguments": [executable, "--background"], "RunAtLoad": true], format: .xml, options: 0)
                try data.write(to: agentPath, options: .atomic)
            }
            refreshLogin()
            let alert = NSAlert(); alert.messageText = "设置已保存"; alert.informativeText = "下次登录 macOS 时生效。请保持应用位置不变；插入设备时客户端会弹出。"; alert.runModal()
        } catch {
            let alert = NSAlert(error: error); alert.runModal()
        }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { window.orderOut(nil); return false }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { showWindow(); return true }
}
let app = NSApplication.shared
let delegate = Client(); app.delegate = delegate
app.setActivationPolicy(.regular); app.run()
