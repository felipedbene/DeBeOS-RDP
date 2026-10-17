// The automatic font fetch (font_fetch.hpp): the guest paths it will and will
// not ask for, the remote command (pinned as a golden string), the stream that
// command prints, the 'head' check that decides whether a file is the one the
// server named, the fetch itself -- through a scripted runner, and through a
// fake `ssh` on PATH that runs the client's real remote command with sh against
// a fake guest root -- and the session's half: noticing a font that is not
// here, and asking for a repaint once it is.
//
// The oracles are independent of the code under test: FreeType reads the
// 'head' fields of every real file used here (FT_Get_Sfnt_Table), synthetic
// sfnt bytes are spelled out by hand, base64 is encoded here by a separate
// implementation, and the remote command is a literal.

#include "haiku_remote/font_fetch.hpp"
#include "haiku_remote/protocol.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/text_engine.hpp"
#include "haiku_remote/wire_decoder.hpp"

#include "font_fixture.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace haiku_remote;
using namespace font_fixture;
namespace fs = std::filesystem;

namespace {

int checks = 0;
int failures = 0;
int skips = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void skip(std::string_view message, std::string_view reason)
{
    ++skips;
    std::cout << "SKIP: " << message << " (" << reason << ")\n";
}

fs::path scratch(const std::string& name)
{
    static const fs::path root = [] {
        fs::path dir = fs::temp_directory_path()
            / ("haiku-remote-font-fetch-tests-"
               + std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
        fs::create_directories(dir);
        return dir;
    }();
    const fs::path dir = root / name;
    fs::create_directories(dir);
    return dir;
}

// RFC 4648 with 76-column lines, the way coreutils base64 prints it. A second
// implementation, so the decoder under test is not checked against itself.
std::string base64_lines(std::string_view bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string flat;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16)
            | (static_cast<unsigned char>(bytes[i + 1]) << 8)
            | static_cast<unsigned char>(bytes[i + 2]);
        flat += alphabet[(v >> 18) & 63];
        flat += alphabet[(v >> 12) & 63];
        flat += alphabet[(v >> 6) & 63];
        flat += alphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        flat += alphabet[(v >> 18) & 63];
        flat += alphabet[(v >> 12) & 63];
        flat += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16)
            | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        flat += alphabet[(v >> 18) & 63];
        flat += alphabet[(v >> 12) & 63];
        flat += alphabet[(v >> 6) & 63];
        flat += '=';
    }
    std::string out;
    for (std::size_t at = 0; at < flat.size(); at += 76)
        out += flat.substr(at, 76) + "\n";
    return out;
}

class ScriptedRunner : public CommandRunner {
public:
    std::string stdout_text;
    int exit_code = 0;
    int calls = 0;
    std::vector<std::string> last_argv;

    int run(const std::vector<std::string>& argv, std::string& out,
            std::string& error) override
    {
        ++calls;
        last_argv = argv;
        out = stdout_text;
        if (exit_code != 0)
            error = "scripted failure";
        return exit_code;
    }
};

SshConfig guest_ssh()
{
    SshConfig ssh;
    ssh.user = "baron";
    ssh.host = "guest.example";
    ssh.port = 2222;
    ssh.identity_file = "/keys/id_guest";
    ssh.known_hosts_file = "/keys/known_hosts";
    return ssh;
}

// -- the path filter --------------------------------------------------------

