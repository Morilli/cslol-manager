#ifdef _WIN32
#    include <algorithm>
#    include <array>
#    include <chrono>
#    include <fstream>
#    include <lol/error.hpp>
#    include <lol/patcher/patcher.hpp>
#    include <thread>

// do not reorder
#    include "utility/lineconfig.hpp"
#    include "utility/ppp.hpp"
#    include "utility/process.hpp"

using namespace lol;
using namespace lol::patcher;
using namespace std::chrono_literals;

// clang-format off
constexpr inline char PAT_REVISION[] = "patcher-win64-v3";
constexpr auto const find_open =
    &ppp::any<"C7 44 24 20 03 00 00 00 "
              "45 8D 41 01 "
              "FF 15 r[?? ?? ?? ??]"_pattern,
              "BA 89 00 12 00 "
              "89 7C 24 ?? "
              "48 8B C8 4C "
              "89 ?? 24 ?? ?? ?? ?? "
              "FF 15 r[?? ?? ?? ??]"_pattern>;
constexpr auto const find_ret =
    &ppp::any<"B9 A0 02 00 00 "
              "48 89 ?? ?? ?? "
              "E8 ?? ?? ?? ?? "
              "o[??]"_pattern>;
constexpr auto const find_wopen =
    &ppp::any<"C7 45 ?? 18 00 00 00 "
              "48 89 75 ?? "
              "89 45 ?? 4C "
              "89 75 ?? "
              "FF 15 r[?? ?? ?? ??]"_pattern>;
constexpr auto const find_alloc =
    &ppp::any<"48 83 C4 28 "
              "C3 "
              "C7 05 r[?? ?? ?? ??] 00 00 00 00 "
              "48 83 C4 28 "
              "E9 r[?? ?? ?? ??]"_pattern>;
// clang-format on

struct ImportTrampoline {
    uint8_t data[64] = {};

    static ImportTrampoline make(Ptr<uint8_t> where) {
        ImportTrampoline result = {{0x48, 0xB8u, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0}};
        memcpy(result.data + 2, &where, sizeof(where));
        return result;
    }
};

struct CodePayload {
    // Pointers to be consumed by shellcode
    Ptr<uint8_t> org_open_ptr = {};
    Ptr<char16_t> prefix_open_ptr = {};
    Ptr<uint8_t> wopen_ptr = {};
    Ptr<uint8_t> org_alloc_ptr = {};
    Ptr<uint8_t> find_ret_addr = {};
    Ptr<uint8_t> hook_ret_addr = {};

    // clang-format off
    // Actual data and shellcode storage
    uint8_t hook_open_data[0xC0] = {
        0xc8, 0x00, 0x10, 0x00, 0x53, 0x57, 0x56, 0x48,
        0x83, 0xec, 0x48, 0x4c, 0x89, 0x4d, 0x28, 0x4c,
        0x89, 0x45, 0x20, 0x48, 0x89, 0x55, 0x18, 0x48,
        0x89, 0x4d, 0x10, 0xe8, 0x00, 0x00, 0x00, 0x00,
        0x5b, 0x48, 0x81, 0xe3, 0x00, 0xf0, 0xff, 0xff,
        0x48, 0x8d, 0xbd, 0x00, 0xf0, 0xff, 0xff, 0x48,
        0x8b, 0x73, 0x08, 0x66, 0xad, 0x66, 0xab, 0x66,
        0x85, 0xc0, 0x75, 0xf7, 0x48, 0x83, 0xef, 0x02,
        0x48, 0x8b, 0x75, 0x10, 0xb4, 0x00, 0xac, 0x3c,
        0x2f, 0x75, 0x02, 0xb0, 0x5c, 0x66, 0xab, 0x84,
        0xc0, 0x75, 0xf3, 0x48, 0x8b, 0x45, 0x48, 0x48,
        0x89, 0x44, 0x24, 0x30, 0x48, 0x8b, 0x45, 0x38,
        0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8b, 0x45,
        0x30, 0x48, 0x89, 0x44, 0x24, 0x20, 0x4c, 0x8b,
        0x4d, 0x28, 0x4c, 0x8b, 0x45, 0x20, 0x48, 0x8b,
        0x55, 0x18, 0x48, 0x8d, 0x8d, 0x00, 0xf0, 0xff,
        0xff, 0xff, 0x53, 0x10, 0x48, 0x83, 0xf8, 0xff,
        0x75, 0x2d, 0x48, 0x8b, 0x45, 0x48, 0x48, 0x89,
        0x44, 0x24, 0x30, 0x48, 0x8b, 0x45, 0x38, 0x48,
        0x89, 0x44, 0x24, 0x28, 0x48, 0x8b, 0x45, 0x30,
        0x48, 0x89, 0x44, 0x24, 0x20, 0x4c, 0x8b, 0x4d,
        0x28, 0x4c, 0x8b, 0x45, 0x20, 0x48, 0x8b, 0x55,
        0x18, 0x48, 0x8b, 0x4d, 0x10, 0xff, 0x13, 0x48,
        0x83, 0xc4, 0x48, 0x5e, 0x5f, 0x5b, 0xc9, 0xc3
    };
    uint8_t hook_alloc_data[0x40] = {
        0xc8, 0x00, 0x00, 0x00, 0x53, 0x56, 0xe8, 0x00,
        0x00, 0x00, 0x00, 0x5b, 0x48, 0x81, 0xe3, 0x00,
        0xf0, 0xff, 0xff, 0x48, 0x8b, 0x73, 0x20, 0x48,
        0x89, 0xe8, 0x48, 0x05, 0x80, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xe8, 0x08, 0x48, 0x39, 0xe8, 0x74,
        0x0c, 0x48, 0x3b, 0x30, 0x75, 0xf2, 0x48, 0x8b,
        0x73, 0x28, 0x48, 0x89, 0x30, 0x48, 0x8b, 0x43,
        0x18, 0x5e, 0x5b, 0xc9, 0xff, 0xe0
    };
    uint8_t hook_ret_data[0x40] = {
        0xe8, 0x00, 0x00, 0x00, 0x00, 0x58, 0x48, 0x25,
        0x00, 0xf0, 0xff, 0xff, 0x48, 0x8b, 0x40, 0x20,
        0x50, 0x48, 0x31, 0xc0, 0x48, 0xff, 0xc0, 0xc3
    };
    // clang-format on

