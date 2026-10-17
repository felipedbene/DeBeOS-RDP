#include "haiku_remote/font_fetch.hpp"

#include "haiku_remote/text_engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace haiku_remote {

namespace {

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string hex8(std::uint32_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[value & 0xfu];
        value >>= 4;
    }
    return out;
}

std::uint32_t be32(std::string_view bytes, std::size_t at)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

std::uint16_t be16(std::string_view bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) << 8)
        | static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])));
}

// RFC 4648 base64, newlines ignored; false on any other character or a bad
// length. `limit` bounds the decoded size (checked before it is reached).
bool decode_base64(std::string_view text, std::string& out, std::size_t limit)
{
    std::array<int, 256> value {};
    value.fill(-1);
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alphabet.size(); ++i)
        value[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    out.clear();
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    std::size_t symbols = 0;
    for (const char ch : text) {
        if (ch == '\n' || ch == '\r')
            continue;
        if (ch == '=') {
            ++padding;
            ++symbols;
            continue;
        }
        if (padding != 0)
            return false; // data after padding
        const int v = value[static_cast<unsigned char>(ch)];
        if (v < 0)
            return false;
        ++symbols;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out.size() >= limit)
                return false;
            out.push_back(static_cast<char>((buffer >> bits) & 0xffu));
        }
    }
    return symbols % 4 == 0 && padding <= 2;
}

std::string_view next_line(std::string_view& rest)
{
    const auto end = rest.find('\n');
    std::string_view line = rest.substr(0, end);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    return line;
}

bool parse_size(std::string_view text, std::size_t& out)
{
    if (text.empty() || text.size() > 19)
        return false;
    out = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return false;
        out = out * 10 + static_cast<std::size_t>(c - '0');
    }
    return true;
}

bool write_file_atomically(const std::filesystem::path& path,
                           std::string_view bytes, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".partial";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "write failed for " + temporary.string();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    // Data, never code: no execute bit for anyone, whatever the umask.
    using std::filesystem::perms;
    std::filesystem::permissions(
        temporary, perms::owner_read | perms::owner_write | perms::group_read
                       | perms::others_read,
        ec);
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = "cannot rename into " + path.string() + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool file_identity(const std::filesystem::path& path, std::uint32_t face_index,
                   std::uint32_t& checksum, std::uint32_t& revision)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    return sfnt_identity(bytes, face_index, checksum, revision);
}

} // namespace

std::filesystem::path font_cache_directory()
{
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / "haiku-remote" / "fonts";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Caches" / "haiku-remote"
            / "fonts";
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
        return std::filesystem::path(cache) / "haiku-remote" / "fonts";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "haiku-remote" / "fonts";
#endif
    return {};
}

bool font_fetch_enabled_by_environment(const char* value)
{
    if (value == nullptr)
        return true;
    const std::string v = lowered(value);
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}

bool guest_font_path_is_safe(std::string_view path)
{
    if (path.size() < 7 || path.size() > 1024 || path.substr(0, 6) != "/boot/")
        return false;
    for (const char c : path) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '\'' || c == '\\')
            return false;
    }
    // No "." or ".." component, and no empty one ("//").
    std::string_view rest = path.substr(1);
    while (!rest.empty()) {
        const auto end = rest.find('/');
        const std::string_view part = rest.substr(0, end);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        rest.remove_prefix(end + 1);
    }
    if (path.back() == '/')
        return false;
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    const std::string ext = lowered(path.substr(dot));
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

std::string build_font_fetch_command(const std::vector<std::string>& paths,
                                     std::size_t max_file_bytes)
{
    // Every path was checked by guest_font_path_is_safe: absolute, no single
    // quote, no control character -- so inside single quotes it is one literal
    // word whatever else it contains.
    std::string command = "for f in";
    for (const std::string& path : paths)
        command += " '" + path + "'";
    command +=
        "; do "
        "if [ -f \"$f\" ] && [ -r \"$f\" ]; then "
        "s=$(wc -c < \"$f\"); s=$((s + 0)); "
        "if [ \"$s\" -le " + std::to_string(max_file_bytes) + " ]; then "
        "printf '@@ ok %s %s\\n' \"$s\" \"$f\"; base64 < \"$f\"; printf '@@ end\\n'; "
        "else printf '@@ too-large %s %s\\n' \"$s\" \"$f\"; fi; "
        "else printf '@@ missing %s\\n' \"$f\"; fi; "
        "done";
    return command;
}

