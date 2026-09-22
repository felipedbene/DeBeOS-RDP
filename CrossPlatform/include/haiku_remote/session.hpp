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

    void start();
    void ingest(std::span<const std::uint8_t> bytes);
    bool send_client_message(std::span<const std::uint8_t> bytes);
    bool request_full_repaint();
    [[nodiscard]] Surface& surface() { return surface_; }
    [[nodiscard]] const Surface& surface() const { return surface_; }
    [[nodiscard]] std::size_t message_count() const { return message_count_; }
    [[nodiscard]] const CursorState& cursor() const { return cursor_; }
    [[nodiscard]] std::uint32_t negotiated_version() const { return negotiated_version_; }
    [[nodiscard]] std::uint32_t negotiated_capabilities() const
    {
        return negotiated_capabilities_;
    }
    [[nodiscard]] const std::unordered_map<std::uint16_t, std::size_t>& unhandled() const
    {
        return unhandled_;
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
    std::uint32_t negotiated_version_ = 0;
    std::uint32_t negotiated_capabilities_ = 0;
    std::unordered_map<std::uint16_t, std::size_t> unhandled_;

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