    ImportTrampoline org_open_data = {};
    char16_t prefix_open_data[0x400] = {};
};

struct Context {
    LineConfig<std::uint64_t,
               PAT_REVISION,
               "checksum",
               "open",
               "wopen",
               "ret",
               "alloc_ptr",
               "alloc_fn">
        config;
    std::string config_str;
    std::u16string prefix;

    auto load_config(fs::path const& path) -> void {
        if (std::ifstream file(path, std::ios::binary); file) {
            if (auto str = std::string{}; std::getline(file, str)) {
                config.from_string(str);
            }
        }
        config_str = config.to_string();
    }

    auto save_config(fs::path const& path) -> void {
        config_str = config.to_string();
        auto ec = std::error_code{};
        fs::create_directories(path.parent_path(), ec);
        if (std::ofstream file(path, std::ios::binary); file) {
            file.write(config_str.data(), config_str.size());
        }
    }

    auto set_prefix(fs::path const& profile_path) -> void {
        prefix = fs::absolute(profile_path.lexically_normal()).generic_u16string();
        for (auto& c : prefix) {
            if (c == u'/') {
                c = u'\\';
            }
        }
        if (!prefix.starts_with(u"\\\\")) {
            prefix = u"\\\\?\\" + prefix;
        }
        if (!prefix.ends_with(u"\\")) {
            prefix.push_back(u'\\');
        }
        if ((prefix.size() + 1) * sizeof(char16_t) >= sizeof(CodePayload::prefix_open_data)) {
            lol_throw_msg("Prefix path too big!");
        }
    }

    auto scan(Process const& process) -> void {
        lol_trace_func();
        auto const base = process.Base();
        auto const data = process.Dump();
        auto const data_span = std::span<char const>(data);

        auto const open_match = find_open(data_span, base);
        lol_throw_if_msg(!open_match, "Failed to find fopen!");

        auto const wopen_match = find_wopen(data_span, base);
        lol_throw_if_msg(!wopen_match, "Failed to find wfopen!");

        auto const ret_match = find_ret(data_span, base);
        lol_throw_if_msg(!ret_match, "Failed to find ret!");

        auto const alloc_match = find_alloc(data_span, base);
        lol_throw_if_msg(!alloc_match, "Failed to find alloc!");

        config.get<"open">() = process.Debase((PtrStorage)std::get<1>(*open_match));
        config.get<"wopen">() = process.Debase((PtrStorage)std::get<1>(*wopen_match));
        config.get<"ret">() = process.Debase((PtrStorage)std::get<1>(*ret_match));
        config.get<"alloc_ptr">() = process.Debase((PtrStorage)std::get<1>(*alloc_match) + 8);
        config.get<"alloc_fn">() = process.Debase((PtrStorage)std::get<2>(*alloc_match));
        config.get<"checksum">() = process.Checksum();
    }

    auto check(Process const& process) const -> bool {
        return config.check() && process.Checksum() == config.get<"checksum">();
    }

