// quic_loss_bench -- one measurement run for Haiku-Graviton #766 box 6:
// "QUIC vs wss under tc netem loss. This decides whether the multi-stream step
// is worth starting."
//
// It is the real client stack (make_transport + Session, so zstd, the decoder
// and the renderer all run) driven through a fixed, client-generated workload,
// so the same run means the same bytes on quic://, wss:// or tcp://:
//
//   phase 0  connect + handshake, then wait for the initial full repaint to go
//            quiet (no byte for --settle-ms). "first full frame" is the last
//            byte of that burst, measured from before connect().
//   phase A  probes only: an RP_MOUSE_MOVED every 1000/--probe-hz ms at a
//            fresh coordinate. app_server echoes each as RP_MOVE_CURSOR_TO
//            (RemoteHWInterface.cpp:1487-1493, reached from the event stream,
//            RemoteHWInterface.cpp:757-760), so send -> echo is input-to-echo
//            latency with nothing else on the stream. This approximates what an
//            input/cursor stream of its own would see in a multi-stream layout:
//            it only ever waits for its *own* lost packets.
//   phase B  the same probes, plus a full repaint (RP_UPDATE_DISPLAY_MODE at the
//            current size, Session::request_full_repaint(); app_server repaints
//            the whole frame, RemoteHWInterface.cpp:983-998) every
//            --repaint-ms. Now every echo is queued on the one stream behind
//            repaint bytes, so a lost repaint packet stalls the cursor too:
//            head-of-line blocking, the thing multi-stream QUIC removes.
//
// Per phase it reports the inter-arrival gap between consecutive receive()
// completions (the "stall"), the echo latency, and in phase B per-repaint
// completion time, bytes and messages. One JSON object on stdout; human notes
// on stderr. Echoes coalesced by the server (a later coordinate arrives first)
// count for every earlier outstanding probe at the time the later one arrives:
// that is when the user sees the cursor get at least that far.
//
// Usage (transport arguments are the client's own, see --help):
//   quic_loss_bench --url quic://HOST:10902 --creds FILE [--width 1024
//       --height 700] [--settle-ms 2000] [--idle-s 8] [--bulk-s 12]
//       [--probe-hz 50] [--repaint-ms 1000] [--label TEXT]
// --creds FILE is the broker credential fetch's stdout (token, the PEM marker,
// broker.pem, the QUIC marker, broker.quic), the format managed_transport.cpp
// fetch_broker_credentials() parses; the certificate becomes the pin.

#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/known_brokers.hpp"
#include "haiku_remote/session.hpp"
#include "haiku_remote/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace haiku_remote;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point origin, Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(t - origin).count();
}

struct Stats {
    std::size_t n = 0;
    double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};

Stats stats_of(std::vector<double> values)
{
    Stats s;
    s.n = values.size();
    if (values.empty())
        return s;
    std::sort(values.begin(), values.end());
    auto pct = [&](double p) {
        // nearest-rank
        std::size_t rank = static_cast<std::size_t>(
            std::ceil(p / 100.0 * static_cast<double>(values.size())));
        rank = std::clamp<std::size_t>(rank, 1, values.size());
        return values[rank - 1];
    };
    s.p50 = pct(50);
    s.p95 = pct(95);
    s.p99 = pct(99);
    s.max = values.back();
    double sum = 0;
    for (double v : values)
        sum += v;
    s.mean = sum / static_cast<double>(values.size());
    return s;
}

std::string json_stats(const Stats& s)
{
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"n\":%zu,\"p50\":%.2f,\"p95\":%.2f,\"p99\":%.2f,"
                  "\"max\":%.2f,\"mean\":%.2f}",
                  s.n, s.p50, s.p95, s.p99, s.max, s.mean);
    return buffer;
}

std::string json_escape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            out += ' ';
        else
            out += c;
    }
    return out;
}

// What one receive() returned.
struct Arrival {
    double at_ms;
    std::size_t bytes;
    std::size_t messages; // Session::message_count() delta
    int echoes;           // probes this chunk answered
};

struct Probe {
    double sent_ms;
    double echo_ms = -1;
};

// managed_transport.cpp kBrokerPemMarker / kBrokerQuicMarker.
const char* kPemMarker = "===BROKER_PEM===";
const char* kQuicMarker = "===BROKER_QUIC===";

