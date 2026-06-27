#include "cache/file_cache.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

FileCache::~FileCache()
{
    int closed = 0;
    for (auto& [key, file] : files_) {
        if (file.fd >= 0) {
            ::close(file.fd);
            closed++;
        }
    }
    if (closed > 0)
        std::cout << "[cache] 关闭 " << closed << " 个文件 fd" << std::endl;
}

void FileCache::LoadDirectory(const std::string& doc_root)
{
    doc_root_ = doc_root;
    auto root = fs::absolute(doc_root);
    int count = 0, fd_count = 0;
    size_t total = 0;

    if (!fs::exists(root)) {
        std::cerr << "[cache] 目录不存在: " << root << std::endl;
        return;
    }

    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        auto rel = fs::relative(path, root);
        std::string key = "/" + rel.generic_string();

        auto file_size = static_cast<size_t>(entry.file_size());

        // Open fd for sendfile (always keep open)
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            std::cerr << "[cache] 无法打开: " << path << std::endl;
            continue;
        }
        fd_count++;

        // In-memory copy for SSL fallback (only small files)
        std::string content;
        if (file_size <= kSmallFileMax) {
            std::ifstream file(path, std::ios::binary);
            if (file) {
                std::stringstream ss;
                ss << file.rdbuf();
                content = ss.str();
            }
        }

        files_[key] = CachedFile{
            std::move(content),
            DetectMime(path),
            fd,
            file_size
        };
        total += file_size;
        count++;
    }

    std::cout << "[cache] 加载 " << count << " 个文件，共 "
              << (total / 1024) << " KB, "
              << fd_count << " 个 fd 已打开" << std::endl;
}

const CachedFile* FileCache::Get(const std::string& path) const
{
    auto it = files_.find(path);
    return it != files_.end() ? &it->second : nullptr;
}

std::string FileCache::DetectMime(const fs::path& p)
{
    auto ext = p.extension().string();
    if (ext == ".html") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".svg")  return "image/svg+xml";
    return "application/octet-stream";
}
