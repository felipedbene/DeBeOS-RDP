#include "haiku_remote/desktop_session.hpp"

#include "haiku_remote/input_encoder.hpp"
#include "haiku_remote/test_script.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace haiku_remote {

// --- OptionsLink -----------------------------------------------------------

OptionsLink::OptionsLink(TransportOptions options, std::unique_ptr<Transport> connected)
    : options_(std::move(options)), transport_(std::move(connected))
{
}

OptionsLink::~OptionsLink()
{
    teardown();
}

void OptionsLink::teardown()
{
    cancel_and_join();
    transport_.reset();
}

void OptionsLink::begin_connect(Session&, std::function<void()> wake)
{
    teardown();
    std::string error;
    auto transport = make_transport(options_, error);
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = false;
    ok_ = false;
    outcome_ = ConnectionOutcome::connect_failed;
    error_.clear();
    if (transport == nullptr) {
        outcome_ = ConnectionOutcome::refused; // a misinvocation: never retry
        error_ = error;
        done_ = true;
        wake();
        return;
    }
    stage_ = "connecting to " + transport->describe();
    pending_ = std::move(transport);
    // The worker owns the transport's connect; abort() (thread-safe) is what a
    // cancel uses to end it.
    worker_ = std::thread([this, wake = std::move(wake), transport = pending_.get()] {
        std::string why;
        const bool ok = connect_with_broker_trust(
            *transport, terminal_trust_policy(options_), why);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ok_ = ok;
            if (!ok) {
                ConnectionResult result;
                result.connect_failure = transport->connect_failure();
                outcome_ = classify_connection(result);
                if (result.connect_failure == ConnectFailure::broker_unknown
                    || result.connect_failure == ConnectFailure::broker_changed)
                    outcome_ = ConnectionOutcome::refused; // a decision
                error_ = "connect to " + transport->describe() + " failed: " + why;
            }
            done_ = true;
        }
        wake();
    });
}

bool OptionsLink::connect_done()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
}

std::string OptionsLink::connect_stage()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stage_;
}

bool OptionsLink::finish_connect(ConnectionOutcome& outcome, std::string& error)
{
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok_) {
        outcome = outcome_;
        error = error_;
        pending_.reset();
        return false;
    }
    transport_ = std::move(pending_);
    return true;
}

std::string OptionsLink::route() const
{
    return transport_ ? transport_->describe() : std::string();
}

void OptionsLink::cancel_and_join()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ != nullptr && !done_)
            pending_->abort();
    }
    if (worker_.joinable())
        worker_.join();
    pending_.reset();
}

// --- ProfileLink -----------------------------------------------------------

ProfileLink::ProfileLink(ConnectionProfile profile, ConnectionLibrary& library,
                         ConnectResult connected)
    : profile_(std::move(profile)), library_(library), result_(std::move(connected))
{
    mark_stderr();
}

ProfileLink::~ProfileLink()
{
    teardown();
}

void ProfileLink::teardown()
{
    // An attempt in flight first: AsyncConnect's destructor cancels it and
    // joins once its ssh child is reaped and its socket shut down.
    attempt_.reset();
    // What ssh said while this connection was up -- a channel the far end
    // refused, a dropped link -- goes to this process's stderr before the
    // tunnel goes (#86's reader thread has kept it all; nothing had to drain it
    // meanwhile).
    if (result_.connection.tunnel) {
        const std::string said = result_.connection.tunnel->messages_since(stderr_mark_);
        std::size_t at = 0;
        while (at < said.size()) {
            const std::size_t end = said.find('\n', at);
            std::cerr << "ssh: " << said.substr(at, end - at) << '\n';
            at = end + 1;
        }
    }
    // The transport, so its socket closes before the forward it runs through;
    // then the substrate, whose destructor terminates and reaps the ssh child
    // (ManagedProcess).
    result_.transport.reset();
    result_.connection = ManagedConnection {};
    result_.ok = false;
}

void ProfileLink::begin_connect(Session& session, std::function<void()> wake)
{
    teardown();
    // The handshake runs on the desktop's own Session (reset() first), so
    // "connected" means app_server answered, exactly as on the progress
    // screen, and the surface carries across the reconnect.
    ConnectOptions options;
    options.session = &session;
    options.session_log = [](std::string_view line) { std::cerr << line << '\n'; };
    attempt_.emplace(profile_, options, std::move(wake));
}

