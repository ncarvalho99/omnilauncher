#pragma once
// Rollback journal for replacing installed files (launcher updates).
//
// Pure std C++ (no libnx), so the logic is unit-tested on the host
// (tests/install_txn_test.cpp). It does not unpack anything: the extractor asks
// it to copy the file about to be replaced into a backup tree, commit it, and
// then record that BEFORE the new file is renamed into place. The installed
// file stays present until a complete new file is ready.
//
//   <backupRoot>/journal       append-only, one record per line:
//                                R<TAB><relative path>   replaced: original is in files/
//                                N<TAB><relative path>   created: nothing was there before
//                                B                       a pass started
//                                C                       that pass finished
//                                X                       a rollback started (so a cut during it is resumed)
//                                A                       the update was accepted; only cleanup remains
//   <backupRoot>/files/<rel>   the original file, copied before the swap
//
// A journal whose last record is not C or A was interrupted (every pass writes B first,
// so a second pass that only touches already-saved files is still detected) (power cut, crash, a
// failed step): rollback() puts every original back and deletes every file the
// pass created. rollback() is idempotent, so an interrupted rollback is simply
// run again. A path is backed up once: if a second pass replaces a file the
// first pass already replaced, the original backup is kept, so rolling back
// always returns to the state before the FIRST pass.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace switchu::install_txn {

// Durability barrier. The journal record that says a file is about to be moved must
// reach the card BEFORE the move, or a power cut can leave a moved file nobody
// knows about. Pure std code cannot commit the card, so the owner installs a hook
// (fsdevCommitDevice on the console). Null on the host.
inline bool (*g_syncHook)() = nullptr;
inline bool sync() { return g_syncHook ? g_syncHook() : true; }

// Where launcher updates keep their rollback backup on the card.
inline constexpr const char* kUpdateBackupRoot = "sdmc:/config/OmniLaunch/update/rollback";
inline constexpr const char* kCardRoot = "sdmc:/";

inline std::string journalPath(const std::string& backupRoot) { return backupRoot + "/journal"; }
inline std::string backupPath(const std::string& backupRoot, const std::string& rel) {
    return backupRoot + "/files/" + rel;
}

// Copy a regular file to a new path without trusting a short read/write as success.
inline bool copyFile(const std::string& src, const std::string& dst) {
    std::FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;
    std::FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    char buf[64 * 1024];
    bool ok = true;
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (std::ferror(in)) ok = false;
    if (std::fflush(out) != 0) ok = false;
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);
    if (!ok) std::remove(dst.c_str());
    return ok;
}
struct Record {
    char kind = 0;          // 'R', 'N', 'B', 'C', 'X' or 'A'
    std::string rel;
};

inline bool readJournal(const std::string& backupRoot, std::vector<Record>& out) {
    out.clear();
    std::FILE* f = std::fopen(journalPath(backupRoot).c_str(), "rb");
    if (!f)
        return false;
    std::string text;
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            break;                                  // a line without its newline was cut short
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line == "C" || line == "B" || line == "X" || line == "A") {
            out.push_back({line[0], {}});
        } else if (line.size() > 2 && (line[0] == 'R' || line[0] == 'N') && line[1] == '\t') {
            out.push_back({line[0], line.substr(2)});
        }
        // Anything else is ignored: an unknown record must not hide the rest.
    }
    return true;
}

// True when a journal exists and its last record is neither a completed pass nor an
// acceptance: a pass or a rollback was cut short and has to be (re)done.
inline bool interrupted(const std::string& backupRoot) {
    std::vector<Record> recs;
    if (!readJournal(backupRoot, recs))
        return false;
    return recs.empty() || (recs.back().kind != 'C' && recs.back().kind != 'A');
}

// True when a backup of a finished, not yet accepted update is held.
inline bool pending(const std::string& backupRoot) {
    std::vector<Record> recs;
    if (!readJournal(backupRoot, recs))
        return false;
    return recs.empty() || recs.back().kind != 'A';
}

