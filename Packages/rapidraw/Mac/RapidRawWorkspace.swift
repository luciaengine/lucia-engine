import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor
private final class RapidRawWorkspace: ObservableObject {
    private let projectAssets: URL
    private let undoManager = UndoManager()
    private var revision = 0
    private var renderedRevision: Int?
    @Published private(set) var document: RapidRawDocument?
    @Published private(set) var preview: CGImage?
    @Published private(set) var status = ""
    @Published private(set) var rendering = false

    init(projectAssets: URL) {
        self.projectAssets = projectAssets.resolvingSymlinksInPath().standardizedFileURL
        undoManager.groupsByEvent = false
        if RapidRawRenderer.executable == nil { status = "Install RapidRAW 1.7.1 or later to render RAW previews." }
    }

    var canUndo: Bool { undoManager.canUndo }
    var canRedo: Bool { undoManager.canRedo }
    var dirty: Bool { document?.dirty == true }

    func browse() {
        guard confirmClose() else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = RapidRawDocument.extensions.sorted().compactMap { UTType(filenameExtension: $0) }
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try open(url) } catch { status = error.localizedDescription }
    }

    @discardableResult
    private func open(_ url: URL) throws -> RapidRawDocument {
        guard RapidRawRenderer.executable != nil else {
            throw RapidRawError.message("Install RapidRAW 1.7.1 or later to render RAW previews.")
        }
        let opened = try RapidRawDocument(source: url)
        document = opened
        undoManager.removeAllActions()
        preview = nil
        refreshPreview()
        return opened
    }

    func value(_ field: RapidRawAdjustment) -> Double {
        document?.adjustments[field.rawValue] as? Double ?? 0
    }

    func change(_ field: RapidRawAdjustment, to value: Double) {
        guard let document, value != self.value(field) else { return }
        var values = document.adjustments
        values[field.rawValue] = value
        replaceAdjustments(values)
    }

    private func replaceAdjustments(_ values: [String: Any]) {
        guard var updated = document else { return }
        let before = updated.adjustments
        let beginGroup = !undoManager.isUndoing && !undoManager.isRedoing
        if beginGroup { undoManager.beginUndoGrouping() }
        undoManager.registerUndo(withTarget: self) { target in
            MainActor.assumeIsolated { target.replaceAdjustments(before) }
        }
        if beginGroup { undoManager.endUndoGrouping() }
        updated.adjustments = values
        document = updated
        refreshPreview()
    }

    func undo() { undoManager.undo() }
    func redo() { undoManager.redo() }
    func saveFromPanel() { do { try save() } catch { status = error.localizedDescription } }

    private func save() throws {
        guard var updated = document else { return }
        try updated.save()
        document = updated
        if !rendering { status = "Adjustments saved." }
    }

    func confirmClose() -> Bool {
        guard let document, document.dirty else { return true }
        let prompt = NSAlert()
        prompt.messageText = "Save changes to “\(document.source.lastPathComponent)”?"
        prompt.informativeText = "RapidRAW adjustments are saved alongside the photo."
        prompt.addButton(withTitle: "Save")
        prompt.addButton(withTitle: "Don’t Save")
        prompt.addButton(withTitle: "Cancel")
        switch LuciaMediaDialog.run(prompt) {
        case .alertFirstButtonReturn:
            do { try save(); return true } catch { status = error.localizedDescription; return false }
        case .alertSecondButtonReturn: return true
        default: return false
        }
    }

    private func refreshPreview() {
        revision += 1
        if !rendering { renderPreview() }
    }

    private func renderPreview() {
        guard let document, let executable = RapidRawRenderer.executable,
              let adjustments = try? JSONSerialization.data(withJSONObject: document.adjustments) else { return }
        let source = document.source
        let requested = revision
        rendering = true
        status = "Rendering preview…"
        Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Result { try RapidRawRenderer.preview(source: source, adjustments: adjustments, executable: executable) }
            }.value
            guard let self else { return }
            rendering = false
            guard requested == revision else { renderPreview(); return }
            switch result {
            case .success(let image):
                preview = image
                renderedRevision = requested
                status = ""
            case .failure(let error):
                status = error.localizedDescription
            }
        }
    }

    func command(_ request: [String: Any]) -> [String: Any] {
        do {
            let operation = request["operation"] as? String
            if operation == "inspect" {
                guard let document else { return ["photoOpen": false, "status": status] }
                return ["photoOpen": true, "documentId": document.id, "name": document.source.lastPathComponent,
                        "adjustments": Dictionary(uniqueKeysWithValues: RapidRawAdjustment.allCases.map {
                            ($0.rawValue, value($0))
                        }), "dirty": document.dirty, "previewReady": renderedRevision == revision, "status": status]
            }
            if operation == "open_raw_photo" {
                guard !dirty else { throw RapidRawError.message("Save the current adjustments before opening another photo.") }
                guard let path = request["path"] as? String, !path.isEmpty, !path.hasPrefix("/") else {
                    throw RapidRawError.message("Provide a RAW photo path relative to the project Assets folder.")
                }
                let source = projectAssets.appendingPathComponent(path).resolvingSymlinksInPath().standardizedFileURL
                guard source.pathComponents.starts(with: projectAssets.pathComponents),
                      source.pathComponents.count > projectAssets.pathComponents.count else {
                    throw RapidRawError.message("Choose a RAW photo under the project Assets folder.")
                }
                let opened = try open(source)
                return ["documentId": opened.id, "name": source.lastPathComponent]
            }
            guard let document, request["documentId"] as? String == document.id else {
                throw RapidRawError.message("The RAW photo changed. Inspect the active photo again.")
            }
            guard preview != nil else { throw RapidRawError.message("Wait for a valid RAW preview before editing or saving.") }
            switch operation {
            case "set_raw_adjustment":
                guard let name = request["adjustment"] as? String, let field = RapidRawAdjustment(rawValue: name),
                      let number = request["value"] as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
                      field.range.contains(number.doubleValue) else {
                    throw RapidRawError.message("Provide a supported adjustment and a value in its range.")
                }
                change(field, to: number.doubleValue)
                return ["documentId": document.id, "adjustment": name, "value": number.doubleValue, "dirty": dirty]
            case "save_raw_adjustments":
                try save()
                return ["documentId": document.id, "saved": true]
            default:
                throw RapidRawError.message("Choose a supported RapidRAW operation.")
            }
        } catch {
            return ["error": error.localizedDescription]
        }
    }
}