void load_credentials(const std::string& path, TransportOptions& options)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read --creds " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    const std::size_t pem = text.find(kPemMarker);
    if (pem == std::string::npos)
        throw std::runtime_error("--creds: no certificate marker");
    std::string token = text.substr(0, pem);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
        token.pop_back();
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
        token.erase(token.begin());
    const std::size_t quic = text.find(kQuicMarker, pem);
    const std::size_t begin = pem + std::string(kPemMarker).size();
    const std::string cert = text.substr(
        begin, quic == std::string::npos ? std::string::npos : quic - begin);
    Fingerprint fingerprint;
    std::string error;
    if (!fingerprint_pem_certificate(cert, fingerprint, error))
        throw std::runtime_error("--creds: " + error);
    options.token = token;
    options.pin_sha256 = format_fingerprint(fingerprint);
}

} // namespace

int main(int argc, char** argv)
{
    TransportOptions transport_options;
    int width = 1024, height = 700;
    int settle_ms = 2000, idle_s = 8, bulk_s = 12, probe_hz = 50,
        repaint_ms = 1000, first_frame_cap_s = 20;
    std::string label;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error("missing value for " + argument);
                return argv[i];
            };
            if (parse_transport_argument(transport_options, argument, value)) {}
            else if (argument == "--creds") load_credentials(value(), transport_options);
            else if (argument == "--width") width = std::stoi(value());
            else if (argument == "--height") height = std::stoi(value());
            else if (argument == "--settle-ms") settle_ms = std::stoi(value());
            else if (argument == "--idle-s") idle_s = std::stoi(value());
            else if (argument == "--bulk-s") bulk_s = std::stoi(value());
            else if (argument == "--probe-hz") probe_hz = std::stoi(value());
            else if (argument == "--repaint-ms") repaint_ms = std::stoi(value());
            else if (argument == "--label") label = value();
            else if (argument == "--help" || argument == "-h") {
                std::cout << "Usage: quic_loss_bench" << transport_usage()
                          << "\n  [--creds FILE] [--width PX] [--height PX]"
                             " [--settle-ms MS] [--idle-s S] [--bulk-s S]"
                             " [--probe-hz N] [--repaint-ms MS] [--label T]\n";
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + argument);
            }
        }
        if (probe_hz < 1 || probe_hz > 1000 || repaint_ms < 50)
            throw std::runtime_error("--probe-hz 1..1000, --repaint-ms >= 50");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }

    std::string error;
    std::unique_ptr<Transport> transport = make_transport(transport_options, error);
    if (transport == nullptr) {
        std::cerr << error << '\n';
        return 2;
    }

    const Clock::time_point t0 = Clock::now();
    auto now_ms = [&] { return ms_since(t0, Clock::now()); };
    TrustPolicy refuse_unknown; // pinned: nothing to decide
    if (!connect_with_broker_trust(*transport, refuse_unknown, error)) {
        std::cout << "{\"label\":\"" << json_escape(label)
                  << "\",\"ok\":false,\"error\":\"connect: " << json_escape(error)
                  << "\"}\n";
        return connect_exit_status(*transport);
    }
    const double connect_ms = now_ms();

    Session session(width, height,
                    [&](std::span<const std::uint8_t> bytes) {
                        std::string send_error;
                        return transport->send_all(bytes, send_error);
                    });
    session.start();

    // Probe coordinates: a fresh point each time, inside an area of the default
    // desktop with nothing under it (no Deskbar, no icon), so a move causes no
    // drawing beyond the echo itself.
    auto probe_point = [](std::size_t index) {
        const float x = 260.0f + static_cast<float>((index % 250) * 2);
        const float y = 220.0f + static_cast<float>(((index / 250) % 120) * 2);
        return std::make_pair(x, y);
    };

    std::vector<std::uint8_t> buffer(1 << 18);
    std::vector<Arrival> arrivals;
    std::vector<Probe> probes;
    std::map<std::pair<float, float>, std::size_t> probe_at;
    std::size_t oldest_unanswered = 0;
    std::uint64_t total_bytes = 0;
    bool closed = false;
    std::string close_reason;
    Point last_cursor = session.cursor().position;

    // One receive(); records the arrival and resolves echoes.
    auto pump = [&](int timeout_ms) -> bool {
        const int n = transport->receive(
            std::span<std::uint8_t>(buffer.data(), buffer.size()), timeout_ms, error);
        if (n < 0) {
            closed = true;
            close_reason = transport->peer_closed() ? "peer closed: " + error
                                                    : "error: " + error;
            return false;
        }
        if (n == 0)
            return true;
        const double at = now_ms();
        const std::size_t before = session.message_count();
        session.ingest(std::span<const std::uint8_t>(buffer.data(),
                                                     static_cast<std::size_t>(n)));
        total_bytes += static_cast<std::uint64_t>(n);
        int echoes = 0;
        const Point position = session.cursor().position;
        if (position.x != last_cursor.x || position.y != last_cursor.y) {
            last_cursor = position;
            const auto found = probe_at.find({position.x, position.y});
            if (found != probe_at.end()) {
                for (std::size_t j = oldest_unanswered; j <= found->second; ++j) {
                    if (probes[j].echo_ms < 0) {
                        probes[j].echo_ms = at;
                        ++echoes;
                    }
                }
                oldest_unanswered = std::max(oldest_unanswered, found->second + 1);
            }
        }
        arrivals.push_back({at, static_cast<std::size_t>(n),
                            session.message_count() - before, echoes});
        if (session.server_closed()) {
            closed = true;
            close_reason = "RP_CLOSE_CONNECTION";
            return false;
        }
        return true;
    };

    // --- phase 0: first full frame -------------------------------------------
    double last_data_ms = connect_ms;
    while (!closed) {
        const double now = now_ms();
        if (now - last_data_ms >= settle_ms || now > first_frame_cap_s * 1000.0)
            break;
        const std::size_t count = arrivals.size();
        pump(50);
        if (arrivals.size() != count)
            last_data_ms = arrivals.back().at_ms;
    }
    const double first_frame_ms = last_data_ms;
    const std::uint64_t first_frame_bytes = total_bytes;
    const std::size_t first_frame_messages = session.message_count();
    const std::size_t phase0_end = arrivals.size();

    // --- phases A and B --------------------------------------------------------
    const double interval = 1000.0 / probe_hz;
    struct Repaint {
        double requested_ms;
        double done_ms = -1;
        std::size_t bytes = 0, messages = 0;
    };
    std::vector<Repaint> repaints;
    std::size_t phase_a_begin = arrivals.size(), phase_b_begin = 0;
    std::size_t probes_a_end = 0;
    double phase_a_start = now_ms(), phase_b_start = 0, phase_b_end = 0;

    auto run_phase = [&](double start, double seconds, bool bulk) {
        double next_probe = start;
        double next_repaint = start;
        const double end = start + seconds * 1000.0;
        while (!closed) {
            double now = now_ms();
            if (now >= end)
                break;
            if (now >= next_probe) {
                const std::size_t index = probes.size();
                const auto [x, y] = probe_point(index);
                probe_at[{x, y}] = index;
                probes.push_back({now});
                if (!session.send_client_message(InputEncoder::mouse_moved(x, y))) {
                    closed = true;
                    close_reason = "send failed";
                    break;
                }
                next_probe += interval;
            }
            if (bulk && now >= next_repaint) {
                repaints.push_back({now});
                session.request_full_repaint();
                next_repaint += repaint_ms;
            }
            now = now_ms();
            double wait = std::min(next_probe, bulk ? next_repaint : end) - now;
            wait = std::clamp(wait, 0.0, 50.0);
            pump(static_cast<int>(std::ceil(wait)));
        }
    };

    run_phase(phase_a_start, idle_s, false);
    probes_a_end = probes.size();
    phase_b_begin = arrivals.size();
    phase_b_start = now_ms();
    run_phase(phase_b_start, bulk_s, true);
    phase_b_end = now_ms();
    // drain: let outstanding echoes and the last repaint land
    const double drain_until = now_ms() + 3000;
    while (!closed && now_ms() < drain_until)
        pump(50);
    const std::size_t total_end = arrivals.size();
    transport->close();

    // --- analysis --------------------------------------------------------------
    // A chunk is repaint content, not an echo, when it carried more messages
    // than echoes (the echo is a single RP_MOVE_CURSOR_TO).
    auto is_bulk = [](const Arrival& a) {
        return a.messages > static_cast<std::size_t>(a.echoes ? 1 : 0)
            && a.bytes > 64;
    };
    for (std::size_t r = 0; r < repaints.size(); ++r) {
        const double from = repaints[r].requested_ms;
        const double to = r + 1 < repaints.size() ? repaints[r + 1].requested_ms
                                                  : 1e18;
        for (std::size_t i = phase_b_begin; i < total_end; ++i) {
            const Arrival& a = arrivals[i];
            if (a.at_ms < from || a.at_ms >= to || !is_bulk(a))
                continue;
            repaints[r].done_ms = a.at_ms;
            repaints[r].bytes += a.bytes;
            repaints[r].messages += a.messages;
        }
    }

    auto gaps = [&](std::size_t begin, std::size_t end) {
        std::vector<double> out;
        for (std::size_t i = begin + 1; i < end; ++i)
            out.push_back(arrivals[i].at_ms - arrivals[i - 1].at_ms);
        return out;
    };
    // Bulk stall: gaps between consecutive arrivals while a repaint is still
    // being delivered (from its request to its last bulk chunk).
    std::vector<double> repaint_gaps;
    for (const Repaint& rp : repaints) {
        if (rp.done_ms < 0)
            continue;
        double previous = rp.requested_ms;
        for (std::size_t i = phase_b_begin; i < total_end; ++i) {
            const double at = arrivals[i].at_ms;
            if (at < rp.requested_ms)
                continue;
            if (at > rp.done_ms)
                break;
            repaint_gaps.push_back(at - previous);
            previous = at;
        }
    }

    std::vector<double> echo_a, echo_b;
    std::size_t lost_a = 0, lost_b = 0;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        const bool a = i < probes_a_end;
        if (probes[i].echo_ms < 0) {
            (a ? lost_a : lost_b)++;
            continue;
        }
        (a ? echo_a : echo_b).push_back(probes[i].echo_ms - probes[i].sent_ms);
    }
    std::vector<double> repaint_ms_values;
    std::uint64_t repaint_bytes = 0;
    std::size_t repaint_messages = 0, repaints_done = 0;
    for (const Repaint& rp : repaints) {
        if (rp.done_ms < 0)
            continue;
        ++repaints_done;
        repaint_ms_values.push_back(rp.done_ms - rp.requested_ms);
        repaint_bytes += rp.bytes;
        repaint_messages += rp.messages;
    }
    const Stats repaint_stats = stats_of(repaint_ms_values);
    std::size_t phase_b_messages = 0;
    std::uint64_t phase_b_bytes = 0;
    for (std::size_t i = phase_b_begin; i < total_end; ++i) {
        phase_b_messages += arrivals[i].messages;
        phase_b_bytes += arrivals[i].bytes;
    }
    const double phase_b_s = (phase_b_end - phase_b_start) / 1000.0;

    char head[1024];
    std::snprintf(head, sizeof(head),
                  "{\"label\":\"%s\",\"ok\":%s,\"close\":\"%s\",\"transport\":\"%s\","
                  "\"compressed\":%s,\"connect_ms\":%.1f,\"first_frame_ms\":%.1f,"
                  "\"first_frame_bytes\":%llu,\"first_frame_messages\":%zu,"
                  "\"total_bytes\":%llu,\"total_messages\":%zu,"
                  "\"bulk_bytes_per_s\":%.0f,\"bulk_messages_per_s\":%.0f,"
                  "\"repaints\":%zu,\"repaints_done\":%zu,"
                  "\"repaint_bytes_mean\":%.0f,\"repaint_messages_mean\":%.0f,"
                  "\"probes_a\":%zu,\"lost_a\":%zu,\"probes_b\":%zu,\"lost_b\":%zu,",
                  json_escape(label).c_str(),
                  (!closed || close_reason.empty()) ? "true" : "false",
                  json_escape(close_reason).c_str(),
                  json_escape(transport->describe()).c_str(),
                  session.compressed() ? "true" : "false", connect_ms,
                  first_frame_ms,
                  static_cast<unsigned long long>(first_frame_bytes),
                  first_frame_messages,
                  static_cast<unsigned long long>(total_bytes),
                  session.message_count(),
                  phase_b_s > 0 ? static_cast<double>(phase_b_bytes) / phase_b_s : 0.0,
                  phase_b_s > 0 ? static_cast<double>(phase_b_messages) / phase_b_s : 0.0,
                  repaints.size(), repaints_done,
                  repaints_done ? static_cast<double>(repaint_bytes) / repaints_done : 0.0,
                  repaints_done ? static_cast<double>(repaint_messages) / repaints_done : 0.0,
                  probes_a_end, lost_a, probes.size() - probes_a_end, lost_b);
    std::cout << head
              << "\"first_frame_gap\":" << json_stats(stats_of(gaps(0, phase0_end)))
              << ",\"gap_a\":" << json_stats(stats_of(gaps(phase_a_begin, phase_b_begin)))
              << ",\"gap_b\":" << json_stats(stats_of(gaps(phase_b_begin, total_end)))
              << ",\"repaint_gap\":" << json_stats(stats_of(repaint_gaps))
              << ",\"repaint_ms\":" << json_stats(repaint_stats)
              << ",\"echo_a\":" << json_stats(stats_of(echo_a))
              << ",\"echo_b\":" << json_stats(stats_of(echo_b)) << "}\n";
    if (closed && !close_reason.empty())
        std::cerr << "session ended early: " << close_reason << '\n';
    return closed && !close_reason.empty() ? 1 : 0;
}
