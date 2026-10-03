#pragma once

#include <filesystem>
#include <string>

namespace lsxhome {

/// Normalizes a `--model` value to the path the engine expects.
///
/// `LsxModelFactory` autodetects the model type by reading
/// `<model_path>/logestix/model.index.json`, so it wants the model ROOT. Users,
/// however, see the converted `logestix/` directory in their model folder and
/// pass that; handing it over unchanged fails autodetection with "cannot
/// autodetect model type from ...". Both spellings resolve to the same root.
///
/// Anything unrecognized is returned untouched, so the engine reports its own
/// diagnostic instead of the shell silently rewriting the user's path.
inline std::string ResolveModelRoot(const std::string& path) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        return path;
    }
    std::error_code ec;
    if (fs::exists(fs::path(path) / "logestix" / "model.index.json", ec)) {
        return path;  // already the root
    }
    if (fs::exists(fs::path(path) / "model.index.json", ec)) {
        const fs::path parent = fs::path(path).parent_path();
        return parent.empty() ? path : parent.string();
    }
    return path;
}

}  // namespace lsxhome