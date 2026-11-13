import SwiftUI
import CoreGraphics

/// The Windows board's home screen: the machine's size (automatic or by hand), a Start button, and the log.
struct LintelView: View {
    @AppStorage("lintel.auto") private var auto = true
    @AppStorage("lintel.cpus") private var manualCPUs = 4
    @AppStorage("lintel.ramGB") private var manualRAM = 2.0
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var status = "Stopped"
    @State private var busy = false
    @State private var frame: CGImage?

    private static let cores = max(ProcessInfo.processInfo.activeProcessorCount, 1)
    private static let deviceGB = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824

    /// Best size for this device: every core (at most 8, at least 2), and about 60% of its memory in half-GB steps.
    static var autoCPUs: Int { min(max(cores, 2), 8) }
    static var autoRAM: Double { min(max((deviceGB * 0.6 * 2).rounded(.down) / 2, 1), 8) }

    private var cpus: Int { auto ? Self.autoCPUs : manualCPUs }
    private var ram: Double { auto ? Self.autoRAM : manualRAM }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    hero
                    screenCard
                    sizeCard
                    startCard
                    Text("Lintel is a fork of Henge by Leviidev (GPL-2.0-or-later).")
                        .font(.caption2).foregroundStyle(Desk.faint)
                        .multilineTextAlignment(.center).padding(.top, 4)
                }
                .padding(16)
            }
            .background(Desk.page)
            .navigationTitle("Lintel")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Text("Credits: Leviidev").font(.system(size: 9)).foregroundStyle(Desk.faint)
                }
            }
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Windows on your \(Device.name)").font(.title2.bold()).foregroundStyle(Desk.text)
            Text("Experimental. Right now this boots the UEFI firmware only: no screen or disk yet. Output goes to the log.")
                .font(.footnote).foregroundStyle(Desk.dim)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16).background(Desk.card, in: RoundedRectangle(cornerRadius: 20))
    }

    private var screenCard: some View {
        ZStack {
            Color.black
            if let frame {
                Image(decorative: frame, scale: 1).resizable().interpolation(.medium).aspectRatio(contentMode: .fit)
            } else {
                Text("No picture yet").font(.footnote).foregroundStyle(.gray)
            }
        }
        .aspectRatio(16.0 / 10.0, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 20))
        .task {
            while !Task.isCancelled {
                if status.hasPrefix("Running") { frame = BootDisplay.snapshot() ?? frame }
                try? await Task.sleep(nanoseconds: 150_000_000)
            }
        }
    }

    private var sizeCard: some View {
        VStack(alignment: .leading, spacing: 14) {
            Toggle(isOn: $auto) {
                VStack(alignment: .leading) {
                    Text("Automatic").font(.headline).foregroundStyle(Desk.text)
                    Text("Best for this \(Device.name): \(Self.autoCPUs) cores, \(Self.fmt(Self.autoRAM)) GB")
                        .font(.footnote).foregroundStyle(Desk.dim)
                }
            }
            .tint(Desk.accentLight)
            if !auto {
                Stepper(value: $manualCPUs, in: 1...Self.cores) {
                    Text("CPU cores: \(manualCPUs)").foregroundStyle(Desk.text)
                }
                Stepper(value: $manualRAM, in: 1...max(Self.deviceGB.rounded(.down), 1), step: 0.5) {
                    Text("Memory: \(Self.fmt(manualRAM)) GB").foregroundStyle(Desk.text)
                }
                Text("This \(Device.name) has \(Self.cores) cores and \(Self.fmt(Self.deviceGB)) GB. Giving the machine most of the memory can make iOS close the app.")
                    .font(.footnote).foregroundStyle(Desk.dim)
            }
        }
        .padding(16).background(Desk.card, in: RoundedRectangle(cornerRadius: 20))
    }

    private var startCard: some View {
        VStack(spacing: 12) {
            HStack {
                Circle().fill(status.hasPrefix("Running") ? Desk.good : status.hasPrefix("Failed") ? Desk.bad : Desk.faint)
                    .frame(width: 10, height: 10)
                Text(status).font(.footnote).foregroundStyle(Desk.dim)
                Spacer()
            }
            HStack(spacing: 10) {
                Button(action: start) {
                    Text("Start").frame(maxWidth: .infinity).padding(.vertical, 12)
                }
                .buttonStyle(.borderedProminent).tint(Desk.accentLight).disabled(busy)
                Button(role: .destructive) { henge_vm_stop(); status = "Stopped" } label: {
                    Text("Stop").frame(maxWidth: .infinity).padding(.vertical, 12)
                }
                .buttonStyle(.bordered)
            }
            ShareLink(item: HengeLog.logFileURL) { Label("Share henge.log", systemImage: "square.and.arrow.up") }
                .font(.footnote)
        }
        .padding(16).background(Desk.card, in: RoundedRectangle(cornerRadius: 20))
    }

    private static func fmt(_ g: Double) -> String { g == g.rounded() ? String(Int(g)) : String(format: "%.1f", g) }

    private func start() {
        guard let fw = Bundle.main.path(forResource: "QEMU_EFI", ofType: "fd") else {
            status = "Failed: this build has no bundled firmware (QEMU_EFI.fd)."
            return
        }
        busy = true; status = "Starting \(cpus) cores, \(Self.fmt(ram)) GB..."
        let c = cpus, mb = UInt64(ram * 1024)
        DispatchQueue.global().async {
            var err = [CChar](repeating: 0, count: 512)
            let r = henge_vm_boot_windows(fw, mb, Int32(c), &err, err.count)
            DispatchQueue.main.async {
                busy = false
                status = r == 0 ? "Running. Watch henge.log for the firmware's output." : "Failed: " + String(cString: err)
            }
        }
    }
}