// Finish a leftover cleanup (an accepted update whose backup directory removal
// was not completed). Safe to call unconditionally at boot: no-op when there
// is nothing to clean.
inline void resumeCleanup(const std::string& backupRoot) {
    std::vector<Record> recs;
    if (!readJournal(backupRoot, recs))
        return;
    if (!recs.empty() && recs.back().kind == 'A') {
        std::error_code ec;
        std::filesystem::remove_all(backupRoot + "/files", ec);
        std::filesystem::remove(journalPath(backupRoot), ec);
        std::filesystem::remove(backupRoot, ec);
    }
}

class Txn {
public:
    // `destRoot` is the directory the relative paths are under (e.g. "sdmc:/").
    Txn(std::string backupRoot, std::string destRoot)
        : m_backup(std::move(backupRoot)), m_dest(std::move(destRoot)) {}
    ~Txn() { closeJournal(); }
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    bool begin() {
        std::error_code ec;
        std::filesystem::create_directories(m_backup + "/files", ec);
        if (ec)
            return false;
        m_journal = std::fopen(journalPath(m_backup).c_str(), "ab");
        if (m_journal == nullptr || !record('B', std::string()))
            return false;
        return sync();
    }

    // Called with the new file already complete next to its target. Copies the
    // existing target (if any) into the backup tree, commits it, and journals
    // it before returning; the target stays installed until the new file swap.
    bool prepareReplace(const std::string& rel) {
        // Trust boundary: the extractor validates this separately, but never let a
        // different caller journal an absolute path or a traversal.
        if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos ||
            rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos ||
            rel.find('\n') != std::string::npos || rel.find('\t') != std::string::npos)
            return false;
        const std::string target = m_dest + rel;
        std::error_code ec;
        const bool existed = std::filesystem::exists(target, ec);
        if (ec || (existed && !std::filesystem::is_regular_file(target, ec)))
            return false;
        const std::string saved = backupPath(m_backup, rel);
        if (std::filesystem::exists(saved, ec))
            return true;                               // original already backed up by an earlier pass
        if (existed) {
            std::filesystem::create_directories(std::filesystem::path(saved).parent_path(), ec);
            if (ec)
                return false;
            const std::string backupPart = saved + ".part";
            if (!copyFile(target, backupPart))
                return false;
            // Write and commit the complete copy before it is published as a
            // backup. A cut mid-copy leaves only .part, never a valid backup.
            if (!sync() || std::rename(backupPart.c_str(), saved.c_str()) != 0)
                return false;
            if (!sync() || !record('R', rel))          // backup durable before R is written
                return false;
            if (!sync()) return false;               // journal durable before swap
            return true;
        }
        if (!record('N', rel))
            return false;
        return sync();
    }
    // The new file could not be moved into place after prepareReplace() set the
    // old one aside: put it straight back.
    bool undoReplace(const std::string& rel) {
        const std::string target = m_dest + rel;
        const std::string saved = backupPath(m_backup, rel);
        std::error_code ec;
        if (!std::filesystem::exists(saved, ec))
            return true;
        const std::string staging = target + ".rollback.part";
        if (!copyFile(saved, staging)) return false;
        std::remove(target.c_str());
        if (std::rename(staging.c_str(), target.c_str()) == 0) return true;
        std::remove(staging.c_str());
        return false;
    }

    bool completePass() {
        if (!record('C', std::string()))
            return false;
        return sync();
    }

private:
    bool record(char kind, const std::string& rel) {
        if (!m_journal)
            return false;
        const std::string line = (kind == 'C' || kind == 'B')
            ? std::string(1, kind) + "\n"
            : std::string(1, kind) + "\t" + rel + "\n";
        if (std::fwrite(line.data(), 1, line.size(), m_journal) != line.size())
            return false;
        return std::fflush(m_journal) == 0;
    }
    void closeJournal() {
        if (m_journal) {
            std::fclose(m_journal);
            m_journal = nullptr;
        }
    }
    std::string m_backup;
    std::string m_dest;
    std::FILE* m_journal = nullptr;
};

