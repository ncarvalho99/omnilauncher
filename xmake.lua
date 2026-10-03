set_project("OmniLaunch")

add_repositories("switchu-repo local-repo")
add_repositories("switch-repo https://github.com/PoloNX/switch-repo.git")

includes("toolchain/*.lua")
add_rules("mode.debug", "mode.release")

-- OmniLaunch versioning
local version = "1.0.0"
local upstream_version = "SwitchU + sLaunch"
local version_define = string.format('SWITCHU_VERSION="%s"', version)
local upstream_define = string.format('SWITCHU_UPSTREAM_VERSION="%s"', upstream_version)

set_version(version)

add_requires("libsdl", "libsdl_mixer", "libsdl_ttf", "zlib", "libwebp", "nlohmann_json", "fmt", "libcurl", "curlpp", {configs = {toolchains = "devkita64"}})
if get_config("backend") ~= "sdl2" then
    add_requires("deko3d", {configs = {toolchains = "devkita64"}})
    if is_mode("debug") then
        add_requires("imgui", {configs = {toolchains = "devkita64"}})
    end
end

option("homebrew")
    set_default(false)
    set_showmenu(true)
    set_description("Build a standalone .nro homebrew for testing")
option_end()

option("allow_no_client_key")
    set_default(false)
    set_showmenu(true)
    set_description("Allow building without official client key (unit tests only)")
option_end()

option("backend")
    set_default("deko3d")
    set_showmenu(true)
    set_description("GPU rendering backend: deko3d or sdl2")
    set_values("deko3d", "sdl2")
option_end()

option("termination_queue_test")
    set_default(false)
    set_showmenu(true)
    set_description("Enable explicit application lifecycle diagnostics")
option_end()

option("preflight_matrix_test")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the RAM-only launch-preflight rejection matrix")
option_end()

option("preflight_edge_test")
    set_default(false)
    set_showmenu(true)
    set_description("Enable sleep/wake and failed-launch preflight diagnostics")
option_end()

option("resume_failure_test")
    set_default(false)
    set_showmenu(true)
    set_description("Enable deterministic suspended-application resume failure diagnostics")
option_end()

target("nxui")
    set_kind("static")
    set_default(false)
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("c++20")

    add_files("lib/nxui/src/core/Application.cpp")
    add_files("lib/nxui/src/core/Animation.cpp")
    add_files("lib/nxui/src/core/I18n.cpp")
    add_files("lib/nxui/src/core/Input.cpp")
    add_files("lib/nxui/src/core/Theme.cpp")
    add_files("lib/nxui/src/widgets/*.cpp")
    add_files("lib/nxui/src/focus/*.cpp")
    add_files("lib/nxui/src/core/Font.cpp")
    -- Shared by both backends: the 3D quad primitive is pure geometry ending in
    -- addVertex, so a single copy keeps the deko3d and SDL2 paths identical.
    add_files("lib/nxui/src/core/Renderer_quad3d.cpp")

    if get_config("backend") == "sdl2" then
        add_defines("NXUI_BACKEND_SDL2", {public = true})
        add_files("lib/nxui/src/core/GpuDevice_sdl2.cpp")
        add_files("lib/nxui/src/core/Renderer_sdl2.cpp")
        add_files("lib/nxui/src/core/Texture_sdl2.cpp")
    else
        add_defines("NXUI_BACKEND_DEKO3D", {public = true})
        add_files("lib/nxui/src/core/GpuDevice.cpp")
        add_files("lib/nxui/src/core/Renderer.cpp")
        add_files("lib/nxui/src/core/Texture.cpp")
    end

    add_includedirs("lib/nxui/include", {public = true})
    add_includedirs("lib/nxui/include/nxui/third_party/stb")

    add_packages("libsdl", "libsdl_ttf", "libwebp")

    add_cxxflags("-frtti", "-fexceptions", {force = true})
    if get_config("backend") == "deko3d" then
        add_packages("deko3d", {links = is_mode("debug") and "deko3dd" or "deko3d"})
    end

    if is_mode("release") then
        add_cxflags("-O3", "-flto=auto", "-ffast-math", {force = true})
    end
target_end()