bool ProfileLink::connect_done()
{
    if (!attempt_)
        return true;
    // No trust prompt here: a broker certificate that became unknown or changed
    // since the connection was first made is not something to accept from
    // inside a reconnect loop. It fails the attempt, and the policy does not
    // retry it (outcome_for_connect_error).
    if (attempt_->trust_question())
        attempt_->answer_trust(TrustChoice::reject);
    return attempt_->finished();
}

std::string ProfileLink::connect_stage()
{
    if (!attempt_)
        return {};
    const ConnectFlow flow = attempt_->flow();
    const auto active = flow.active_stage();
    return active ? flow.stages()[*active].label : std::string();
}

bool ProfileLink::finish_connect(ConnectionOutcome& outcome, std::string& error)
{
    if (!attempt_) {
        outcome = ConnectionOutcome::refused;
        error = "no connect attempt";
        return false;
    }
    attempt_->wait();
    result_ = attempt_->take_result();
    attempt_.reset();
    if (!result_.ok || result_.transport == nullptr) {
        outcome = outcome_for_connect_error(result_.error);
        error = result_.cancelled ? "cancelled" : result_.error.summary();
        teardown();
        return false;
    }
    std::cerr << "route: " << result_.route_note << '\n';
    mark_stderr();
    return true;
}

void ProfileLink::established()
{
    library_.mark_connected(profile_.id);
    const SaveResult saved = library_.save();
    if (!saved.ok)
        std::cerr << "connection library: could not record the connection: "
                  << saved.message << '\n';
}

std::unique_ptr<Session> ProfileLink::take_session()
{
    return std::move(result_.session);
}

std::string ProfileLink::route() const
{
    return route_status(profile_, result_.connection);
}

bool ProfileLink::through_forward() const
{
    return result_.connection.tunnel != nullptr;
}

void ProfileLink::mark_stderr()
{
    stderr_mark_ = result_.connection.tunnel ? result_.connection.tunnel->stderr_mark()
                                             : 0;
}

// --- small helpers -----------------------------------------------------------

bool report_session_end(bool orderly, std::size_t messages, std::string_view reason)
{
    if (!orderly) {
        std::cerr << "receive failed: " << reason << '\n';
        return false;
    }
    if (messages == 0) {
        std::cerr << "the server closed the connection before sending any"
                     " drawing data: the session was refused or never started\n";
        return false;
    }
    std::cerr << "session ended: the server closed the connection after "
              << messages << " messages\n";
    return true;
}

std::string format_bytes(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= 10ull * 1024 * 1024)
        std::snprintf(text, sizeof text, "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof text, "%.1f KiB",
                      static_cast<double>(bytes) / 1024.0);
    return text;
}

// --- DesktopController -------------------------------------------------------

namespace {

std::unique_ptr<Session> session_or_fresh(std::unique_ptr<Session> prestarted,
                                          int width, int height)
{
    if (prestarted != nullptr)
        return prestarted;
    return std::make_unique<Session>(
        width, height, [](std::span<const std::uint8_t>) { return false; },
        [](std::string_view line) { std::cerr << line << '\n'; });
}

} // namespace

DesktopController::DesktopController(SessionLink& link, std::string name, int width,
                                     int height, ReconnectConfig reconnect,
                                     DesktopWindow& window,
                                     std::unique_ptr<Session> prestarted,
                                     std::function<void()> wake)
    : link_(link), reconnect_(reconnect), window_(window), wake_(std::move(wake)),
      first_handshaken_(prestarted != nullptr),
      session_(session_or_fresh(std::move(prestarted), width, height)),
      shell_(std::move(name), session_->surface().width(),
             session_->surface().height()),
      schedule_(reconnect), stats_since_(Clock::now()), buffer_(256 * 1024)
{
}

DesktopController::~DesktopController()
{
    link_.teardown();
}

