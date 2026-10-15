// NanoSyncMac.swift -- "NanoSync Mac": send music from the Mac to an iPod
// nano 7G for the on-device NanoSync homebrew app to add to the library.
//
// How NanoSync works (strings of /Apps/Executables/Nanosync.hbapp): on the
// iPod it imports every .mp3 found in /Apps/Data/Importar/ into the music
// library (iTunesCDB + SQLite + ArtworkDB, JPEG covers from the ID3 tag),
// removes each source after importing it, and leaves its report in
// /Apps/Data/Importar/log.txt and done.txt. A dryrun.txt file in that
// folder makes it a rehearsal that writes nothing.
//
// So the Mac side only has to put MP3 files there while the iPod is
// connected over USB as a disk (Finder shows it as a drive with an
// iPod_Control folder). This app:
//   - finds that drive (and notices it being plugged in or ejected),
//   - takes files and folders dropped on its window,
//   - copies MP3s as they are and converts other audio (m4a, aac, wav,
//     aiff, flac, ogg, opus, wma, alac) to MP3 with ffmpeg when it is
//     installed (brew install ffmpeg), keeping the tags and the cover,
//   - gives each file a short FAT-safe name (NanoSync skips names that are
//     too long), never overwriting a file already waiting there,
//   - ejects the iPod and shows NanoSync's last report.
// Video files are listed but not sent: NanoSync only imports MP3 audio.
//
// Command line (scripts and tests):
//   "NanoSync Mac" --cli [--ipod /Volumes/NAME] FILE_OR_FOLDER...
//
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import SwiftUI

// MARK: - files

enum Kind { case mp3, convert, video, other }

let convertExts: Set<String> = ["m4a", "aac", "wav", "aif", "aiff", "flac",
                                "ogg", "oga", "opus", "wma", "alac", "caf"]
let videoExts: Set<String> = ["mp4", "m4v", "mov", "mkv", "avi", "webm",
                              "wmv", "mpg", "mpeg", "3gp"]

func kind(of url: URL) -> Kind {
    let e = url.pathExtension.lowercased()
    if e == "mp3" { return .mp3 }
    if convertExts.contains(e) { return .convert }
    if videoExts.contains(e) { return .video }
    return .other
}

/// every regular file under the dropped items (folders are walked)
func expand(_ urls: [URL]) -> [URL] {
    var out: [URL] = []
    let fm = FileManager.default
    for u in urls {
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { continue }
        if isDir.boolValue {
            let en = fm.enumerator(at: u, includingPropertiesForKeys: nil,
                                   options: [.skipsHiddenFiles])
            while let f = en?.nextObject() as? URL {
                var d: ObjCBool = false
                if fm.fileExists(atPath: f.path, isDirectory: &d), !d.boolValue {
                    out.append(f)
                }
            }
        } else if !u.lastPathComponent.hasPrefix(".") {
            out.append(u)
        }
    }
    return out
}

/// a short FAT-safe ASCII name ending in .mp3 (NanoSync skips long names)
func safeName(for url: URL) -> String {
    let base = url.deletingPathExtension().lastPathComponent
    let folded = base.applyingTransform(.toLatin, reverse: false) ?? base
    let ascii = folded.folding(options: [.diacriticInsensitive, .widthInsensitive],
                               locale: nil)
    var s = ""
    for ch in ascii.unicodeScalars {
        if ch.isASCII, ch.value >= 0x20, !"\\/:*?\"<>|".unicodeScalars.contains(ch) {
            s.unicodeScalars.append(ch)
        } else {
            s.append("_")
        }
    }
    s = s.trimmingCharacters(in: CharacterSet(charactersIn: " ._"))
    if s.isEmpty { s = "track" }
    if s.count > 56 { s = String(s.prefix(56)).trimmingCharacters(in: .whitespaces) }
    return s + ".mp3"
}

// MARK: - the iPod

