#pragma once

#include "ThemeTransferState.hpp"

#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace nxui {
class ThreadPool;
}

class ThemeCatalogClient {
public:
    struct Entry {
        std::string id;
        std::string name;
        std::string author;
        std::string version;
        std::string path;
        std::string manifest;
        std::string cover;
        // Um zip com o tema inteiro, quando o catalogo oferece. Vazio faz o
        // instalador voltar a baixar arquivo por arquivo, que e o que todo
        // catalogo publicado antes deste campo continua servindo.
        std::string package;
        std::uint64_t packageBytes = 0;
        // O que o tema ocupa depois de descompactado. Os dois numeros ficam
        // longe um do outro num tema -- os quadros comprimem muito -- e quem
        // decide se cabe no cartao precisa do que fica, nao do que trafega.
        // So o catalogo sabe: o console teria de baixar o pacote para
        // descobrir, que e justamente a decisao em questao.
        std::uint64_t installedBytes = 0;
        // Miniatura animada: uma folha de sprites com os quadros em grade.
        // Vazio faz o cartao usar a capa parada, que e o que todo catalogo
        // sem estes campos continua servindo.
        std::string thumbSheet;
        // A mesma animacao em celulas maiores, para quando o tema e expandido.
        std::string thumbSheetHd;
        int thumbCols = 0;
        int thumbRows = 0;
        float thumbFps = 10.f;
        std::vector<std::string> screenshots;
        // Which index this came from. Entries carry relative paths, so once two
        // catalogues are merged the resolving base can no longer be a single
        // property of the client.
        std::string catalogUrl;
    };

    struct Snapshot {
        ThemeTransferState loading;
        std::vector<Entry> entries;
        std::uint64_t revision = 0;
    };

    // Our own catalogue. PoloNX's still exists and this is not a replacement of
    // his work — but his index describes themes built for his build, and the
    // shop has to point somewhere we can add to. Themes download on demand, so
    // none of this is in the package.
    // Off the code repository on purpose. Content complaints act on whole
    // repositories, so a catalog served from the same one as the source puts
    // the code behind a claim about a wallpaper. Mirrored at
    // github.com/ncarvalho99/SwitchU-themes.
    //
    // Every path in the catalog resolves against the directory this URL sits
    // in, so the tree works the same served from a domain root or a subfolder.
    static constexpr const char* kDefaultCatalogUrl = "https://themes.nclabs.dev/omnilaunch.json";
    static constexpr const char* kFallbackCatalogUrl = "https://themes.nclabs.dev/index.json";

    // PoloNX's catalogue is read alongside ours rather than replaced by it: his
    // themes work here, and there is no reason to offer fewer of them. Ours is
    // listed first, so an id present in both resolves to ours.
    static constexpr const char* kUpstreamCatalogUrl = "https://raw.githubusercontent.com/PoloNX/SwitchU-Themes/main/index.json";

    explicit ThemeCatalogClient(std::string catalogUrl = kDefaultCatalogUrl);

    void setCatalogUrl(std::string catalogUrl);
    void setCatalogUrls(std::vector<std::string> urls);
    const std::string& catalogUrl() const {
        return m_catalogUrls.empty() ? m_emptyUrl : m_catalogUrls.front();
    }

    void refresh(nxui::ThreadPool& pool);
    bool poll();
    Snapshot snapshot() const;

private:
    static Snapshot loadCatalog(const std::string& url);

    std::vector<std::string> m_catalogUrls;
    std::string m_emptyUrl;
    mutable std::mutex m_mutex;
    ThemeTransferState m_loading;
    std::vector<Entry> m_entries;
    std::future<void> m_future;
    std::uint64_t m_revision = 0;
};
