import Darwin
import Foundation
import ImageIO

enum RapidRawRenderer {
    static var executable: String? {
        let files = FileManager.default
        let configured = ProcessInfo.processInfo.environment["LUCIA_RAPIDRAW_APP"]
        let applications = configured.map { [URL(fileURLWithPath: $0)] } ?? [
            URL(fileURLWithPath: "/Applications/RapidRAW.app"),
            files.homeDirectoryForCurrentUser.appendingPathComponent("Applications/RapidRAW.app"),
        ]
        return applications.lazy.compactMap { Bundle(url: $0)?.executableURL }.compactMap { url -> String? in
            // Tauri requires the physical executable path. Foundation may shorten /private/tmp to /tmp.
            guard let path = realpath(url.path, nil) else { return nil }
            defer { free(path) }
            return String(cString: path)
        }.first { files.isExecutableFile(atPath: $0) }
    }

    static func preview(source: URL, adjustments: Data, executable: String) throws -> CGImage {
        let files = FileManager.default
        let scratch = files.temporaryDirectory.appendingPathComponent("lucia-rapidraw-" + UUID().uuidString)
        try files.createDirectory(at: scratch, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? files.removeItem(at: scratch) }
        let input = scratch.appendingPathComponent(source.lastPathComponent)
        let output = scratch.appendingPathComponent("preview.jpg")
        let preset = scratch.appendingPathComponent("adjustments.json")
        let diagnostics = scratch.appendingPathComponent("renderer.log")
        // Keep RapidRAW's export and metadata updates away from the original photo.
        try files.copyItem(at: source, to: input)
        try adjustments.write(to: preset)
        try Data().write(to: diagnostics)
        let log = try FileHandle(forWritingTo: diagnostics)
        defer { try? log.close() }
        let process = Process()
        // Process rewrites executable URLs. The exec utility preserves the physical path as an argument.
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = [executable, "export", input.path, "--output", output.path, "--format", "jpeg",
                             "--adjustments", preset.path]
        process.currentDirectoryURL = scratch
        process.standardOutput = log
        process.standardError = log
        try process.run()
        let deadline = ProcessInfo.processInfo.systemUptime + 120
        while process.isRunning && ProcessInfo.processInfo.systemUptime < deadline {
            Thread.sleep(forTimeInterval: 0.05)
        }
        if process.isRunning {
            process.terminate()
            Thread.sleep(forTimeInterval: 0.25)
            if process.isRunning { kill(process.processIdentifier, SIGKILL) }
            process.waitUntilExit()
            throw RapidRawError.message("RapidRAW did not finish rendering the preview.")
        }
        guard process.terminationStatus == 0,
              let image = CGImageSourceCreateWithURL(output as CFURL, nil),
              let thumbnail = CGImageSourceCreateThumbnailAtIndex(image, 0, [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 2048,
              ] as CFDictionary) else {
            if let reader = try? FileHandle(forReadingFrom: diagnostics) {
                defer { try? reader.close() }
                let detail = (try? reader.read(upToCount: 4096)) ?? Data()
                NSLog("RapidRAW renderer exited with status %d: %@", process.terminationStatus,
                      String(decoding: detail, as: UTF8.self))
            }
            throw RapidRawError.message("RapidRAW could not render this photo.")
        }
        return thumbnail
    }
}