target("espeak-ucd")
    set_kind("static")
    set_default(false)
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("gnu11")

    add_files("lib/espeak-ng/src/ucd-tools/src/case.c")
    add_files("lib/espeak-ng/src/ucd-tools/src/categories.c")
    add_files("lib/espeak-ng/src/ucd-tools/src/ctype.c")
    add_files("lib/espeak-ng/src/ucd-tools/src/proplist.c")
    add_files("lib/espeak-ng/src/ucd-tools/src/scripts.c")
    add_files("lib/espeak-ng/src/ucd-tools/src/tostring.c")

    add_includedirs("lib/espeak-ng/src/ucd-tools/src/include", {public = true})
target_end()

target("espeak-ng")
    set_kind("static")
    set_default(false)
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("gnu11")

    add_deps("espeak-ucd")

    add_files("lib/espeak-ng/src/libespeak-ng/common.c")
    add_files("lib/espeak-ng/src/libespeak-ng/mnemonics.c")
    add_files("lib/espeak-ng/src/libespeak-ng/error.c")
    add_files("lib/espeak-ng/src/libespeak-ng/ieee80.c")
    add_files("lib/espeak-ng/src/libespeak-ng/compiledata.c")
    add_files("lib/espeak-ng/src/libespeak-ng/compiledict.c")
    add_files("lib/espeak-ng/src/libespeak-ng/dictionary.c")
    add_files("lib/espeak-ng/src/libespeak-ng/encoding.c")
    add_files("lib/espeak-ng/src/libespeak-ng/intonation.c")
    add_files("lib/espeak-ng/src/libespeak-ng/langopts.c")
    add_files("lib/espeak-ng/src/libespeak-ng/numbers.c")
    add_files("lib/espeak-ng/src/libespeak-ng/phoneme.c")
    add_files("lib/espeak-ng/src/libespeak-ng/phonemelist.c")
    add_files("lib/espeak-ng/src/libespeak-ng/readclause.c")
    add_files("lib/espeak-ng/src/libespeak-ng/setlengths.c")
    add_files("lib/espeak-ng/src/libespeak-ng/soundicon.c")
    add_files("lib/espeak-ng/src/libespeak-ng/spect.c")
    add_files("lib/espeak-ng/src/libespeak-ng/ssml.c")
    add_files("lib/espeak-ng/src/libespeak-ng/synthdata.c")
    add_files("lib/espeak-ng/src/libespeak-ng/synthesize.c")
    add_files("lib/espeak-ng/src/libespeak-ng/tr_languages.c")
    add_files("lib/espeak-ng/src/libespeak-ng/translate.c")
    add_files("lib/espeak-ng/src/libespeak-ng/translateword.c")
    add_files("lib/espeak-ng/src/libespeak-ng/voices.c")
    add_files("lib/espeak-ng/src/libespeak-ng/wavegen.c")
    add_files("lib/espeak-ng/src/libespeak-ng/speech.c")
    add_files("lib/espeak-ng/src/libespeak-ng/espeak_api.c")

    add_includedirs("projects/menu/src/core/espeak_config", {public = false})
    add_includedirs("lib/nxui/include", {public = false})
    add_includedirs("lib/espeak-ng/src/include", {public = true})
    add_includedirs("lib/espeak-ng/src/libespeak-ng", {public = false})
    add_includedirs("lib/espeak-ng/src/ucd-tools/src/include", {public = false})
    add_defines("LIBESPEAK_NG_EXPORT=1")
    add_defines("_GNU_SOURCE")
    add_defines('PATH_ESPEAK_DATA="romfs:/espeak-ng-data"')
    add_cflags("-fwrapv", "-fvisibility=hidden", {force = true})
target_end()

target("atmosphere-stratosphere")
    set_kind("phony")
    set_default(false)
    if not is_plat("cross") then return end

    on_build(function (target)
        -- libstratosphere is by far the longest step in a sysmodule build:
        -- ~20 minutes of a 21 minute CI run, against ~90 seconds for this
        -- project's own sources. It was being made serially, so hand make the
        -- core count.
        import("core.base.option")
        local jobs = option.get("jobs") or os.cpuinfo("ncpu") or 4
        os.execv("make", {"-C", "lib/Atmosphere-libs/libstratosphere",
                          "-j" .. tostring(jobs), "nx_release"})
    end)