void test_guest_paths_that_are_fetched_and_refused()
{
    for (const char* good : {
             "/boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf",
             "/boot/system/data/fonts/otfonts/NotoSansCJK-Regular.ttc",
             "/boot/home/config/non-packaged/data/fonts/My Font [wght].otf",
             "/boot/home/config/data/fonts/UPPER.TTF",
             "/boot/x.otc",
         }) {
        check(guest_font_path_is_safe(good),
              std::string("a guest font path is fetched: ") + good);
    }
    for (const char* bad : {
             "", "NotoSans.ttf", "boot/x.ttf", "/etc/passwd", "/etc/fonts/x.ttf",
             "/boot/../etc/x.ttf", "/boot/a/../b.ttf", "/boot/a/./b.ttf",
             "/boot//x.ttf", "/boot/x.ttf/", "/boot/x'y.ttf", "/boot/x\\y.ttf",
             "/boot/x\ny.ttf", "/boot/x\ty.ttf", "/boot/home/.ssh/id_ed25519",
             "/boot/home/config/settings/remote_desktop/token",
             "/boot/system/data/fonts/psfonts/x.pfb", "/boot/x.ttf.sh",
         }) {
        check(!guest_font_path_is_safe(bad),
              std::string("a path that is not a guest font is refused: ") + bad);
    }
    check(!guest_font_path_is_safe("/boot/" + std::string(1100, 'a') + ".ttf"),
          "an over-long path is refused");
}

// -- the remote command -----------------------------------------------------

void test_the_remote_command_is_pinned()
{
    // Golden: what goes to the guest's sh, character for character.
    const std::string want =
        "for f in '/boot/a b.ttf' '/boot/c.otf'; do "
        "if [ -f \"$f\" ] && [ -r \"$f\" ]; then "
        "s=$(wc -c < \"$f\"); s=$((s + 0)); "
        "if [ \"$s\" -le 1000 ]; then "
        "printf '@@ ok %s %s\\n' \"$s\" \"$f\"; base64 < \"$f\"; printf '@@ end\\n'; "
        "else printf '@@ too-large %s %s\\n' \"$s\" \"$f\"; fi; "
        "else printf '@@ missing %s\\n' \"$f\"; fi; "
        "done";
    check(build_font_fetch_command({"/boot/a b.ttf", "/boot/c.otf"}, 1000) == want,
          "the font fetch command is exactly the pinned string");
}

// -- the stream -------------------------------------------------------------

void test_the_stream_parser()
{
    const FontFetchLimits limits;
    {
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        const std::string stream =
            "@@ ok 3 /boot/a.ttf\nQUJD\n@@ end\n"
            "@@ missing /boot/b.ttf\n"
            "@@ too-large 99999999999 /boot/c.ttf\n"
            "@@ ok 5 /boot/d e.ttf\nSGVs\nbG8=\n@@ end\n";
        check(parse_font_fetch_stream(stream, limits, files, problems),
              "a well-formed stream parses");
        check(files.size() == 2 && files[0].path == "/boot/a.ttf"
                  && files[0].bytes == "ABC" && files[1].path == "/boot/d e.ttf"
                  && files[1].bytes == "Hello",
              "both sent files decode, base64 across lines included");
        check(problems.size() == 2
                  && problems[0] == "/boot/b.ttf: not on the guest"
                  && problems[1] == "/boot/c.ttf: larger than the per-file cap",
              "a missing and a too-large file are each one problem");
    }
    {
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 4 /boot/a.ttf\nQUJD\n@@ end\n", limits,
                                files, problems);
        check(files.empty() && problems.size() == 1
                  && problems[0] == "/boot/a.ttf: 3 bytes arrived, 4 announced",
              "a size that does not match the bytes is refused");
    }
    {
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 3 /boot/a.ttf\nQU*D\n@@ end\n", limits,
                                files, problems);
        check(files.empty() && problems.size() == 1
                  && problems[0] == "/boot/a.ttf: not valid base64",
              "bytes outside the base64 alphabet are refused");
    }
    {
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 3 /boot/a.ttf\nQUJD\n", limits, files,
                                problems);
        check(files.empty() && problems.size() == 1
                  && problems[0] == "/boot/a.ttf: the stream ended inside the file",
              "a stream cut off inside a file stores nothing");
    }
    {
        FontFetchLimits small;
        small.max_total_bytes = 5;
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 3 /boot/a.ttf\nQUJD\n@@ end\n"
                                "@@ ok 3 /boot/b.ttf\nREVG\n@@ end\n",
                                small, files, problems);
        check(files.size() == 1 && files[0].path == "/boot/a.ttf"
                  && problems.size() == 1
                  && problems[0] == "/boot/b.ttf: over the per-fetch cap",
              "the per-fetch cap stops the second file");
    }
    {
        FontFetchLimits small;
        small.max_file_bytes = 2;
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 3 /boot/a.ttf\nQUJD\n@@ end\n", small,
                                files, problems);
        check(files.empty() && problems.size() == 1
                  && problems[0] == "/boot/a.ttf: larger than the per-file cap",
              "the per-file cap is enforced locally too, not only on the guest");
    }
    {
        FontFetchLimits one;
        one.max_files = 1;
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        parse_font_fetch_stream("@@ ok 3 /boot/a.ttf\nQUJD\n@@ end\n"
                                "@@ ok 3 /boot/b.ttf\nREVG\n@@ end\n",
                                one, files, problems);
        check(files.size() == 1 && problems.size() == 1
                  && problems[0] == "/boot/b.ttf: over the file-count cap",
              "the file-count cap is enforced");
    }
    {
        std::vector<GuestFontFile> files;
        std::vector<std::string> problems;
        check(!parse_font_fetch_stream("sh: base64: not found\n", limits, files,
                                       problems),
              "output that is not the stream at all is reported as such");
    }
}