bool parse_font_fetch_stream(std::string_view stream, const FontFetchLimits& limits,
                             std::vector<GuestFontFile>& files,
                             std::vector<std::string>& problems)
{
    std::size_t total = 0;
    bool recognised = stream.empty();
    std::string_view rest = stream;
    while (!rest.empty()) {
        const std::string_view line = next_line(rest);
        if (line.empty())
            continue;
        if (line.substr(0, 3) != "@@ ") {
            problems.push_back("unexpected output from the guest: "
                               + std::string(line.substr(0, 80)));
            continue;
        }
        recognised = true;
        std::string_view header = line.substr(3);
        if (header.substr(0, 8) == "missing ") {
            problems.push_back(std::string(header.substr(8))
                               + ": not on the guest");
            continue;
        }
        if (header.substr(0, 10) == "too-large ") {
            std::string_view what = header.substr(10);
            if (const auto space = what.find(' '); space != std::string_view::npos)
                what.remove_prefix(space + 1);
            problems.push_back(std::string(what) + ": larger than the per-file cap");
            continue;
        }
        if (header.substr(0, 3) != "ok ") {
            problems.push_back("unexpected header from the guest: "
                               + std::string(header.substr(0, 80)));
            continue;
        }
        header.remove_prefix(3);
        const auto space = header.find(' ');
        std::size_t size = 0;
        if (space == std::string_view::npos
            || !parse_size(header.substr(0, space), size)) {
            problems.push_back("malformed header from the guest: "
                               + std::string(line.substr(0, 80)));
            return false;
        }
        const std::string path(header.substr(space + 1));
        // The base64 body, up to its end line.
        const auto end = rest.find("@@ end");
        if (end == std::string_view::npos) {
            problems.push_back(path + ": the stream ended inside the file");
            return recognised;
        }
        const std::string_view body = rest.substr(0, end);
        rest.remove_prefix(end);
        (void)next_line(rest); // the end line itself
        if (files.size() >= limits.max_files) {
            problems.push_back(path + ": over the file-count cap");
            continue;
        }
        if (size > limits.max_file_bytes) {
            problems.push_back(path + ": larger than the per-file cap");
            continue;
        }
        if (total + size > limits.max_total_bytes) {
            problems.push_back(path + ": over the per-fetch cap");
            continue;
        }
        GuestFontFile file;
        file.path = path;
        if (!decode_base64(body, file.bytes, limits.max_file_bytes)) {
            problems.push_back(path + ": not valid base64");
            continue;
        }
        if (file.bytes.size() != size) {
            problems.push_back(path + ": " + std::to_string(file.bytes.size())
                               + " bytes arrived, " + std::to_string(size)
                               + " announced");
            continue;
        }
        total += size;
        files.push_back(std::move(file));
    }
    return recognised;
}

