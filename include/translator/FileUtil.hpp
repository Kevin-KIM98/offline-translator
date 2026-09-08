// Thin std::filesystem helpers with error-code (non-throwing) semantics.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace translator {
namespace fs {

bool exists(const std::string& path);
bool isFile(const std::string& path);
bool isDirectory(const std::string& path);
std::uint64_t fileSize(const std::string& path); // 0 if missing

bool makeDirs(const std::string& path);
bool remove(const std::string& path);      // single file or empty dir
bool removeAll(const std::string& path);   // recursive
// Rename; falls back to copy+delete when crossing filesystems. Overwrites `to`.
bool moveFile(const std::string& from, const std::string& to);
bool moveDirectory(const std::string& from, const std::string& to);

std::string join(const std::string& a, const std::string& b);
std::string parent(const std::string& path);
std::string basename(const std::string& path);

bool readFile(const std::string& path, std::string& out);
// Writes to a temp sibling and renames so readers never observe a partial file.
bool writeFileAtomic(const std::string& path, const std::string& data);

std::vector<std::string> listDirectory(const std::string& path); // names only, unsorted
std::vector<std::string> listSubdirectories(const std::string& path);

} // namespace fs
} // namespace translator
