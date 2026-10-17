#include "haiku_remote/connect_flow.hpp"

#include "haiku_remote/connection_profile.hpp"
#include "haiku_remote/protocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace haiku_remote {

namespace {

bool has(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

const char* label_for(StageKind kind)
{
    switch (kind) {
    case StageKind::contacting_broker:
        return "Contacting broker";
    case StageKind::authenticating:
        return "Authenticating with broker";
    case StageKind::fetching_cookie:
        return "Fetching session cookie";
    case StageKind::starting_tunnel:
        return "Starting SSH tunnel";
    case StageKind::waiting_for_forward:
        return "Waiting for the SSH forward";
    case StageKind::connecting_transport:
        return "Connecting to the session port";
    case StageKind::waiting_for_protocol_handshake:
        return "Waiting for the server handshake";
    case StageKind::requesting_display:
        return "Requesting the display";
    case StageKind::fetching_fonts:
        return "Fetching the server's fonts";
    }
    return "";
}

// Distinguishes two runs of the same route kind in one plan by how they
// travel. A quic:// broker step is still RouteKind::broker.
std::string route_flavour(const RouteStep& step)
{
    if (step.transport.url.rfind("quic://", 0) == 0)
        return " (QUIC)";
    return "";
}

std::string route_display_name(const RouteStep& step)
{
    if (step.kind == RouteKind::tunnel)
        return "SSH tunnel";
    if (step.kind == RouteKind::direct)
        return "Direct TCP";
    return step.transport.url.rfind("quic://", 0) == 0 ? "Broker over QUIC"
                                                       : "Broker over WSS";
}

StageKind stage_for_phase(ConnectPhase phase)
{
    switch (phase) {
    case ConnectPhase::contacting_broker:
        return StageKind::contacting_broker;
    case ConnectPhase::fetching_cookie:
        return StageKind::fetching_cookie;
    case ConnectPhase::opening_tunnel:
        return StageKind::starting_tunnel;
    case ConnectPhase::waiting_for_forward:
        return StageKind::waiting_for_forward;
    }
    return StageKind::connecting_transport;
}

// fetch_broker_credentials / fetch_session_cookie report a non-zero exit as
// "ssh to <host> exited <N>". ssh(1): "ssh exits with the exit status of the
// remote command or with 255 if an error occurred." So any N but 255 is the
// *remote command's* status: ssh itself worked.
std::optional<int> remote_exit_status(const std::string& message)
{
    const std::string marker = " exited ";
    const std::size_t at = message.rfind(marker);
    if (at == std::string::npos)
        return std::nullopt;
    const std::string digits = message.substr(at + marker.size());
    if (digits.empty()
        || !std::all_of(digits.begin(), digits.end(),
                        [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::atoi(digits.c_str());
}

constexpr const char* ssh_by_hand_remedy =
    "Check the SSH user, identity file and host name/port, and that you can"
    " ssh to it by hand. Then Retry.";

// Title/detail/remedy for a cause classify_ssh_stderr() returned. `during`
// names what SSH was doing ("contacting the broker", "opening the tunnel").
void describe_ssh_cause(ConnectError& e, ConnectErrorCause cause,
                        const std::string& during)
{
    e.cause = cause;
    switch (cause) {
    case ConnectErrorCause::ssh_not_found:
        e.title = "SSH is not installed";
        e.detail = "The ssh program could not be run, so nothing was attempted.";
        e.remedy = "Install the OpenSSH client and make sure ssh is on PATH."
                   " Then Retry.";
        break;
    case ConnectErrorCause::identity_missing:
        e.title = "Identity file not found";
        e.detail = "The profile's SSH identity file does not exist, so SSH had"
                   " no key to offer while " + during + ".";
        e.remedy = "Fix the identity file path in the profile (or clear it to"
                   " use ssh-agent and your default keys).";
        break;
    case ConnectErrorCause::identity_unreadable:
        e.title = "Identity file cannot be used";
        e.detail = "SSH refused to load the profile's identity file while "
                   + during + ": it is unreadable, in a format SSH does not"
                   " accept, or readable by other users.";
        e.remedy = "Check the file is a private key you own, then restrict it"
                   " with chmod 600. Then Retry.";
        break;
    case ConnectErrorCause::ssh_auth_failed:
        e.title = "SSH authentication failed";
        e.detail = "The host rejected every key SSH offered while " + during
                   + ".";
        e.remedy = "Check the SSH user and identity file (or that ssh-agent"
                   " holds the key), and that the key is authorized on the"
                   " host. Then Retry.";
        break;
    case ConnectErrorCause::ssh_host_key:
        e.title = "Host key verification failed";
        e.detail = "SSH refused the host's key while " + during
                   + ": it differs from the one recorded in known_hosts, or"
                   " none is recorded and strict checking is on. Someone"
                   " could be intercepting the connection, or the host was"
                   " reinstalled.";
        e.remedy = "Verify the host's key out of band. Only if it legitimately"
                   " changed, remove the old entry (ssh-keygen -R <host>) and"
                   " connect again.";
        e.can_retry = false; // a retry meets the same key.
        break;
    case ConnectErrorCause::host_not_found:
        e.title = "Host name not found";
        e.detail = "The host name does not resolve, so SSH could not start "
                   + during + ".";
        e.remedy = "Check the host name in the profile (and the VPN, if it is an"
                   " internal name). Then Retry.";
        break;
    case ConnectErrorCause::ssh_refused:
        e.title = "SSH connection refused";
        e.detail = "The host answered, but nothing accepts SSH connections on"
                   " its SSH port.";
        e.remedy = "Check the SSH port in the profile and that sshd is running"
                   " on the host. Then Retry.";
        break;
    case ConnectErrorCause::host_unreachable:
        e.title = "Host unreachable";
        e.detail = "The SSH connection to the host timed out or had no route"
                   " while " + during + ".";
        e.remedy = "Check the host name and that it is reachable (VPN up,"
                   " security group open), then Retry.";
        break;
    case ConnectErrorCause::ssh_disconnected:
        e.title = "SSH connection closed by the host";
        e.detail = "The host closed the SSH connection before it was"
                   " established, while " + during + ".";
        e.remedy = "The host's sshd may be starting up or limiting connections;"
                   " Retry in a moment.";
        break;
    case ConnectErrorCause::local_port_in_use:
        e.title = "Local port already in use";
        e.detail = "SSH could not listen on the local end of the forward:"
                   " another program holds that port.";
        e.remedy = "Set the profile's local port to automatic (0), or free the"
                   " port. Then Retry.";
        break;
    case ConnectErrorCause::server_listener:
        e.title = "Nothing listening on the remote port";
        e.detail = "SSH is up, but app_server's remote interface did not accept"
                   " the forwarded connection on the host.";
        e.remedy = "Confirm app_server's remote interface is listening on the"
                   " remote port. Then Retry.";
        break;
    default:
        e.title = "SSH connection failed";
        e.detail = "SSH failed while " + during + ".";
        e.remedy = ssh_by_hand_remedy;
        break;
    }
}

// ssh failed and its stderr did not name a cause this recognises: say so
// honestly, and show what it did say.
void describe_ssh_unknown(ConnectError& e, const std::string& during)
{
    e.cause = ConnectErrorCause::ssh_failed;
    e.title = "SSH connection failed";
    e.detail = "SSH failed while " + during
               + (e.ssh_stderr.empty()
                      ? std::string(" and printed nothing to say why.")
                      : std::string(". What it printed is below."));
    e.remedy = ssh_by_hand_remedy;
}

} // namespace

const char* stage_kind_name(StageKind kind)
{
    switch (kind) {
    case StageKind::contacting_broker:
        return "contacting_broker";
    case StageKind::authenticating:
        return "authenticating";
    case StageKind::fetching_cookie:
        return "fetching_cookie";
    case StageKind::starting_tunnel:
        return "starting_tunnel";
    case StageKind::waiting_for_forward:
        return "waiting_for_forward";
    case StageKind::connecting_transport:
        return "connecting_transport";
    case StageKind::waiting_for_protocol_handshake:
        return "waiting_for_protocol_handshake";
    case StageKind::requesting_display:
        return "requesting_display";
    case StageKind::fetching_fonts:
        return "fetching_fonts";
    }
    return "";
}

const char* flow_state_name(FlowState state)
{
    switch (state) {
    case FlowState::idle:
        return "idle";
    case FlowState::connecting:
        return "connecting";
    case FlowState::connected:
        return "connected";
    case FlowState::reconnecting:
        return "reconnecting";
    case FlowState::failed:
        return "failed";
    case FlowState::disconnecting:
        return "disconnecting";
    }
    return "";
}

std::vector<StageKind> route_stages(const RouteStep& step)
{
    if (step.kind == RouteKind::broker)
        return {StageKind::contacting_broker, StageKind::authenticating,
                StageKind::waiting_for_protocol_handshake,
                StageKind::requesting_display};
    // Direct TCP stands nothing up: no ssh, no forward, a cookie that is the
    // profile's own (or none, which the transport refuses locally).
    if (step.kind == RouteKind::direct)
        return {StageKind::connecting_transport,
                StageKind::waiting_for_protocol_handshake,
                StageKind::requesting_display};
    std::vector<StageKind> stages;
    // open_connection() skips the SSH cookie fetch when the profile already
    // supplied the cookie; a stage that never runs is not listed.
    if (step.transport.cookie.empty())
        stages.push_back(StageKind::fetching_cookie);
    for (const StageKind kind :
         {StageKind::starting_tunnel, StageKind::waiting_for_forward,
          StageKind::connecting_transport,
          StageKind::waiting_for_protocol_handshake,
          StageKind::requesting_display})
        stages.push_back(kind);
    return stages;
}

// ---------------------------------------------------------------------------
// ConnectError
// ---------------------------------------------------------------------------

std::string ConnectError::summary() const
{
    if (title.empty())
        return raw;
    if (remedy.empty())
        return title;
    return title + " \xE2\x80\x94 " + remedy;
}

// ---------------------------------------------------------------------------
// classify_ssh_stderr -- OpenSSH's wording -> cause
// ---------------------------------------------------------------------------

ConnectErrorCause classify_ssh_stderr(const std::string& s)
{
    if (s.empty())
        return ConnectErrorCause::none;
    // In the order ssh meets them: it has to run, resolve the name, connect,
    // verify the host key, load the identity, authenticate, then bind the
    // forward. The earliest failure is the one that stopped it; a warning ssh
    // printed on the way (a missing identity file is only a warning) matters
    // only if nothing earlier failed.
    //
    // Our own line from a failed execvp() (managed_transport.cpp).
    if (has(s, "haiku-remote: cannot execute"))
        return ConnectErrorCause::ssh_not_found;
    if (has(s, "Could not resolve hostname"))
        return ConnectErrorCause::host_not_found;
    // "ssh: connect to host H port P: Connection refused" -- the SSH port
    // itself. Not "channel N: open failed: connect failed: Connection
    // refused", which is the forward's far end.
    if (has(s, "connect to host") && has(s, "Connection refused"))
        return ConnectErrorCause::ssh_refused;
    // Win32-OpenSSH (the ssh.exe Windows ships) connects asynchronously and
    // learns of the refusal only at the banner exchange: "banner exchange:
    // Connection to UNKNOWN port -1: Connection refused" (9.5p2).
    if (has(s, "banner exchange: Connection to") && has(s, "Connection refused"))
        return ConnectErrorCause::ssh_refused;
    if (has(s, "open failed: connect failed"))
        return ConnectErrorCause::server_listener;
    if (has(s, "Connection timed out") || has(s, "Operation timed out")
        || has(s, "No route to host") || has(s, "Network is unreachable"))
        return ConnectErrorCause::host_unreachable;
    if (has(s, "REMOTE HOST IDENTIFICATION HAS CHANGED")
        || has(s, "Host key verification failed")
        || has(s, "and you have requested strict checking"))
        return ConnectErrorCause::ssh_host_key;
    // ssh warns about the identity and goes on to fail authentication; the
    // identity is the cause worth reporting.
    if (has(s, "Identity file ") && has(s, " not accessible"))
        return ConnectErrorCause::identity_missing;
    if (has(s, "Load key \"") || has(s, "UNPROTECTED PRIVATE KEY FILE"))
        return ConnectErrorCause::identity_unreadable;
    if (has(s, "Permission denied ("))
        return ConnectErrorCause::ssh_auth_failed;
    if (has(s, "Address already in use") || has(s, "cannot listen to port")
        || has(s, "Could not request local forwarding"))
        return ConnectErrorCause::local_port_in_use;
    if (has(s, "Connection closed by") || has(s, "Connection reset by")
        || has(s, "kex_exchange_identification")
        || has(s, "Received disconnect from"))
        return ConnectErrorCause::ssh_disconnected;
    return ConnectErrorCause::none;
}

// ---------------------------------------------------------------------------
// classify_connect_failure -- the cause -> message/affordance table
// ---------------------------------------------------------------------------
//
// Every branch is driven only by what the layer actually told us: the fail
// point, the ConnectFailure flag, the raw message string the layer wrote, and
// ssh's stderr. Each fail point has its own block so a new route kind adds a
// block rather than threading conditions through the others.

ConnectError classify_connect_failure(ConnectFailPoint where,
                                      ConnectFailure transport_failure,
                                      const std::string& message,
                                      const std::string& ssh_stderr)
{
    ConnectError e;
    e.raw = message;
    e.ssh_stderr = ssh_stderr;
    e.can_retry = true;

    if (message == "cancelled") {
        e.cause = ConnectErrorCause::cancelled;
        e.title = "Cancelled";
        e.detail = "The connection attempt was cancelled.";
        return e;
    }
    const ConnectErrorCause ssh_cause = classify_ssh_stderr(ssh_stderr);

    switch (where) {
    case ConnectFailPoint::preflight:
        if (has(message, "ssh executable")) {
            describe_ssh_cause(e, ConnectErrorCause::ssh_not_found, "starting");
        } else if (has(message, "does not exist")) {
            describe_ssh_cause(e, ConnectErrorCause::identity_missing,
                               "starting");
            e.detail = "The profile's SSH identity file does not exist, so"
                       " nothing was started.";
        } else {
            describe_ssh_cause(e, ConnectErrorCause::identity_unreadable,
                               "starting");
            e.detail = "The profile's SSH identity file cannot be used: it is"
                       " not a readable file, or other users can read it (SSH"
                       " refuses such a key). Nothing was started.";
        }
        break;

    case ConnectFailPoint::substrate_broker:
        if (ssh_cause != ConnectErrorCause::none) {
            describe_ssh_cause(e, ssh_cause, "contacting the broker");
        } else if (has(message, "timed out")) {
            describe_ssh_cause(e, ConnectErrorCause::host_unreachable,
                               "contacting the broker");
        } else if (has(message, "no token") || has(message, "no certificate")
                   || has(message, "certificate marker")) {
            e.cause = ConnectErrorCause::broker_unavailable;
            e.title = "Broker not available";
            e.detail = "SSH reached the host, but its remote-desktop broker did"
                       " not return a token and certificate.";
            e.remedy = "Make sure remote_broker and app_server are running on the"
                       " host, then Retry.";
        } else if (const auto rc = remote_exit_status(message);
                   rc && *rc != 255) {
            e.cause = ConnectErrorCause::broker_unavailable;
            e.title = "Broker not available";
            e.detail = "SSH worked, but the broker command on the host failed"
                       " (exit " + std::to_string(*rc) + ").";
            e.remedy = "Make sure remote_broker is installed on the host, then"
                       " Retry.";
        } else {
            describe_ssh_unknown(e, "contacting the broker");
        }
        break;

    case ConnectFailPoint::substrate_tunnel:
        if (ssh_cause != ConnectErrorCause::none) {
            describe_ssh_cause(e, ssh_cause, "opening the tunnel");
        } else if (has(message, "session_cookie")) {
            e.cause = ConnectErrorCause::cookie_unavailable;
            e.title = "No session cookie on the server";
            e.detail = "SSH worked, but app_server's per-boot session cookie could"
                       " not be read on the host -- so it is missing, or the remote"
                       " interface has not published one yet.";
            e.remedy = "Confirm app_server's remote interface is running on the host"
                       " and that the cookie file is readable by your SSH user."
                       " Then Retry.";
        } else if (has(message, "command timed out")) {
            describe_ssh_cause(e, ConnectErrorCause::host_unreachable,
                               "fetching the session cookie");
        } else if (has(message, "timed out waiting for the ssh forward")) {
            e.cause = ConnectErrorCause::forward_timeout;
            e.title = "SSH forward did not open";
            e.detail = "SSH stayed connected, but the local end of the forward"
                       " never accepted a connection before the timeout.";
            e.remedy = "The host or the link may be very slow; Retry. If it"
                       " persists, check that the host allows TCP forwarding"
                       " (AllowTcpForwarding in its sshd_config).";
        } else if (const auto rc = remote_exit_status(message);
                   rc && *rc != 255) {
            e.cause = ConnectErrorCause::cookie_unavailable;
            e.title = "No session cookie on the server";
            e.detail = "SSH worked, but reading app_server's session cookie on"
                       " the host failed (exit " + std::to_string(*rc) + ").";
            e.remedy = "Confirm app_server's remote interface is running on the"
                       " host. Then Retry.";
        } else {
            describe_ssh_unknown(e, "opening the tunnel");
        }
        break;

    case ConnectFailPoint::transport_broker:
        // QUIC's own failures arrive typed from QuicTransport (the handshake
        // saw no datagram, a stalled handshake, or the broker's
        // no_application_protocol alert), so they are read from the flag,
        // never from the message.
        if (transport_failure == ConnectFailure::quic_no_answer) {
            e.cause = ConnectErrorCause::quic_udp_blocked;
            e.title = "No answer over QUIC";
            e.detail = "Nothing came back from the broker's UDP port: UDP is"
                       " blocked somewhere between here and the host, or the"
                       " broker is not running with -q.";
            e.remedy = "Use Auto mode, which falls back to the broker over TCP"
                       " (wss), or allow UDP to the broker port (10902 unless the"
                       " profile sets brokerPort). Then Retry.";
        } else if (transport_failure == ConnectFailure::quic_alpn_refused) {
            e.cause = ConnectErrorCause::quic_alpn_mismatch;
            e.title = "Broker speaks another QUIC protocol";
            e.detail = "The broker refused this client's QUIC protocol (ALPN"
                       " debeos-urp/1): one of the two is older than the other.";
            e.remedy = "Update the client or the broker so both speak"
                       " debeos-urp/1. Auto mode reaches the same broker over wss"
                       " meanwhile.";
            e.can_retry = false; // the same ALPN is refused the same way.
        } else if (transport_failure == ConnectFailure::quic_handshake_timeout) {
            e.cause = ConnectErrorCause::quic_handshake_timeout;
            e.title = "QUIC handshake timed out";
            e.detail = "The broker answered over UDP, but the QUIC handshake did"
                       " not finish in time: a lossy path, a broker too busy to"
                       " answer, or a broker that does not speak debeos-urp/1"
                       " (OpenSSL's QUIC server refuses an unknown protocol"
                       " silently).";
            e.remedy = "Retry. Auto mode falls back to the broker over TCP (wss);"
                       " if it persists, check that client and broker are"
                       " current.";
        } else if (transport_failure == ConnectFailure::missing_credential) {
            e.cause = ConnectErrorCause::missing_credential;
            e.title = "No broker token";
            e.detail = "A broker (wss or quic) connection needs an authentication token and"
                       " none was available.";
            e.remedy = "The token is normally fetched for you; Retry, or check the"
                       " broker's token file on the host.";
        } else if (transport_failure == ConnectFailure::broker_unknown) {
            e.cause = ConnectErrorCause::broker_cert_unknown;
            e.title = "Broker certificate not trusted";
            e.detail = "The authenticity of this broker can't be established: its"
                       " certificate is not in known_brokers, and it was not"
                       " accepted.";
            e.remedy = "Compare the fingerprint below with the broker's"
                       " broker.fingerprint on the host; Retry and choose Trust if"
                       " they match.";
        } else if (transport_failure == ConnectFailure::broker_changed) {
            e.cause = ConnectErrorCause::broker_cert_changed;
            e.title = "BROKER IDENTIFICATION HAS CHANGED";
            e.detail = "The broker presented a certificate that differs from the"
                       " one recorded for this host. Someone could be intercepting"
                       " the connection, or the broker's certificate was"
                       " regenerated. The connection was refused.";
            e.remedy = "Check broker.fingerprint on the host. Only if the"
                       " certificate was legitimately regenerated, connect again"
                       " and choose to replace the stored fingerprint.";
            e.can_retry = false;
        } else if (has(message, "pin mismatch")) {
            e.cause = ConnectErrorCause::broker_cert_pin;
            e.title = "Broker certificate changed";
            e.detail = "The broker's TLS certificate does not match the pinned"
                       " fingerprint for this host.";
            e.remedy = "If the broker was reinstalled, update or clear the pinned"
                       " fingerprint. Otherwise do not trust this connection.";
            e.can_retry = false; // a second try pins the same mismatch.
        } else if (has(message, "denied the authentication token")) {
            e.cause = ConnectErrorCause::broker_token_denied;
            e.title = "Broker rejected the token";
            e.detail = "The broker denied the authentication token.";
            e.remedy = "The token may be stale; Retry to fetch a fresh one, or"
                       " check the broker's token file on the host.";
        } else if (has(message, "no session to attach")) {
            e.cause = ConnectErrorCause::server_listener;
            e.title = "No session behind the broker";
            e.detail = "The broker accepted the token but has no app_server session"
                       " to attach to.";
            e.remedy = "Start app_server's remote interface on the host, then"
                       " Retry.";
        } else if (has(message, "could not read app_server's session cookie")) {
            e.cause = ConnectErrorCause::server_listener;
            e.title = "Broker cannot read the session cookie";
            e.detail = "The broker authenticated but could not read app_server's"
                       " session cookie, so it has nothing to open a session with.";
            e.remedy = "Make sure app_server's remote interface is listening and its"
                       " cookie file is readable by the broker's user. Then Retry.";
        } else if (has(message, "TLS handshake") || has(message, "trust store")
                   || has(message, "ca-file") || has(message, "host name")
                   || has(message, "no certificate to pin")) {
            e.cause = ConnectErrorCause::broker_cert_untrusted;
            e.title = "Broker certificate not trusted";
            e.detail = "The TLS handshake with the broker could not be verified.";
            e.remedy = "Check the broker's certificate or CA, or the pinned"
                       " fingerprint. Then Retry.";
        } else if (has(message, "could not connect to any resolved address")) {
            // TcpSocket::connect() (tcp_socket.cpp) once every address failed.
            e.cause = ConnectErrorCause::tcp_failed;
            e.title = "Could not reach the broker port";
            e.detail = "The TCP connection to the broker's port failed: it is"
                       " blocked, or remote_broker is not listening.";
            e.remedy = "Check that the broker port is open to you (security"
                       " group, firewall) and remote_broker is running. Then"
                       " Retry.";
        } else if (has(message, "Name or service not known")
                   || has(message, "nodename nor servname")) {
            // getaddrinfo's gai_strerror() wording (glibc, then BSD/macOS).
            e.cause = ConnectErrorCause::host_not_found;
            e.title = "Host name not found";
            e.detail = "The broker's host name does not resolve.";
            e.remedy = "Check the host name in the profile. Then Retry.";
        } else {
            e.cause = ConnectErrorCause::other;
            e.title = "Broker connection failed";
            e.detail = message;
            e.remedy = "Retry, or inspect the broker on the host.";
        }
        break;

    case ConnectFailPoint::transport_tunnel:
        if (transport_failure == ConnectFailure::missing_credential) {
            e.cause = ConnectErrorCause::missing_credential;
            e.title = "No session cookie";
            e.detail = "The connection through the tunnel needs app_server's"
                       " per-boot cookie and none was available.";
            e.remedy = "Retry to fetch it over SSH again, or set a cookie source"
                       " on the profile.";
        } else if (ssh_cause != ConnectErrorCause::none) {
            describe_ssh_cause(e, ssh_cause, "connecting through the tunnel");
        } else {
            e.cause = ConnectErrorCause::tcp_failed;
            e.title = "TCP connection failed";
            e.detail = "The SSH forward was up, but the TCP connection to its"
                       " local end failed.";
            e.remedy = "The tunnel may have dropped; Retry.";
        }
        break;

    case ConnectFailPoint::transport_direct:
        // No ssh ran, so there is no ssh stderr to read and nothing about a
        // tunnel to say: this is one TCP connection to host:remotePort.
        if (transport_failure == ConnectFailure::missing_credential) {
            e.cause = ConnectErrorCause::missing_credential;
            e.title = "No session cookie";
            e.detail = "Direct TCP presents app_server's per-boot session cookie"
                       " itself, and the profile names no cookie source to read"
                       " it from.";
            e.remedy = "Set Cookie source to a copy of the guest's"
                       " session_cookie.<port>, or use SSH tunnel mode, which"
                       " fetches it for you. Then Retry.";
        } else if (has(message, "Name or service not known")
                   || has(message, "nodename nor servname")) {
            e.cause = ConnectErrorCause::host_not_found;
            e.title = "Host name not found";
            e.detail = "The profile's host name does not resolve.";
            e.remedy = "Check the host name in the profile. Then Retry.";
        } else {
            e.cause = ConnectErrorCause::tcp_failed;
            e.title = "Could not reach the session port";
            e.detail = "Direct TCP connects straight to the session port, with no"
                       " SSH, and nothing accepted the connection there.";
            e.remedy = "app_server listens on 127.0.0.1 only, so Direct TCP needs"
                       " that port exposed to this machine: an SSM port-forward, a"
                       " forward you run, or a client on the guest. Check it is"
                       " up, or use SSH tunnel mode. Then Retry.";
        }
        break;

    case ConnectFailPoint::handshake:
        return classify_handshake_failure(HandshakeFailure::receive_failed,
                                          RouteKind::tunnel, message, ssh_stderr);

    case ConnectFailPoint::session_refused:
        e.cause = ConnectErrorCause::session_refused;
        e.title = "Session refused by the server";
        e.detail = "The connection opened but the server closed it without sending"
                   " any drawing. A stale session cookie, or an app_server remote"
                   " interface that is not listening, both look like this.";
        e.remedy = "Reconnect to fetch a fresh cookie; if it persists, check"
                   " app_server's remote interface on the host.";
        break;
    }

    if (e.title.empty()) {
        e.cause = ConnectErrorCause::other;
        e.title = "Connection failed";
        e.detail = message;
    }
    return e;
}

ConnectError classify_handshake_failure(HandshakeFailure failure, RouteKind route,
                                        const std::string& message,
                                        const std::string& ssh_stderr)
{
    ConnectError e;
    e.raw = message;
    e.ssh_stderr = ssh_stderr;
    e.can_retry = true;
    switch (failure) {
    case HandshakeFailure::timeout:
        e.cause = ConnectErrorCause::handshake_timeout;
        e.title = "No answer from the server";
        e.detail = "The connection is open, but app_server did not acknowledge"
                   " RP_INIT_CONNECTION before the handshake timeout.";
        e.remedy = "app_server's remote interface may be stuck or the host"
                   " overloaded. Retry; if it persists, restart the remote"
                   " interface on the host.";
        break;
    case HandshakeFailure::decode:
        e.cause = ConnectErrorCause::decode_failed;
        e.title = "Could not decode the server's stream";
        e.detail = "The server answered with bytes this client cannot decode"
                   " -- a protocol mismatch, or something other than app_server"
                   " on the port.";
        e.remedy = "Check that the port is app_server's remote interface and"
                   " that this client is up to date. Then Retry.";
        break;
    case HandshakeFailure::reset:
        if (route == RouteKind::tunnel) {
            // Through ssh -L a reset before the echo is ssh, not app_server:
            // ssh accepted locally, failed to open the channel (nothing
            // listening yet -- a guest that is still booting) and closed the
            // socket with our pipelined bytes unread. app_server's own refusal
            // reaches us through the forward as a FIN, because ssh already
            // read those bytes (found live on feat/1-desktop-shell; see
            // ConnectionResult::through_forward). Retriable, not a refusal.
            describe_ssh_cause(e, ConnectErrorCause::server_listener,
                               "connecting through the tunnel");
            break;
        }
        // A reset with our pipelined bytes unread is app_server's refusal
        // signature (CLAUDE.md, "A refused connection closes with RST").
        e.cause = ConnectErrorCause::session_refused;
        e.title = "Session refused by the server";
        e.detail = "The server reset the connection before acknowledging it --"
                   " what app_server does with a stale or wrong session cookie.";
        e.remedy = "Retry to fetch a fresh cookie; if it persists, check"
                   " app_server's remote interface on the host.";
        break;
    case HandshakeFailure::closed:
    case HandshakeFailure::send_failed:
    case HandshakeFailure::receive_failed:
        if (route == RouteKind::tunnel
            && classify_ssh_stderr(ssh_stderr) == ConnectErrorCause::server_listener) {
            // ssh accepted the local connection, then could not open the
            // channel to the remote port and closed it (ssh: "channel N: open
            // failed: connect failed: Connection refused").
            describe_ssh_cause(e, ConnectErrorCause::server_listener,
                               "connecting through the tunnel");
        } else {
            e.cause = ConnectErrorCause::remote_closed;
            e.title = "The server closed the connection";
            e.detail = "The connection closed during the handshake, before"
                       " app_server acknowledged it.";
            e.remedy = "Retry; if it persists, check app_server's remote"
                       " interface on the host.";
        }
        break;
    }
    return e;
}

// ---------------------------------------------------------------------------
// DiagnosticLog
// ---------------------------------------------------------------------------

DiagnosticLog::DiagnosticLog(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)),
      start_(std::chrono::steady_clock::now())
{
}

std::string DiagnosticLog::redact(std::string line) const
{
    for (const std::string& secret : secrets_) {
        for (std::size_t at = line.find(secret); at != std::string::npos;
             at = line.find(secret, at)) {
            line.replace(at, secret.size(), "[redacted]");
            at += 10;
        }
    }
    return line;
}

void DiagnosticLog::add(const std::string& text)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_)
                        .count();
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "+%lld.%03llds ",
                  static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t start = 0;
    // One entry per line, so a multi-line message cannot evade the ring bound.
    do {
        const std::size_t nl = text.find('\n', start);
        std::string line = text.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        start = nl == std::string::npos ? text.size() + 1 : nl + 1;
        line = redact(std::move(line));
        if (line.size() > max_line_length)
            line = line.substr(0, max_line_length) + " [...]";
        lines_.push_back(stamp + line);
        if (lines_.size() > capacity_) {
            lines_.pop_front();
            ++dropped_;
        }
    } while (start <= text.size());
}

void DiagnosticLog::add_secret(const std::string& secret)
{
    if (secret.size() < min_secret_length)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(secrets_.begin(), secrets_.end(), secret) != secrets_.end())
        return;
    secrets_.push_back(secret);
    for (std::string& line : lines_)
        line = redact(std::move(line));
}