void DesktopController::connection_ended(ConnectionOutcome outcome, std::string reason)
{
    live_ = false;
    established_ = false;
    link_.teardown();
    end_.outcome = outcome;
    end_.reason = reason;
    last_modifiers_ = 0;
    update_secure_attention();
    const auto now = Clock::now();
    if (schedule_.connection_ended(outcome, now)) {
        const auto delay
            = std::chrono::duration_cast<std::chrono::milliseconds>(schedule_.due_at() - now);
        std::cerr << "connection ended (" << reason << "); reconnecting in "
                  << delay.count() << " ms (attempt " << schedule_.attempts_made() + 1
                  << " of " << reconnect_.max_attempts << ")\n";
        return;
    }
    if (schedule_.exhausted())
        end_.reason += " -- reconnect gave up after "
            + std::to_string(schedule_.attempts_made())
            + (schedule_.attempts_made() == 1 ? " attempt" : " attempts");
    running_ = false;
}

void DesktopController::start_connection(bool first)
{
    Session& session = *session_;
    Transport* transport = link_.transport();
    // A connect that ran the RP handshake itself (connect_flow's handshake-gated
    // "connected") left the session started and acknowledged; starting it again
    // would send a second RP_INIT_CONNECTION on the same connection.
    const bool handshaken = first ? first_handshaken_ : link_.runs_handshake();
    if (!first && !handshaken)
        session.reset();
    session.set_sender([this, transport](std::span<const std::uint8_t> bytes) {
        return transport->send_all(bytes, socket_error_);
    });
    // Session::start() returns void and ignores whether the handshake went
    // out; the sender only assigns socket_error_ on failure, so an empty string
    // afterwards means every handshake message was written.
    socket_error_.clear();
    if (!handshaken)
        session.start();
    if (!socket_error_.empty()) {
        const std::string reason = "handshake send to " + transport->describe()
            + " failed: " + socket_error_;
        if (first)
            throw std::runtime_error(reason);
        std::cerr << reason << '\n';
        ConnectionResult result;
        result.connected = true;
        result.peer_closed = transport->peer_closed();
        result.connection_reset = transport->connection_reset();
        result.through_forward = link_.through_forward();
        connection_ended(classify_connection(result), reason);
        return;
    }
    live_ = true;
    established_ = false;
    end_.status = exit_status::ok;
    end_.refused_no_data = false;
    end_.reason.clear();
    shell_.set_link(DesktopShell::Link::connecting, link_.route());
    update_secure_attention();
}

void DesktopController::start()
{
    start_connection(true);
}

void DesktopController::observe_end(bool receive_failed)
{
    Transport* transport = link_.transport();
    ConnectionResult result;
    result.connected = true;
    result.message_count = session_->message_count();
    result.server_closed = session_->server_closed();
    result.peer_closed = transport->peer_closed();
    result.connection_reset = transport->connection_reset();
    result.through_forward = link_.through_forward();
    const bool orderly = result.peer_closed || result.server_closed;
    end_.refused_no_data = orderly && result.message_count == 0;
    const bool ok = report_session_end(orderly, result.message_count, socket_error_);
    end_.status = ok ? exit_status::ok : exit_status::failed;
    const ConnectionOutcome outcome = classify_connection(result);
    std::string reason(outcome_name(outcome));
    if (receive_failed && !orderly && !socket_error_.empty())
        reason += ": " + socket_error_;
    connection_ended(outcome, reason);
}

std::string DesktopController::attempt_label() const
{
    return "attempt " + std::to_string(schedule_.attempts_made()) + " of "
        + std::to_string(std::max(reconnect_.max_attempts, schedule_.attempts_made()));
}

void DesktopController::attempt_reconnect()
{
    schedule_.attempt_started();
    ++reconnects_;
    connecting_ = true;
    shell_.set_link(DesktopShell::Link::connecting, attempt_label());
    link_.begin_connect(*session_, wake_);
    test_script_reconnect_started();
}

void DesktopController::collect_reconnect()
{
    ConnectionOutcome failure = ConnectionOutcome::connect_failed;
    std::string error;
    const bool ok = link_.finish_connect(failure, error);
    connecting_ = false;
    dirty_ = true;
    if (!ok) {
        std::cerr << "reconnect failed: " << error << '\n';
        end_.status = exit_status::failed;
        connection_ended(failure, error);
        return;
    }
    start_connection(false);
    window_.reconnected();
    if (deferred_size_) {
        const DisplaySize size = *deferred_size_;
        deferred_size_.reset();
        apply_display_size(size);
    }
}

