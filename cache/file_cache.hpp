#pragma once
#include <string>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

struct CachedFile {
    std::string content;   // in-memory body (empty for large files)
    std::string mime;      // MIME type
    int fd = -1;           // file descriptor for sendfile
    size_t file_size = 0;  // actual file size on disk
    time_t mtime = 0;      // modification time (for Last-Modified / ETag)
};

class FileCache {
public:
    ~FileCache();

    void LoadDirectory(const std::string& doc_root);
    const CachedFile* Get(const std::string& path) const;
    const std::string& DocRoot() const { return doc_root_; }

    /// Small-file threshold in bytes — files <= this are cached in
    /// memory; larger files keep only their fd + size.
    static constexpr size_t kSmallFileMax = 65536;

private:
    std::string doc_root_;
    std::unordered_map<std::string, CachedFile> files_;
    static std::string DetectMime(const fs::path& p);
};