std::vector<std::string> DiagnosticLog::lines() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

std::size_t DiagnosticLog::dropped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::string DiagnosticLog::text() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    if (dropped_ > 0)
        out = "[" + std::to_string(dropped_) + " earlier lines dropped]\n";
    for (const std::string& line : lines_) {
        out += line;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
// ConnectFlow
// ---------------------------------------------------------------------------

ConnectFlow::ConnectFlow(const LaunchPlan& plan)
{
    plan_note_ = plan.note;
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const RouteStep& step = plan.steps[i];
        // Auto mode reaches the broker twice, over QUIC and then over wss;
        // the QUIC stages say so (route_flavour), so the two do not read as a
        // repeat and the fallback from one to the other is visible.
        const std::string flavour = route_flavour(step);
        for (const StageKind sk : route_stages(step)) {
            ProgressStage stage;
            stage.kind = sk;
            stage.route = step.kind;
            stage.step_index = i;
            stage.state = StageState::pending;
            stage.label = label_for(sk) + flavour;
            stage.route_name = route_display_name(step);
            stages_.push_back(std::move(stage));
        }
    }
}

std::optional<std::size_t> ConnectFlow::find_stage(std::size_t step_index,
                                                   StageKind kind) const
{
    for (std::size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].step_index == step_index && stages_[i].kind == kind)
            return i;
    return std::nullopt;
}

