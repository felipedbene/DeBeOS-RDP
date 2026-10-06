#pragma once

#include <cstddef>
#include <cstdint>
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

    // Consumes `bytes` of segment stream and returns the plain RP bytes they
    // decode to (possibly none, when a segment is still incomplete).
    std::vector<std::uint8_t> feed(std::span<const std::uint8_t> bytes);

    // Fresh connection: forget any half-read segment and start a new zstd
    // stream. The server creates a new compressor per connection too.
    void reset();

    [[nodiscard]] std::uint64_t wire_bytes() const { return wire_bytes_; }
    [[nodiscard]] std::uint64_t plain_bytes() const { return plain_bytes_; }
    [[nodiscard]] std::uint64_t segments() const { return segments_; }

    static constexpr std::size_t max_varint_size = 5;
    static constexpr std::size_t max_payload = 64u * 1024 * 1024;

private:
    std::vector<std::uint8_t> buffer_;
    void* stream_ = nullptr; // ZSTD_DStream*, opaque so the header needs no zstd.h
    std::string failure_;
    std::uint64_t wire_bytes_ = 0;
    std::uint64_t plain_bytes_ = 0;
    std::uint64_t segments_ = 0;

    void decompress(std::span<const std::uint8_t> payload,
                    std::vector<std::uint8_t>& out);
    [[noreturn]] void fail(const std::string& description);
};

} // namespace haiku_remote