struct IPod {
    let root: URL
    var name: String { root.lastPathComponent }
    var importDir: URL { root.appendingPathComponent("Apps/Data/Importar") }
    var hasNanoSync: Bool {
        FileManager.default.fileExists(
            atPath: root.appendingPathComponent("Apps/Executables/Nanosync.hbapp").path)
    }
    var freeBytes: Int64 {
        let v = try? root.resourceValues(forKeys: [.volumeAvailableCapacityKey])
        return Int64(v?.volumeAvailableCapacity ?? 0)
    }
    var dryRun: Bool {
        FileManager.default.fileExists(atPath: importDir.appendingPathComponent("dryrun.txt").path)
    }
    /// files already waiting for NanoSync
    var pending: [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: importDir.path)) ?? [])
            .filter { $0.lowercased().hasSuffix(".mp3") && !$0.hasPrefix(".") }
    }
    /// NanoSync's last report (done.txt, else the end of log.txt)
    var lastReport: String {
        for f in ["done.txt", "log.txt"] {
            let p = importDir.appendingPathComponent(f)
            if let t = (try? String(contentsOf: p, encoding: .utf8))
                ?? (try? String(contentsOf: p, encoding: .isoLatin1)) {
                let lines = t.split(separator: "\n", omittingEmptySubsequences: true)
                return lines.suffix(8).joined(separator: "\n")
            }
        }
        return ""
    }

    /// mounted drives that look like an iPod (they have iPod_Control)
    static func find() -> [IPod] {
        let keys: [URLResourceKey] = [.volumeIsRemovableKey]
        let vols = FileManager.default.mountedVolumeURLs(
            includingResourceValuesForKeys: keys, options: [.skipHiddenVolumes]) ?? []
        return vols.filter {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent("iPod_Control").path)
        }.map { IPod(root: $0) }
    }
}

// MARK: - sending

func ffmpegPath() -> String? {
    for p in ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]
    where FileManager.default.isExecutableFile(atPath: p) {
        return p
    }
    return nil
}

/// audio -> MP3 (tags and a JPEG cover kept; ID3v2.3, which NanoSync reads)
func convert(_ src: URL, to dst: URL) throws {
    guard let ff = ffmpegPath() else {
        throw NSError(domain: "NanoSync", code: 1, userInfo: [NSLocalizedDescriptionKey:
            "ffmpeg is needed to convert this file (brew install ffmpeg)"])
    }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: ff)
    p.arguments = ["-nostdin", "-loglevel", "error", "-y", "-i", src.path,
                   "-map", "0:a:0", "-map", "0:v:0?", "-c:a", "libmp3lame",
                   "-q:a", "2", "-c:v", "mjpeg", "-disposition:v", "attached_pic",
                   "-map_metadata", "0", "-id3v2_version", "3", dst.path]
    let err = Pipe()
    p.standardError = err
    try p.run()
    p.waitUntilExit()
    if p.terminationStatus != 0 {
        let msg = String(data: err.fileHandleForReading.readDataToEndOfFile(),
                         encoding: .utf8) ?? ""
        throw NSError(domain: "NanoSync", code: 2, userInfo: [NSLocalizedDescriptionKey:
            "ffmpeg failed: " + msg.trimmingCharacters(in: .whitespacesAndNewlines)])
    }
}

/// copy the bytes only: no extended attributes, so macOS writes no "._"
/// AppleDouble companion that NanoSync would see as another .mp3
func copyData(_ src: URL, to dst: URL) throws {
    if copyfile(src.path, dst.path, nil, copyfile_flags_t(COPYFILE_DATA)) != 0 {
        throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
    }
}

/// remove the "._*" AppleDouble files macOS leaves on FAT drives
@discardableResult
func removeAppleDouble(in dir: URL) -> Bool {
    let fm = FileManager.default
    var found = false
    for n in (try? fm.contentsOfDirectory(atPath: dir.path)) ?? [] where n.hasPrefix("._") {
        try? fm.removeItem(at: dir.appendingPathComponent(n))
        found = true
    }
    return found
}