bool sfnt_identity(std::string_view bytes, std::uint32_t face_index,
                   std::uint32_t& checksum, std::uint32_t& revision)
{
    std::size_t offset = 0;
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "ttcf") {
        const std::uint32_t count = be32(bytes, 8);
        const std::uint32_t member = face_index & 0xffffu;
        if (member >= count || 12 + 4 * static_cast<std::size_t>(member) + 4 > bytes.size())
            return false;
        offset = be32(bytes, 12 + 4 * static_cast<std::size_t>(member));
    } else if ((face_index & 0xffffu) != 0) {
        return false;
    }
    if (offset + 12 > bytes.size())
        return false;
    const std::uint32_t version = be32(bytes, offset);
    if (version != 0x00010000u && version != 0x4f54544fu /* OTTO */
        && version != 0x74727565u /* true */)
        return false;
    const std::uint16_t tables = be16(bytes, offset + 4);
    for (std::uint16_t i = 0; i < tables; ++i) {
        const std::size_t record = offset + 12 + 16 * static_cast<std::size_t>(i);
        if (record + 16 > bytes.size())
            return false;
        if (bytes.substr(record, 4) != "head")
            continue;
        const std::size_t at = be32(bytes, record + 8);
        const std::size_t length = be32(bytes, record + 12);
        if (length < 12 || at > bytes.size() || length > bytes.size() - at)
            return false;
        // OpenType 'head': version (4), fontRevision (4), checkSumAdjustment (4).
        revision = be32(bytes, at + 4);
        checksum = be32(bytes, at + 8);
        return true;
    }
    return false;
}

std::filesystem::path fetched_font_path(const std::filesystem::path& cache_dir,
                                        const FontIdentity& identity)
{
    std::string name = identity.path;
    if (const auto slash = name.rfind('/'); slash != std::string::npos)
        name = name.substr(slash + 1);
    return cache_dir / "fetched"
        / (hex8(identity.checksum_adjustment) + "-" + hex8(identity.font_revision))
        / name;
}