std::optional<std::size_t> ConnectFlow::active_stage() const
{
    for (std::size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].state == StageState::active)
            return i;
    return std::nullopt;
}

void ConnectFlow::begin_step(std::size_t index, RouteKind kind)
{
    (void)kind;
    state_ = FlowState::connecting;
    // Reaching any step after the first means the earlier route(s) failed and
    // we fell back -- the broker->tunnel fallback of direct/auto mode.
    if (index > 0)
        fell_back_ = true;
}

void ConnectFlow::enter(std::size_t index, StageKind kind)
{
    const auto target = find_stage(index, kind);
    if (!target)
        return;
    state_ = FlowState::connecting;
    // Earlier stages of the same step are now behind us.
    for (std::size_t i = 0; i < *target; ++i) {
        if (stages_[i].step_index == index
            && (stages_[i].state == StageState::active
                || stages_[i].state == StageState::pending)) {
            stages_[i].state = StageState::done;
        }
    }
    stages_[*target].state = StageState::active;
}

void ConnectFlow::add_stage(std::size_t index, StageKind kind)
{
    if (find_stage(index, kind))
        return;
    std::optional<std::size_t> last;
    for (std::size_t i = 0; i < stages_.size(); ++i)
        if (stages_[i].step_index == index)
            last = i;
    if (!last)
        return;
    ProgressStage stage;
    stage.kind = kind;
    stage.route = stages_[*last].route;
    stage.step_index = index;
    stage.state = StageState::pending;
    stage.route_name = stages_[*last].route_name;
    stage.label = std::string(label_for(kind))
        + (stage.route_name == "Broker over QUIC" ? " (QUIC)" : "");
    stages_.insert(stages_.begin() + static_cast<std::ptrdiff_t>(*last + 1),
                   std::move(stage));
}

