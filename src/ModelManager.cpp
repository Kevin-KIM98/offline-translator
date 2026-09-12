#include "translator/ModelManager.hpp"

#include "translator/FileUtil.hpp"
#include "translator/Sha256.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace translator {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

std::string toLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string urlBasename(const std::string& url) {
    std::string u = url;
    const auto q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    const auto slash = u.find_last_of('/');
    return slash == std::string::npos ? u : u.substr(slash + 1);
}

bool looksLikeArchive(const std::string& name) {
    const std::string n = toLower(name);
    return endsWith(n, ".zip") || endsWith(n, ".tar.gz") || endsWith(n, ".tgz");
}

std::int64_t nowUnix() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

// ---------------------------------------------------------------------------
// ModelEntry / ModelStatus
// ---------------------------------------------------------------------------

const char* ModelEntry::kindName(Kind k) {
    switch (k) {
        case Kind::Stt: return "stt";
        case Kind::Nmt: return "nmt";
        case Kind::Tokenizer: return "tokenizer";
        case Kind::Llm: return "llm";
    }
    return "unknown";
}

std::uint64_t ModelEntry::totalBytes() const {
    std::uint64_t t = 0;
    for (const auto& d : downloads) t += d.sizeBytes;
    return t;
}

std::string ModelEntry::signature() const {
    Sha256 h;
    h.update(version);
    h.update("|");
    for (const auto& d : downloads) {
        h.update(d.filename);
        h.update(":");
        h.update(d.sha256.empty() ? std::to_string(d.sizeBytes) : toLower(d.sha256));
        h.update("|");
    }
    return h.hexDigest();
}

const char* modelStateName(ModelState s) {
    switch (s) {
        case ModelState::Ready: return "ready";
        case ModelState::Missing: return "missing";
        case ModelState::Corrupt: return "corrupt";
        case ModelState::UpdateAvailable: return "update_available";
        case ModelState::Unverified: return "unverified";
    }
    return "unknown";
}

const char* verifyResultName(VerifyResult r) {
    switch (r) {
        case VerifyResult::Ok: return "ok";
        case VerifyResult::SizeMismatch: return "size_mismatch";
        case VerifyResult::HashMismatch: return "hash_mismatch";
        case VerifyResult::IoError: return "io_error";
        case VerifyResult::Aborted: return "aborted";
    }
    return "unknown";
}

Json ModelStatus::toJson() const {
    Json j = Json::object();
    j.set("id", entry.id);
    j.set("kind", ModelEntry::kindName(entry.kind));
    j.set("pair", entry.pair);
    j.set("label", entry.label);
    j.set("version", entry.version);
    j.set("state", modelStateName(state));
    j.set("needs_download", needsDownload());
    j.set("detail", detail);
    j.set("installed_version", installedVersion);
    j.set("install_path", entry.installPath);
    j.set("total_bytes", entry.totalBytes());
    Json dl = Json::array();
    for (const auto& d : entry.downloads) {
        Json item = Json::object();
        item.set("url", d.url);
        item.set("filename", d.filename);
        item.set("size_bytes", d.sizeBytes);
        item.set("sha256", d.sha256);
        item.set("archive", d.isArchive);
        dl.push_back(item);
    }
    j.set("downloads", dl);
    Json req = Json::array();
    for (const auto& f : entry.requiredFiles) req.push_back(f);
    j.set("required_files", req);
    return j;
}

// ---------------------------------------------------------------------------
// ModelManager — paths
// ---------------------------------------------------------------------------

ModelManager::ModelManager(std::string modelsRoot) : modelsRoot_(std::move(modelsRoot)) {}

std::string ModelManager::sttDir() const { return fs::join(modelsRoot_, "stt"); }
std::string ModelManager::nmtRootDir() const { return fs::join(modelsRoot_, "nmt"); }
std::string ModelManager::stagingRoot() const { return fs::join(modelsRoot_, "staging"); }
std::string ModelManager::installedRecordPath() const { return fs::join(modelsRoot_, "installed.json"); }
std::string ModelManager::cachedManifestPath() const { return fs::join(modelsRoot_, "manifest.json"); }

std::vector<const ModelEntry*> ModelManager::sttEntries() const {
    std::vector<const ModelEntry*> out;
    for (const auto& e : entries_)
        if (e.kind == ModelEntry::Kind::Stt) out.push_back(&e);
    return out;
}