    auto is_patchable(const Process& process) const noexcept -> bool {
        auto is_valid_ptr = [](PtrStorage ptr) { return ptr > 0x10000 && ptr < (1ull << 48); };

        auto const alloc_ptr = process.Rebase<PtrStorage>(config.get<"alloc_ptr">());
        if (auto result = process.TryRead(alloc_ptr); !result || !is_valid_ptr(*result)) {
            return false;
        }

        auto const open = process.Rebase<PtrStorage>(config.get<"open">());
        if (auto result = process.TryRead(open); !result || !is_valid_ptr(*result)) {
            return false;
        }

        auto const wopen = process.Rebase<PtrStorage>(config.get<"wopen">());
        if (auto result = process.TryRead(wopen); !result || !is_valid_ptr(*result)) {
            return false;
        }
        return true;
    }

    auto patch(Process const& process) const -> void {
        lol_trace_func();
        lol_throw_if_msg(!config.check(), "Config invalid");

        // Prepare pointers
        auto mod_code = process.Allocate<CodePayload>();
        auto ptr_org_open = Ptr<Ptr<ImportTrampoline>>(process.Rebase(config.get<"open">()));
        auto ptr_wopen = Ptr<Ptr<uint8_t>>(process.Rebase(config.get<"wopen">()));
        auto find_ret_addr = process.Rebase(config.get<"ret">());
        auto ptr_alloc = Ptr<Ptr<uint8_t>>(process.Rebase(config.get<"alloc_ptr">()));
        auto org_alloc_ptr = Ptr<uint8_t>(process.Rebase(config.get<"alloc_fn">()));

        // Read pointers to pointers
        auto org_open = process.Read(ptr_org_open);
        auto wopen = process.Read(ptr_wopen);

        // Prepare payload
        auto payload = CodePayload{};
        payload.org_open_ptr = Ptr(mod_code->org_open_data.data);
        payload.prefix_open_ptr = Ptr(mod_code->prefix_open_data);
        payload.wopen_ptr = wopen;
        payload.org_alloc_ptr = org_alloc_ptr;
        payload.find_ret_addr = find_ret_addr;
        payload.hook_ret_addr = Ptr(mod_code->hook_ret_data);
        payload.org_open_data = process.Read(org_open);
        std::copy_n(prefix.data(), prefix.size(), payload.prefix_open_data);

        // Write payload
        process.Write(mod_code, payload);
        process.MarkExecutable(mod_code);

        // Write hooks
        process.Write(ptr_alloc, Ptr(mod_code->hook_alloc_data));
        process.Write(org_open, ImportTrampoline::make(mod_code->hook_open_data));
    }

    auto verify_path(Process const& process, fs::path game_path) -> void {
        if (game_path.empty()) return;
        auto process_path = fs::absolute(process.Path().parent_path());
        game_path = fs::absolute(game_path);
        lol_trace_func(lol_trace_var("{}", process_path), lol_trace_var("{}", game_path));
        lol_throw_if_msg(game_path != process_path, "Wrong game directory!");
    }
};

static bool skinhack_detected() {
    std::error_code ec = {};
    return fs::exists("C:/Fraps/LOLPRO.exe", ec);
}

[[noreturn]] static void newpatch_detected() {
    lol_trace_func("Skipping first game on a new patch for config...");
    lol_trace_func("Patching should work normally next game!");
    lol_throw_msg("NEW PATCH DETECTED, THIS IS NOT AN ERROR!\n");
}

auto patcher::run(std::function<bool(Message, char const*)> update,
                  fs::path const& profile_path,
                  fs::path const& config_path,
                  fs::path const& game_path) -> void {
    lol_throw_if(skinhack_detected());
    auto ctx = Context{};
    ctx.set_prefix(profile_path);
    ctx.load_config(config_path);
    (void)game_path;
    for (;;) {
        auto process = Process::Find("League of Legends.exe", "League of Legends (TM) Client");
        if (!process) {
            if (!update(M_WAIT_START, "")) return;
            std::this_thread::sleep_for(250ms);
            continue;
        }

        ctx.verify_path(*process, game_path);

        if (!update(M_FOUND, "")) return;

        if (!ctx.check(*process)) {
            for (std::uint32_t timeout = 3 * 60 * 1000; timeout; timeout -= 1) {
                if (!update(M_WAIT_INIT, "")) return;
                if (process->WaitInitialized(1)) {
                    break;
                }
            }

            if (!update(M_SCAN, "")) return;
            ctx.scan(*process);

            if (!update(M_NEED_SAVE, "")) return;
            ctx.save_config(config_path);

            // newpatch_detected();
        } else {
            for (std::uint32_t timeout = 3 * 60 * 1000; timeout; timeout -= 1) {
                if (!update(M_WAIT_PATCHABLE, "")) return;
                if (ctx.is_patchable(*process)) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
        }

        if (!update(M_PATCH, ctx.config_str.c_str())) return;
        ctx.patch(*process);

        for (std::uint32_t timeout = 3 * 60 * 60 * 1000; timeout; timeout -= 250) {
            if (!update(M_WAIT_EXIT, "")) return;
            if (process->WaitExit(250)) {
                break;
            }
        }

        if (!update(M_DONE, "")) return;
    }
}

#endif
