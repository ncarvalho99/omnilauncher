#include "switchu/install_txn.hpp"

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace switchu::install_txn;

static void put(const std::string& path, const std::string& body) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << body;
}
static std::string get(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static bool has(const std::string& p) { return fs::exists(p); }

// What the extractor does for one file: new bytes already sit at <target>.part.
static bool install(Txn& t, const std::string& root, const std::string& rel,
                    const std::string& body, bool failRename = false) {
    const std::string target = root + rel;
    put(target + ".part", body);
    if (!t.prepareReplace(rel)) {
        std::remove((target + ".part").c_str());
        return false;
    }
    if (failRename || std::rename((target + ".part").c_str(), target.c_str()) != 0) {
        t.undoReplace(rel);
        std::remove((target + ".part").c_str());
        return false;
    }
    return true;
}

int main() {
    const std::string base = (fs::temp_directory_path() / "install_txn_test").string();
    fs::remove_all(base);
    const std::string root = base + "/sd/";
    const std::string bak = base + "/backup";

    // 1. Successful pass replaces and creates; commit drops the backup.
    {
        put(root + "switch/a.nro", "old-a");
        put(root + "switch/b.nro", "old-b");
        Txn t(bak, root);
        assert(t.begin());
        assert(install(t, root, "switch/a.nro", "new-a"));
        assert(install(t, root, "switch/c.nro", "new-c"));      // did not exist
        assert(t.completePass());
        assert(!interrupted(bak) && pending(bak));
        assert(get(root + "switch/a.nro") == "new-a" && get(root + "switch/c.nro") == "new-c");
        assert(commit(bak) && !has(bak));
    }
    // 2. Failure in the middle: rollback restores everything, removes created files.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "old-a");
        put(root + "atmosphere/x.nsp", "old-x");
        {
            Txn t(bak, root);
            assert(t.begin());
            assert(install(t, root, "switch/a.nro", "new-a"));
            assert(install(t, root, "switch/new.nro", "new-n"));
            assert(install(t, root, "atmosphere/x.nsp", "new-x"));
            assert(!install(t, root, "switch/boom.nro", "new-b", /*failRename=*/true));
            // no completePass(): the pass failed
        }
        assert(interrupted(bak));
        std::string failed;
        assert(rollback(bak, root, &failed) && failed.empty());
        assert(get(root + "switch/a.nro") == "old-a");
        assert(get(root + "atmosphere/x.nsp") == "old-x");
        assert(!has(root + "switch/new.nro") && !has(root + "switch/boom.nro"));
        assert(!has(bak));
        assert(!has(root + "switch/boom.nro.part"));
    }
    // 3. Interrupted rollback is simply run again and finishes the job.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "old-a");
        put(root + "switch/b.nro", "old-b");
        {
            Txn t(bak, root);
            assert(t.begin());
            assert(install(t, root, "switch/a.nro", "new-a"));
            assert(install(t, root, "switch/b.nro", "new-b"));
        }
        // Simulate a power cut after b was restored but before a was:
        // restore b by hand, leave the journal and a's backup in place.
        fs::remove(root + "switch/b.nro");
        fs::copy_file(backupPath(bak, "switch/b.nro"), root + "switch/b.nro");
        assert(rollback(bak, root));
        assert(get(root + "switch/a.nro") == "old-a" && get(root + "switch/b.nro") == "old-b");
        assert(rollback(bak, root));                              // idempotent: nothing left
    }
    // 4. A crash BETWEEN setting the old file aside and renaming the new one in:
    //    the target is absent, the original is in the backup; rollback restores it.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "old-a");
        {
            Txn t(bak, root);
            assert(t.begin());
            put(root + "switch/a.nro.part", "new-a");
            assert(t.prepareReplace("switch/a.nro"));
            assert(get(root + "switch/a.nro") == "old-a");       // power cut here: old file still installed
        }
        assert(interrupted(bak));
        assert(rollback(bak, root));
        assert(get(root + "switch/a.nro") == "old-a");
    }
    // 5. Two passes over the same file: rollback returns to the state before the FIRST.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "orig");
        {
            Txn t(bak, root);
            assert(t.begin());
            assert(install(t, root, "switch/a.nro", "daemon-half"));
            assert(t.completePass());                             // pass 1: the menu's daemon half
        }
        {
            Txn t2(bak, root);                                    // pass 2: the daemon at the next boot
            assert(t2.begin());
            assert(install(t2, root, "switch/a.nro", "full-pass"));
            // second pass never completes
        }
        assert(interrupted(bak));
        assert(rollback(bak, root));
        assert(get(root + "switch/a.nro") == "orig");
    }
    // 6. A completed pass stays restorable until the update is accepted.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "old-a");
        {
            Txn t(bak, root);
            assert(t.begin());
            assert(install(t, root, "switch/a.nro", "new-a"));
            assert(t.completePass());
        }
        assert(!interrupted(bak) && pending(bak));
        assert(rollback(bak, root));                              // e.g. new menu never healthy
        assert(get(root + "switch/a.nro") == "old-a" && !has(bak));
    }
    // 7. Truncated journal line is ignored, not misread.
    {
        fs::remove_all(base);
        put(root + "switch/a.nro", "old-a");
        fs::create_directories(bak + "/files");
        std::ofstream(journalPath(bak), std::ios::binary) << "R\tswitch/a.nro";   // no newline
        assert(interrupted(bak));
        std::vector<Record> r;
        assert(readJournal(bak, r) && r.empty());
        assert(rollback(bak, root));
        assert(get(root + "switch/a.nro") == "old-a");
    }
    // 9. A directory where a file belongs is refused, and nothing is journalled for it.
    {
        fs::remove_all(base);
        fs::create_directories(root + "switch/dir.nro");
        put(root + "switch/dir.nro/keep", "x");
        Txn t(bak, root);
        assert(t.begin());
        assert(!install(t, root, "switch/dir.nro", "new"));
        assert(fs::is_directory(root + "switch/dir.nro") && get(root + "switch/dir.nro/keep") == "x");
        assert(!has(root + "switch/dir.nro.part"));
    }
    // 8. No journal: rollback and interrupted() are no-ops.
    {
        fs::remove_all(base);
        assert(!interrupted(bak) && !pending(bak) && rollback(bak, root));
    }
    fs::remove_all(base);
    std::puts("INSTALL_TXN_TEST_PASS");
    return 0;
}