void ConnectFlow::phase(std::size_t index, ConnectPhase p)
{
    enter(index, stage_for_phase(p));
}

void ConnectFlow::begin_transport(std::size_t index, RouteKind kind)
{
    enter(index, kind == RouteKind::broker ? StageKind::authenticating
                                           : StageKind::connecting_transport);
}

void ConnectFlow::begin_handshake(std::size_t index)
{
    enter(index, StageKind::waiting_for_protocol_handshake);
}

void ConnectFlow::request_display(std::size_t index)
{
    enter(index, StageKind::requesting_display);
}

void ConnectFlow::step_failed(std::size_t index, RouteKind kind,
                              const std::string& error)
{
    (void)kind;
    (void)error;
    // The stage running now is the one that failed; anything still pending in
    // this step is skipped (the fallback, if any, starts a fresh step).
    bool marked = false;
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i].step_index != index)
            continue;
        if (stages_[i].state == StageState::active) {
            stages_[i].state = StageState::failed;
            marked = true;
        } else if (stages_[i].state == StageState::pending) {
            stages_[i].state = StageState::skipped;
        }
    }
    if (!marked) {
        // No stage was active (defensive): fail the step's first stage.
        for (std::size_t i = 0; i < stages_.size(); ++i) {
            if (stages_[i].step_index == index) {
                stages_[i].state = StageState::failed;
                break;
            }
        }
    }
}