const ModelEntry* ModelManager::sttEntry(const std::string& sttId) const {
    const ModelEntry* first = nullptr;
    for (const auto& e : entries_) {
        if (e.kind != ModelEntry::Kind::Stt) continue;
        if (e.id == sttId) return &e;
        if (!first) first = &e;
    }
    return first;   // unknown id: the default, as for the LLM
}

std::string ModelManager::sttModelPath(const std::string& sttId) const {
    const ModelEntry* e = sttEntry(sttId);
    return e ? e->installPath : std::string();
}

std::string ModelManager::llmDir() const { return fs::join(modelsRoot_, "llm"); }

std::vector<const ModelEntry*> ModelManager::llmEntries() const {
    std::vector<const ModelEntry*> out;
    for (const auto& e : entries_)
        if (e.kind == ModelEntry::Kind::Llm) out.push_back(&e);
    return out;
}

const ModelEntry* ModelManager::llmEntry(const std::string& llmId) const {
    const ModelEntry* first = nullptr;
    for (const auto& e : entries_) {
        if (e.kind != ModelEntry::Kind::Llm) continue;
        if (e.id == llmId) return &e;
        if (!first) first = &e;
    }
    // An unknown id (a choice saved before the manifest dropped that model) falls back to the
    // default rather than leaving the app without an LLM.
    return first;
}

std::string ModelManager::llmModelPath(const std::string& llmId) const {
    const ModelEntry* e = llmEntry(llmId);
    return e ? e->installPath : std::string();
}

