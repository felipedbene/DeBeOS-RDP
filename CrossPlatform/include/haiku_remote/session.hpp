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
    // True once the server has sent RP_CLOSE_CONNECTION. That is an orderly
    // teardown, not a failure: RemoteHWInterface::_Disconnect() sends it and
    // then closes the endpoint (RemoteHWInterface.cpp:706-717), and the native
    // in-tree client answers it by quitting (RemoteView.cpp:522-526). A read
    // loop should stop, and a capture should still be written.
    [[nodiscard]] bool server_closed() const { return server_closed_; }
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
    std::size_t message_count_ = 0;
    std::uint32_t negotiated_version_ = 0;
    std::uint32_t negotiated_capabilities_ = 0;
    bool server_closed_ = false;
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