void ConnectFlow::connected()
{
    if (const auto active = active_stage())
        stages_[*active].state = StageState::done;
    state_ = FlowState::connected;
}

void ConnectFlow::fail(ConnectError error)
{
    if (const auto active = active_stage())
        stages_[*active].state = StageState::failed;
    error_ = std::move(error);
    state_ = FlowState::failed;
}

void ConnectFlow::begin_disconnect()
{
    // The stage that was running did not fail; it was stopped.
    if (const auto active = active_stage())
        stages_[*active].state = StageState::skipped;
    for (ProgressStage& stage : stages_)
        if (stage.state == StageState::pending)
            stage.state = StageState::skipped;
    state_ = FlowState::disconnecting;
}

void ConnectFlow::finish_disconnect()
{
    state_ = FlowState::idle;
}

std::string issue_state_name(const ConnectFlow& flow)
{
    if (flow.state() == FlowState::connecting) {
        if (const auto active = flow.active_stage())
            return stage_kind_name(flow.stages()[*active].kind);
    }
    return flow_state_name(flow.state());
}

// ---------------------------------------------------------------------------
// connect_with_progress
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t max_stderr_bytes = 16 * 1024;

// Bridges open_connection's observer callbacks into the flow, the diagnostic
// log and the repaint hook, and remembers the last step failure, the ready
// step and ssh's stderr for the phase that failed, so the driver can classify
// precisely and start the right transport-connect stage.
class FlowBridge final : public ConnectObserver {
public:
    FlowBridge(ConnectFlow& flow, const std::function<void()>& on_progress,
               CancelToken* cancel, DiagnosticLog* log)
        : flow_(flow), on_progress_(on_progress), cancel_(cancel), log_(log)
    {
    }

    void note(const std::string& line)
    {
        if (log_ != nullptr)
            log_->add(line);
    }

    // Repaint, and log the stage that is now running if it changed.
    void tick()
    {
        const auto active = flow_.active_stage();
        if (active != last_logged_ && active.has_value()) {
            const ProgressStage& stage = flow_.stages()[*active];
            note("stage " + std::string(stage_kind_name(stage.kind)) + " (route "
                 + std::to_string(stage.step_index) + "): " + stage.label);
        }
        last_logged_ = active;
        on_progress_();
    }

    void on_step_begin(std::size_t index, RouteKind kind) override
    {
        note("route " + std::to_string(index) + ": "
             + (kind == RouteKind::broker   ? "broker"
                : kind == RouteKind::direct ? "direct tcp"
                                            : "ssh tunnel"));
        flow_.begin_step(index, kind);
        tick();
    }
    void on_phase(std::size_t index, ConnectPhase phase) override
    {
        // Classification wants the stderr of the phase that failed, not of an
        // earlier phase that succeeded with a warning.
        phase_stderr.clear();
        flow_.phase(index, phase);
        tick();
    }
    void on_step_failed(std::size_t index, RouteKind kind,
                        const std::string& error) override
    {
        have_fail = true;
        fail_kind = kind;
        fail_error = error;
        fail_stderr = phase_stderr;
        note("route " + std::to_string(index) + " failed: " + error);
        flow_.step_failed(index, kind, error);
        tick();
    }
    void on_step_ready(std::size_t index, RouteKind kind) override
    {
        ready_index = index;
        ready_kind = kind;
    }
    bool wants_ssh_stderr() const override { return true; }
    void on_ssh_stderr(std::size_t index, const std::string& line) override
    {
        (void)index;
        if (phase_stderr.size() < max_stderr_bytes)
            phase_stderr += line + "\n";
        note("ssh: " + line);
    }
    void on_secret(const std::string& secret) override
    {
        if (log_ != nullptr)
            log_->add_secret(secret);
    }
    const CancelToken* cancel_token() const override { return cancel_; }

