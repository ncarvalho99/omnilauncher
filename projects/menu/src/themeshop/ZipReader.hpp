#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace switchu::install_txn { class Txn; }

// Extracts a theme package into a directory.
//
// Deliberately small: it reads the archives the theme catalogue produces and
// nothing else. There is no zip64, no encryption, no multi-disk. A file that
// needs any of those is not one of ours, and guessing at it would be a way to
// hand the filesystem something unexpected.
//
// Every entry is checked before a byte is written. The archive arrives over the
// network from a catalogue anyone can contribute to, and it is unpacked into a
// directory the console then reads: absolute paths, traversal outside the
// destination, and unexpected file types are all refused.
//
// Reads from disk with seeks rather than from a buffer. A theme package is tens
// of megabytes, and holding one in memory -- after curl had already buffered
// and copied it -- peaked past 150 MB and killed the download with "Failed
// writing body". This way the cost is one entry at a time.
namespace themeshop {

struct ZipExtractResult {
    bool success = false;
    int filesWritten = 0;
    std::uint64_t bytesWritten = 0;
    std::string error;
};

// What an archive is allowed to contain. The defaults describe a theme package:
// media and text only, anywhere inside the destination.
//
// A launcher update is the other case. It carries executables -- an .nsp, an
// .npdm, a file called "main" with no extension at all -- so the media list
// would refuse every one of them. In exchange it must prove it stays inside the
// directories SwitchU owns, because it is unpacked at the root of a card that
// also holds the bootloader.
struct ZipExtractPolicy {
    bool allowExecutablePayload = false;
    // When set, every file that replaces an existing one is first moved into the
    // transaction's backup tree and journalled (see switchu/install_txn.hpp), so a
    // failed or unhealthy update can be rolled back. destinationDir must be the
    // transaction's destination root. Null: behaviour is unchanged.
    switchu::install_txn::Txn* txn = nullptr;
    // When non-empty, every entry must start with one of these.
    std::vector<std::string> requiredRoots;
    // When non-empty, only entries starting with one of these are written.
    // The others are still checked and then passed over, which is how one
    // archive can be unpacked in two goes, by two different processes.
    std::vector<std::string> onlyRoots;
};

// Runs every check extraction would run -- path safety, file types, the
// required roots, duplicates -- and writes nothing. Lets an update be refused
// while a screen is still up to say why, instead of at boot with nobody
// watching.
ZipExtractResult inspectZipFile(const std::string& archivePath,
                                const ZipExtractPolicy& policy = {});

// onProgress receives how many entries are done out of the total.
ZipExtractResult extractZipFile(const std::string& archivePath,
                                const std::string& destinationDir,
                                const std::function<void(int, int)>& onProgress = {},
                                const ZipExtractPolicy& policy = {});

} // namespace themeshop
