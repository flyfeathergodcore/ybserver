#include "file_cache.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

void FileCache::LoadDirectory(const std::string& doc_root)
{
    doc_root_ = doc_root;
    auto root = fs::absolute(doc_root);
    int count = 0;
    size_t total = 0;

    if (!fs::exists(root)) {
        std::cerr << "[cache] 目录不存在: " << root << std::endl;
        return;
    }

    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;

        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        auto rel = fs::relative(path, root);
        std::string key = "/" + rel.generic_string();

        files_[key] = CachedFile{
            std::move(content),
            DetectMime(path)
        };
        total += files_[key].content.size();
        count++;
    }

    std::cout << "[cache] 加载 " << count << " 个文件，共 "
              << (total / 1024) << " KB" << std::endl;
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