/// remove "._*" files in dir for a while, from a separate process that
/// outlives this one
func sweepLater(_ dir: URL) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/bin/sh")
    p.arguments = ["-c", "for i in 1 2 3 4 5 6; do sleep 2; " +
                   "rm -f \"$0\"/._* 2>/dev/null; done", dir.path]
    p.standardInput = FileHandle.nullDevice
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try? p.run()
}

struct SendResult { var sent = 0; var skipped: [String] = []; var failed: [String] = [] }

/// copy / convert `files` into the iPod's Importar folder
func send(_ files: [URL], to ipod: IPod, progress: (Int, String) -> Void) -> SendResult {
    var r = SendResult()
    let fm = FileManager.default
    do {
        try fm.createDirectory(at: ipod.importDir, withIntermediateDirectories: true)
    } catch {
        r.failed.append("could not create Apps/Data/Importar: \(error.localizedDescription)")
        return r
    }
    var used = Set(ipod.pending.map { $0.lowercased() })
    for (i, f) in files.enumerated() {
        progress(i, f.lastPathComponent)
        let k = kind(of: f)
        if k == .video || k == .other {
            r.skipped.append(f.lastPathComponent + (k == .video ?
                " (video: NanoSync imports music only)" : " (format not supported)"))
            continue
        }
        var name = safeName(for: f)
        var n = 2
        while used.contains(name.lowercased()) {        // never overwrite
            name = String(safeName(for: f).dropLast(4).prefix(52)) + " \(n).mp3"
            n += 1
        }
        let dst = ipod.importDir.appendingPathComponent(name)
        let tmp = ipod.importDir.appendingPathComponent("." + name + ".part")
        do {
            try? fm.removeItem(at: tmp)
            if k == .mp3 {
                try copyData(f, to: tmp)
            } else {
                let local = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".mp3")
                defer { try? fm.removeItem(at: local) }
                try convert(f, to: local)
                try copyData(local, to: tmp)
            }
            try fm.moveItem(at: tmp, to: dst)       // complete files only
            used.insert(name.lowercased())
            r.sent += 1
        } catch {
            try? fm.removeItem(at: tmp)
            r.failed.append(f.lastPathComponent + ": " + error.localizedDescription)
        }
    }
    // macOS writes the "._" provenance companions lazily, up to when this
    // process exits: a small detached sweeper removes them for the next 12 s
    // (the window also sweeps every 2 s while it stays open, see Model)
    sweepLater(ipod.importDir)
    progress(files.count, "")
    return r
}

func fmtBytes(_ b: Int64) -> String {
    ByteCountFormatter.string(fromByteCount: b, countStyle: .file)
}

// MARK: - command line

func runCLI(_ args: [String]) -> Int32 {
    var ipodPath: String?
    var items: [URL] = []
    var i = 0
    while i < args.count {
        if args[i] == "--ipod", i + 1 < args.count {
            ipodPath = args[i + 1]; i += 2; continue
        }
        items.append(URL(fileURLWithPath: args[i])); i += 1
    }
    let ipod: IPod
    if let p = ipodPath {
        ipod = IPod(root: URL(fileURLWithPath: p))
    } else if let first = IPod.find().first {
        ipod = first
    } else {
        FileHandle.standardError.write("No iPod connected as a disk.\n".data(using: .utf8)!)
        return 1
    }
    let files = expand(items)
    print("iPod: \(ipod.root.path) (\(fmtBytes(ipod.freeBytes)) free)")
    let r = send(files, to: ipod) { n, name in
        if !name.isEmpty { print("[\(n + 1)/\(files.count)] \(name)") }
    }
    print("sent \(r.sent), skipped \(r.skipped.count), failed \(r.failed.count)")
    r.skipped.forEach { print("  skipped: " + $0) }
    r.failed.forEach { print("  failed: " + $0) }
    print("Waiting on the iPod: \(ipod.pending.count) song(s)")
    return r.failed.isEmpty ? 0 : 2
}

// MARK: - window

struct Item: Identifiable {
    let id = UUID()
    let url: URL
    var kind: Kind { NanoSyncMac.kind(of: url) }
}