    bool have_fail = false;
    RouteKind fail_kind = RouteKind::broker;
    std::string fail_error;
    std::string fail_stderr;
    std::string phase_stderr;
    std::size_t ready_index = 0;
    RouteKind ready_kind = RouteKind::broker;

private:
    ConnectFlow& flow_;
    const std::function<void()>& on_progress_;
    CancelToken* cancel_;
    DiagnosticLog* log_;
    std::optional<std::size_t> last_logged_;
};

// The executable `name` as the spawn will run it (managed_transport.hpp), or
// empty: the preflight and the spawn search the same way.
std::string find_on_path(const std::string& name)
{
    return find_executable_on_path(name);
}

// Checks that need no network and no child process, so a missing ssh or a
// mistyped key path is reported as itself instead of as an ssh exit status.
std::optional<std::string> preflight(const ConnectionProfile& profile,
                                     const LaunchPlan& plan, bool check_ssh)
{
    // The broker's credentials and the tunnel's cookie are fetched over ssh,
    // and the tunnel is an ssh -L; a plan of direct steps runs no ssh at all,
    // so neither ssh nor the identity file is its business.
    const bool runs_ssh = std::any_of(plan.steps.begin(), plan.steps.end(),
                                      [](const RouteStep& step) {
                                          return step.kind != RouteKind::direct;
                                      });
    if (!runs_ssh)
        return std::nullopt;
    if (check_ssh && find_on_path("ssh").empty())
        return std::string("no ssh executable on PATH");
    if (!profile.identity_file.empty()) {
        namespace fs = std::filesystem;
        const std::string path = expand_user_path(profile.identity_file);
        std::error_code ec;
        if (!fs::exists(path, ec))
            return "identity file " + path + " does not exist";
        if (!fs::is_regular_file(path, ec))
            return "identity file " + path + " is not a regular file";
        std::ifstream probe(path, std::ios::binary);
        if (!probe.good())
            return "identity file " + path + " cannot be read";
#if !defined(_WIN32)
        // OpenSSH (authfile.c, sshkey_perm_ok) ignores a key file the user owns
        // that group or others can access: "UNPROTECTED PRIVATE KEY FILE".
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && st.st_uid == ::getuid()
            && (st.st_mode & 077) != 0)
            return "identity file " + path + " is accessible by other users"
                   " (ssh refuses it)";
#endif
    }
    return std::nullopt;
}

enum class HandshakeOutcome { ok, cancelled, failed };

struct HandshakeResult {
    HandshakeOutcome outcome = HandshakeOutcome::failed;
    HandshakeFailure failure = HandshakeFailure::timeout;
    std::string message;
};

// RP handshake on a connected transport: send RP_INIT_CONNECTION + RP_HELLO
// (Session::start), read until app_server echoes RP_INIT_CONNECTION
// (RemoteHWInterface.cpp:764-786), at which point the session sends
// RP_GET_SYSTEM_PALETTE and RP_UPDATE_DISPLAY_MODE (session.cpp, the
// Op::init_connection handler). The server sends no reply to the display mode
// (RemoteHWInterface.cpp:983-999 only records it), so the handshake is
// complete once that request is on the wire. Everything the server sent along
// the way has been ingested by `session`.
HandshakeResult run_handshake(Transport& transport, Session& session,
                              int timeout_ms, const CancelToken* cancel,
                              const std::function<void()>& on_display_request)
{
    HandshakeResult r;
    std::string send_error;
    bool display_sent = false;
    session.set_sender([&](std::span<const std::uint8_t> bytes) {
        const bool display = bytes.size() >= 2
            && (bytes[0] | (bytes[1] << 8))
                == static_cast<int>(Op::update_display_mode);
        if (display)
            on_display_request();
        const bool sent = transport.send_all(bytes, send_error);
        if (sent && display)
            display_sent = true;
        return sent;
    });
    const auto cancelled = [cancel] {
        return cancel != nullptr && cancel->cancelled();
    };

    session.start();
    if (!send_error.empty()) {
        r.failure = HandshakeFailure::send_failed;
        r.message = "handshake send failed: " + send_error;
        return r;
    }
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (!display_sent) {
        if (cancelled()) {
            r.outcome = HandshakeOutcome::cancelled;
            return r;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            r.failure = HandshakeFailure::timeout;
            r.message = "no RP_INIT_CONNECTION acknowledgement within "
                        + std::to_string(timeout_ms) + " ms";
            return r;
        }
        // The whole remaining time in one wait: a cancel shuts the socket
        // down (Transport::abort), which ends the wait at once.
        const int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count()) + 1;
        std::string receive_error;
        const int n = transport.receive(buffer, wait_ms, receive_error);
        if (n < 0) {
            if (cancelled()) {
                r.outcome = HandshakeOutcome::cancelled;
                return r;
            }
            r.failure = transport.connection_reset() ? HandshakeFailure::reset
                        : transport.peer_closed()     ? HandshakeFailure::closed
                                                      : HandshakeFailure::receive_failed;
            r.message = "during the handshake: " + receive_error;
            return r;
        }
        if (n > 0) {
            try {
                session.ingest(std::span<const std::uint8_t>(
                    buffer.data(), static_cast<std::size_t>(n)));
            } catch (const std::exception& error) {
                r.failure = HandshakeFailure::decode;
                r.message = std::string("protocol decode failed: ") + error.what();
                return r;
            }
        }
        if (!send_error.empty()) {
            r.failure = HandshakeFailure::send_failed;
            r.message = "handshake send failed: " + send_error;
            return r;
        }
        if (!display_sent && session.server_closed()) {
            r.failure = HandshakeFailure::closed;
            r.message = "the server sent RP_CLOSE_CONNECTION during the handshake";
            return r;
        }
    }
    r.outcome = HandshakeOutcome::ok;
    return r;
}

// After the handshake, before deciding which of the server's font files are
// missing: keep reading for RP_HELLO_ACK and, when it negotiated
// RP_CAP_FONT_IDENTITY, the RP_SET_FONT replay app_server writes right behind it
// (the RP_HELLO handler calls _ReplayFonts after sending the ack,
// RemoteHWInterface.cpp:858-897 on origin/graviton). One ordered stream, so a
// short tail after the ack is enough; a server that never acks costs the whole
// window once. Everything read is ingested exactly as the session loop would.
HandshakeResult collect_font_replay(Transport& transport, Session& session,
                                    int window_ms, const CancelToken* cancel)
{
    HandshakeResult r;
    r.outcome = HandshakeOutcome::ok;
    const auto cancelled = [cancel] {
        return cancel != nullptr && cancel->cancelled();
    };
    const auto names_fonts = [&session] {
        return (session.negotiated_capabilities() & cap_font_identity) != 0;
    };
    constexpr auto tail = std::chrono::milliseconds(200);
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(window_ms);
    bool acked = session.negotiated_version() != 0;
    if (acked) {
        if (!names_fonts())
            return r;
        deadline = std::min(deadline, std::chrono::steady_clock::now() + tail);
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        if (cancelled()) {
            r.outcome = HandshakeOutcome::cancelled;
            return r;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline || session.server_closed())
            return r;
        const int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count()) + 1;
        std::string receive_error;
        const int n = transport.receive(buffer, wait_ms, receive_error);
        if (n < 0) {
            if (cancelled()) {
                r.outcome = HandshakeOutcome::cancelled;
                return r;
            }
            r.outcome = HandshakeOutcome::failed;
            r.failure = transport.connection_reset() ? HandshakeFailure::reset
                        : transport.peer_closed()     ? HandshakeFailure::closed
                                                      : HandshakeFailure::receive_failed;
            r.message = "right after the handshake: " + receive_error;
            return r;
        }
        if (n > 0) {
            try {
                session.ingest(std::span<const std::uint8_t>(
                    buffer.data(), static_cast<std::size_t>(n)));
            } catch (const std::exception& error) {
                r.outcome = HandshakeOutcome::failed;
                r.failure = HandshakeFailure::decode;
                r.message = std::string("protocol decode failed: ") + error.what();
                return r;
            }
        }
        if (!acked && session.negotiated_version() != 0) {
            acked = true;
            if (!names_fonts())
                return r;
            deadline = std::min(deadline, std::chrono::steady_clock::now() + tail);
        }
    }
}