// -- the 'head' check -------------------------------------------------------

// A minimal sfnt with one 'head' table, spelled out by hand.
std::string synthetic_sfnt(std::uint32_t revision, std::uint32_t checksum)
{
    std::string b(12 + 16 + 54, '\0');
    put_be32(b, 0, 0x00010000u);
    b[4] = 0;
    b[5] = 1;                       // numTables
    b.replace(12, 4, "head");
    put_be32(b, 12 + 8, 28);        // offset
    put_be32(b, 12 + 12, 54);       // length
    put_be32(b, 28, 0x00010000u);   // head version
    put_be32(b, 32, revision);
    put_be32(b, 36, checksum);
    return b;
}

void test_sfnt_identity_reads_the_head_table()
{
    std::uint32_t checksum = 0;
    std::uint32_t revision = 0;
    const std::string one = synthetic_sfnt(0x00028000u, 0xdeadbeefu);
    check(sfnt_identity(one, 0, checksum, revision) && checksum == 0xdeadbeefu
              && revision == 0x00028000u,
          "a single-face sfnt's head checksum and revision");
    check(!sfnt_identity(one, 1, checksum, revision),
          "face 1 of a single-face file does not exist");

    // A collection: 'ttcf', version, count 2, two offsets, then the faces.
    const std::string first = synthetic_sfnt(0x00010000u, 0x11111111u);
    const std::string second = synthetic_sfnt(0x00020000u, 0x22222222u);
    std::string ttc(20, '\0');
    ttc.replace(0, 4, "ttcf");
    put_be32(ttc, 4, 0x00010000u);
    put_be32(ttc, 8, 2);
    put_be32(ttc, 12, 20);
    put_be32(ttc, 16, static_cast<std::uint32_t>(20 + first.size()));
    // Table offsets in a collection are from the start of the file.
    std::string a = first;
    std::string b = second;
    put_be32(a, 12 + 8, 20 + 28);
    put_be32(b, 12 + 8, static_cast<std::uint32_t>(20 + first.size() + 28));
    ttc += a + b;
    check(sfnt_identity(ttc, 1, checksum, revision) && checksum == 0x22222222u
              && revision == 0x00020000u,
          "member 1 of a collection");
    check(sfnt_identity(ttc, 0x00030001u, checksum, revision)
              && checksum == 0x22222222u,
          "a named instance's high bits do not pick the member");
    check(!sfnt_identity(ttc, 2, checksum, revision),
          "a member past the collection's count does not exist");
    check(!sfnt_identity("not a font at all", 0, checksum, revision),
          "bytes that are not an sfnt have no identity");
    check(!sfnt_identity(one.substr(0, 40), 0, checksum, revision),
          "a truncated head table has no identity");

    if (const auto path = host_ttf()) {
        const auto head = freetype_head(*path);
        const std::string bytes = read_file(*path);
        check(head && sfnt_identity(bytes, 0, checksum, revision)
                  && checksum == head->checksum && revision == head->revision,
              "on a real file, the head fields agree with FreeType's ("
                  + path->string() + ")");
    } else {
        skip("sfnt_identity against FreeType on a real file",
             "no TrueType file on this host");
    }
}

