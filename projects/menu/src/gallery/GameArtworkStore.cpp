#include "GameArtworkStore.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

constexpr const char* kArtworkRoot = "sdmc:/config/OmniLaunch/game_art";
constexpr std::size_t kMaxArtworkBytes = 16 * 1024 * 1024;

std::string titleDirectory(std::uint64_t titleId) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(titleId));
    return std::string(kArtworkRoot) + "/" + id;
}

const char* extensionFor(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() >= sizeof(png) && std::memcmp(bytes.data(), png, sizeof(png)) == 0)
        return ".png";
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return ".jpg";
    return nullptr;
}

void writeFile(const std::string& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not open artwork file");
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output)
        throw std::runtime_error("could not write artwork file");
}

void replaceFile(const std::string& staged, const std::string& destination) {
    const std::string previous = destination + ".previous";
    std::remove(previous.c_str());

    const bool hadPrevious = std::filesystem::exists(destination);
    if (hadPrevious && std::rename(destination.c_str(), previous.c_str()) != 0)
        throw std::runtime_error("could not stage existing artwork");

    if (std::rename(staged.c_str(), destination.c_str()) == 0) {
        std::remove(previous.c_str());
        return;
    }

    if (hadPrevious)
        std::rename(previous.c_str(), destination.c_str());
    throw std::runtime_error("could not replace artwork");
}

nlohmann::json readMetadata(const std::string& path) {
    std::ifstream input(path);
    if (!input) return nlohmann::json::object();
    try {
        nlohmann::json result;
        input >> result;
        return result.is_object() ? result : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

std::string existingArtworkPath(std::uint64_t titleId, const char* slot) {
    if (titleId == 0)
        return {};
    const std::string directory = titleDirectory(titleId);
    for (const char* extension : {".png", ".jpg"}) {
        const std::string path = directory + "/" + slot + extension;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && !ec)
            return path;
    }
    return {};
}

} // namespace

gallery::ArtworkSaveResult gallery::GameArtworkStore::save(
    std::uint64_t titleId, ArtworkKind kind, const GameGalleryClient::Asset& asset,
    const std::vector<std::uint8_t>& bytes) {
    ArtworkSaveResult result;
    result.titleId = titleId;
    result.kind = kind;
    try {
        if (titleId == 0)
            throw std::runtime_error("invalid title id");
        if (bytes.empty() || bytes.size() > kMaxArtworkBytes)
            throw std::runtime_error("invalid artwork size");
        const char* extension = extensionFor(bytes);
        if (!extension)
            throw std::runtime_error("unsupported artwork format");

        std::error_code ec;
        std::filesystem::create_directories(kArtworkRoot, ec);
        if (ec) throw std::runtime_error("could not create artwork root");
        const std::string directory = titleDirectory(titleId);
        std::filesystem::create_directories(directory, ec);
        if (ec) throw std::runtime_error("could not create game artwork directory");

        const char* slot = kind == ArtworkKind::Cover ? "cover" : "background";
        const std::string artworkPath = directory + "/" + slot + extension;
        const std::string stagedArtwork = artworkPath + ".new";
        writeFile(stagedArtwork, bytes.data(), bytes.size());
        replaceFile(stagedArtwork, artworkPath);

        // A player can replace a PNG selection with a JPEG selection (and the
        // inverse).  pathFor() deliberately checks PNG first for compatibility
        // with artwork written by earlier builds, so retaining the alternate
        // extension here would make the old cover win forever.  The new file
        // is already atomically in place at this point; remove only the stale
        // sibling after that succeeds.
        const char* alternateExtension = std::strcmp(extension, ".png") == 0 ? ".jpg" : ".png";
        const std::string alternatePath = directory + "/" + slot + alternateExtension;
        std::filesystem::remove(alternatePath, ec);
        if (ec)
            throw std::runtime_error("could not remove previous artwork format");
        std::filesystem::remove(alternatePath + ".previous", ec);
        if (ec)
            throw std::runtime_error("could not remove previous artwork staging file");
        std::filesystem::remove(alternatePath + ".new", ec);
        if (ec)
            throw std::runtime_error("could not remove previous artwork staging file");

        const std::string metadataPath = directory + "/artwork.json";
        nlohmann::json metadata = readMetadata(metadataPath);
        char id[17];
        std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(titleId));
        metadata["titleId"] = id;
        metadata[slot] = {
            {"file", std::string(slot) + extension},
            {"source", asset.url},
            {"author", asset.author},
            {"style", asset.style},
            {"width", asset.width},
            {"height", asset.height},
        };
        const std::string serialized = metadata.dump(2);
        const std::string stagedMetadata = metadataPath + ".new";
        writeFile(stagedMetadata, reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
        replaceFile(stagedMetadata, metadataPath);

        result.ok = true;
        result.path = artworkPath;
    } catch (const std::exception& ex) {
        result.error = ex.what();
    }
    return result;
}

gallery::ArtworkSaveResult gallery::GameArtworkStore::restoreDefault(
    std::uint64_t titleId, ArtworkKind kind) {
    ArtworkSaveResult result;
    result.titleId = titleId;
    result.kind = kind;
    result.restored = true;
    try {
        if (titleId == 0)
            throw std::runtime_error("invalid title id");

        const std::string directory = titleDirectory(titleId);
        const char* slot = kind == ArtworkKind::Cover ? "cover" : "background";
        for (const char* extension : {".png", ".jpg"}) {
            const std::string path = directory + "/" + slot + extension;
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (ec)
                throw std::runtime_error("could not remove artwork file");
            std::filesystem::remove(path + ".previous", ec);
            std::filesystem::remove(path + ".new", ec);
        }

        const std::string metadataPath = directory + "/artwork.json";
        nlohmann::json metadata = readMetadata(metadataPath);
        if (!metadata.empty()) {
            metadata.erase(slot);
            const std::string serialized = metadata.dump(2);
            const std::string stagedMetadata = metadataPath + ".new";
            writeFile(stagedMetadata, reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
            replaceFile(stagedMetadata, metadataPath);
        }
        result.ok = true;
    } catch (const std::exception& ex) {
        result.error = ex.what();
    }
    return result;
}

std::string gallery::GameArtworkStore::pathFor(std::uint64_t titleId, ArtworkKind kind) {
    return existingArtworkPath(titleId, kind == ArtworkKind::Cover ? "cover" : "background");
}

std::vector<std::uint8_t> gallery::GameArtworkStore::loadCover(std::uint64_t titleId) {
    const std::string path = pathFor(titleId, ArtworkKind::Cover);
    if (path.empty())
        return {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxArtworkBytes))
        return {};
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input ? bytes : std::vector<std::uint8_t>{};
}
