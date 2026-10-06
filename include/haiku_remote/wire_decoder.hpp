#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace haiku_remote {

// True when this build can decode RP_CAP_COMPRESS_ZSTD, i.e. was compiled with
// HAIKU_REMOTE_HAVE_ZSTD against libzstd. A build without it must never
// advertise the capability (see cap_compress_zstd in protocol.hpp).
[[nodiscard]] bool zstd_decoder_available();

// Undoes the server's URP/1 segment layer, the transport record format that sits
// *below* RP message framing on the server -> client direction once
// RP_CAP_COMPRESS_ZSTD is negotiated. Transcribed from the server's encoder and
// framing header, not from another client:
//
//   header   varint (little-endian base 128), value = (payloadLength << 1) | raw
//   payload  payloadLength bytes
//
//   raw 0 -- a fragment of ONE session-long zstd stream. Feeding every compressed
//            payload, in order, to one decompression context reproduces the
//            plain RP stream (RemoteWireFormat.h:20-38). The server flushes only
//            on message boundaries but batches several messages per flush
//            (RemoteWireFormat.h:30-38), so the output is a byte stream for the
//            Framer, never assumed to end on a frame.
//   raw 1 -- plain RP bytes passed through untouched and kept out of the
//            compressor's history (RemoteWireFormat.h:40-48).
//
// A varint is at most five bytes and may declare at most
// REMOTE_SEGMENT_MAX_PAYLOAD = 64 MiB (RemoteWireFormat.h:58-71,100-124); either
// violation means the stream has desynchronised, which is fatal and latched,
// like the Framer's own framing errors: ProtocolError, and every later feed()
// rethrows it.
class SegmentDecoder {
public:
    SegmentDecoder();
    ~SegmentDecoder();
    SegmentDecoder(const SegmentDecoder&) = delete;
    SegmentDecoder& operator=(const SegmentDecoder&) = delete;

    // Receives decoded plain RP bytes, at most max_chunk at a time.
    using Sink = std::function<void(std::span<const std::uint8_t>)>;

    // Consumes `bytes` of segment stream and hands the plain RP bytes they
    // decode to to `sink` in chunks of at most max_chunk, as they are
    // produced. Nothing decoded is accumulated here: zstd's ratio is
    // unbounded (a 32 KB segment can inflate to 1 GiB) and a legitimate burst
    // of browser frames in one read decodes to tens of MB, so the decoded
    // stream must reach the Framer -- whose own bounds then apply per message
    // -- in pieces, never as one vector per read. If the sink throws, the
    // decoder latches the failure and rethrows.
    void feed(std::span<const std::uint8_t> bytes, const Sink& sink);

    // False when the decompression context could not be created; a session
    // must then not offer the capability (decided before announcing, as the
    // server does in RemoteWireReader::_StartCodec).
    [[nodiscard]] bool usable() const;

    // Fresh connection: forget any half-read segment and start a new zstd
    // stream. The server creates a new compressor per connection too.
    void reset();

    [[nodiscard]] std::uint64_t wire_bytes() const { return wire_bytes_; }
    [[nodiscard]] std::uint64_t plain_bytes() const { return plain_bytes_; }
    [[nodiscard]] std::uint64_t segments() const { return segments_; }

    static constexpr std::size_t max_varint_size = 5;
    static constexpr std::size_t max_payload = 64u * 1024 * 1024;
    // Largest piece handed to the sink in one call.
    static constexpr std::size_t max_chunk = 128u * 1024;
    // The server's window (RemoteWireWriter.cpp kWindowLog) and what its own
    // reader allows (RemoteWireReader.cpp kWindowLogMax): a frame asking for a
    // larger window is refused rather than allocated.
    static constexpr int window_log_max = 20;

private:
    std::vector<std::uint8_t> buffer_;
    // decompress()'s fixed output buffer, max_chunk bytes once a compressed
    // segment has been seen; kept so a segment does not allocate one.
    std::vector<std::uint8_t> staging_;
    void* stream_ = nullptr; // ZSTD_DStream*, opaque so the header needs no zstd.h
    std::string failure_;
    std::uint64_t wire_bytes_ = 0;
    std::uint64_t plain_bytes_ = 0;
    std::uint64_t segments_ = 0;

    void decompress(std::span<const std::uint8_t> payload, const Sink& sink);
    void emit(std::span<const std::uint8_t> bytes, const Sink& sink);
    [[noreturn]] void fail(const std::string& description);
};

} // namespace haiku_remote
