#if canImport(OfflineTranslatorObjC)
import OfflineTranslatorObjC
#endif
import Foundation

/// Model download / verify / install orchestration for iOS.
///
/// Networking (URLSession, resumable) stays in Swift; integrity + atomic install is done by the
/// C++ core through `OTModelManager`. Zip archives require an unzip step — wire your preferred
/// library (e.g. ZIPFoundation) into `unzipHandler`, or publish "files" mode manifests which
/// need no archive handling at all (recommended).
public final class ModelDownloader: NSObject {

    public enum Event {
        case started(id: String, totalBytes: UInt64)
        case downloading(id: String, file: String, bytesDone: UInt64, bytesTotal: UInt64)
        case verifying(id: String, bytesDone: UInt64, bytesTotal: UInt64)
        case installed(id: String)
        case failed(id: String, reason: String)
        case allDone(failed: [String])
    }

    public typealias UnzipHandler = (_ archive: URL, _ destination: URL) throws -> Void

    public let manager: OTModelManager
    public var unzipHandler: UnzipHandler?
    private let session: URLSession
    private var manifestURL: URL?

    /// - Parameters:
    ///   - modelsRoot: defaults to <Application Support>/models (excluded from iCloud backup).
    ///   - manifestURL: remote manifest; when nil only the cached / bundled manifest is used.
    ///   - bundledManifest: name of a manifest.json in the app bundle used on first launch.
    /// Default manifest: models hosted on the project GitHub releases. Override to self-host.
    public static let defaultManifestURL = URL(string: "https://raw.githubusercontent.com/Kevin-KIM98/offline-translator/main/assets/manifest.json")!

    public init(modelsRoot: URL? = nil, manifestURL: URL? = ModelDownloader.defaultManifestURL, bundledManifest: String? = "manifest") {
        let root = modelsRoot ?? Self.defaultModelsRoot()
        manager = OTModelManager(modelsRoot: root.path)
        self.manifestURL = manifestURL
        let cfg = URLSessionConfiguration.default
        cfg.timeoutIntervalForRequest = 30
        cfg.timeoutIntervalForResource = 60 * 60
        cfg.waitsForConnectivity = true
        session = URLSession(configuration: cfg)
        super.init()

        if !manager.loadCachedManifest(), let name = bundledManifest,
           let url = Bundle.main.url(forResource: name, withExtension: "json"),
           let json = try? String(contentsOf: url, encoding: .utf8) {
            if manager.loadManifestJSON(json) { manager.saveManifest() }
        }
    }

    public static func defaultModelsRoot() -> URL {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return support.appendingPathComponent("models", isDirectory: true)
    }

    public var hasManifest: Bool { !manager.manifestVersion.isEmpty }

    /// Fetches the latest manifest; keeps the cached one on failure.
    public func refreshManifest() async -> Bool {
        guard let url = manifestURL else { return hasManifest }
        guard let (data, _) = try? await session.data(from: url),
              let json = String(data: data, encoding: .utf8) else { return hasManifest }
        let ok = manager.loadManifestJSON(json)
        if ok { manager.saveManifest() }
        return ok || hasManifest
    }

    public func status(deepVerify: Bool = false) -> [OTModelStatus] { manager.status(withDeepVerify: deepVerify) }

    public func status(languages: [String], deepVerify: Bool = false, llmMode: OTLlmMode = .ifNeeded) -> [OTModelStatus] {
        manager.status(forLanguages: languages, deepVerify: deepVerify, llmMode: llmMode)
    }

    public func freeBytes() -> UInt64 {
        let values = try? URL(fileURLWithPath: manager.modelsRoot).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return UInt64(max(0, values?.volumeAvailableCapacityForImportantUsage ?? 0))
    }