// -- the fetch, through a scripted runner ----------------------------------

void test_fetch_with_a_scripted_runner()
{
    const auto source = host_ttf();
    if (!source) {
        skip("fetch_guest_fonts", "no TrueType file on this host");
        return;
    }
    const fs::path guest = scratch("scripted-guest");
    const fs::path cache = scratch("scripted-cache");
    const std::string guest_path = "/boot/system/data/fonts/ttfonts/Scripted.ttf";
    const auto font = make_guest_font(*source, guest, guest_path, 0x5c217e01u);
    check(font.has_value(), "the patched guest font reads back with its new checksum");
    if (!font)
        return;

    ScriptedRunner runner;
    runner.stdout_text = "@@ ok " + std::to_string(font->bytes.size()) + " "
        + guest_path + "\n" + base64_lines(font->bytes) + "@@ end\n";
    const FontFetchResult r =
        fetch_guest_fonts(runner, guest_ssh(), {font->identity}, cache);
    check(r.ran && runner.calls == 1 && r.error.empty(),
          "one ssh command fetches a missing font");
    // The hardened ssh of every other SSH call, then user@host, then the
    // command -- written out, not rebuilt with build_ssh_base_argv.
    const std::vector<std::string> prefix = {
        "ssh", "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=3", "-o", "ControlMaster=no",
        "-o", "ControlPath=none", "-p", "2222", "-i", "/keys/id_guest",
        "-o", "IdentitiesOnly=yes", "-o", "UserKnownHostsFile=/keys/known_hosts",
        "baron@guest.example"};
    check(runner.last_argv.size() == prefix.size() + 1
              && std::equal(prefix.begin(), prefix.end(), runner.last_argv.begin()),
          "the fetch goes over the route's own hardened ssh, nothing else");
    check(!runner.last_argv.empty()
              && runner.last_argv.back()
                     == build_font_fetch_command({guest_path}, FontFetchLimits {}.max_file_bytes),
          "the remote command names exactly the server's path");
    char revision_hex[9];
    std::snprintf(revision_hex, sizeof revision_hex, "%08x",
                  font->identity.font_revision);
    const fs::path want = cache / "fetched"
        / (std::string("5c217e01-") + revision_hex) / "Scripted.ttf";
    check(r.stored.size() == 1 && r.stored[0] == want,
          "the file is stored as fetched/<checksum>-<revision>/<name>");
    check(read_file(want) == font->bytes, "the stored bytes are the guest's");
    check(r.bytes == font->bytes.size(), "the byte count is reported");
#if !defined(_WIN32)
    struct stat st {};
    check(::stat(want.c_str(), &st) == 0 && (st.st_mode & 0111) == 0
              && (st.st_mode & 0777) == 0644,
          "the stored font is 0644: data, never executable");
#endif
    check(!fs::exists(want.string() + ".partial"), "no partial file is left");

    // Already here: nothing is run.
    const FontFetchResult again =
        fetch_guest_fonts(runner, guest_ssh(), {font->identity}, cache);
    check(!again.ran && runner.calls == 1 && again.stored == r.stored,
          "a font already in the cache runs no ssh");

    // The guest sends other bytes under that path: discarded, nothing written.
    FontIdentity other = font->identity;
    other.checksum_adjustment = 0x5c217e02u;
    const FontFetchResult wrong =
        fetch_guest_fonts(runner, guest_ssh(), {other}, cache);
    check(wrong.ran && wrong.stored.empty() && wrong.problems.size() == 1
              && wrong.problems[0].find("not the file the server named")
                     != std::string::npos,
          "bytes whose head does not match the server's are discarded");
    check(!fs::exists(fetched_font_path(cache, other)),
          "nothing is written for a file that failed verification");

    // The same checksum under another fontRevision is another release too:
    // the server named both, so both must match.
    FontIdentity other_release = font->identity;
    other_release.font_revision ^= 0x00010000u;
    const FontFetchResult wrong_release =
        fetch_guest_fonts(runner, guest_ssh(), {other_release}, cache);
    check(wrong_release.ran && wrong_release.stored.empty()
              && wrong_release.problems.size() == 1
              && wrong_release.problems[0].find("not the file the server named")
                     != std::string::npos,
          "bytes whose head revision does not match the server's are discarded");
    check(!fs::exists(fetched_font_path(cache, other_release)),
          "nothing is written for a file whose revision failed verification");

    // Nothing to verify against, or not a path to fetch from: no ssh at all.
    FontIdentity unnamed = font->identity;
    unnamed.checksum_adjustment = 0;
    FontIdentity escaping = font->identity;
    escaping.path = "/boot/../etc/shadow.ttf";
    FontIdentity memory = font->identity;
    memory.path.clear();
    const int before = runner.calls;
    const FontFetchResult none = fetch_guest_fonts(
        runner, guest_ssh(), {unnamed, escaping, memory}, cache);
    check(!none.ran && runner.calls == before && none.problems.size() == 3,
          "no checksum, an unsafe path, or a memory font: nothing is run");

    // ssh fails: said, nothing stored.
    ScriptedRunner failing;
    failing.exit_code = 255;
    FontIdentity fresh = font->identity;
    fresh.checksum_adjustment = 0x5c217e03u;
    const FontFetchResult failed =
        fetch_guest_fonts(failing, guest_ssh(), {fresh}, cache);
    check(failed.ran && failed.exit_status == 255 && failed.error == "scripted failure"
              && failed.stored.empty(),
          "a failed ssh is an error, and stores nothing");
}

