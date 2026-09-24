#pragma once

#include "haiku_remote/protocol.hpp"
#include "haiku_remote/surface.hpp"
#include "haiku_remote/text_engine.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace haiku_remote {

// The server's pointer, as RP_SET_CURSOR, RP_SET_CURSOR_VISIBLE and
// RP_MOVE_CURSOR_TO describe it. The struct is deliberately not named Cursor:
// Xlib typedefs that name at global scope, and x11_main.cpp pulls this
// namespace in wholesale.
struct CursorState {
    // Premultiplication is *inconsistent* on the wire, so the bits are kept
    // exactly as they arrived and composited as unassociated alpha -- which is
    // what the ordinary cursor actually carries, and what the HTML5 reference
    // client assumes. HWInterface.cpp:1024-1039 premultiplies only the bitmap
    // app_server synthesises for a drag (cursor over drag image);
    // HWInterface.cpp:951 and :1049 pass the untouched, unassociated-alpha
    // ServerCursor straight through, and HWInterface.cpp:600-606 then blends
    // *both* as if premultiplied. Taking the server's local blend as the rule
    // would darken every antialiased cursor edge and overflow the light ones.
    Bitmap bitmap;
    Point hotspot;
    Point position;
    bool visible = false;
    // Bumped on every RP_SET_CURSOR so a front end can rebuild a native cursor
    // object when the shape changes instead of once per frame.
    std::uint64_t generation = 0;

    [[nodiscard]] bool drawable() const
    {
        return visible && bitmap.width > 0 && bitmap.height > 0;
    }

    // HWInterface.cpp:887-889: the bitmap's top left sits at
    // position - hotspot; HWInterface.cpp:576-577 floors it.
    [[nodiscard]] IntRect bounds() const
    {
        if (bitmap.width <= 0 || bitmap.height <= 0)
            return {};
        const int left = raster_coordinate(
            std::floor(static_cast<double>(position.x) - hotspot.x), 0);
        const int top = raster_coordinate(
            std::floor(static_cast<double>(position.y) - hotspot.y), 0);
        return {left, top, left + bitmap.width - 1, top + bitmap.height - 1};
    }
};

// Blend the cursor over a 32-bit BGRA buffer, honouring visibility and the
// hotspot, and return the rect it touched (empty when nothing was drawn).
// `stride` is in bytes.
IntRect composite_cursor(const CursorState& cursor, std::span<std::uint8_t> bgra,
                         int width, int height, std::size_t stride);
IntRect composite_cursor(const CursorState& cursor, Surface& target);

class Session {
public:
    using Send = std::function<bool(std::span<const std::uint8_t>)>;
    using Log = std::function<void(std::string_view)>;

    Session(int width, int height, Send send, Log log = {});

    // Rebind the callback the session writes bytes through. Used by the
    // reconnect loop to point a persistent Session at each new connection's
    // transport; the surface and the session identity carry across, the sender
    // does not. Does not send anything itself.
    void set_sender(Send send) { send_ = std::move(send); }

    void start();
    void ingest(std::span<const std::uint8_t> bytes);
    bool send_client_message(std::span<const std::uint8_t> bytes);
    bool request_full_repaint();

    // Discard every scrap of per-session client state before a reconnect drives
    // a fresh connection through this same Session: the cached drawing states
    // (and with them every token's pattern, colours, font, transform and clip),
    // the colour-map palette, the cursor, the framer's half-read bytes, and the
    // handshake bookkeeping. Reusing any of it across a reconnect is the client
    // half of the reconnect black screen this project already diagnosed on the
    // server side -- the new connection's replay would be composited on top of
    // the previous session's stale state. The session identity (id and
    // generation) is *not* cleared: it is what lets the next RP_HELLO_ACK be
    // recognised as the same session at a new generation. Call it, then
    // start(), on every reconnect.
    void reset();