bool DesktopController::advance(Clock::time_point now)
{
    if (connecting_ && link_.connect_done()) {
        collect_reconnect();
        return true;
    }
    if (!connecting_ && schedule_.due(now)) {
        attempt_reconnect();
        return true;
    }
    return false;
}

bool DesktopController::receive(int timeout_ms)
{
    Transport* transport = link_.transport();
    const int received = transport->receive(buffer_, timeout_ms, socket_error_);
    if (received < 0) {
        observe_end(true);
        dirty_ = true;
        return true;
    }
    if (received > 0) {
        bytes_received_ += static_cast<std::size_t>(received);
        session_->ingest(std::span(buffer_.data(), static_cast<std::size_t>(received)));
        dirty_ = true;
        update_secure_attention();
    }
    // The handshake succeeded once the server sent anything back: a refused
    // candidate never gets a single message (classify_connection's refused
    // case). This is the moment a connection counts as made -- not when it
    // later ends cleanly, which a dropped session never does.
    if (!established_ && session_->message_count() > 0) {
        established_ = true;
        schedule_.established();
        link_.established();
        shell_.set_link(DesktopShell::Link::connected, link_.route());
    }
    // RP_CLOSE_CONNECTION arrives before the EOF does. Stop on it rather than
    // idling against a socket that will never say anything again.
    if (session_->server_closed()) {
        observe_end(false);
        dirty_ = true;
        return true;
    }
    return false;
}

int DesktopController::idle_wait_ms(Clock::time_point now)
{
    int timeout = 1000;
    if (connecting_) {
        // The worker's wake ends this wait at every stage.
        const std::string stage = link_.connect_stage();
        shell_.set_link(DesktopShell::Link::connecting,
                        stage.empty() ? attempt_label() : attempt_label() + ": " + stage);
        if (test_script_reconnect_tick(stage))
            timeout = 50;
    } else if (schedule_.pending()) {
        const auto delay
            = std::chrono::duration_cast<std::chrono::milliseconds>(schedule_.due_at() - now);
        // Wake on the next whole second of the countdown, so the number on the
        // bar ticks on time, or on the deadline.
        const long long to_tick = delay.count() % 1000;
        timeout = static_cast<int>(
            std::clamp<long long>(to_tick == 0 ? 1000 : to_tick, 1, 1000));
        shell_.set_link(DesktopShell::Link::waiting, reconnect_countdown(schedule_, now));
    } else {
        shell_.set_link(DesktopShell::Link::lost, end_.reason);
    }
    if (const auto settles = settler_.deadline()) {
        const auto delay
            = std::chrono::duration_cast<std::chrono::milliseconds>(*settles - now);
        timeout = std::min(timeout,
                           static_cast<int>(std::clamp<long long>(delay.count(), 1, 1000)));
    }
    return timeout;
}

void DesktopController::send(const std::vector<std::uint8_t>& message)
{
    if (!live_)
        return;
    if (!session_->send_client_message(message))
        std::cerr << "send failed: " << socket_error_ << '\n';
    // The receive path observes the dead transport and ends the connection
    // through the one place that classifies endings.
}

void DesktopController::set_modifiers(std::uint32_t modifiers)
{
    if (modifiers == last_modifiers_)
        return;
    last_modifiers_ = modifiers;
    send(InputEncoder::modifiers_changed(modifiers));
}

void DesktopController::focus_lost()
{
    last_modifiers_ = 0;
    send(InputEncoder::modifiers_changed(0));
}

void DesktopController::window_resized(Clock::time_point now)
{
    dirty_ = true;
    if (shell_.fit_to_window())
        settler_.window_resized(window_.drawable_size(), now);
}

void DesktopController::user_quit()
{
    end_.user_ended = true;
    running_ = false;
}

