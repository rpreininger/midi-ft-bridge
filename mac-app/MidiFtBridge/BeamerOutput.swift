import AppKit

/// Borderless windows refuse key status by default. With only one screen the
/// beamer window covers the app, so it has to take the keyboard (see show()).
private final class BeamerWindow: NSWindow {
    override var canBecomeKey: Bool { true }
}

/// Fullscreen output for gigs with a projector instead of the LED panels.
/// Shows what it is handed with hard pixels: nearest-neighbour, no smoothing.
/// The 8-bit look is the point. Stays square; fills the screen height by default.
final class BeamerOutput {
    private var window: NSWindow?
    private let imageLayer = CALayer()
    private var imageSide = 0
    private var fillHeight = true
    private var displaySleepBlock: NSObjectProtocol?
    private var screenObserver: NSObjectProtocol?

    /// Called when the output closes itself because its screen went away.
    var onClosedByItself: (() -> Void)?

    /// True when no second screen exists and the window hides the app's UI.
    /// Esc is then the way back out.
    private(set) var coversControlWindow = false

    var isShowing: Bool { window != nil }

    init() {
        imageLayer.magnificationFilter = .nearest
        imageLayer.minificationFilter = .nearest
        imageLayer.contentsGravity = .resize
        imageLayer.edgeAntialiasingMask = []
    }

    func show() {
        guard window == nil else { return }

        let view = NSView()
        let root = CALayer()
        root.backgroundColor = NSColor.black.cgColor
        root.addSublayer(imageLayer)
        view.layer = root
        view.wantsLayer = true

        let w = BeamerWindow(contentRect: .zero, styleMask: .borderless,
                             backing: .buffered, defer: false)
        w.contentView = view
        w.backgroundColor = .black
        w.isReleasedWhenClosed = false
        w.hidesOnDeactivate = false
        w.level = .mainMenu + 1     // above the menu bar of its screen
        w.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
        window = w

        // A sleeping display mid-set is a black beamer.
        displaySleepBlock = ProcessInfo.processInfo.beginActivity(
            options: [.idleDisplaySleepDisabled, .userInitiated],
            reason: "Beamer output")

        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil, queue: .main) { [weak self] _ in self?.screensChanged() }

        place()
        NSCursor.setHiddenUntilMouseMoves(true)
    }

    func hide() {
        guard let w = window else { return }
        window = nil
        coversControlWindow = false
        w.orderOut(nil)
        imageLayer.removeFromSuperlayer()
        if let token = displaySleepBlock {
            ProcessInfo.processInfo.endActivity(token)
            displaySleepBlock = nil
        }
        if let obs = screenObserver {
            NotificationCenter.default.removeObserver(obs)
            screenObserver = nil
        }
    }

    /// `image` must be square. Main thread only.
    func display(_ image: CGImage) {
        guard window != nil else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        imageLayer.contents = image
        if image.width != imageSide {
            imageSide = image.width
            layoutImage()
        }
        CATransaction.commit()
    }

    /// Full height (blocks vary by a pixel, 1080 doesn't divide by 128) or the
    /// largest whole-number scale (every block identical, wider black edge).
    func toggleFillHeight() {
        fillHeight.toggle()
        layoutImage()
    }

    func clear() {
        guard imageLayer.contents != nil else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        imageLayer.contents = nil
        CATransaction.commit()
    }

    // MARK: - Screens

    /// The projector is whichever screen doesn't hold the control window.
    private func controlAndTargetScreen() -> (control: NSScreen?, target: NSScreen?) {
        let control = NSApp.windows
            .first { $0 !== window && $0.isVisible && $0.level == .normal }?.screen
            ?? NSScreen.main
        let other = NSScreen.screens.first { $0 != control }
        return (control, other ?? control)
    }

    private func place() {
        guard let w = window else { return }
        let (control, target) = controlAndTargetScreen()
        guard let screen = target else { return }

        coversControlWindow = (screen == control)
        w.setFrame(screen.frame, display: true)
        if coversControlWindow {
            w.makeKeyAndOrderFront(nil)
        } else {
            w.orderFrontRegardless()   // keyboard stays with the control window
        }
        layoutImage()
    }

    private func screensChanged() {
        // The projector was unplugged: don't drop a black window over the app
        // in the middle of a set, hand the screen back to the controls.
        if !coversControlWindow, NSScreen.screens.count < 2 {
            hide()
            onClosedByItself?()
            return
        }
        place()
    }

    /// Centred square, worked out in device pixels so its edges land on them.
    private func layoutImage() {
        guard let w = window, let screen = w.screen ?? NSScreen.main, imageSide > 0 else { return }
        let backing = screen.backingScaleFactor
        let pixels = min(screen.frame.width, screen.frame.height) * backing
        // The 1024 clips are the 128 panel with 8x8 blocks, so whole-number means
        // a multiple of 128 (1024 -> 1920 is exactly 15 device pixels per block).
        let grid = CGFloat(imageSide % 128 == 0 ? 128 : imageSide)
        let wholeBlocks = max(grid, floor(pixels / grid) * grid)
        let sidePixels = fillHeight ? pixels : wholeBlocks
        let side = sidePixels / backing

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        imageLayer.contentsScale = backing
        imageLayer.frame = CGRect(
            x: floor((screen.frame.width - side) / 2 * backing) / backing,
            y: floor((screen.frame.height - side) / 2 * backing) / backing,
            width: side, height: side)
        CATransaction.commit()
    }
}
