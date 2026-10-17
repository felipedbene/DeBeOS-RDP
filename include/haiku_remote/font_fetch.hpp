#pragma once

// Fetching the guest's own font files, automatically, over the SSH credential
// the session already reached the guest with.
//
// Why: with RP_CAP_FONT_IDENTITY every RP_SET_FONT names the file app_server
// lays the text out with -- family, style, face index, the sfnt 'head'
// checkSumAdjustment and fontRevision, and its path on the guest. The server
// owns layout (RemoteDrawingEngine::DrawString returns ServerFont::StringWidth),
// so a client drawing from any other file puts its glyphs somewhere else. A
// host's "Noto Sans" is usually a different release from the guest's. So when
// the server names a file this client does not have, the client fetches that
// file -- that one, by the path the server gave -- and keeps it.
//
// The rules, each enforced here rather than left to the caller:
//
//   - Only over SSH that authenticated the host: the caller hands in the
//     SshConfig its route used (ManagedConnection::ssh), which is set for the
//     tunnel and the broker and never for direct. The same hardened argv
//     (build_ssh_exec_argv: BatchMode, the profile's key and known_hosts).
//   - Only what was named, only from where fonts live: an absolute path under
//     /boot/ with a font file extension, no "..", no quote or control
//     character (guest_font_path_is_safe). A server cannot steer this into
//     reading anything else, and the remote command cannot be broken out of.
//   - Verified: a file is kept only if its own 'head' table, at the server's
//     face index, carries exactly the checksum and revision the server
//     announced (sfnt_identity). Anything else is discarded unwritten.
//   - Bounded: per-file, per-fetch and file-count caps, enforced on the guest
//     (a file over the cap is never sent), by the runner (its stdout is cut
//     off past the cap) and again by the parser.
//   - Data, never code: files are written 0644 into the per-OS cache
//     (font_cache_directory()), opened only by FreeType, never executed.
//   - Once: an identity tried is never tried again by the same fetcher,
//     whatever the outcome.

#include "haiku_remote/managed_transport.hpp"
#include "haiku_remote/types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace haiku_remote {

// Where fetched fonts are kept, and where TextEngine looks for the server's
// files (after any HAIKU_REMOTE_FONT_DIR): $XDG_CACHE_HOME/haiku-remote/fonts
// (~/.cache/... when unset) on Linux and Haiku, ~/Library/Caches/haiku-remote/
// fonts on macOS, %LOCALAPPDATA%/haiku-remote/fonts on Windows. Empty when the
// environment names no home at all.
[[nodiscard]] std::filesystem::path font_cache_directory();

// HAIKU_REMOTE_FONT_FETCH: unset or anything else = fetch; "0", "false",
// "off" or "no" (any case) = never fetch. Exposed for the tests.
[[nodiscard]] bool font_fetch_enabled_by_environment(const char* value);

struct FontFetchLimits {
    // The largest single file. Haiku's biggest stock face (Noto Sans CJK) is
    // about 20 MB.
    std::size_t max_file_bytes = 32u * 1024u * 1024u;
    // All files of one fetch together.
    std::size_t max_total_bytes = 64u * 1024u * 1024u;
    // Files per fetch.
    std::size_t max_files = 32;
    // The ssh command's wall-clock limit.
    int timeout_seconds = 120;
};

// An absolute guest path under /boot/ ending in .ttf, .otf, .ttc or .otc (any
// case), with no "." or ".." component, no single quote, backslash or control
// character, at most 1024 bytes.
[[nodiscard]] bool guest_font_path_is_safe(std::string_view path);

// The remote sh command that sends `paths` (each already safe). For each, in
// order, one header line and then, for a file that is sent, its base64 and an
// end line:
//     @@ ok <size> <path>        then base64, then  @@ end
//     @@ too-large <size> <path>
//     @@ missing <path>
// '@' is not in the base64 alphabet, so the stream frames itself. Needs only
// sh, wc and base64 on the guest.
[[nodiscard]] std::string build_font_fetch_command(
    const std::vector<std::string>& paths, std::size_t max_file_bytes);

struct GuestFontFile {
    std::string path;
    std::string bytes;
};