    /// Downloads + installs everything in `models` that needs it. Events arrive on the stream.
    public func installAll(_ models: [OTModelStatus]) -> AsyncStream<Event> {
        AsyncStream { continuation in
            let task = Task.detached(priority: .utility) { [self] in
                var failed: [String] = []
                for m in models where m.needsDownload {
                    continuation.yield(.started(id: m.identifier, totalBytes: m.totalBytes))
                    do {
                        try await installOne(m) { continuation.yield($0) }
                        continuation.yield(.installed(id: m.identifier))
                    } catch is CancellationError {
                        break
                    } catch {
                        failed.append(m.identifier)
                        manager.clearStaging(forModel: m.identifier)
                        continuation.yield(.failed(id: m.identifier, reason: error.localizedDescription))
                    }
                }
                continuation.yield(.allDone(failed: failed))
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    public struct InstallError: LocalizedError {
        public let message: String
        public init(message: String) { self.message = message }
        public var errorDescription: String? { message }
    }

    private func installOne(_ m: OTModelStatus, emit: @escaping (Event) -> Void) async throws {
        let staging = URL(fileURLWithPath: manager.stagingDir(forModel: m.identifier), isDirectory: true)
        var downloaded: UInt64 = 0
        for d in m.downloads {
            let target = staging.appendingPathComponent(d.filename)
            let base = downloaded
            try await download(d.url, to: target, expectedSize: d.sizeBytes) { done, total in
                emit(.downloading(id: m.identifier, file: d.filename, bytesDone: base + done,
                                  bytesTotal: m.totalBytes > 0 ? m.totalBytes : total))
            }
            downloaded += d.sizeBytes
            try Task.checkCancellation()

            if d.isArchive {
                // Verify the archive itself; extracted files carry no hashes.
                let v = manager.verifyFile(target.path, sha256: d.sha256, expectedSize: d.sizeBytes) { done, total in
                    emit(.verifying(id: m.identifier, bytesDone: done, bytesTotal: total))
                    return !Task.isCancelled
                }
                guard v == .ok else {
                    try? FileManager.default.removeItem(at: target)
                    throw InstallError(message: "verification failed for \(d.filename): \(manager.lastError)")
                }
                guard let unzip = unzipHandler else {
                    throw InstallError(message: "archive manifests need an unzipHandler (or use files-mode manifests)")
                }
                try unzip(target, staging)
                try? FileManager.default.removeItem(at: target)
            }
        }
        let ok = manager.installModel(m.identifier, stagedPath: staging.path, verifyHashes: true) { done, total in
            emit(.verifying(id: m.identifier, bytesDone: done, bytesTotal: total))
            return !Task.isCancelled
        }
        guard ok else { throw InstallError(message: "install failed: \(manager.lastError)") }
    }

    /// Resumable download using HTTP Range into `target` (via a `.part` file).
    private func download(_ url: URL, to target: URL, expectedSize: UInt64,
                          progress: @escaping (UInt64, UInt64) -> Void) async throws {
        let fm = FileManager.default
        try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let attrs = try? fm.attributesOfItem(atPath: target.path), expectedSize > 0,
           (attrs[.size] as? UInt64) == expectedSize {
            progress(expectedSize, expectedSize)
            return
        }
        let part = URL(fileURLWithPath: target.path + ".part")
        var offset: UInt64 = (try? fm.attributesOfItem(atPath: part.path)[.size] as? UInt64) ?? 0
        if expectedSize > 0, offset >= expectedSize { try? fm.removeItem(at: part); offset = 0 }

        var request = URLRequest(url: url)
        if offset > 0 { request.setValue("bytes=\(offset)-", forHTTPHeaderField: "Range") }
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse else { throw InstallError(message: "bad response for \(url)") }
        switch http.statusCode {
        case 206: break
        case 200: offset = 0; try? fm.removeItem(at: part)
        default: throw InstallError(message: "HTTP \(http.statusCode) for \(url.lastPathComponent)")
        }
        if !fm.fileExists(atPath: part.path) { fm.createFile(atPath: part.path, contents: nil) }
        let handle = try FileHandle(forWritingTo: part)
        defer { try? handle.close() }
        try handle.seekToEnd()

        let total = expectedSize > 0 ? expectedSize : UInt64(max(0, http.expectedContentLength)) + offset
        var done = offset
        var buffer = Data(capacity: 256 * 1024)
        var lastReport = done
        for try await byte in bytes {
            buffer.append(byte)
            if buffer.count >= 256 * 1024 {
                try handle.write(contentsOf: buffer)
                done += UInt64(buffer.count)
                buffer.removeAll(keepingCapacity: true)
                if done - lastReport >= 512 * 1024 { lastReport = done; progress(done, total) }
                try Task.checkCancellation()
            }
        }
        if !buffer.isEmpty {
            try handle.write(contentsOf: buffer)
            done += UInt64(buffer.count)
        }
        try handle.synchronize()
        progress(done, total)
        if expectedSize > 0, done != expectedSize {
            throw InstallError(message: "incomplete download: \(done) of \(expectedSize) bytes")
        }
        if fm.fileExists(atPath: target.path) { try fm.removeItem(at: target) }
        try fm.moveItem(at: part, to: target)
    }
}
