#if LUCIA_HOSTED
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
#endif