@MainActor
final class Model: ObservableObject {
    @Published var items: [Item] = []
    @Published var ipod: IPod?
    @Published var busy = false
    @Published var done = 0
    @Published var current = ""
    @Published var message = ""
    @Published var refreshTick = 0
    private var observers: [NSObjectProtocol] = []

    init() {
        refresh()
        startSweeper()
        let nc = NSWorkspace.shared.notificationCenter
        for n in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification] {
            observers.append(nc.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.refresh() }
            })
        }
    }

    func refresh() {
        ipod = IPod.find().first
        refreshTick += 1
    }

    /// every 2 s while an iPod is connected: drop the "._" files macOS
    /// keeps adding next to the waiting MP3s
    private var sweeper: Timer?
    func startSweeper() {
        sweeper = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in
                if let d = self?.ipod?.importDir { removeAppleDouble(in: d) }
            }
        }
    }

    var sendable: [URL] { items.map(\.url).filter { [.mp3, .convert].contains(NanoSyncMac.kind(of: $0)) } }
    var needsFFmpeg: Bool { items.contains { $0.kind == .convert } && ffmpegPath() == nil }
    var totalBytes: Int64 {
        sendable.reduce(0) { s, u in
            s + Int64((try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        }
    }

    func add(_ urls: [URL]) {
        let known = Set(items.map(\.url))
        for u in expand(urls) where !known.contains(u) {
            items.append(Item(url: u))
        }
    }

    func sync() {
        guard let ipod, !busy else { return }
        let files = sendable
        busy = true
        done = 0
        message = ""
        Task.detached {
            let r = send(files, to: ipod) { n, name in
                Task { @MainActor in self.done = n; self.current = name }
            }
            await MainActor.run {
                self.busy = false
                self.current = ""
                var m = "Sent \(r.sent) song(s) to the iPod."
                if !r.skipped.isEmpty { m += "\nSkipped: " + r.skipped.joined(separator: "; ") }
                if !r.failed.isEmpty { m += "\nFailed: " + r.failed.joined(separator: "; ") }
                if r.sent > 0 {
                    m += "\n\nNow eject the iPod, open NanoSync on it and tap Importar."
                    self.items.removeAll { [.mp3, .convert].contains($0.kind) }
                }
                self.message = m
                self.refresh()
            }
        }
    }

    func eject() {
        guard let ipod else { return }
        removeAppleDouble(in: ipod.importDir)
        do {
            try NSWorkspace.shared.unmountAndEjectDevice(at: ipod.root)
            message = "iPod ejected. Open NanoSync on the iPod and tap Importar."
        } catch {
            message = "Could not eject: \(error.localizedDescription)"
        }
        refresh()
    }
}

struct ContentView: View {
    @StateObject var m = Model()
    @State var hovering = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            ipodBox
            dropZone
            if !m.items.isEmpty { list }
            HStack {
                Button("Clear List") { m.items.removeAll() }
                    .disabled(m.items.isEmpty || m.busy)
                Spacer()
                if m.busy {
                    ProgressView(value: Double(m.done), total: Double(max(m.sendable.count, 1)))
                        .frame(width: 160)
                    Text(m.current).lineLimit(1).font(.caption).frame(maxWidth: 180)
                }
                Button("Sync") { m.sync() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(m.ipod == nil || m.sendable.isEmpty || m.busy)
            }
            if !m.message.isEmpty {
                Text(m.message).font(.callout).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10).background(Color.secondary.opacity(0.12)).cornerRadius(8)
            }
        }
        .padding(18)
        .frame(minWidth: 520, minHeight: 460)
    }

    var ipodBox: some View {
        GroupBox {
            if let ipod = m.ipod {
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Image(systemName: "ipod").font(.title2)
                        Text("iPod connected: \(ipod.name)").font(.headline)
                        Spacer()
                        Button("Eject") { m.eject() }.disabled(m.busy)
                    }
                    Text("Free: \(fmtBytes(ipod.freeBytes))   Waiting to import: \(ipod.pending.count) song(s)")
                        .font(.callout).foregroundStyle(.secondary)
                    if !ipod.hasNanoSync {
                        Text("NanoSync is not on this iPod (Apps/Executables/Nanosync.hbapp).")
                            .font(.callout).foregroundStyle(.orange)
                    }
                    if ipod.dryRun {
                        Text("Rehearsal mode is on (Importar/dryrun.txt): NanoSync will not write anything.")
                            .font(.callout).foregroundStyle(.orange)
                    }
                    let rep = ipod.lastReport
                    if !rep.isEmpty {
                        Text("NanoSync's last report:").font(.caption).padding(.top, 4)
                        Text(rep).font(.caption.monospaced()).foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                }.frame(maxWidth: .infinity, alignment: .leading)
            } else {
                HStack(spacing: 10) {
                    Image(systemName: "cable.connector").font(.title2)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Connect the iPod with its USB cable").font(.headline)
                        Text("It has to show up in Finder as a drive (with an iPod_Control folder).")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Look Again") { m.refresh() }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    var dropZone: some View {
        RoundedRectangle(cornerRadius: 12)
            .strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [7]))
            .foregroundStyle(hovering ? Color.accentColor : Color.secondary)
            .frame(height: m.items.isEmpty ? 180 : 80)
            .overlay(
                VStack(spacing: 6) {
                    Image(systemName: "music.note.list").font(.largeTitle)
                    Text("Drop songs or folders here").font(.headline)
                    Text("MP3 goes straight in. M4A, AAC, WAV, AIFF, FLAC and OGG are converted to MP3. Click to choose files.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            )
            .contentShape(Rectangle())
            .onTapGesture { pick() }
            .onDrop(of: [.fileURL], isTargeted: $hovering) { providers in
                for p in providers {
                    _ = p.loadObject(ofClass: URL.self) { url, _ in
                        if let url { Task { @MainActor in m.add([url]) } }
                    }
                }
                return true
            }
    }

    var list: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("\(m.sendable.count) song(s) to send, \(fmtBytes(m.totalBytes))")
                .font(.callout)
            if m.needsFFmpeg {
                Text("To convert files that are not MP3, install ffmpeg: brew install ffmpeg")
                    .font(.caption).foregroundStyle(.orange)
            }
            List {
                ForEach(m.items) { it in
                    HStack {
                        Image(systemName: icon(it.kind)).foregroundStyle(color(it.kind))
                        Text(it.url.lastPathComponent).lineLimit(1)
                        Spacer()
                        Text(label(it.kind)).font(.caption).foregroundStyle(color(it.kind))
                    }
                }
                .onDelete { m.items.remove(atOffsets: $0) }
            }
            .frame(minHeight: 120)
        }
    }

    func icon(_ k: Kind) -> String {
        switch k {
        case .mp3: return "music.note"
        case .convert: return "arrow.triangle.2.circlepath"
        case .video: return "film"
        case .other: return "questionmark.circle"
        }
    }
    func label(_ k: Kind) -> String {
        switch k {
        case .mp3: return "MP3"
        case .convert: return "converted to MP3"
        case .video: return "video: not supported"
        case .other: return "not supported"
        }
    }
    func color(_ k: Kind) -> Color {
        switch k {
        case .mp3: return .green
        case .convert: return .blue
        default: return .secondary
        }
    }

    func pick() {
        let p = NSOpenPanel()
        p.allowsMultipleSelection = true
        p.canChooseDirectories = true
        if p.runModal() == .OK { m.add(p.urls) }
    }
}

@main
struct NanoSyncMac: App {
    static func kind(of url: URL) -> Kind { NanoSyncMacKind(url) }

    init() {
        let args = Array(CommandLine.arguments.dropFirst())
        if let i = args.firstIndex(of: "--cli") {
            var rest = args
            rest.remove(at: i)
            exit(runCLI(rest))
        }
    }

    var body: some Scene {
        WindowGroup("NanoSync Mac") { ContentView() }
            .windowResizability(.contentMinSize)
    }
}

func NanoSyncMacKind(_ url: URL) -> Kind { kind(of: url) }