// -- the fetch, through a fake ssh that runs the real command --------------

#if !defined(_WIN32)
// `ssh` for this test: whatever options it is given, the last argument is the
// remote command, run by sh with every '/boot/... path moved under the fake
// guest root -- the client's own command, wc and base64 and all -- and the root
// taken back out of what it prints. Each invocation's argv is logged.
constexpr const char* fake_ssh = R"SH(#!/bin/sh
for a in "$@"; do cmd="$a"; done
printf '%s\n' "$*" >> "$FAKE_SSH_ARGS"
root="$FAKE_SSH_FONT_ROOT"
sh -c "$(printf '%s' "$cmd" | sed "s|'/boot/|'$root/boot/|g")" | sed "s|$root/boot/|/boot/|"
)SH";

struct FakeSshPath {
    fs::path dir;
    std::string saved_path;
    explicit FakeSshPath(const fs::path& guest_root)
    {
        dir = scratch("fake-ssh-bin");
        write_file(dir / "ssh", fake_ssh);
        ::chmod((dir / "ssh").c_str(), 0755);
        const char* path = std::getenv("PATH");
        saved_path = path != nullptr ? path : "";
        ::setenv("PATH", (dir.string() + ":" + saved_path).c_str(), 1);
        ::setenv("FAKE_SSH_FONT_ROOT", guest_root.c_str(), 1);
        ::setenv("FAKE_SSH_ARGS", (dir / "args").c_str(), 1);
    }
    ~FakeSshPath()
    {
        ::setenv("PATH", saved_path.c_str(), 1);
        ::unsetenv("FAKE_SSH_FONT_ROOT");
        ::unsetenv("FAKE_SSH_ARGS");
    }
    [[nodiscard]] int invocations() const
    {
        std::ifstream in(dir / "args");
        int n = 0;
        std::string line;
        while (std::getline(in, line))
            ++n;
        return n;
    }
};
#endif