private struct RapidRawView: View {
    @ObservedObject var workspace: RapidRawWorkspace
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Button("Open RAW…", action: workspace.browse)
                Button("Save Adjustments", action: workspace.saveFromPanel).disabled(!workspace.dirty)
                Button("Undo", action: workspace.undo).disabled(!workspace.canUndo)
                Button("Redo", action: workspace.redo).disabled(!workspace.canRedo)
                Spacer()
                if let document = workspace.document {
                    Text(document.source.lastPathComponent + (document.dirty ? " •" : "")).lineLimit(1)
                }
            }
            .buttonStyle(LuciaMediaButtonStyle())
            .padding(8)
            Divider()
            HStack(spacing: 0) {
                ZStack {
                    Color(nsColor: .windowBackgroundColor)
                    if let preview = workspace.preview {
                        Image(decorative: preview, scale: 1).resizable().scaledToFit().padding(12)
                    } else if workspace.rendering {
                        ProgressView()
                    } else {
                        Text("Open a RAW photo to begin.").foregroundStyle(.secondary)
                    }
                }
                Divider()
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        ForEach(RapidRawAdjustment.allCases) { field in
                            RapidRawSlider(field: field, workspace: workspace)
                        }
                    }.padding(12)
                }.frame(width: 220).disabled(workspace.preview == nil)
            }
            if !workspace.status.isEmpty {
                Divider()
                Text(workspace.status).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(8)
            }
        }
        .font(.system(size: 13))
        .background(Color(nsColor: .controlBackgroundColor))
        .preferredColorScheme(.dark)
    }
}

private struct RapidRawSlider: View {
    let field: RapidRawAdjustment
    @ObservedObject var workspace: RapidRawWorkspace
    @State private var draft: Double? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(field.rawValue.capitalized)
                Spacer()
                Text((draft ?? workspace.value(field)).formatted(.number.precision(.fractionLength(0...2))))
                    .monospacedDigit()
            }
            Slider(value: Binding(get: { draft ?? workspace.value(field) }, set: { value in
                if draft != nil { draft = value } else { workspace.change(field, to: value) }
            }), in: field.range, step: field.step, onEditingChanged: { editing in
                if editing {
                    draft = workspace.value(field)
                } else if let value = draft {
                    draft = nil
                    workspace.change(field, to: value)
                }
            })
            .accessibilityLabel(field.rawValue.capitalized)
            .tint(LuciaMediaControls.accent)
        }
    }
}

public typealias RapidRawPreviewCallback = @convention(c) (UnsafePointer<CChar>?, UnsafePointer<UInt8>?, UInt32, UInt32, Int) -> Void

@_cdecl("LuciaCreateProjectMediaWorkspace")
public func createRapidRawWorkspace(_ bundle: UnsafePointer<CChar>, _ assets: UnsafePointer<CChar>,
                                    _ preview: RapidRawPreviewCallback?) -> UnsafeMutableRawPointer? {
    let address = MainActor.assumeIsolated {
        let state = RapidRawWorkspace(projectAssets: URL(fileURLWithPath: String(cString: assets)))
        let view = NSHostingView(rootView: RapidRawView(workspace: state))
        let host = LuciaMediaWorkspaceHost(view: view) { state.confirmClose() }
        host.documentCommand = state.command
        return UInt(bitPattern: Unmanaged.passRetained(host).toOpaque())
    }
    return UnsafeMutableRawPointer(bitPattern: address)
}

@_cdecl("LuciaPlaceMediaWorkspace")
public func placeRapidRawWorkspace(_ handle: UnsafeMutableRawPointer, _ window: UnsafeMutableRawPointer?,
                                   _ x: Double, _ y: Double, _ width: Double, _ height: Double) {
    MainActor.assumeIsolated {
        let view = Unmanaged<LuciaMediaWorkspaceHost>.fromOpaque(handle).takeUnretainedValue().view
        guard let window, let content = Unmanaged<NSWindow>.fromOpaque(window).takeUnretainedValue().contentView else {
            view.isHidden = true
            return
        }
        if view.superview !== content { content.addSubview(view, positioned: .above, relativeTo: nil) }
        let top = content.isFlipped ? y : content.bounds.height - y - height
        let frame = NSRect(x: x, y: top, width: width, height: height)
        if view.frame != frame { view.frame = frame }
        view.isHidden = false
    }
}

@_cdecl("LuciaDestroyMediaWorkspace")
public func destroyRapidRawWorkspace(_ handle: UnsafeMutableRawPointer) {
    MainActor.assumeIsolated {
        Unmanaged<LuciaMediaWorkspaceHost>.fromOpaque(handle).takeRetainedValue().view.removeFromSuperview()
    }
}
