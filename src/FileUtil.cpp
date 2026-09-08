#include "translator/FileUtil.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace stdfs = std::filesystem;

namespace translator {
namespace fs {

namespace {
stdfs::path P(const std::string& s) { return stdfs::u8path(s); }
std::string S(const stdfs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}
} // namespace

bool exists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(P(path), ec);
}

bool isFile(const std::string& path) {
    std::error_code ec;
    return stdfs::is_regular_file(P(path), ec);
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return stdfs::is_directory(P(path), ec);
}

std::uint64_t fileSize(const std::string& path) {
    std::error_code ec;
    const auto sz = stdfs::file_size(P(path), ec);
    return ec ? 0 : static_cast<std::uint64_t>(sz);
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    if (stdfs::is_directory(P(path), ec)) return true;
    stdfs::create_directories(P(path), ec);
    return !ec;
}

bool remove(const std::string& path) {
    std::error_code ec;
    return stdfs::remove(P(path), ec) || !exists(path);
}

bool removeAll(const std::string& path) {
    std::error_code ec;
    stdfs::remove_all(P(path), ec);
    return !exists(path);
}

bool moveFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    makeDirs(parent(to));
    stdfs::remove(P(to), ec);
    ec.clear();
    stdfs::rename(P(from), P(to), ec);
    if (!ec) return true;
    // Cross-device: copy then delete.
    ec.clear();
    stdfs::copy_file(P(from), P(to), stdfs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    stdfs::remove(P(from), ec);
    return true;
}

bool moveDirectory(const std::string& from, const std::string& to) {
    std::error_code ec;
    makeDirs(parent(to));
    if (exists(to) && !removeAll(to)) return false;
    stdfs::rename(P(from), P(to), ec);
    if (!ec) return true;
    ec.clear();
    stdfs::copy(P(from), P(to), stdfs::copy_options::recursive | stdfs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    stdfs::remove_all(P(from), ec);
    return true;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return S(P(a) / P(b));
}

std::string parent(const std::string& path) { return S(P(path).parent_path()); }
std::string basename(const std::string& path) { return S(P(path).filename()); }

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(P(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    makeDirs(parent(path));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(P(tmp), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    return moveFile(tmp, path);
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : stdfs::directory_iterator(P(path), ec)) out.push_back(S(e.path().filename()));
    return out;
}

std::vector<std::string> listSubdirectories(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : stdfs::directory_iterator(P(path), ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2)) out.push_back(S(e.path().filename()));
    }
    return out;
}

} // namespace fs
} // namespace translator