// A live resolution change. The session reallocates its canvas and, on a live
// connection, sends RP_UPDATE_DISPLAY_MODE; app_server then repaints the whole
// new frame. Without a connection only the local size changes, and the next
// handshake asks for it. The window's mapping follows, so input keeps landing
// on desktop pixels.
bool DesktopController::apply_display_size(DisplaySize size)
{
    if (connecting_) {
        deferred_size_ = size;
        std::cerr << "display size " << size.width << 'x' << size.height
                  << " deferred until the reconnect finishes\n";
        return false;
    }
    if (!valid_display_size(size.width, size.height)) {
        std::cerr << "display size " << size.width << 'x' << size.height
                  << " refused: each side must be 1.." << Surface::max_dimension << '\n';
        return false;
    }
    Session& session = *session_;
    if (size.width == session.display_width() && size.height == session.display_height())
        return false;
    if (live_) {
        const auto change = session.change_display_mode(size.width, size.height);
        std::cerr << "display mode " << size.width << 'x' << size.height
                  << (change == Session::DisplayModeChange::sent
                          ? " requested (RP_UPDATE_DISPLAY_MODE)"
                          : " adopted locally; the send failed")
                  << '\n';
    } else {
        session.set_display_size(size.width, size.height);
        std::cerr << "display size " << size.width << 'x' << size.height
                  << " adopted; the next connection asks for it\n";
    }
    window_.set_desktop_size(size);
    shell_.set_desktop_size(size.width, size.height);
    dirty_ = true;
    return true;
}

// The Keys row offers Ctrl+Alt+Del only while a live connection's server has
// acknowledged RP_CAP_SECURE_ATTENTION. Called wherever either can change: the
// acknowledgement arrives with some later ingest, not with the connect.
void DesktopController::update_secure_attention()
{
    shell_.set_secure_attention_available(live_ && !connecting_
                                          && secure_attention_available(*session_));
}

void DesktopController::after_events(Clock::time_point now)
{
    Session& session = *session_;
    switch (shell_.take_action()) {
    case DesktopShell::Action::none:
    case DesktopShell::Action::stats:
        break;
    case DesktopShell::Action::disconnect:
        end_.user_ended = true;
        running_ = false;
        break;
    case DesktopShell::Action::reconnect:
        std::cerr << "reconnect requested\n";
        if (live_ || connecting_) {
            // An attempt already running is cancelled and torn down (bounded:
            // cancel wakes every wait in it) before the fresh one starts.
            live_ = false;
            connecting_ = false;
            established_ = false;
            link_.teardown();
            update_secure_attention();
            dirty_ = true;
        }
        schedule_.request_now(Clock::now());
        break;
    case DesktopShell::Action::repaint:
        if (live_) {
            const RepaintLever lever = request_repaint(session);
            std::cerr << "full repaint requested ("
                      << (lever == RepaintLever::resync         ? "RP_RESYNC"
                          : lever == RepaintLever::display_mode ? "RP_UPDATE_DISPLAY_MODE"
                                                                : "send failed")
                      << ")\n";
        }
        break;
    case DesktopShell::Action::fullscreen:
        window_.toggle_fullscreen();
        dirty_ = true;
        break;
    case DesktopShell::Action::shortcut:
        if (const RemoteShortcut* chord = shell_.take_shortcut()) {
            for (const auto& message : encode_shortcut(*chord, last_modifiers_))
                send(message);
        }
        break;
    case DesktopShell::Action::secure_attention:
        if (live_) {
            const bool sent = request_secure_attention(session);
            std::cerr << (sent ? "secure attention requested (RP_SECURE_ATTENTION)\n"
                               : "secure attention not sent: the server did not"
                                 " negotiate RP_CAP_SECURE_ATTENTION, or the send"
                                 " failed\n");
        }
        break;
    case DesktopShell::Action::display_size:
        if (const auto choice = shell_.take_display_size()) {
            using Kind = DisplaySizeChoice::Kind;
            switch (choice->kind) {
            case Kind::preset:
                settler_.cancel();
                if (apply_display_size(choice->size))
                    window_.fit_window_to(choice->size);
                break;
            case Kind::window:
                settler_.cancel();
                apply_display_size(window_.drawable_size());
                break;
            case Kind::fit:
                if (shell_.fit_to_window())
                    apply_display_size(window_.drawable_size());
                else
                    settler_.cancel();
                break;
            }
        }
        break;
    }

    now = Clock::now();
    if (shell_.fit_to_window()) {
        if (const auto settled = settler_.take(shell_.desktop_size(), now))
            apply_display_size(*settled);
    }

    now = Clock::now();
    shell_.tick(now);
    refresh_stats(now);
}

