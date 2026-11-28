import Foundation

@main
struct RapidRawDocumentTests {
    static func main() throws {
        let files = FileManager.default
        let root = files.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try files.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? files.removeItem(at: root) }
        let raw = root.appendingPathComponent("photo.dng")
        let bytes = Data([7, 12, 42])
        try bytes.write(to: raw)
        let sidecar = raw.appendingPathExtension("rrdata")
        var doc = try RapidRawDocument(source: raw)
        doc.adjustments["exposure"] = 1.25
        precondition(doc.dirty && !files.fileExists(atPath: sidecar.path))
        try doc.save()
        precondition(!doc.dirty)
        let reopened = try RapidRawDocument(source: raw)
        precondition(reopened.adjustments["exposure"] as? Double == 1.25)

        try Data(#"{"version":1,"rating":4,"future":{"flag":true},"adjustments":{"crop":{"x":12}}}"#.utf8)
            .write(to: sidecar, options: .atomic)
        doc.adjustments["exposure"] = 2
        do {
            try doc.save()
            preconditionFailure("An external sidecar modification was overwritten")
        } catch RapidRawError.message {}
        precondition(doc.dirty)

        var edited = try RapidRawDocument(source: raw)
        edited.adjustments["saturation"] = -15
        try edited.save()
        let saved = try JSONSerialization.jsonObject(with: Data(contentsOf: sidecar)) as? [String: Any]
        precondition(saved?["rating"] as? Int == 4)
        precondition((saved?["future"] as? [String: Bool])?["flag"] == true)
        precondition((saved?["adjustments"] as? [String: Any])?["crop"] != nil)
        let untouched = try Data(contentsOf: raw)
        precondition(untouched == bytes)
    }
}