ConnectResult connect_impl(const ConnectionProfile& profile, ConnectFlow& flow,
                           FlowBridge& bridge, const ConnectOptions& options)
{
    ConnectResult result;
    DiagnosticLog* log = options.log;
    CancelToken* cancel = options.cancel;
    const auto cancelled = [cancel] {
        return cancel != nullptr && cancel->cancelled();
    };
    const auto finish_cancelled = [&]() {
        bridge.note("cancelled");
        flow.begin_disconnect();
        bridge.tick();
        result = ConnectResult {};
        result.cancelled = true;
        result.error = classify_connect_failure(ConnectFailPoint::preflight,
                                                ConnectFailure::none, "cancelled");
        return std::move(result);
    };
    const auto fail = [&](ConnectError error) {
        bridge.note("failed: " + error.title + " -- " + error.raw);
        result.error = std::move(error);
        flow.fail(result.error);
        bridge.tick();
        return std::move(result);
    };

    const LaunchPlan plan = plan_launch(profile);
    bridge.note("plan: " + plan.note);
    if (log != nullptr)
        for (const RouteStep& step : plan.steps) {
            log->add_secret(step.transport.cookie);
            log->add_secret(step.transport.token);
        }
    const std::size_t step_count = plan.steps.size();

    const bool check_ssh = options.check_ssh_executable.value_or(
        options.runner == nullptr);
    if (const auto problem = preflight(profile, plan, check_ssh)) {
        if (!plan.steps.empty()) {
            flow.begin_step(0, plan.steps[0].kind);
            flow.enter(0, route_stages(plan.steps[0]).front());
        }
        return fail(classify_connect_failure(ConnectFailPoint::preflight,
                                             ConnectFailure::none, *problem));
    }

    // One pass per route the plan still has. open_connection() brings up a
    // route's substrate and stops; the transport connect happens here, so a
    // route whose substrate came up can still fail -- a broker whose token and
    // certificate arrived over SSH but whose port is blocked or whose daemon is
    // down. Auto mode's plan is [broker, tunnel], and such a failure has to
    // fall back to the tunnel exactly as a failed credential fetch does inside
    // open_connection().
    std::size_t first_step = 0;
    for (;;) {
        if (cancelled())
            return finish_cancelled();
        ManagedConnection conn =
            open_connection(profile, options.runner, &bridge, first_step);
        if (!conn.ok) {
            if (cancelled())
                return finish_cancelled();
            const ConnectFailPoint where =
                bridge.have_fail && bridge.fail_kind == RouteKind::broker
                    ? ConnectFailPoint::substrate_broker
                    : ConnectFailPoint::substrate_tunnel;
            const std::string msg = bridge.have_fail ? bridge.fail_error : conn.error;
            return fail(classify_connect_failure(where, ConnectFailure::none, msg,
                                                 bridge.fail_stderr));
        }
        if (log != nullptr) {
            log->add_secret(conn.transport.token);
            log->add_secret(conn.transport.cookie);
        }
        bridge.note(conn.note);

        const std::size_t index = bridge.ready_index;
        const RouteKind kind = conn.kind;
        const std::size_t next = index + 1;
        const bool can_fall_back = kind == RouteKind::broker && next < step_count;
        const ConnectFailPoint transport_point
            = kind == RouteKind::broker   ? ConnectFailPoint::transport_broker
            : kind == RouteKind::direct ? ConnectFailPoint::transport_direct
                                        : ConnectFailPoint::transport_tunnel;
        // A broker route that came up but cannot be used still leaves the
        // plan's next route to try.
        const auto fall_back = [&](const std::string& error) {
            bridge.have_fail = true;
            bridge.fail_kind = RouteKind::broker;
            bridge.fail_error = error;
            bridge.note("route " + std::to_string(index) + " failed: " + error);
            flow.step_failed(index, RouteKind::broker, error);
            bridge.tick();
            first_step = next;
        };

        // The tunnel's stderr is the only place ssh reports a forward whose far
        // end refused, which surfaces as the connection closing. What ssh said
        // from here until the handshake is done is read back from the tunnel's
        // transcript (#86's reader thread keeps it), logged, and given to the
        // classifier -- no sink, nothing to drain.
        const std::size_t stderr_mark = conn.tunnel ? conn.tunnel->stderr_mark() : 0;
        const auto ssh_said = [&]() -> std::string {
            if (!conn.tunnel)
                return {};
            const std::string lines = conn.tunnel->messages_since(stderr_mark);
            std::size_t at = 0;
            while (at < lines.size()) {
                const std::size_t end = lines.find('\n', at);
                bridge.note("ssh: " + lines.substr(at, end - at));
                at = end + 1;
            }
            return lines;
        };

        flow.begin_transport(index, kind);
        bridge.tick();

        std::string error;
        auto transport = make_transport(conn.transport, error);
        if (transport == nullptr) {
            // e.g. a build without OpenSSL has no wss transport at all.
            if (can_fall_back) {
                fall_back(error);
                continue;
            }
            return fail(classify_connect_failure(transport_point,
                                                 ConnectFailure::other, error));
        }
        CancelToken::Hook abort_hook;
        if (cancel != nullptr)
            abort_hook = cancel->on_cancel([t = transport.get()] { t->abort(); });

        // The prompt repaints after it returns so the progress view comes back.
        TrustPolicy policy;
        policy.accept_new = conn.transport.trust_new_broker;
        if (options.ask_trust) {
            policy.prompt = [&](const BrokerCheck& check) {
                bridge.note("asking about the broker certificate");
                const TrustChoice choice = options.ask_trust(check);
                bridge.tick();
                return choice;
            };
        }
        if (!connect_with_broker_trust(*transport, policy, error)) {
            if (cancelled())
                return finish_cancelled();
            const ConnectFailure failure = transport->connect_failure();
            // A certificate the user did not (or could not) accept is a
            // decision about this host, never a reason to quietly try another
            // route: falling back would turn a changed-certificate warning
            // into a silent success.
            const bool trust_decision = failure == ConnectFailure::broker_unknown
                || failure == ConnectFailure::broker_changed;
            if (can_fall_back && !trust_decision) {
                fall_back(error);
                continue;
            }
            return fail(classify_connect_failure(transport_point, failure, error,
                                                 ssh_said()));
        }
        bridge.note("transport connected: " + transport->describe());

        // The RP handshake. A failure here is terminal even in auto mode: both
        // routes end at the same app_server, so the tunnel would meet the same
        // server that just failed to answer.
        flow.begin_handshake(index);
        bridge.tick();
        Session* session = options.session;
        std::unique_ptr<Session> owned;
        if (session != nullptr) {
            session->reset();
        } else {
            const int width = options.width > 0 ? options.width : profile.width;
            const int height = options.height > 0 ? options.height : profile.height;
            owned = std::make_unique<Session>(
                width, height, [](std::span<const std::uint8_t>) { return false; },
                options.session_log);
            session = owned.get();
        }
        const HandshakeResult hs = run_handshake(
            *transport, *session, options.handshake_timeout_ms, cancel, [&] {
                flow.request_display(index);
                bridge.tick();
            });
        // From here the session's sender must not refer to this frame.
        session->set_sender([t = transport.get()](std::span<const std::uint8_t> b) {
            std::string ignored;
            return t->send_all(b, ignored);
        });
        if (hs.outcome == HandshakeOutcome::cancelled
            || (hs.outcome == HandshakeOutcome::failed && cancelled()))
            return finish_cancelled();
        if (hs.outcome == HandshakeOutcome::failed)
            return fail(classify_handshake_failure(hs.failure, kind, hs.message,
                                                   ssh_said()));
        if (session->negotiated_version() != 0)
            bridge.note("handshake: URP/" + std::to_string(session->negotiated_version())
                        + (session->compressed() ? ", zstd" : ""));
        else
            bridge.note("handshake: RP_INIT_CONNECTION acknowledged");

        // The server's own font files (font_fetch.hpp). Only where this route
        // reached the guest over SSH -- the tunnel, or the broker's credential
        // round trip -- and over exactly that credential; a direct route runs
        // no ssh and never fetches. Nothing is asked of the user: the files
        // are verified against the checksum the server announced, capped in
        // size, and stored as data.
        session->set_font_wanted_handler({});
        const bool fetch_fonts = options.fetch_fonts.value_or(
            font_fetch_enabled_by_environment(std::getenv("HAIKU_REMOTE_FONT_FETCH")));
        if (conn.ssh && fetch_fonts) {
            const HandshakeResult replay = collect_font_replay(
                *transport, *session, options.font_replay_ms, cancel);
            if (replay.outcome == HandshakeOutcome::cancelled
                || (replay.outcome == HandshakeOutcome::failed && cancelled()))
                return finish_cancelled();
            if (replay.outcome == HandshakeOutcome::failed)
                return fail(classify_handshake_failure(replay.failure, kind,
                                                       replay.message, ssh_said()));
            const Session::Log session_log = options.session_log;
            auto fetcher = std::make_shared<FontFetcher>(
                *conn.ssh,
                options.font_cache_dir.empty() ? font_cache_directory()
                                               : options.font_cache_dir,
                FontFetchLimits {}, options.runner,
                [session_log](const std::string& line) {
                    if (session_log)
                        session_log(line);
                });
            const std::vector<FontIdentity> wanted = session->wanted_fonts();
            if (!wanted.empty()) {
                flow.add_stage(index, StageKind::fetching_fonts);
                flow.enter(index, StageKind::fetching_fonts);
                bridge.tick();
                bridge.note("fonts: the server names " + std::to_string(wanted.size())
                            + " font file(s) this host does not have; fetching"
                              " them over SSH from " + conn.ssh->host);
                const FontFetchResult fetched = fetcher->fetch(wanted, cancel);
                if (cancelled())
                    return finish_cancelled();
                if (!fetched.error.empty())
                    bridge.note("fonts: fetch failed: " + fetched.error);
                for (const auto& path : fetched.stored)
                    bridge.note("fonts: stored " + path.string());
                for (const auto& problem : fetched.problems)
                    bridge.note("fonts: " + problem);
                // Whatever arrived replaces the stand-in faces, and the server
                // repaints what was already drawn with them.
                (void)session->refresh_fonts();
            }
            const std::weak_ptr<FontFetcher> weak = fetcher;
            session->set_font_wanted_handler([weak](const FontIdentity& identity) {
                if (const auto live = weak.lock())
                    live->want(identity);
            });
            result.fonts = std::move(fetcher);
        }

        abort_hook.reset();
        (void)ssh_said(); // into the log, as on a failure
        flow.connected();
        bridge.note("connected");
        bridge.tick();
        result.ok = true;
        result.route_note = conn.note;
        result.transport = std::move(transport);
        result.session = std::move(owned);
        result.connection = std::move(conn);
        return result;
    }
}

} // namespace

ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    const ConnectOptions& options)
{
    const std::function<void()> tick =
        on_progress ? on_progress : std::function<void()>([] {});
    FlowBridge bridge(flow, tick, options.cancel, options.log);
    try {
        return connect_impl(profile, flow, bridge, options);
    } catch (const std::exception& error) {
        ConnectResult result;
        result.error.cause = ConnectErrorCause::other;
        result.error.title = "Connection failed";
        result.error.detail = error.what();
        result.error.raw = error.what();
        flow.fail(result.error);
        bridge.note(std::string("failed: ") + error.what());
        tick();
        return result;
    }
}