void DesktopController::refresh_stats(Clock::time_point now)
{
    if (now - stats_since_ < std::chrono::milliseconds(500))
        return;
    const double seconds = std::chrono::duration<double>(now - stats_since_).count();
    frame_rate_ = static_cast<double>(frames_) / seconds;
    frames_ = 0;
    stats_since_ = now;
    if (!shell_.stats_visible() || connecting_)
        return;
    Session& session = *session_;
    char line[96];
    std::vector<std::string> lines;
    lines.push_back("messages: " + std::to_string(session.message_count()));
    lines.push_back("received: " + format_bytes(bytes_received_));
    if (session.compressed()) {
        const auto wire = session.compressed_wire_bytes();
        const auto plain = session.compressed_plain_bytes();
        std::snprintf(line, sizeof line, "zstd: %s -> %s (%.1fx)",
                      format_bytes(wire).c_str(), format_bytes(plain).c_str(),
                      wire == 0 ? 0.0
                                : static_cast<double>(plain) / static_cast<double>(wire));
        lines.emplace_back(line);
    } else {
        lines.emplace_back("compression: off");
    }
    lines.push_back("blocking replies: " + std::to_string(session.sync_replies()));
    std::snprintf(line, sizeof line, "frames/s: %.1f", frame_rate_);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "session %08x  generation %u  reconnects %d",
                  session.session_id(), session.generation(), reconnects_);
    lines.emplace_back(line);
    shell_.set_stats_lines(std::move(lines));
}

bool DesktopController::needs_present() const
{
    const bool overlay_changed
        = shell_.dirty() && (shell_.visible() || shell_.stats_visible());
    return surface_changed() || overlay_changed || shell_.visible() != shell_was_visible_
        || shell_.stats_visible() != stats_were_visible_;
}

void DesktopController::presented()
{
    if (!connecting_)
        dirty_ = false;
    if (shell_.visible() || shell_.stats_visible())
        shell_.mark_clean();
    shell_was_visible_ = shell_.visible();
    stats_were_visible_ = shell_.stats_visible();
    ++frames_;
}

DesktopEnd DesktopController::finish()
{
    link_.teardown();
    return end_;
}

// --- the app flow -------------------------------------------------------------

int run_endpoint(FrontEnd& front_end, const TransportOptions& options, DesktopSize size)
{
    std::string socket_error;
    auto transport = make_transport(options, socket_error);
    if (transport == nullptr) {
        std::cerr << socket_error << '\n';
        return exit_status::failed;
    }
    if (!connect_with_broker_trust(*transport, terminal_trust_policy(options),
                                   socket_error)) {
        std::cerr << "connect to " << transport->describe() << " failed: "
                  << socket_error << '\n';
        // See exit_status_usage(): a credential this client was never given is
        // its own exit code, so a harness can tell a local misinvocation from a
        // refusal out on the wire.
        return connect_exit_status(*transport);
    }
    const std::string name = transport->describe();
    OptionsLink link(options, std::move(transport));
    try {
        // A user-initiated close keeps the status of the connection it closed:
        // ok, unless that connection had already failed.
        return front_end.run_desktop(size, link, name, {}).status;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return exit_status::usage;
    }
}

