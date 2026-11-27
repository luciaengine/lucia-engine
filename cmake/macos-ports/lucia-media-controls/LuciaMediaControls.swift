#if LUCIA_HOSTED
import AppKit
import SwiftUI

/// Mirrors the Lucia editor's standard secondary button and slider styles.
enum LuciaMediaControls {
    static let accent = Color(red: 0, green: 111.0 / 255, blue: 1)
    static let sliderFill = Color(red: 59.0 / 255, green: 133.0 / 255, blue: 224.0 / 255)
    static let sliderTrack = Color(white: 89.0 / 255)
    static let sliderThumbSize: CGFloat = 8

    static func sliderThumb(enabled: Bool = true) -> some View {
        Circle()
            .fill(Color.white.opacity(enabled ? 1 : 0.55))
            .frame(width: sliderThumbSize, height: sliderThumbSize)
            .shadow(color: enabled ? accent : .clear, radius: 2)
    }
}

struct LuciaMediaButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        Control(configuration: configuration)
    }

    private struct Control: View {
        let configuration: Configuration
        @Environment(\.isEnabled) private var enabled
        @State private var hovering = false

        var body: some View {
            configuration.label
                .font(.system(size: 13, weight: .medium))
                .foregroundStyle(Color.white.opacity(enabled ? 0.9 : 0.36))
                .padding(.horizontal, 10)
                .frame(height: 26)
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(Color(white: configuration.isPressed ? 0.18 : hovering && enabled ? 0.26 : 0.20))
                )
                .contentShape(RoundedRectangle(cornerRadius: 6))
                .onHover { hovering = $0 }
        }
    }
}
/// Presents workspace confirmations with Lucia chrome over the current editor.
@MainActor
enum LuciaMediaDialog {
    static func run(_ alert: NSAlert, over parent: NSWindow? = nil) -> NSApplication.ModalResponse {
        let owner = parent ?? NSApp.keyWindow
        let panel = NSPanel(contentRect: .zero, styleMask: [.titled, .fullSizeContentView],
                            backing: .buffered, defer: false)
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.backgroundColor = .clear
        panel.isOpaque = false
        panel.isReleasedWhenClosed = false

        let actions = alert.buttons.map(\.title)
        let defaultIndex = alert.buttons.firstIndex { $0.keyEquivalent == "\r" } ?? 0
        let cancelIndex = alert.buttons.firstIndex { $0.keyEquivalent == "\u{1b}" || $0.title == "Cancel" }
        let content = Content(title: alert.messageText, message: alert.informativeText,
                              actions: actions, defaultIndex: defaultIndex, cancelIndex: cancelIndex) { index in
            let response = index.map {
                NSApplication.ModalResponse(rawValue: NSApplication.ModalResponse.alertFirstButtonReturn.rawValue + $0)
            } ?? .cancel
            NSApp.stopModal(withCode: response)
        }
        let view = NSHostingView(rootView: content)
        panel.contentView = view
        panel.setContentSize(view.fittingSize)
        if let owner {
            panel.setFrameOrigin(NSPoint(x: owner.frame.midX - panel.frame.width / 2,
                                         y: owner.frame.midY - panel.frame.height / 2))
            owner.addChildWindow(panel, ordered: .above)
        } else {
            panel.center()
        }
        let response = NSApp.runModal(for: panel)
        owner?.removeChildWindow(panel)
        panel.orderOut(nil)
        return response
    }

    private struct Content: View {
        let title: String
        let message: String
        let actions: [String]
        let defaultIndex: Int
        let cancelIndex: Int?
        let choose: (Int?) -> Void

        var body: some View {
            VStack(alignment: .leading, spacing: 14) {
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                if !message.isEmpty {
                    Text(message)
                        .font(.system(size: 13))
                        .foregroundStyle(Color.white.opacity(0.74))
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(spacing: 6) {
                    Spacer(minLength: 0)
                    ForEach(actions.indices.reversed(), id: \.self) { index in
                        let button = Button(actions[index]) { choose(index) }
                        if index == defaultIndex {
                            button.keyboardShortcut(.defaultAction)
                        } else if index == cancelIndex {
                            button.keyboardShortcut(.cancelAction)
                        } else {
                            button
                        }
                    }
                }
                .buttonStyle(LuciaMediaButtonStyle())
            }
            .padding(16)
            .frame(width: 440, alignment: .leading)
            .foregroundStyle(Color.white.opacity(0.9))
            .background(RoundedRectangle(cornerRadius: 8).fill(Color(white: 0.14)))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.white.opacity(0.13)))
            .onExitCommand { choose(cancelIndex) }
        }
    }
}


#endif

#if LUCIA_HOSTED
@MainActor
final class LuciaMediaWorkspaceHost {
    let view: NSView
    let confirmClose: @MainActor () async -> Bool
    var documentCommand: (([String: Any]) -> [String: Any])?

    init(view: NSView, confirmClose: @escaping @MainActor () async -> Bool) {
        self.view = view
        self.confirmClose = confirmClose
    }
}

public typealias LuciaMediaDocumentReply = @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>) -> Void

@_cdecl("LuciaRunMediaDocumentCommand")
public func luciaRunMediaDocumentCommand(
    _ handle: UnsafeMutableRawPointer, _ request: UnsafePointer<CChar>,
    _ context: UnsafeMutableRawPointer?, _ reply: LuciaMediaDocumentReply
) {
    MainActor.assumeIsolated {
        let workspace = Unmanaged<LuciaMediaWorkspaceHost>.fromOpaque(handle).takeUnretainedValue()
        let bytes = Data(String(cString: request).utf8)
        guard let args = (try? JSONSerialization.jsonObject(with: bytes)) as? [String: Any],
              let command = workspace.documentCommand,
              let result = try? JSONSerialization.data(withJSONObject: command(args)) else {
            "{\"error\":\"The document request could not be handled.\"}".withCString { reply(context, $0) }
            return
        }
        String(decoding: result, as: UTF8.self).withCString { reply(context, $0) }
    }
}

public typealias LuciaMediaCloseReply = @convention(c) (UnsafeMutableRawPointer?, Int32) -> Void

@_cdecl("LuciaConfirmMediaWorkspaceClose")
public func luciaConfirmMediaWorkspaceClose(
    _ handle: UnsafeMutableRawPointer, _ context: UnsafeMutableRawPointer?, _ reply: LuciaMediaCloseReply
) {
    MainActor.assumeIsolated {
        let workspace = Unmanaged<LuciaMediaWorkspaceHost>.fromOpaque(handle).takeUnretainedValue()
        let contextAddress = context.map { UInt(bitPattern: $0) }
        Task { @MainActor in
            let approved = await workspace.confirmClose()
            reply(contextAddress.flatMap { UnsafeMutableRawPointer(bitPattern: $0) }, approved ? 1 : 0)
        }
    }
}
#endif