const ModelEntry* ModelManager::find(const std::string& id) const {
    for (const auto& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

std::string ModelManager::stagingDir(const std::string& id) const {
    const std::string dir = fs::join(stagingRoot(), id);
    fs::makeDirs(dir);
    return dir;
}

bool ModelManager::clearStaging(const std::string& id) {
    return fs::removeAll(id.empty() ? stagingRoot() : fs::join(stagingRoot(), id));
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------

std::string ModelManager::resolveUrl(const std::string& url) const {
    if (url.empty()) return url;
    if (url.find("://") != std::string::npos) return url;
    if (baseUrl_.empty()) return url;
    if (endsWith(baseUrl_, "/") || url.front() == '/') return baseUrl_ + (url.front() == '/' ? url.substr(1) : url);
    return baseUrl_ + "/" + url;
}

bool ModelManager::parseFileItem(const Json& j, const std::string& defaultDirUrl, DownloadItem& out, std::string* error) const {
    out.filename = j.getString("filename");
    out.url = j.getString("download_url", j.getString("url"));
    if (out.url.empty() && !defaultDirUrl.empty() && !out.filename.empty()) out.url = defaultDirUrl + "/" + out.filename;
    if (out.filename.empty() && !out.url.empty()) out.filename = urlBasename(out.url);
    if (out.filename.empty()) {
        if (error) *error = "manifest file item missing filename/download_url";
        return false;
    }
    out.url = resolveUrl(out.url);
    out.sizeBytes = static_cast<std::uint64_t>(j.getInt64("size_bytes", 0));
    out.sha256 = toLower(j.getString("sha256"));
    out.isArchive = j.getBool("archive", looksLikeArchive(out.filename));
    return true;
}

bool ModelManager::loadManifestFile(const std::string& path, std::string* error) {
    std::string text;
    if (!fs::readFile(path, text)) {
        if (error) *error = "cannot read manifest: " + path;
        return false;
    }
    return loadManifestJson(text, error);
}

bool ModelManager::loadManifestJson(const std::string& jsonText, std::string* error) {
    std::string perr;
    const Json m = Json::parse(jsonText, &perr);
    if (!perr.empty() || !m.isObject()) {
        if (error) *error = "manifest parse error: " + (perr.empty() ? std::string("not an object") : perr);
        return false;
    }
    return loadManifest(m, error);
}

bool ModelManager::loadCachedManifest(std::string* error) {
    if (!fs::isFile(cachedManifestPath())) {
        if (error) *error = "no cached manifest";
        return false;
    }
    return loadManifestFile(cachedManifestPath(), error);
}

bool ModelManager::saveManifest(std::string* error) {
    if (!manifest_.isObject()) {
        if (error) *error = "no manifest loaded";
        return false;
    }
    if (!fs::writeFileAtomic(cachedManifestPath(), manifest_.dump(2))) {
        if (error) *error = "cannot write " + cachedManifestPath();
        return false;
    }
    return true;
}

bool ModelManager::loadManifest(const Json& manifest, std::string* error) {
    std::vector<ModelEntry> entries;
    baseUrl_ = manifest.getString("base_url");
    const std::string manifestVersion = manifest.getString("manifest_version", "0");

    // ---- STT: "stt" is the default; "stt_options" lists further whisper models (one file each) ----
    auto parseStt = [&](const Json& node) -> bool {
        ModelEntry e;
        e.kind = ModelEntry::Kind::Stt;
        e.id = node.getString("id", "whisper");
        e.version = node.getString("version", "1");
        e.label = node.getString("label");
        DownloadItem d;
        if (!parseFileItem(node, resolveUrl("stt"), d, error)) return false;
        d.isArchive = false;
        e.downloads.push_back(d);
        e.installPath = fs::join(sttDir(), d.filename);
        e.requiredFiles = {d.filename};
        entries.push_back(std::move(e));
        return true;
    };
    const Json& stt = manifest["stt"];
    if (stt.isObject() && !parseStt(stt)) return false;
    const Json& sttOptions = manifest["stt_options"];
    if (sttOptions.isArray())
        for (const auto& node : sttOptions.asArray())
            if (node.isObject() && !parseStt(node)) return false;

    // ---- LLM: "llm" is the default; "llm_options" lists further choices (one GGUF each) ----
    auto parseLlm = [&](const Json& node) -> bool {
        ModelEntry e;
        e.kind = ModelEntry::Kind::Llm;
        e.id = node.getString("id", "llm");
        e.version = node.getString("version", "1");
        e.label = node.getString("label");
        DownloadItem d;
        if (!parseFileItem(node, resolveUrl("llm"), d, error)) return false;
        d.isArchive = false;
        e.downloads.push_back(d);
        e.installPath = fs::join(llmDir(), d.filename);
        e.requiredFiles = {d.filename};
        entries.push_back(std::move(e));
        return true;
    };
    const Json& llm = manifest["llm"];
    if (llm.isObject() && !parseLlm(llm)) return false;
    const Json& llmOptions = manifest["llm_options"];
    if (llmOptions.isArray())
        for (const auto& node : llmOptions.asArray())
            if (node.isObject() && !parseLlm(node)) return false;

    // ---- Shared tokenizer (optional) ----
    const Json& tok = manifest["tokenizer"];
    if (tok.isObject()) {
        ModelEntry e;
        e.kind = ModelEntry::Kind::Tokenizer;
        e.id = "nmt-tokenizer";
        e.version = tok.getString("version", "1");
        const std::string dirName = tok.getString("dir_name", "tokenizer");
        e.installPath = fs::join(nmtRootDir(), dirName);
        if (tok["files"].isArray()) {
            for (const auto& f : tok["files"].asArray()) {
                DownloadItem d;
                if (!parseFileItem(f, resolveUrl("nmt/" + dirName), d, error)) return false;
                e.requiredFiles.push_back(d.filename);
                e.downloads.push_back(std::move(d));
            }
        } else {
            DownloadItem d;
            if (!parseFileItem(tok, "", d, error)) return false;
            e.downloads.push_back(d);
            e.requiredFiles = {"source.spm"};
        }
        entries.push_back(std::move(e));
    }

    // ---- NMT pairs ----
    const Json& nmt = manifest["nmt"];
    if (nmt.isArray()) {
        for (const auto& item : nmt.asArray()) {
            ModelEntry e;
            e.kind = ModelEntry::Kind::Nmt;
            e.pair = item.getString("pair");
            if (e.pair.empty()) {
                if (error) *error = "nmt entry missing \"pair\"";
                return false;
            }
            const std::string dirName = item.getString("dir_name", e.pair);
            e.id = nmtId(e.pair);
            e.version = item.getString("version", "1");
            e.installPath = fs::join(nmtRootDir(), dirName);

            if (item["required_files"].isArray()) {
                for (const auto& f : item["required_files"].asArray()) e.requiredFiles.push_back(f.asString());
            }

            if (item["files"].isArray()) {
                for (const auto& f : item["files"].asArray()) {
                    DownloadItem d;
                    if (!parseFileItem(f, resolveUrl("nmt/" + dirName), d, error)) return false;
                    d.isArchive = false;
                    if (e.requiredFiles.empty() || std::find(e.requiredFiles.begin(), e.requiredFiles.end(), d.filename) == e.requiredFiles.end()) {
                        if (!item["required_files"].isArray()) e.requiredFiles.push_back(d.filename);
                    }
                    e.downloads.push_back(std::move(d));
                }
            } else {
                DownloadItem d;
                if (!parseFileItem(item, "", d, error)) return false;
                if (d.url.empty()) {
                    if (error) *error = "nmt entry " + e.pair + " has neither files[] nor download_url";
                    return false;
                }
                d.isArchive = item.getBool("archive", true);
                e.downloads.push_back(std::move(d));
                if (e.requiredFiles.empty()) e.requiredFiles = {"model.bin"};
            }
            entries.push_back(std::move(e));
        }
    }

    if (entries.empty()) {
        if (error) *error = "manifest contains no models";
        return false;
    }

    manifest_ = manifest;
    manifestVersion_ = manifestVersion;
    entries_ = std::move(entries);
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// Installed record
// ---------------------------------------------------------------------------

Json ModelManager::readInstalledRecord() const {
    std::string text;
    if (!fs::readFile(installedRecordPath(), text)) return Json::object();
    Json j = Json::parse(text);
    return j.isObject() ? j : Json::object();
}

bool ModelManager::writeInstalledRecord(const Json& rec) const {
    return fs::writeFileAtomic(installedRecordPath(), rec.dump(2));
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

ModelStatus ModelManager::statusOf(const ModelEntry& entry, bool deepVerify, const ProgressFn& progress) const {
    ModelStatus s;
    s.entry = entry;

    // 1. Presence of required files.
    std::vector<std::string> missing;
    for (const auto& rel : entry.requiredFiles) {
        const std::string p = entry.isSingleFile() ? entry.installPath : fs::join(entry.installPath, rel);
        if (!fs::isFile(p)) missing.push_back(rel);
    }
    if (!missing.empty()) {
        s.state = ModelState::Missing;
        s.detail = "missing: ";
        for (std::size_t i = 0; i < missing.size(); ++i) s.detail += (i ? ", " : "") + missing[i];
        return s;
    }

    // 2. Installed record vs manifest signature.
    const Json rec = readInstalledRecord()[entry.id];
    const bool hasRecord = rec.isObject();
    if (hasRecord) s.installedVersion = rec.getString("version");

    // Per-file checks are possible when downloads map 1:1 to installed files.
    const bool perFileHashes = !entry.downloads.empty() &&
                               std::none_of(entry.downloads.begin(), entry.downloads.end(), [](const DownloadItem& d) { return d.isArchive; });

    if (hasRecord && rec.getString("signature") != entry.signature()) {
        s.state = ModelState::UpdateAvailable;
        s.detail = "manifest version " + entry.version + " differs from installed " + s.installedVersion;
        return s;
    }

    // 3. Quick size check.
    if (perFileHashes) {
        for (const auto& d : entry.downloads) {
            const std::string p = entry.isSingleFile() ? entry.installPath : fs::join(entry.installPath, d.filename);
            if (d.sizeBytes > 0 && fs::fileSize(p) != d.sizeBytes) {
                s.state = ModelState::Corrupt;
                s.detail = d.filename + ": size " + std::to_string(fs::fileSize(p)) + " != " + std::to_string(d.sizeBytes);
                return s;
            }
        }
    }

    // 4. Deep verification.
    if (deepVerify && perFileHashes) {
        std::uint64_t total = entry.totalBytes(), doneBase = 0;
        for (const auto& d : entry.downloads) {
            if (d.sha256.empty()) continue;
            const std::string p = entry.isSingleFile() ? entry.installPath : fs::join(entry.installPath, d.filename);
            const std::uint64_t base = doneBase;
            const VerifyResult vr = verifyFile(p, d.sha256, d.sizeBytes, progress ? [&](std::uint64_t done, std::uint64_t) {
                return progress(base + done, total);
            } : ProgressFn());
            doneBase += d.sizeBytes;
            if (vr == VerifyResult::Aborted) {
                s.state = ModelState::Unverified;
                s.detail = "verification aborted";
                return s;
            }
            if (vr != VerifyResult::Ok) {
                s.state = ModelState::Corrupt;
                s.detail = d.filename + ": " + verifyResultName(vr);
                return s;
            }
        }
        s.state = ModelState::Ready;
        s.detail = "verified";
        return s;
    }

    if (!hasRecord) {
        // Files exist but nothing installed them through us (e.g. adb push during development).
        s.state = (perFileHashes ? ModelState::Unverified : ModelState::Unverified);
        s.detail = "present but not verified (no installed.json record)";
        return s;
    }

    s.state = ModelState::Ready;
    s.detail = "installed record matches manifest";
    return s;
}

std::vector<ModelStatus> ModelManager::status(bool deepVerify, const ProgressFn& progress) const {
    std::vector<ModelStatus> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(statusOf(e, deepVerify, progress));
    return out;
}

std::vector<ModelStatus> ModelManager::pending(bool deepVerify) const {
    std::vector<ModelStatus> out;
    for (auto& s : status(deepVerify))
        if (s.needsDownload()) out.push_back(std::move(s));
    return out;
}

Json ModelManager::statusJson(bool deepVerify) const {
    Json j = Json::object();
    j.set("manifest_version", manifestVersion_);
    j.set("models_root", modelsRoot_);
    Json arr = Json::array();
    std::uint64_t pendingBytes = 0;
    for (const auto& s : status(deepVerify)) {
        if (s.needsDownload()) pendingBytes += s.entry.totalBytes();
        arr.push_back(s.toJson());
    }
    j.set("pending_bytes", pendingBytes);
    j.set("models", arr);
    return j;
}

std::vector<ModelStatus> ModelManager::statusForLanguages(const std::vector<std::string>& langs, bool deepVerify,
                                                          LlmMode llmMode, const std::string& llmId,
                                                          const std::string& sttId) const {
    std::vector<ModelStatus> out;
    const ModelEntry* selectedLlm = llmEntry(llmId);
    const ModelEntry* selectedStt = sttEntry(sttId);
    // Which Marian pairs exist in the manifest (for the "is the LLM needed" decision).
    std::vector<std::string> pairs;
    for (const auto& e : entries_)
        if (e.kind == ModelEntry::Kind::Nmt) pairs.push_back(e.pair);
    auto hasPair = [&](const std::string& a, const std::string& b) {
        return std::find(pairs.begin(), pairs.end(), a + "-" + b) != pairs.end();
    };
    // Every direction between two chosen languages needs a route: a direct pair, two pairs through
    // English, or the LLM. What gets downloaded is exactly the pairs those routes use, including
    // the English hops that a choice like {ko, ja} never names itself.
    bool llmNeeded = false;
    std::vector<std::string> wantedPairs;
    auto want = [&](const std::string& pair) {
        if (std::find(wantedPairs.begin(), wantedPairs.end(), pair) == wantedPairs.end()) wantedPairs.push_back(pair);
    };
    for (const auto& a : langs)
        for (const auto& b : langs) {
            if (a == b) continue;
            if (hasPair(a, b)) {
                want(a + "-" + b);
            } else if (a != "en" && b != "en" && hasPair(a, "en") && hasPair("en", b)) {
                want(a + "-en");
                want("en-" + b);
            } else {
                llmNeeded = true;
                // The pipeline gives the LLM English when a dedicated model can produce it.
                if (a != "en" && b != "en" && hasPair(a, "en")) want(a + "-en");
            }
        }

    for (const auto& e : entries_) {
        bool wanted = true;
        if (e.kind == ModelEntry::Kind::Stt) {
            // One speech model at a time: the selected one, or the default.
            wanted = selectedStt != nullptr && &e == selectedStt;
        } else if (e.kind == ModelEntry::Kind::Nmt) {
            wanted = std::find(wantedPairs.begin(), wantedPairs.end(), e.pair) != wantedPairs.end();
        } else if (e.kind == ModelEntry::Kind::Llm) {
            // One LLM at a time: the selected one, or the default.
            const bool selected = selectedLlm != nullptr && &e == selectedLlm;
            wanted = selected && (llmMode == LlmMode::Always || (llmMode == LlmMode::IfNeeded && llmNeeded));
        }
        if (wanted) out.push_back(statusOf(e, deepVerify));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Verification & install
// ---------------------------------------------------------------------------

VerifyResult ModelManager::verifyFile(const std::string& path, const std::string& expectedSha256,
                                      std::uint64_t expectedSize, const ProgressFn& progress) const {
    if (!fs::isFile(path)) return VerifyResult::IoError;
    if (expectedSize > 0 && fs::fileSize(path) != expectedSize) return VerifyResult::SizeMismatch;
    if (expectedSha256.empty()) return VerifyResult::Ok;

    bool aborted = false;
    const std::string actual = Sha256::hashFile(path, [&](std::uint64_t done, std::uint64_t total) {
        if (progress && !progress(done, total)) {
            aborted = true;
            return false;
        }
        return true;
    });
    if (aborted) return VerifyResult::Aborted;
    if (actual.empty()) return VerifyResult::IoError;
    return Sha256::equalsIgnoreCase(actual, expectedSha256) ? VerifyResult::Ok : VerifyResult::HashMismatch;
}

std::string ModelManager::findStagedRoot(const std::string& stagedPath, const std::vector<std::string>& requiredFiles) {
    auto hasAll = [&](const std::string& dir) {
        for (const auto& f : requiredFiles)
            if (!fs::isFile(fs::join(dir, f))) return false;
        return true;
    };
    if (hasAll(stagedPath)) return stagedPath;
    // Archives often extract into a single nested folder (e.g. ko-en/ko-en/model.bin).
    const auto subs = fs::listSubdirectories(stagedPath);
    for (const auto& sub : subs) {
        const std::string p = fs::join(stagedPath, sub);
        if (hasAll(p)) return p;
    }
    return {};
}

bool ModelManager::install(const std::string& id, const std::string& stagedPath, bool verifyHashes,
                           const ProgressFn& progress, std::string* error) {
    const ModelEntry* e = find(id);
    if (!e) {
        if (error) *error = "unknown model id: " + id;
        return false;
    }

    if (e->isSingleFile()) {
        const std::string filename = fs::basename(e->installPath);
        std::string src = stagedPath;
        if (fs::isDirectory(src)) src = fs::join(src, filename);
        if (!fs::isFile(src)) {
            if (error) *error = "staged file not found: " + src;
            return false;
        }
        const DownloadItem& d = e->downloads.front();
        const VerifyResult vr = verifyFile(src, verifyHashes ? d.sha256 : std::string(), d.sizeBytes, progress);
        if (vr != VerifyResult::Ok) {
            if (error) *error = std::string("verification failed: ") + verifyResultName(vr);
            return false;
        }
        if (!fs::moveFile(src, e->installPath)) {
            if (error) *error = "cannot move into place: " + e->installPath;
            return false;
        }
    } else {
        if (!fs::isDirectory(stagedPath)) {
            if (error) *error = "staged directory not found: " + stagedPath;
            return false;
        }
        const std::string root = findStagedRoot(stagedPath, e->requiredFiles);
        if (root.empty()) {
            if (error) {
                *error = "staged directory lacks required files (";
                for (std::size_t i = 0; i < e->requiredFiles.size(); ++i) *error += (i ? ", " : "") + e->requiredFiles[i];
                *error += ")";
            }
            return false;
        }
        if (verifyHashes) {
            std::uint64_t total = e->totalBytes(), doneBase = 0;
            for (const auto& d : e->downloads) {
                if (d.isArchive) continue; // verified before extraction by the caller
                const std::string p = fs::join(root, d.filename);
                const std::uint64_t base = doneBase;
                const VerifyResult vr = verifyFile(p, d.sha256, d.sizeBytes, progress ? [&](std::uint64_t done, std::uint64_t) {
                    return progress(base + done, total);
                } : ProgressFn());
                doneBase += d.sizeBytes;
                if (vr != VerifyResult::Ok) {
                    if (error) *error = d.filename + ": " + verifyResultName(vr);
                    return false;
                }
            }
        }
        if (!fs::moveDirectory(root, e->installPath)) {
            if (error) *error = "cannot move into place: " + e->installPath;
            return false;
        }
        if (root != stagedPath) fs::removeAll(stagedPath);
    }

    Json rec = readInstalledRecord();
    Json item = Json::object();
    item.set("version", e->version);
    item.set("signature", e->signature());
    item.set("installed_at", nowUnix());
    rec.set(e->id, item);
    if (!writeInstalledRecord(rec)) {
        if (error) *error = "cannot write installed.json";
        return false;
    }
    fs::removeAll(fs::join(stagingRoot(), id));
    if (error) error->clear();
    return true;
}

bool ModelManager::remove(const std::string& id, std::string* error) {
    const ModelEntry* e = find(id);
    if (!e) {
        if (error) *error = "unknown model id: " + id;
        return false;
    }
    const bool ok = e->isSingleFile() ? fs::remove(e->installPath) : fs::removeAll(e->installPath);
    Json rec = readInstalledRecord();
    if (rec.isObject()) {
        auto& obj = rec.asObject();
        obj.erase(std::remove_if(obj.begin(), obj.end(), [&](const std::pair<std::string, Json>& kv) { return kv.first == id; }), obj.end());
        writeInstalledRecord(rec);
    }
    if (!ok && error) *error = "cannot remove " + e->installPath;
    return ok;
}

} // namespace translator
