#include "haiku_remote/wire_decoder.hpp"

#include "haiku_remote/protocol.hpp"

#ifdef HAIKU_REMOTE_HAVE_ZSTD
#include <zstd.h>
#endif

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
    reset();
}

SegmentDecoder::~SegmentDecoder()
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    ZSTD_freeDStream(static_cast<ZSTD_DStream*>(stream_));
#endif
}

void SegmentDecoder::reset()
{
    buffer_.clear();
    failure_.clear();
    wire_bytes_ = plain_bytes_ = segments_ = 0;
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    if (stream_ == nullptr)
        stream_ = ZSTD_createDStream();
    if (stream_ != nullptr)
        ZSTD_DCtx_reset(static_cast<ZSTD_DStream*>(stream_),
                        ZSTD_reset_session_and_parameters);
#endif
}

void SegmentDecoder::fail(const std::string& description)
{
    failure_ = description;
    // Nothing after a bad segment header can be trusted; do not keep holding it.
    buffer_.clear();
    buffer_.shrink_to_fit();
    throw ProtocolError(failure_);
}

std::vector<std::uint8_t> SegmentDecoder::feed(std::span<const std::uint8_t> bytes)
{
    if (!failure_.empty())
        throw ProtocolError(failure_);

    wire_bytes_ += bytes.size();
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::vector<std::uint8_t> out;
    std::size_t offset = 0;
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
            // A continuation bit on the fifth byte cannot be a valid uint32
            // varint (RemoteWireFormat.h:119-121).
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
        if (raw)
            out.insert(out.end(), payload.begin(), payload.end());
        else
            decompress(payload, out);
        ++segments_;
        offset += header + static_cast<std::size_t>(length);
    }
    if (offset != 0)
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    plain_bytes_ += out.size();
    return out;
}

void SegmentDecoder::decompress(std::span<const std::uint8_t> payload,
                                std::vector<std::uint8_t>& out)
{
#ifdef HAIKU_REMOTE_HAVE_ZSTD
    auto* stream = static_cast<ZSTD_DStream*>(stream_);
    if (stream == nullptr)
        fail("zstd: could not create a decompression context");
    ZSTD_inBuffer input { payload.data(), payload.size(), 0 };
    const std::size_t chunk = ZSTD_DStreamOutSize();
    // Run until the input is consumed AND the last call left room in its
    // output: a full output buffer may mean more is pending inside zstd even
    // with no input left.
    while (true) {
        const std::size_t base = out.size();
        out.resize(base + chunk);
        ZSTD_outBuffer output { out.data() + base, chunk, 0 };
        const std::size_t result = ZSTD_decompressStream(stream, &output, &input);
        out.resize(base + output.pos);
        if (ZSTD_isError(result))
            fail(std::string("zstd: ") + ZSTD_getErrorName(result));
        if (input.pos == input.size && output.pos < chunk)
            break;
    }
#else
    (void)payload;
    (void)out;
    fail("compressed segment received, but this build has no zstd decoder "
         "(it must not have advertised RP_CAP_COMPRESS_ZSTD)");
#endif
}

} // namespace haiku_remote