void test_fetch_through_a_fake_ssh()
{
#if defined(_WIN32)
    skip("the fetch through a fake ssh", "needs a POSIX sh");
#else
    const auto source = host_ttf();
    if (!source) {
        skip("the fetch through a fake ssh", "no TrueType file on this host");
        return;
    }
    const fs::path guest = scratch("sh-guest");
    const fs::path cache = scratch("sh-cache");
    const auto font = make_guest_font(
        *source, guest, "/boot/system/data/fonts/ttfonts/Fetched Face.ttf",
        0x5c217e10u);
    const auto big = make_guest_font(
        *source, guest, "/boot/home/config/non-packaged/data/fonts/Big.ttf",
        0x5c217e11u);
    check(font && big, "the guest fonts were made");
    if (!font || !big)
        return;
    FontIdentity absent = font->identity;
    absent.checksum_adjustment = 0x5c217e12u;
    absent.path = "/boot/system/data/fonts/ttfonts/Absent.ttf";

    FakeSshPath ssh(guest);
    SystemCommandRunner runner(30);
    FontFetchLimits limits;
    // Big.ttf is the same size as the other; a cap between nothing and it
    // is what the guest's -le test must enforce. So: one file at a time.
    limits.max_file_bytes = font->bytes.size();
    const FontFetchResult r = fetch_guest_fonts(
        runner, guest_ssh(), {font->identity, absent}, cache, limits);
    check(r.ran && r.exit_status == 0 && r.error.empty(),
          "the real remote command ran through the fake ssh (" + r.error + ")");
    check(r.stored.size() == 1
              && read_file(fetched_font_path(cache, font->identity)) == font->bytes,
          "the guest's file arrived byte for byte, through sh, wc and base64");
    check(std::find(r.problems.begin(), r.problems.end(),
                    absent.path + ": not on the guest")
              != r.problems.end(),
          "a path the guest does not have is reported missing");

    FontFetchLimits tight;
    tight.max_file_bytes = font->bytes.size() - 1;
    const FontFetchResult capped =
        fetch_guest_fonts(runner, guest_ssh(), {big->identity}, cache, tight);
    check(capped.ran && capped.stored.empty() && capped.problems.size() == 1
              && capped.problems[0] == big->identity.path
                                           + ": larger than the per-file cap",
          "the guest refuses a file over the cap itself (-le)");

    // The runner's own cap: stdout past it kills the command.
    SystemCommandRunner bounded(30);
    bounded.set_max_output_bytes(1000);
    FontIdentity another = big->identity;
    const FontFetchResult cut =
        fetch_guest_fonts(bounded, guest_ssh(), {another}, cache);
    check(cut.ran && cut.exit_status == -1 && cut.stored.empty()
              && cut.error == "command output exceeded 1000 bytes",
          "output past the runner's cap is cut off and nothing stored ("
              + cut.error + ")");

    // FontFetcher: each identity once, and the TextEngine told.
    const int before = ssh.invocations();
    {
        FontFetcher fetcher(guest_ssh(), cache);
        FontIdentity third = big->identity; // Big.ttf, now under the default cap
        const FontFetchResult first = fetcher.fetch({third});
        check(first.stored.size() == 1, "the fetcher stores a missing font");
        const FontFetchResult second = fetcher.fetch({third});
        check(!second.ran && fetcher.fetches_run() == 1,
              "the fetcher never tries an identity twice");
        TextEngine engine;
        check(!engine.available() || engine.has_identical_face(third),
              "a fetched file is known to every TextEngine at once");
    }
    {
        // A fetch that fails is not retried either (nothing cached to hide it).
        ScriptedRunner failing;
        failing.exit_code = 255;
        FontFetcher fetcher(guest_ssh(), cache, {}, &failing);
        FontIdentity never = font->identity;
        never.checksum_adjustment = 0x5c217e13u;
        (void)fetcher.fetch({never});
        (void)fetcher.fetch({never});
        fetcher.want(never);
        check(fetcher.wait_idle(std::chrono::seconds(10)) && failing.calls == 1
                  && fetcher.fetches_run() == 1,
              "an identity whose fetch failed is never tried again");
        FontIdentity later = never;
        later.checksum_adjustment = 0x5c217e14u;
        fetcher.want(later);
        fetcher.want(later);
        check(fetcher.wait_idle(std::chrono::seconds(10)) && failing.calls == 2,
              "a font named mid-session is fetched on the worker, once");
    }
    check(ssh.invocations() == before + 1,
          "one ssh invocation for the fetcher's one fetch");
#endif
}