ConnectResult connect_with_progress(const ConnectionProfile& profile,
                                    ConnectFlow& flow,
                                    const std::function<void()>& on_progress,
                                    CommandRunner* runner,
                                    const TrustPrompt& ask_trust)
{
    ConnectOptions options;
    options.runner = runner;
    options.ask_trust = ask_trust;
    return connect_with_progress(profile, flow, on_progress, options);
}

// ---------------------------------------------------------------------------
// AsyncConnect
// ---------------------------------------------------------------------------

AsyncConnect::AsyncConnect(ConnectionProfile profile, ConnectOptions options,
                           std::function<void()> wake)
    : wake_(std::move(wake)),
      start_(std::chrono::steady_clock::now()),
      shared_flow_(plan_launch(profile)),
      worker_(&AsyncConnect::run, this, std::move(profile), std::move(options))
{
}

AsyncConnect::~AsyncConnect()
{
    cancel();
    if (worker_.joinable())
        worker_.join();
}

void AsyncConnect::cancel()
{
    cancel_.cancel();
    trust_cv_.notify_all();
}

bool AsyncConnect::cancel_requested() const
{
    return cancel_.cancelled();
}

bool AsyncConnect::finished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

void AsyncConnect::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this] { return finished_; });
}

ConnectFlow AsyncConnect::flow() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return shared_flow_;
}

std::chrono::milliseconds AsyncConnect::elapsed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto end = finished_ ? end_ : std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start_);
}

std::optional<BrokerCheck> AsyncConnect::trust_question() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return question_;
}

void AsyncConnect::answer_trust(TrustChoice choice)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!question_)
            return;
        answer_ = choice;
    }
    trust_cv_.notify_all();
}

ConnectResult AsyncConnect::take_result()
{
    wait();
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(result_);
}

void AsyncConnect::run(ConnectionProfile profile, ConnectOptions options)
{
    ConnectFlow flow(plan_launch(profile));
    const auto publish = [&] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shared_flow_ = flow;
        }
        if (wake_)
            wake_();
    };
    options.cancel = &cancel_;
    options.log = &log_;
    // The trust question crosses to the UI thread and the answer comes back;
    // a cancel answers it with a refusal.
    options.ask_trust = [this, &publish](const BrokerCheck& check) {
        std::unique_lock<std::mutex> lock(mutex_);
        question_ = check;
        answer_.reset();
        lock.unlock();
        publish();
        lock.lock();
        trust_cv_.wait(lock, [this] {
            return answer_.has_value() || cancel_.cancelled();
        });
        const TrustChoice choice =
            answer_.has_value() ? *answer_ : TrustChoice::reject;
        question_.reset();
        answer_.reset();
        return choice;
    };

    ConnectResult result = connect_with_progress(profile, flow, publish, options);
    if (result.ok && cancel_.cancelled()) {
        // Connected just as the user cancelled: the cancel wins. Tear down here,
        // on the worker, so finished() still means "nothing left running".
        flow.begin_disconnect();
        publish();
        result = ConnectResult {};
        result.cancelled = true;
    }
    if (result.cancelled) {
        // Everything the attempt started was released inside
        // connect_with_progress (the ManagedConnection and transport went out of
        // scope there): the tunnel's ssh is reaped by now.
        flow.finish_disconnect();
        log_.add("cancelled; nothing left running");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shared_flow_ = flow;
        result_ = std::move(result);
        finished_ = true;
        end_ = std::chrono::steady_clock::now();
    }
    done_cv_.notify_all();
    if (wake_)
        wake_();
}

} // namespace haiku_remote