int run_profile_flow(FrontEnd& front_end, LaunchArguments& launch, LaunchTarget target,
                     DesktopSize size)
{
    ConnectionLibrary library = launch.profiles_file.empty()
        ? ConnectionLibrary()
        : ConnectionLibrary(ProfileStore(launch.profiles_file));
    const LoadResult load = library.load();
    if (load.status == LoadStatus::recovered)
        std::cerr << "connection library: " << load.message << '\n';

    // --profile / --ssh-host: connect that profile now, bypassing the library,
    // through the same progress screen and managed connect a library pick uses;
    // the run ends with the session (issue #1: explicit command-line targets
    // "connect immediately").
    std::optional<ConnectionProfile> immediate;
    if (target == LaunchTarget::saved_profile) {
        const ConnectionProfile* found = find_profile(library.profiles(), launch.profile);
        if (found == nullptr) {
            std::cerr << "no profile named \"" << launch.profile << "\" in "
                      << library.store().file().string() << '\n';
            return exit_status::usage;
        }
        immediate = *found;
    } else if (target == LaunchTarget::ssh_profile) {
        immediate = ssh_launch_profile(launch, size.width, size.height);
    }
    int immediate_status = exit_status::failed;

    for (;;) {
        auto picked = immediate ? immediate : front_end.run_library(library);
        if (!picked)
            return exit_status::ok;

        // Connect with a visible progress screen, then run the desktop. The
        // ManagedConnection the connect produced owns any ssh child (the tunnel
        // route); it moves into the ProfileLink with the transport, which tears
        // both down when a connection ends or the desktop closes, before it
        // builds a replacement -- so the ssh child is never orphaned and two
        // forwards are never up at once. A connect failure shows an actionable
        // error with Back/Retry instead of a black window; Retry re-runs the
        // connect for the same profile, Back returns to the library. Either way
        // the failure is recorded on the library card too.
        for (;;) {
            ConnectOutcome outcome = front_end.run_connect(*picked);
            if (outcome.cancelled) {
                // The user stopped it: not a failure of the host, so the card's
                // last error is left as it was. For an immediate launch this is
                // the user ending the run, like closing the window of a session.
                std::cerr << "connect to '" << picked->name << "' cancelled\n";
                immediate_status = exit_status::ok;
                break;
            }
            if (outcome.result.ok) {
                std::cerr << "route: " << outcome.result.route_note << '\n';
                DesktopSize desktop = size;
                if (!(immediate && size.explicit_size)) {
                    desktop.width = picked->width;
                    desktop.height = picked->height;
                }
                desktop.explicit_size = true;
                const std::string name = picked->name.empty() ? picked->host : picked->name;
                // The link stamps lastConnectedAt the moment the server answers
                // the handshake, so a session that later drops still records
                // its success.
                ProfileLink link(*picked, library, std::move(outcome.result));
                std::optional<DesktopEnd> end;
                try {
                    end = front_end.run_desktop(desktop, link, name,
                                                reconnect_config_for(*picked));
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    library.set_last_error(picked->id, error.what());
                }
                immediate_status = !end || end->refused_no_data ? exit_status::failed
                                                                : end->status;
                if (end && end->refused_no_data) {
                    const ConnectError e = classify_connect_failure(
                        ConnectFailPoint::session_refused, ConnectFailure::none,
                        "the server closed the connection before sending any drawing");
                    library.set_last_error(picked->id, e.summary());
                    std::cerr << "session for '" << picked->name
                              << "' was refused: " << e.summary() << '\n';
                } else if (end && end->status != exit_status::ok && !end->reason.empty()) {
                    library.set_last_error(picked->id, "Connection lost: " + end->reason);
                    std::cerr << "session for '" << picked->name
                              << "' ended: " << end->reason << '\n';
                }
                const SaveResult saved = library.save();
                if (!saved.ok)
                    std::cerr << "connection library: could not save: " << saved.message
                              << '\n';
                break;
            }

            library.set_last_error(picked->id, outcome.result.error.summary());
            std::cerr << "could not connect '" << picked->name
                      << "': " << outcome.result.error.raw << '\n';
            if (const SaveResult saved = library.save(); !saved.ok)
                std::cerr << "connection library: could not save: " << saved.message
                          << '\n';
            if (outcome.action == ConnectScreen::Action::retry)
                continue;
            immediate_status
                = outcome.result.error.cause == ConnectErrorCause::missing_credential
                ? exit_status::no_credential
                : exit_status::failed;
            break;
        }
        if (immediate)
            return immediate_status;
        // SIGTERM/SIGINT (or the system) asked the app to quit: the session and
        // its tunnel are torn down already -- never the library again.
        if (front_end.quit_requested())
            return exit_status::ok;
    }
}

} // namespace haiku_remote