// -- the session's half ------------------------------------------------------

void append_font_record(Writer& writer)
{
    writer.u8(0);
    writer.u8(0);
    writer.u32(0);
    writer.u8(2);       // B_BITMAP_SPACING
    writer.f32(90.0f);  // neutral shear
    writer.f32(0);
    writer.f32(0);
    writer.f32(14.0f);
    writer.u16(0);
    writer.u32(0x00070002u);
}

std::vector<std::uint8_t> set_font_frame(const FontIdentity& id)
{
    Writer font(Op::set_font);
    font.i32(5);
    append_font_record(font);
    font.string(id.family);
    font.string(id.style);
    font.u32(id.face_index);
    font.u32(id.checksum_adjustment);
    font.u32(id.font_revision);
    font.u8(3);
    font.string(id.path);
    return font.finish();
}

void test_the_session_wants_and_then_repaints()
{
    const auto source = host_ttf();
    if (!source) {
        skip("the session's wanted fonts", "no TrueType file on this host");
        return;
    }
    {
        TextEngine probe;
        if (!probe.available()) {
            skip("the session's wanted fonts", "no FreeType library");
            return;
        }
    }
    const fs::path guest = scratch("session-guest");
    const auto font = make_guest_font(
        *source, guest, "/boot/system/data/fonts/ttfonts/Session.ttf", 0x5c217e20u);
    const auto host = freetype_head(*source);
    check(font && host, "the session's guest font was made");
    if (!font || !host)
        return;

    std::vector<std::uint8_t> sent;
    Session session(320, 200, [&](std::span<const std::uint8_t> bytes) {
        sent.insert(sent.end(), bytes.begin(), bytes.end());
        return true;
    });
    std::vector<FontIdentity> handed;
    session.set_font_wanted_handler(
        [&](const FontIdentity& id) { handed.push_back(id); });
    Writer ack(Op::hello_ack);
    ack.u32(protocol_version);
    ack.u32(cap_font_identity);
    session.ingest(ack.finish());

    session.ingest(set_font_frame(font->identity));
    session.ingest(set_font_frame(font->identity));
    check(session.wanted_fonts().size() == 1
              && session.wanted_fonts()[0] == font->identity,
          "a named file that is not here is wanted, once");
    check(handed.size() == 1 && handed[0] == font->identity,
          "the handler hears of it once, however often the server names it");

    // The host's own file, unpatched: here, so not wanted.
    FontIdentity present = font->identity;
    present.checksum_adjustment = host->checksum;
    present.font_revision = host->revision;
    present.path = "/boot/system/data/fonts/ttfonts/Present.ttf";
    session.ingest(set_font_frame(present));
    check(session.wanted_fonts().size() == 1 && handed.size() == 1,
          "a file this host has is not wanted");

    // No checksum: nothing could be fetched for it.
    FontIdentity unnamed = font->identity;
    unnamed.checksum_adjustment = 0;
    unnamed.style = "Unnamed";
    session.ingest(set_font_frame(unnamed));
    check(handed.size() == 1, "an identity without a checksum is never wanted");

    // An engine that already drew in the stand-in face...
    TextEngine engine;
    Font named;
    named.size = 14.0f;
    named.identity = std::make_shared<const FontIdentity>(font->identity);
    check(!engine.selected_face(named).identical,
          "before the file arrives, the face drawn is a stand-in");

    // The file arrives; the next ingest re-checks and asks for a repaint.
    sent.clear();
    TextEngine::add_font_files({font->local.string()});
    const auto after = engine.selected_face(named);
    check(after.identical && after.path == font->local.string(),
          "...re-resolves to the arrived file at once, not its cached stand-in");
    Writer color(Op::set_high_color);
    color.i32(5);
    color.u8(0);
    color.u8(0);
    color.u8(0);
    color.u8(255);
    session.ingest(color.finish());
    check(session.wanted_fonts().empty(), "the arrived font is no longer wanted");
    // RP_UPDATE_DISPLAY_MODE (code 2) at 320x200, the whole frame, nothing else.
    const std::vector<std::uint8_t> repaint = {2, 0, 14, 0, 0, 0,
                                               0x40, 0x01, 0, 0, 0xc8, 0, 0, 0};
    check(sent == repaint,
          "the session asked app_server for a full repaint at its size");
    sent.clear();
    session.ingest(color.finish());
    check(sent.empty(), "and only once");
}

