import Foundation

enum RapidRawError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        switch self { case .message(let text): return text }
    }
}

enum RapidRawAdjustment: String, CaseIterable, Identifiable {
    case exposure, contrast, highlights, shadows, whites, blacks, saturation
    var id: Self { self }
    var range: ClosedRange<Double> { self == .exposure ? -5...5 : -100...100 }
    var step: Double { self == .exposure ? 0.01 : 1 }
}

struct RapidRawDocument {
    static let extensions: Set<String> = [
        "3fr", "arw", "cr2", "cr3", "crw", "dcr", "dng", "erf", "fff", "iiq", "kdc", "mef",
        "mos", "mrw", "nef", "nrw", "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "x3f",
    ]

    let id = UUID().uuidString
    let source: URL
    private let sidecar: URL
    private var baseline: Data?
    private var metadata: [String: Any]
    var adjustments: [String: Any]

    init(source: URL) throws {
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        guard Self.extensions.contains(source.pathExtension.lowercased()),
              try source.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true else {
            throw RapidRawError.message("Choose a supported RAW photo.")
        }
        let sidecar = source.appendingPathExtension("rrdata")
        let baseline = try Self.readSidecar(sidecar)
        let metadata: [String: Any]
        if let baseline {
            guard let data = try JSONSerialization.jsonObject(with: baseline) as? [String: Any],
                  data["version"] == nil || data["version"] as? Int == 1,
                  data["adjustments"] == nil || data["adjustments"] is NSNull || data["adjustments"] is [String: Any] else {
                throw RapidRawError.message("The RapidRAW sidecar could not be read.")
            }
            metadata = data
        } else {
            metadata = ["version": 1, "rating": 0]
        }
        self.source = source
        self.sidecar = sidecar
        self.baseline = baseline
        self.metadata = metadata
        self.adjustments = metadata["adjustments"] as? [String: Any] ?? [:]
    }

    var dirty: Bool {
        !NSDictionary(dictionary: adjustments).isEqual(to: metadata["adjustments"] as? [String: Any] ?? [:])
    }

    mutating func save() throws {
        guard dirty else { return }
        guard try Self.readSidecar(sidecar) == baseline else {
            throw RapidRawError.message("The sidecar changed outside Lucia. Reopen the photo before saving.")
        }
        var updated = metadata
        updated["adjustments"] = adjustments
        let data = try JSONSerialization.data(withJSONObject: updated, options: [.prettyPrinted, .sortedKeys])
        if baseline == nil {
            let staged = sidecar.deletingLastPathComponent().appendingPathComponent(".lucia-" + UUID().uuidString)
            defer { try? FileManager.default.removeItem(at: staged) }
            try data.write(to: staged, options: .withoutOverwriting)
            try FileManager.default.linkItem(at: staged, to: sidecar)
        } else {
            try data.write(to: sidecar, options: .atomic)
        }
        metadata = updated
        baseline = data
    }

    private static func readSidecar(_ url: URL) throws -> Data? {
        do {
            let info = try url.resourceValues(forKeys: [.isSymbolicLinkKey, .isRegularFileKey, .fileSizeKey])
            guard info.isSymbolicLink != true, info.isRegularFile == true,
                  let size = info.fileSize, size <= 32 * 1024 * 1024 else {
                throw RapidRawError.message("The sidecar must be a regular file smaller than 32 MB.")
            }
            return try Data(contentsOf: url)
        } catch let error as NSError
            where error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError {
            return nil
        }
    }
}