FontFetchResult fetch_guest_fonts(CommandRunner& runner, const SshConfig& ssh,
                                  const std::vector<FontIdentity>& wanted,
                                  const std::filesystem::path& cache_dir,
                                  const FontFetchLimits& limits)
{
    FontFetchResult result;
    if (cache_dir.empty()) {
        result.error = "no font cache directory (no home directory?)";
        return result;
    }
    // Which identities each guest path has to satisfy, and the paths to ask
    // for, each once, in the order named.
    std::vector<std::string> paths;
    std::vector<FontIdentity> sought;
    for (const FontIdentity& identity : wanted) {
        if (identity.checksum_adjustment == 0) {
            result.problems.push_back(identity.family + " " + identity.style
                                      + ": no checksum to verify a file against");
            continue;
        }
        if (!guest_font_path_is_safe(identity.path)) {
            result.problems.push_back(
                (identity.path.empty() ? identity.family + " " + identity.style
                                       : identity.path)
                + ": not a path this client fetches from");
            continue;
        }
        // Already here from an earlier session: nothing to fetch.
        const auto stored = fetched_font_path(cache_dir, identity);
        std::uint32_t checksum = 0;
        std::uint32_t revision = 0;
        if (file_identity(stored, identity.face_index, checksum, revision)
            && checksum == identity.checksum_adjustment
            && revision == identity.font_revision) {
            result.stored.push_back(stored);
            continue;
        }
        sought.push_back(identity);
        if (std::find(paths.begin(), paths.end(), identity.path) == paths.end())
            paths.push_back(identity.path);
    }
    if (paths.size() > limits.max_files) {
        for (std::size_t i = limits.max_files; i < paths.size(); ++i)
            result.problems.push_back(paths[i] + ": over the file-count cap");
        paths.resize(limits.max_files);
    }
    if (paths.empty())
        return result;

    const std::vector<std::string> argv =
        build_ssh_exec_argv(ssh, build_font_fetch_command(paths, limits.max_file_bytes));
    std::string stream;
    std::string error;
    result.ran = true;
    result.exit_status = runner.run(argv, stream, error);
    if (result.exit_status != 0) {
        result.error = error.empty()
            ? "ssh to " + ssh.host + " exited " + std::to_string(result.exit_status)
            : error;
        return result;
    }
    std::vector<GuestFontFile> files;
    if (!parse_font_fetch_stream(stream, limits, files, result.problems)) {
        result.error = "the guest's answer was not a font stream";
        return result;
    }
    for (const GuestFontFile& file : files) {
        bool used = false;
        for (const FontIdentity& identity : sought) {
            if (identity.path != file.path)
                continue;
            std::uint32_t checksum = 0;
            std::uint32_t revision = 0;
            if (!sfnt_identity(file.bytes, identity.face_index, checksum, revision)
                || checksum != identity.checksum_adjustment
                || revision != identity.font_revision) {
                continue;
            }
            const auto target = fetched_font_path(cache_dir, identity);
            if (std::find(result.stored.begin(), result.stored.end(), target)
                != result.stored.end()) {
                used = true;
                continue;
            }
            std::string write_error;
            if (!write_file_atomically(target, file.bytes, write_error)) {
                result.problems.push_back(write_error);
                continue;
            }
            result.stored.push_back(target);
            result.bytes += file.bytes.size();
            used = true;
        }
        if (!used) {
            std::uint32_t checksum = 0;
            std::uint32_t revision = 0;
            const bool parsed = sfnt_identity(file.bytes, 0, checksum, revision);
            result.problems.push_back(
                file.path + ": not the file the server named ("
                + (parsed ? "head " + hex8(checksum) + " rev " + hex8(revision)
                          : std::string("no sfnt head table"))
                + "); discarded");
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// FontFetcher
// ---------------------------------------------------------------------------

FontFetcher::FontFetcher(SshConfig ssh, std::filesystem::path cache_dir,
                         FontFetchLimits limits, CommandRunner* runner, Log log)
    : ssh_(std::move(ssh)), cache_dir_(std::move(cache_dir)), limits_(limits),
      runner_(runner), log_(std::move(log))
{
}

FontFetcher::~FontFetcher()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_.cancel(); // a running ssh is killed and reaped by its runner
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

FontFetcher::Key FontFetcher::key_of(const FontIdentity& identity)
{
    return {identity.checksum_adjustment, identity.font_revision,
            identity.face_index, identity.path};
}

FontFetchResult FontFetcher::fetch(const std::vector<FontIdentity>& wanted,
                                   const CancelToken* cancel)
{
    std::vector<FontIdentity> fresh;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const FontIdentity& identity : wanted)
            if (tried_.insert(key_of(identity)).second)
                fresh.push_back(identity);
    }
    FontFetchResult result;
    if (fresh.empty())
        return result;
    SystemCommandRunner system(limits_.timeout_seconds);
    system.set_cancel(cancel);
    // base64 is 4 bytes per 3, plus a line break per 76 and the frame lines.
    system.set_max_output_bytes(limits_.max_total_bytes / 3 * 4
                                + limits_.max_total_bytes / 57 + 64 * 1024);
    CommandRunner& runner = runner_ != nullptr ? *runner_ : system;
    result = fetch_guest_fonts(runner, ssh_, fresh, cache_dir_, limits_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.ran)
            ++fetches_;
    }
    std::vector<std::string> paths;
    for (const auto& path : result.stored)
        paths.push_back(path.string());
    if (!paths.empty())
        TextEngine::add_font_files(paths);
    if (log_) {
        if (!result.error.empty())
            log_("fonts: fetch from " + ssh_.host + " failed: " + result.error);
        for (const auto& path : result.stored)
            log_("fonts: using the guest's " + path.filename().string() + " ("
                 + path.string() + ")");
        for (const auto& problem : result.problems)
            log_("fonts: " + problem);
    }
    return result;
}

void FontFetcher::want(const FontIdentity& identity)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || tried_.count(key_of(identity)) != 0)
            return;
        for (const FontIdentity& queued : queue_)
            if (key_of(queued) == key_of(identity))
                return;
        queue_.push_back(identity);
        if (!worker_.joinable())
            worker_ = std::thread([this] { work(); });
    }
    cv_.notify_all();
}

void FontFetcher::work()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_)
            return;
        std::vector<FontIdentity> batch(queue_.begin(), queue_.end());
        queue_.clear();
        busy_ = true;
        lock.unlock();
        (void)fetch(batch, &cancel_);
        lock.lock();
        busy_ = false;
        cv_.notify_all();
    }
}

bool FontFetcher::wait_idle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this] { return stop_ || (queue_.empty() && !busy_); });
}

std::size_t FontFetcher::fetches_run() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fetches_;
}

} // namespace haiku_remote
