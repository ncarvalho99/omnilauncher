// End-to-end host test: the real ZipReader + install_txn against a real zip.
// Usage: update_rollback_e2e_test <zip> <scratch dir>
// The zip holds atmosphere/a.bin, switch/b.bin and switch/new.bin.
#include "themeshop/ZipReader.hpp"
#include <switchu/install_txn.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace tx = switchu::install_txn;

static void put(const std::string& p, const std::string& b) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << b;
}
static std::string get(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string zip = argv[1];
    const std::string base = argv[2];
    const std::string root = base + "/sd/";
    const std::string bak = base + "/bak";
    themeshop::ZipExtractPolicy pol;
    pol.allowExecutablePayload = true;
    pol.requiredRoots = {"atmosphere/", "switch/"};

    // A: clean apply, then the new menu "keeps failing": whole update rolled back.
    fs::remove_all(base);
    put(root + "atmosphere/a.bin", "old-a");
    put(root + "switch/b.bin", "old-b");
    {
        tx::Txn t(bak, root);
        assert(t.begin());
        pol.txn = &t;
        const auto r = themeshop::extractZipFile(zip, root, {}, pol);
        assert(r.success && t.completePass());
    }
    assert(get(root + "atmosphere/a.bin") == "new-a" && get(root + "switch/b.bin") == "new-b");
    assert(get(root + "switch/new.bin") == "new-new");
    assert(tx::pending(bak) && !tx::interrupted(bak));
    assert(tx::rollback(bak, root));
    assert(get(root + "atmosphere/a.bin") == "old-a" && get(root + "switch/b.bin") == "old-b");
    assert(!fs::exists(root + "switch/new.bin") && !fs::exists(bak));

    // B: one file cannot be replaced (a non-empty directory sits on its name, so
    // the rename onto it fails). The pass fails; rollback restores every file the
    // pass had already replaced and removes the ones it created. The zip's central
    // directory order decides which files are touched first, so only the outcome
    // is asserted, not the order.
    fs::remove_all(base);
    put(root + "atmosphere/a.bin", "old-a");
    put(root + "switch/b.bin/keep", "x");
    {
        tx::Txn t(bak, root);
        assert(t.begin());
        pol.txn = &t;
        const auto r = themeshop::extractZipFile(zip, root, {}, pol);
        std::printf("B: success=%d error=[%s] files=%d\n", (int)r.success, r.error.c_str(),
                    r.filesWritten);
        assert(!r.success);
    }
    assert(tx::interrupted(bak));
    assert(tx::rollback(bak, root));
    assert(get(root + "atmosphere/a.bin") == "old-a");
    assert(fs::is_directory(root + "switch/b.bin") && get(root + "switch/b.bin/keep") == "x");
    assert(!fs::exists(root + "switch/new.bin"));
    for (const auto& e : fs::recursive_directory_iterator(root))
        assert(e.path().extension() != ".part");

    // C: without a transaction the extractor behaves as before (theme installs).
    fs::remove_all(base);
    put(root + "atmosphere/a.bin", "old-a");
    pol.txn = nullptr;
    const auto r = themeshop::extractZipFile(zip, root, {}, pol);
    assert(r.success && get(root + "atmosphere/a.bin") == "new-a");

    fs::remove_all(base);
    std::puts("ZIP_TXN_E2E_PASS");
    return 0;
}
