// Model asset management: manifest parsing, integrity verification (SHA-256), atomic
// installation and status reporting.
//
// Network I/O is intentionally NOT done here. The platform layer (URLSession / OkHttp /
// HttpURLConnection) downloads into stagingDir(id) and then calls verifyFile() + install().
// This keeps the C++ core free of TLS / background-transfer concerns while guaranteeing
// that nothing reaches the models/ tree without passing an integrity check.
//
// Manifest format (superset of the README spec; "files" mode is preferred over zip mode):
// {
//   "manifest_version": "1.1.0",
//   "base_url": "https://assets.example.com/models/",          // optional, for relative URLs
//   "stt": { "id": "whisper-small-q4", "version": "1", "filename": "whisper-small-q4.bin",
//            "size_bytes": 482344960, "sha256": "...", "download_url": "stt/whisper-small-q4.bin" },
//   "tokenizer": { "files": [ { "filename": "source.spm", "size_bytes": 1, "sha256": "...", "download_url": "..." } ] },
//   "nmt": [
//     { "pair": "ko-en", "dir_name": "ko-en", "version": "1",
//       "files": [ { "filename": "model.bin", "size_bytes": 1, "sha256": "...", "download_url": "..." }, ... ] },
//     { "pair": "en-ko", "size_bytes": 1, "sha256": "...", "download_url": "nmt/en-ko.zip" }   // zip mode
//   ]
// }
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "translator/MiniJson.hpp"

namespace translator {

struct DownloadItem {
    std::string url;
    std::string filename;     // name inside stagingDir(id)
    std::uint64_t sizeBytes = 0;
    std::string sha256;       // lower-case hex; may be empty (then only size is checked)
    bool isArchive = false;   // zip to be extracted by the platform layer before install()
};

struct ModelEntry {
    enum class Kind { Stt, Nmt, Tokenizer };

    std::string id;             // "whisper-small-q4", "nmt-ko-en", "nmt-tokenizer"
    Kind kind = Kind::Nmt;
    std::string version;        // free-form; change triggers "update_available"
    std::string pair;           // "ko-en" (nmt only)
    std::string installPath;    // file (stt) or directory (nmt / tokenizer)
    std::vector<DownloadItem> downloads;
    std::vector<std::string> requiredFiles; // relative to installPath (dirs) — for stt: {basename}

    bool isSingleFile() const { return kind == Kind::Stt; }
    std::uint64_t totalBytes() const;
    // Stable fingerprint of the manifest entry (hash of version + per-file hashes).
    std::string signature() const;
    static const char* kindName(Kind k);
};

enum class ModelState { Ready, Missing, Corrupt, UpdateAvailable, Unverified };
const char* modelStateName(ModelState s);

struct ModelStatus {
    ModelEntry entry;
    ModelState state = ModelState::Missing;
    std::string detail;            // human-readable reason
    std::string installedVersion;  // from installed.json, if any
    bool needsDownload() const { return state != ModelState::Ready; }
    Json toJson() const;
};

enum class VerifyResult { Ok, SizeMismatch, HashMismatch, IoError, Aborted };
const char* verifyResultName(VerifyResult r);

class ModelManager {
public:
    // progress(bytes_done, bytes_total) → return false to abort.
    using ProgressFn = std::function<bool(std::uint64_t, std::uint64_t)>;

    explicit ModelManager(std::string modelsRoot);

    const std::string& modelsRoot() const { return modelsRoot_; }
    std::string sttDir() const;
    std::string nmtRootDir() const;
    std::string stagingRoot() const;
    std::string installedRecordPath() const;
    std::string cachedManifestPath() const; // <root>/manifest.json

    // ---- Manifest ------------------------------------------------------------
    bool loadManifestFile(const std::string& path, std::string* error = nullptr);
    bool loadManifestJson(const std::string& jsonText, std::string* error = nullptr);
    bool loadManifest(const Json& manifest, std::string* error = nullptr);
    // Loads <root>/manifest.json if present (last manifest seen while online).
    bool loadCachedManifest(std::string* error = nullptr);
    bool saveManifest(std::string* error = nullptr);
    bool hasManifest() const { return !entries_.empty(); }
    const std::string& manifestVersion() const { return manifestVersion_; }
    const Json& manifest() const { return manifest_; }

    const std::vector<ModelEntry>& entries() const { return entries_; }
    const ModelEntry* find(const std::string& id) const;
    static std::string nmtId(const std::string& pair) { return "nmt-" + pair; }

    // ---- Status --------------------------------------------------------------
    // deepVerify recomputes SHA-256 of installed files where hashes are known
    // (slow for the 0.5–1.5 GB whisper file — run on a background thread).
    ModelStatus statusOf(const ModelEntry& entry, bool deepVerify = false, const ProgressFn& progress = nullptr) const;
    std::vector<ModelStatus> status(bool deepVerify = false, const ProgressFn& progress = nullptr) const;
    std::vector<ModelStatus> pending(bool deepVerify = false) const;
    Json statusJson(bool deepVerify = false) const;
    // Only the models needed for a given language pair (direct or via pivot) plus STT/tokenizer.
    std::vector<ModelStatus> statusForLanguages(const std::vector<std::string>& langs, bool deepVerify = false) const;

    // ---- Download staging / verification / install ------------------------------
    std::string stagingDir(const std::string& id) const; // created on demand
    bool clearStaging(const std::string& id = "");

    // Verifies size (if expectedSize > 0) and SHA-256 (if expectedSha256 non-empty).
    VerifyResult verifyFile(const std::string& path, const std::string& expectedSha256,
                            std::uint64_t expectedSize = 0, const ProgressFn& progress = nullptr) const;

    // stagedPath: for stt a file (or a directory containing the file); for nmt the extracted
    // directory. When verifyHashes is true every download item with a known hash is re-hashed
    // (skip for archives that were verified before extraction). Atomically replaces the
    // installed model and records version + signature in installed.json.
    bool install(const std::string& id, const std::string& stagedPath, bool verifyHashes = true,
                 const ProgressFn& progress = nullptr, std::string* error = nullptr);

    bool remove(const std::string& id, std::string* error = nullptr);

    // Path helpers for wiring into PipelineConfig.
    std::string sttModelPath() const;

private:
    bool parseFileItem(const Json& j, const std::string& defaultDirUrl, DownloadItem& out, std::string* error) const;
    std::string resolveUrl(const std::string& url) const;
    Json readInstalledRecord() const;
    bool writeInstalledRecord(const Json& rec) const;
    static std::string findStagedRoot(const std::string& stagedPath, const std::vector<std::string>& requiredFiles);

    std::string modelsRoot_;
    std::string baseUrl_;
    std::string manifestVersion_;
    Json manifest_;
    std::vector<ModelEntry> entries_;
};

} // namespace translator