void test_the_environment_switch()
{
    check(font_fetch_enabled_by_environment(nullptr), "unset: fetch");
    check(font_fetch_enabled_by_environment("1"), "1: fetch");
    for (const char* off : {"0", "false", "OFF", "No"})
        check(!font_fetch_enabled_by_environment(off),
              std::string("HAIKU_REMOTE_FONT_FETCH=") + off + " turns it off");
}

void test_the_cache_directory_is_per_os()
{
#if defined(_WIN32) || defined(__APPLE__)
    skip("the Linux/Haiku cache directory", "not this OS");
#else
    const char* saved = std::getenv("XDG_CACHE_HOME");
    const std::string keep = saved != nullptr ? saved : "";
    ::setenv("XDG_CACHE_HOME", "/x/cache", 1);
    check(font_cache_directory() == fs::path("/x/cache/haiku-remote/fonts"),
          "$XDG_CACHE_HOME/haiku-remote/fonts");
    ::setenv("XDG_CACHE_HOME", "", 1);
    const char* home = std::getenv("HOME");
    check(home == nullptr
              || font_cache_directory()
                     == fs::path(home) / ".cache" / "haiku-remote" / "fonts",
          "~/.cache/haiku-remote/fonts when XDG_CACHE_HOME is empty");
    ::setenv("XDG_CACHE_HOME", keep.c_str(), 1);
#endif
}

} // namespace

int main()
{
    // Keep the user's own cache and font overrides out of every engine here.
#if !defined(_WIN32)
    ::setenv("XDG_CACHE_HOME", scratch("xdg-cache").c_str(), 1);
    ::unsetenv("HAIKU_REMOTE_FONT_DIR");
#endif
    test_guest_paths_that_are_fetched_and_refused();
    test_the_remote_command_is_pinned();
    test_the_stream_parser();
    test_sfnt_identity_reads_the_head_table();
    test_fetch_with_a_scripted_runner();
    test_fetch_through_a_fake_ssh();
    test_the_session_wants_and_then_repaints();
    test_the_environment_switch();
    test_the_cache_directory_is_per_os();
    if (failures == 0) {
        std::cout << "PASS - " << checks << " font-fetch checks";
        if (skips > 0)
            std::cout << " (" << skips << " SKIPPED, see SKIP: lines)";
        std::cout << '\n';
        return 0;
    }
    std::cerr << failures << " of " << checks << " font-fetch checks failed\n";
    return 1;
}