// Returns every original to its place and removes every file a pass created.
// The journal and backup tree are deleted only once every step succeeded.
// `failed` receives the first path that could not be restored.
inline bool rollback(const std::string& backupRoot, const std::string& destRoot,
                     std::string* failed = nullptr) {
    std::vector<Record> recs;
    if (!readJournal(backupRoot, recs))
        return true;                                // nothing to undo
    std::error_code ec;
    if (!recs.empty() && recs.back().kind == 'A') {
        // Accepted: nothing to restore, only the leftovers of the cleanup.
        std::filesystem::remove_all(backupRoot, ec);
        return true;
    }
    // Say that a rollback is under way BEFORE restoring anything: a cut now leaves
    // a journal that does not end in C, so the next boot resumes this rollback
    // instead of treating the half-restored tree as a finished update.
    if (recs.empty() || recs.back().kind != 'X') {
        if (std::FILE* j = std::fopen(journalPath(backupRoot).c_str(), "ab")) {
            if (std::fputs("X\n", j) < 0 || std::fflush(j) != 0) {
                std::fclose(j);
                return false;
            }
            std::fclose(j);
            if (!sync()) return false;
        } else {
            return false;                           // cannot record it: do not start
        }
    }
    bool ok = true;
    for (std::size_t i = recs.size(); i-- > 0;) {
        const Record& r = recs[i];
        if (r.kind != 'R' && r.kind != 'N')
            continue;
        if (r.rel.empty() || r.rel[0] == '/' || r.rel.find("..") != std::string::npos ||
            r.rel.find('\\') != std::string::npos || r.rel.find(':') != std::string::npos) {
            ok = false;
            if (failed && failed->empty()) *failed = r.rel;
            continue;
        }
        const std::string target = destRoot + r.rel;
        if (r.kind == 'N') {
            if (std::filesystem::exists(target, ec) && std::remove(target.c_str()) != 0) {
                ok = false;
                if (failed && failed->empty()) *failed = r.rel;
            }
            continue;
        }
        const std::string saved = backupPath(backupRoot, r.rel);
        if (!std::filesystem::exists(saved, ec)) {
            // R is written only AFTER the backup was copied and committed. A
            // missing one is not safe to infer as already restored: it may have
            // been lost by a card error, so keep the journal for inspection.
            ok = false;
            if (failed && failed->empty()) *failed = r.rel;
            continue;
        }
        const std::string staging = target + ".rollback.part";
        if (!copyFile(saved, staging)) {
            ok = false;
            if (failed && failed->empty()) *failed = r.rel;
            continue;
        }
        std::remove(target.c_str());
        if (std::rename(staging.c_str(), target.c_str()) != 0) {
            std::remove(staging.c_str());
            ok = false;
            if (failed && failed->empty()) *failed = r.rel;
        }
    }
    if (ok) {
        std::filesystem::remove_all(backupRoot, ec);
    }
    return ok;
}

// The update is accepted: record that first, then drop the backup. A cut during
// the deletion leaves a journal that ends in A, which rollback() only cleans up
// and never restores from.
inline bool commit(const std::string& backupRoot) {
    std::FILE* j = std::fopen(journalPath(backupRoot).c_str(), "ab");
    if (!j)
        return false;                               // no acceptance record, keep the backup
    const bool wrote = std::fputs("A\n", j) >= 0 && std::fflush(j) == 0;
    const bool closed = std::fclose(j) == 0;
    if (!wrote || !closed)
        return false;
    if (!sync()) return false;
    // Delete the files first and the journal last: if the console loses power
    // during cleanup, the surviving A record says to finish cleanup, NOT to
    // restore from the incomplete backup.
    std::error_code ec;
    std::filesystem::remove_all(backupRoot + "/files", ec);
    if (ec) return false;
    std::filesystem::remove(journalPath(backupRoot), ec);
    if (ec) return false;
    std::filesystem::remove(backupRoot, ec);
    return !ec;
}

} // namespace switchu::install_txn
