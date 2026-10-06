#include "haiku_remote/wire_decoder.hpp"

#include "haiku_remote/protocol.hpp"

#ifdef HAIKU_REMOTE_HAVE_ZSTD
#include <zstd.h>
#endif

#include <algorithm>
#include <string>

namespace haiku_remote {

bool zstd_decoder_available()
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

SegmentDecoder::SegmentDecoder()
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    auto* stream = ZSTD_createDStream();
    // Refuse any frame that asks for a window larger than the server uses,
    // instead of letting a peer size our allocation.
    if (stream != nullptr
        && ZSTD_isError(ZSTD_DCtx_setParameter(stream, ZSTD_d_windowLogMax,
                                               window_log_max))) {
        ZSTD_freeDStream(stream);
        stream = nullptr;
    }
    stream_ = stream;
#endif
    reset();
}

SegmentDecoder::~SegmentDecoder()
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    ZSTD_freeDStream(static_cast<ZSTD_DStream*>(stream_));
#endif
}

bool SegmentDecoder::usable() const
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    return stream_ != nullptr;
#else
    return false;
#endif
}

void SegmentDecoder::reset()
{
    buffer_.clear();
    failure_.clear();
    wire_bytes_ = plain_bytes_ = segments_ = 0;
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    // Only the session: the parameters (windowLogMax) are kept.
    if (stream_ != nullptr)
        ZSTD_DCtx_reset(static_cast<ZSTD_DStream*>(stream_),
                        ZSTD_reset_session_only);
#endif
}

void SegmentDecoder::fail(const std::string& description)
{
    failure_ = description;
    // Nothing after a bad segment can be trusted; do not keep holding it.
    buffer_.clear();
    buffer_.shrink_to_fit();
    throw ProtocolError(failure_);
}

void SegmentDecoder::emit(std::span<const std::uint8_t> bytes, const Sink& sink)
{
    for (std::size_t i = 0; i < bytes.size(); i += max_chunk) {
        const auto piece = bytes.subspan(i, std::min(max_chunk, bytes.size() - i));
        plain_bytes_ += piece.size();
        sink(piece);
    }
}

void SegmentDecoder::feed(std::span<const std::uint8_t> bytes, const Sink& sink)
{
    if (!failure_.empty())
        throw ProtocolError(failure_);

    wire_bytes_ += bytes.size();
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::size_t offset = 0;
    try {
        while (offset < buffer_.size()) {
            // remote_segment_header_read(), RemoteWireFormat.h:100-124.
            std::uint64_t value = 0;
            std::size_t header = 0;
            bool complete = false;
            for (std::size_t i = 0;
                 offset + i < buffer_.size() && i < max_varint_size; ++i) {
                const std::uint8_t byte = buffer_[offset + i];
                value |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i);
                if ((byte & 0x80) == 0) {
                    header = i + 1;
                    complete = true;
                    break;
                }
            }
            if (!complete) {
                // A continuation bit on the fifth byte cannot be a valid
                // uint32 varint (RemoteWireFormat.h:119-121).
                if (buffer_.size() - offset >= max_varint_size)
                    fail("compressed stream desync: segment header longer than "
                         + std::to_string(max_varint_size) + " bytes");
                break;
            }
            const std::uint64_t length = value >> 1;
            const bool raw = (value & 1) != 0;
            if (length > max_payload)
                fail("compressed stream desync: segment declares "
                     + std::to_string(length) + " bytes, above the "
                     + std::to_string(max_payload) + " byte limit");
            if (buffer_.size() - offset - header < length)
                break;

            const std::span<const std::uint8_t> payload(
                buffer_.data() + offset + header, static_cast<std::size_t>(length));
            offset += header + static_cast<std::size_t>(length);
            ++segments_;
            if (raw)
                emit(payload, sink);
            else
                decompress(payload, sink);
        }
    } catch (const std::exception& error) {
        // Whatever failed -- this layer, or the Framer behind the sink -- the
        // stream position is lost; latch so a later feed() cannot resume
        // mid-segment.
        if (failure_.empty())
            failure_ = error.what();
        buffer_.clear();
        buffer_.shrink_to_fit();
        throw;
    }
    if (offset != 0)
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
}

void SegmentDecoder::decompress(std::span<const std::uint8_t> payload,
                                const Sink& sink)
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    auto* stream = static_cast<ZSTD_DStream*>(stream_);
    if (stream == nullptr)
        fail("zstd: could not create a decompression context");
    ZSTD_inBuffer input {payload.data(), payload.size(), 0};
    // One fixed staging buffer, handed to the sink every round: the decoded
    // size of a segment never sizes an allocation here.
    std::vector<std::uint8_t> staging(max_chunk);
    // The server's own reader loop (RemoteWireReader.cpp, _Decompress): run
    // until the input is consumed AND a call left room in its output (a full
    // output may mean zstd still holds more), and treat a round that neither
    // reads nor writes as a corrupt stream.
    while (true) {
        ZSTD_outBuffer output {staging.data(), staging.size(), 0};
        const std::size_t consumed_before = input.pos;
        const std::size_t result = ZSTD_decompressStream(stream, &output, &input);
        if (ZSTD_isError(result))
            fail(std::string("zstd: ") + ZSTD_getErrorName(result));
        if (output.pos > 0)
            emit(std::span<const std::uint8_t>(staging.data(), output.pos), sink);
        if (input.pos == input.size && output.pos < output.size)
            break;
        if (input.pos == consumed_before && output.pos == 0)
            fail("zstd: decompressor made no progress (corrupt stream)");
    }
#else
    (void)payload;
    (void)sink;
    fail("compressed segment received, but this build has no zstd decoder "
         "(it must not have advertised RP_CAP_COMPRESS_ZSTD)");
#endif
}

} // namespace haiku_remote