target_end()

target("haze")
    set_kind("static")
    set_default(false)
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("c++23")

    add_includedirs("lib/haze/include", {public = true})
    add_includedirs("lib/Atmosphere-libs/libvapours/include", {public = true})

    add_files("lib/haze/source/*.cpp")
    add_cxxflags("-frtti", "-fexceptions", {force = true})
    if is_mode("release") then
        add_cxflags("-O3", "-flto=auto", {force = true})
    end
target_end()

local function generate_client_key_header(os, io, path, raise_fn)
    local key = os.getenv("SWITCHU_CLIENT_KEY")
    if not key or key == "" then
        local user_home = os.getenv("USERPROFILE") or os.getenv("HOME") or ""
        local key_file = path.join(user_home, ".switchu", "client-key")
        if os.isfile(key_file) then
            key = io.readfile(key_file)
        end
    end
    if key then
        key = key:trim()
    end

    local allow_no_key = has_config("allow_no_client_key")
    if not key or key == "" then
        if allow_no_key then
            key = ""
        else
            raise_fn("Official client key is missing! Set SWITCHU_CLIENT_KEY environment variable or create ~/.switchu/client-key (64-character hex key required).")
        end
    else
        if not key:match("^[0-9a-fA-F]+$") or #key ~= 64 then
            raise_fn("SWITCHU_CLIENT_KEY is invalid! Must be a 64-character hex string (got " .. tostring(#key) .. " chars).")
        end
        key = key:lower()
    end

    local gen_dir = path.join(os.projectdir(), "build/generated")
    if not os.isdir(gen_dir) then
        os.mkdir(gen_dir)
    end
    local header_path = path.join(gen_dir, "ClientKey.hpp")
    local content = string.format([[#pragma once

// Generated by xmake at build time. Do not commit or edit.
namespace switchu::auth {
inline constexpr const char* kClientKey = "%s";
}
]], key)
    local existing = os.isfile(header_path) and io.readfile(header_path) or ""
    if existing ~= content then
        io.writefile(header_path, content)
    end
end

target("SwitchU")
    set_kind("binary")
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("c++20")
    add_rules("switch")

    add_deps("nxui", "haze")
    add_includedirs("projects/common/include", {public = false})
    add_includedirs("projects/menu/src", {public = false})
    add_includedirs("build/generated", {public = false})
    add_includedirs("lib/haze/include", {public = false})
    add_includedirs("lib/Atmosphere-libs/libvapours/include", {public = false})
    add_files("projects/menu/src/**.cpp")
    add_packages("nlohmann_json", "fmt", "libsdl", "libsdl_mixer", "libsdl_ttf", "zlib", "libwebp", "libcurl", "curlpp")
    add_linkgroups("SDL2_ttf", "harfbuzz-subset", "harfbuzz", "freetype", "png16", "bz2", "z", {group = true})
    add_linkgroups("SDL2_mixer", "FLAC++", "FLAC", "vorbisidec", "ogg", "modplug", "opusurl", "opusfile", "opus", {group = true})
    add_linkgroups("avformat", "avcodec", "swscale", "swresample", "avutil", "dav1d", {group = true})
    add_defines("SWITCHU_HAS_VIDEO=1", "SWITCHU_HAS_NVTEGRA=1")

    if is_mode("debug") and get_config("backend") ~= "sdl2" then
        add_packages("imgui")
        add_defines("SWITCHU_DEBUG_UI")
    end

    add_cxxflags("-frtti", "-fexceptions", {force = true})
    if get_config("backend") == "deko3d" then
        add_packages("deko3d", {links = is_mode("debug") and "deko3dd" or "deko3d"})
        add_linkorders(is_mode("debug") and "deko3dd" or "deko3d", "nx")
    end
    add_syslinks("nx")

    if is_mode("release") then
        add_cxflags("-O3", "-flto=auto", "-ffast-math", {force = true})
        add_ldflags("-flto=auto", {force = true})
        -- Keep the linked ELF symbolizable so ns_debug.sh can resolve
        -- addresses from atmosphere/crash_reports. mode.release would strip
        -- it, and it only applies its defaults when the target hasn't set
        -- them. The .nso the console runs is generated from this ELF and
        -- carries no symbol or DWARF sections, so this costs nothing at
        -- runtime.
        set_symbols("debug")
        set_strip("none")
    end

    add_defines(version_define)
    add_defines(upstream_define)
    add_deps("espeak-ng")
    add_includedirs("lib/espeak-ng/src/include")
    add_syslinks("m")

    before_build(function(target)
        generate_client_key_header(os, io, path, raise)
        local source_dir = path.join(os.projectdir(), "lib/espeak-ng")
        local build_dir = path.join(os.projectdir(), "build/espeak-ng-native")
        local data_dir = path.join(build_dir, "espeak-ng-data")

        if not os.isdir(source_dir) then
            raise("eSpeak NG submodule is missing: " .. source_dir)
        end

        cprint("${color.build.target}generating${clear} eSpeak NG data")
        os.vrunv("cmake", {
            "-S", source_dir,
            "-B", build_dir,
            "-DENABLE_TESTS=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DUSE_LIBSONIC=OFF",
            "-DUSE_LIBPCAUDIO=OFF",
            "-DUSE_MBROLA=OFF",
            "-DSONIC_LIB=/usr/lib/libm.so",
            "-DSONIC_INC=/usr/include"
        })
        os.vrunv("cmake", {"--build", build_dir, "--target", "data"})

        if not os.isfile(path.join(data_dir, "phondata")) or
           not os.isfile(path.join(data_dir, "fr_dict")) then
            raise("eSpeak NG data generation did not produce required runtime files")
        end
    end)

    if has_config("homebrew") then
        add_defines("SWITCHU_HOMEBREW")
        set_values("switch.name",    "SwitchU")
        set_values("switch.author",  "PoloNX")
        set_values("switch.version", version)
        set_values("switch.romfs",   "romfs")
        set_values("switch.tid",     "0100000000001000")
        set_values("switch.json",    "SwitchU.json")
        set_values("switch.format",  "nro")
    else
        add_deps("switchu-daemon")

        add_defines("SWITCHU_MENU")
        if has_config("termination_queue_test") then
            add_defines("SWITCHU_TERMINATION_QUEUE_TEST")
        end
        if has_config("preflight_edge_test") then
            add_defines("SWITCHU_PREFLIGHT_EDGE_TEST")
        end
        if has_config("resume_failure_test") then
            add_defines("SWITCHU_RESUME_FAILURE_TEST")
        end
        set_values("switch.name",    "omnilaunch-menu")
        set_values("switch.author",  "PoloNX")
        set_values("switch.version", version)
        set_values("switch.romfs",   "romfs")
        set_values("switch.json",    "projects/menu/menu.json")
        set_values("switch.format",  "nsp")
        set_values("switch.install_contents", false)
        set_values("switch.assets_dir", "OmniLaunch")
        set_values("switch.raw_exefs_dir", "switch/OmniLaunch/bin/menu")
    end
target_end()

target("SwitchU-Manager")
    set_kind("binary")
    if not is_plat("cross") then return end

    set_toolchains("devkita64")
    set_languages("c++20")
    add_rules("switch")

    add_deps("nxui")
    add_includedirs("projects/common/include", {public = false})
    add_includedirs("projects/menu/src", {public = false})
    add_includedirs("projects/manager/src", {public = false})
    add_includedirs("build/generated", {public = false})
    add_files("projects/manager/src/**.cpp")
    add_files("projects/menu/src/widgets/ActionButton.cpp")
    add_files("projects/menu/src/widgets/OverlayDialog.cpp")
    add_files("projects/menu/src/widgets/SelectionCursor.cpp")
    add_packages("libsdl", "libsdl_ttf", "zlib", "libwebp", "libcurl", "nlohmann_json")
    add_links("minizip")
    add_linkgroups("SDL2_ttf", "harfbuzz-subset", "harfbuzz", "freetype", "png16", "bz2", "z", {group = true})

    add_cxxflags("-frtti", "-fexceptions", {force = true})
    if get_config("backend") == "deko3d" then
        add_packages("deko3d", {links = is_mode("debug") and "deko3dd" or "deko3d"})
        add_linkorders(is_mode("debug") and "deko3dd" or "deko3d", "nx")
    end
    add_syslinks("nx")
    add_defines(version_define)

    if is_mode("release") then
        add_cxflags("-O3", "-flto=auto", "-ffast-math", {force = true})
        add_ldflags("-flto=auto", {force = true})
    end

    before_build(function(target)
        generate_client_key_header(os, io, path, raise)
        local romfs_dir = path.join(os.projectdir(), "build/manager-romfs")
        local font_dir = path.join(romfs_dir, "fonts")
        local i18n_dir = path.join(romfs_dir, "i18n")
        os.mkdir(font_dir)
        os.mkdir(i18n_dir)
        os.cp(path.join(os.projectdir(), "romfs/fonts/DejaVuSans.ttf"), font_dir)
        os.cp(path.join(os.projectdir(), "projects/manager/romfs/i18n/*.json"), i18n_dir)
    end)

    set_values("switch.name",    "OmniLaunch-Manager")
    set_values("switch.author",  "PoloNX")
    set_values("switch.version", version)
    set_values("switch.romfs",   "build/manager-romfs")
    set_values("switch.icon",    "projects/manager/icon.jpg")
    set_values("switch.format",  "nro")
target_end()

target("switchu-daemon")
    set_kind("binary")
    add_defines(version_define)
    if not is_plat("cross") then return end
    set_default(not has_config("homebrew"))

    set_toolchains("devkita64")
    set_languages("c++20")
    add_rules("switch")

    add_deps("atmosphere-stratosphere")

    add_files("projects/daemon/src/**.cpp")
    -- The daemon applies launcher updates at boot, which needs the same audited
    -- extractor the menu uses. Shared rather than copied: two implementations of
    -- unpacking an archive onto the card is one too many.
    add_files("projects/menu/src/themeshop/ZipReader.cpp")
    add_includedirs("projects/menu/src", {public = false})

    add_includedirs("projects/common/include", {public = false})
    add_includedirs("lib/Atmosphere-libs/libstratosphere/include")
    add_includedirs("lib/Atmosphere-libs/libvapours/include")

    add_defines(
        "ATMOSPHERE",
        "ATMOSPHERE_IS_STRATOSPHERE",
        "ATMOSPHERE_OS_HORIZON",
        "ATMOSPHERE_BOARD_NINTENDO_NX",
        "ATMOSPHERE_ARCH_ARM64",
        "ATMOSPHERE_ARCH_ARM_V8A",
        "_GNU_SOURCE"
    )
    if has_config("termination_queue_test") then
        add_defines("SWITCHU_TERMINATION_QUEUE_TEST")
    end
    if has_config("preflight_matrix_test") then
        add_defines("SWITCHU_PREFLIGHT_MATRIX_TEST")
    end
    if has_config("preflight_edge_test") then
        add_defines("SWITCHU_PREFLIGHT_EDGE_TEST")
    end
    if has_config("resume_failure_test") then
        add_defines("SWITCHU_RESUME_FAILURE_TEST")
    end
    add_cxxflags("-fno-rtti", "-fexceptions", "-std=gnu++23", {force = true})
    add_packages("zlib")
    add_linkdirs("lib/Atmosphere-libs/libstratosphere/lib/nintendo_nx_arm64_armv8a/release")
    add_links("stratosphere")
    add_syslinks("nx")

    if is_mode("release") then
        add_cxflags("-O3", "-flto=auto", "-ffast-math", {force = true})
        add_ldflags("-flto=auto", {force = true})
        -- See the note on the SwitchU target: keep the ELF symbolizable for
        -- crash report resolution without affecting the generated .nso.
        set_symbols("debug")
        set_strip("none")
    end

    set_values("switch.name",    "switchu-daemon")
    set_values("switch.author",  "PoloNX")
    set_values("switch.version", version)
    set_values("switch.tid",     "0100000000001000")
    set_values("switch.json",    "projects/daemon/daemon.json")
    set_values("switch.format",  "nsp")
target_end()
