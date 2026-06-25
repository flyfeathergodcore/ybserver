#ifndef FILE_CACHE_HPP
#define FILE_CACHE_HPP

#include <string>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

struct CachedFile {
    std::string content;
    std::string mime;
};

class FileCache {
public:
    void LoadDirectory(const std::string& doc_root);
    const CachedFile* Get(const std::string& path) const;

private:
    std::string doc_root_;
    std::unordered_map<std::string, CachedFile> files_;
    static std::string DetectMime(const fs::path& p);
};

#endif