    // Ask the server to replay all drawing state (client -> server RP_RESYNC).
    // The recovery a client reaches for when it knows it has lost its place for
    // a reason the server cannot see. The server answers with a barrier and a
    // full replay. Carries the last generation we saw (0 when unknown). Gated
    // on RP_CAP_RESYNC being negotiated; a no-op otherwise, because a server
    // that did not negotiate it has no RP_RESYNC handler.
    bool request_resync();
    [[nodiscard]] Surface& surface() { return surface_; }
    [[nodiscard]] const Surface& surface() const { return surface_; }
    [[nodiscard]] std::size_t message_count() const { return message_count_; }
    // True once the server has sent RP_CLOSE_CONNECTION. That is an orderly
    // teardown, not a failure: RemoteHWInterface::_Disconnect() sends it and
    // then closes the endpoint (RemoteHWInterface.cpp:706-717), and the native
    // in-tree client answers it by quitting (RemoteView.cpp:522-526). A read
    // loop should stop, and a capture should still be written.
    [[nodiscard]] bool server_closed() const { return server_closed_; }
    [[nodiscard]] const CursorState& cursor() const { return cursor_; }
    [[nodiscard]] std::uint32_t negotiated_version() const { return negotiated_version_; }
    [[nodiscard]] std::uint32_t negotiated_capabilities() const
    {
        return negotiated_capabilities_;
    }
    // The session identity RP_HELLO_ACK carries when RP_CAP_RESYNC is
    // negotiated. session_id() is stable for the life of the server process;
    // generation() bumps on every connection. Both are 0 until an ack that
    // carried them arrives.
    [[nodiscard]] std::uint32_t session_id() const { return session_id_; }
    [[nodiscard]] std::uint32_t generation() const { return generation_; }
    // True once this Session has observed the generation advance while the
    // session id stayed the same -- i.e. a reconnect to the same server session
    // (via a new RP_HELLO_ACK generation, or an inbound RP_RESYNC barrier).
    // Content cached from before the change must be treated as stale.
    [[nodiscard]] bool generation_changed() const { return generation_changed_; }
    [[nodiscard]] const std::unordered_map<std::uint16_t, std::size_t>& unhandled() const
    {
        return unhandled_;
    }
    // Replies the server's drawing thread is *blocked* on while it waits for
    // them: RP_DRAW_STRING_RESULT, RP_STRING_WIDTH_RESULT, RP_READ_BITMAP_RESULT.
    // Each one cost app_server a full round trip of whatever link we are on
    // (RemoteDrawingEngine.cpp:1065-1109, :1154-1198, :1205-1240), so the count
    // is the multiplier on link RTT -- not a byte or CPU cost. Counting them
    // separately from message_count_ is the only way to tell "the desktop is
    // chatty" from "the server is lock-stepping with us".
    [[nodiscard]] std::size_t sync_replies() const { return sync_replies_; }
    [[nodiscard]] std::size_t draw_string_replies() const
    {
        return draw_string_replies_;
    }
    [[nodiscard]] std::size_t string_width_replies() const
    {
        return string_width_replies_;
    }

private:
    int requested_width_;
    int requested_height_;
    Send send_;
    Log log_;
    Framer framer_;
    Surface surface_;
    std::unordered_map<std::int32_t, DrawState> states_;
    std::vector<Color> palette_;
    TextEngine text_;
    CursorState cursor_;
    std::size_t message_count_ = 0;
    std::size_t sync_replies_ = 0;
    std::size_t draw_string_replies_ = 0;
    std::size_t string_width_replies_ = 0;
    std::uint32_t negotiated_version_ = 0;
    std::uint32_t negotiated_capabilities_ = 0;
    std::uint32_t session_id_ = 0;
    std::uint32_t generation_ = 0;
    bool generation_changed_ = false;
    bool server_closed_ = false;
    std::unordered_map<std::uint16_t, std::size_t> unhandled_;

    // Record a generation the server told us about (via RP_HELLO_ACK or an
    // RP_RESYNC barrier) and note whether it advanced within the same session.
    void observe_generation(std::uint32_t session_id, std::uint32_t generation);
    // Throw away the cached drawing state so a replay is not merged with it.
    // Shared by reset() (a fresh connection) and the RP_RESYNC barrier (a
    // replay on the live connection).
    void discard_drawing_state();

    void handle(const Message& message);
    void answer_after_failure(const Message& message);
    std::vector<std::uint8_t> read_bitmap_reply(std::int32_t token,
                                                IntRect requested);
    void handle_session(Op op, Reader& reader);
    void handle_token(Op op, std::int32_t token, Reader& reader);
    DrawState& state(std::int32_t token);
    bool send_message(std::vector<std::uint8_t> bytes);
    void note_unhandled(Op op);
    Bitmap read_bitmap(Reader& reader, const DrawState& draw,
                       bool minimal = false,
                       std::uint32_t inherited_color_space = 0);
};

} // namespace haiku_remote