// Parse what build_font_fetch_command's command printed. Files that decode
// and fit the limits go to `files`; everything else that was said (missing,
// too large, a size that does not match, a cap reached, garbage) is one line in
// `problems`. Returns false only for a stream that is not this format at all.
bool parse_font_fetch_stream(std::string_view stream, const FontFetchLimits& limits,
                             std::vector<GuestFontFile>& files,
                             std::vector<std::string>& problems);

// The 'head' checkSumAdjustment and fontRevision of face `face_index` of an
// sfnt (TrueType/OpenType, or a 'ttcf' collection), read from the bytes alone.
// Only the low 16 bits of the index pick a collection member; the high 16 (a
// named instance) share the file's head. False when the bytes are not an sfnt,
// the face does not exist, or there is no complete head table.
bool sfnt_identity(std::string_view bytes, std::uint32_t face_index,
                   std::uint32_t& checksum, std::uint32_t& revision);

// Where a verified file for `identity` is stored under `cache_dir`:
// fetched/<checksum>-<revision>/<the guest file's name>, eight lowercase hex
// digits each.
[[nodiscard]] std::filesystem::path fetched_font_path(
    const std::filesystem::path& cache_dir, const FontIdentity& identity);

struct FontFetchResult {
    bool ran = false;          // an ssh command was run
    int exit_status = 0;       // its exit status (-1: did not run to the end)
    std::string error;         // why it could not run, when it could not
    std::vector<std::filesystem::path> stored; // verified and written (or already there)
    std::vector<std::string> problems;         // one line per thing not stored
    std::size_t bytes = 0;     // bytes written
};

// Fetch the files `wanted` names into `cache_dir`, over `ssh` through `runner`.
// Identities without a checksum (nothing to verify against) or a safe path are
// skipped as problems, as is anything over the limits. Pure over `runner`, so a
// test drives it with a fake.
FontFetchResult fetch_guest_fonts(CommandRunner& runner, const SshConfig& ssh,
                                  const std::vector<FontIdentity>& wanted,
                                  const std::filesystem::path& cache_dir,
                                  const FontFetchLimits& limits = {});

// fetch_guest_fonts with the bookkeeping a session needs: each identity is
// tried at most once, stored files are handed to TextEngine::add_font_files so
// every engine picks them up, and fonts named mid-session (want()) are fetched
// on a worker thread of this object's own. The destructor cancels a running
// fetch -- its ssh child is killed and reaped -- and joins the worker.
class FontFetcher {
public:
    using Log = std::function<void(const std::string&)>;

    // `runner`, when non-null, is used instead of a SystemCommandRunner (for
    // tests) and must outlive this object; it must also be safe to call from
    // the worker thread.
    FontFetcher(SshConfig ssh, std::filesystem::path cache_dir,
                FontFetchLimits limits = {}, CommandRunner* runner = nullptr,
                Log log = {});
    ~FontFetcher();
    FontFetcher(const FontFetcher&) = delete;
    FontFetcher& operator=(const FontFetcher&) = delete;

    // On the calling thread, now: the connect stage's fetch. Identities
    // already tried are dropped first; `cancel` stops the ssh child.
    FontFetchResult fetch(const std::vector<FontIdentity>& wanted,
                          const CancelToken* cancel = nullptr);
    // Queue one for the worker thread (started on first use). Thread-safe;
    // returns at once. An identity already tried or queued is ignored.
    void want(const FontIdentity& identity);
    // Block until nothing is queued or running, or `timeout` passes.
    bool wait_idle(std::chrono::milliseconds timeout);
    // How many ssh fetch commands this object has run.
    [[nodiscard]] std::size_t fetches_run() const;

private:
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::string>;
    static Key key_of(const FontIdentity& identity);
    void work();

    SshConfig ssh_;
    std::filesystem::path cache_dir_;
    FontFetchLimits limits_;
    CommandRunner* runner_;
    Log log_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::set<Key> tried_;
    std::deque<FontIdentity> queue_;
    bool busy_ = false;
    bool stop_ = false;
    std::size_t fetches_ = 0;
    CancelToken cancel_;
    std::thread worker_;
};

} // namespace haiku_remote
