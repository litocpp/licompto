module;
#include <rstd/enum.hpp>
export module licompto:types;
export import rstd;

using namespace rstd::prelude;

export namespace licompto
{

class Error {
    RSTD_ENUM(Error,
              (InvalidOptions),
              (InvalidState),
              (InvalidData),
              (UnexpectedEof),
              (ChecksumMismatch),
              (DictionaryRequired, (u32 dictionary_id;)),
              (LimitExceeded),
              (Allocation),
              (Io, (rstd::io::Error cause;)),
              (TrailingData))

private:
    u64 input_offset_ {}, output_offset_ {};

public:
    constexpr auto input_offset() const noexcept -> u64 { return input_offset_; }
    constexpr auto output_offset() const noexcept -> u64 { return output_offset_; }
    constexpr auto at(u64 input, u64 output) const noexcept -> Error {
        auto error           = *this;
        error.input_offset_  = input;
        error.output_offset_ = output;
        return error;
    }
};

template<typename T>
using CodecResult = Result<T, Error>;

enum class Format
{
    Deflate,
    Zlib,
    Gzip,
    MultiGzip
};
enum class Compression
{
    None,
    Fast,
    Default,
    Best
};
enum class EncodeAction
{
    Continue,
    SyncFlush,
    Finish
};
enum class InputEnd
{
    More,
    End
};
enum class StreamStatus
{
    Progress,
    NeedInput,
    NeedOutput,
    FlushComplete,
    StreamEnd
};

struct Progress {
    usize        consumed {};
    usize        written {};
    StreamStatus status = StreamStatus::Progress;
};

struct EncodeOptions {
    Compression compression = Compression::Default;
    u32         gzip_mtime {};
    u8          gzip_os { 255 };
    u64         max_output_bytes { ~rstd::uint64_t(0) };
};

struct DecodeLimits {
    u64 max_output_bytes;
    u64 max_header_bytes { 65536 };
    u64 max_members { 1024 };
};

struct DecodeOptions {
    DecodeLimits limits;
    unsigned     max_window_bits = 15;
};

struct GzipHeader {
    u32 mtime {};
    u8  os {};
    u8  flags {};
};

struct DecodedPrefix {
    Vec<u8> bytes;
    usize   consumed;
};

} // namespace licompto

export namespace rstd
{
template<>
struct Impl<fmt::Display, licompto::Error> : ImplBase<licompto::Error> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& e = this->self();
        using namespace rstd::literals;
        auto write = [&](ref<str> label) {
            return f.write_fmt(fmt::Arguments::make("compression error {} at input {}, output {}",
                                                    label,
                                                    e.input_offset(),
                                                    e.output_offset()));
        };
        RSTD_MATCH(e) {
            RSTD_CASE(InvalidOptions) {
                return write("invalid options"_str);
            }
            RSTD_CASE(InvalidState) {
                return write("invalid state"_str);
            }
            RSTD_CASE(InvalidData) {
                return write("invalid compressed data"_str);
            }
            RSTD_CASE(UnexpectedEof) {
                return write("truncated compressed stream"_str);
            }
            RSTD_CASE(ChecksumMismatch) {
                return write("checksum mismatch"_str);
            }
            RSTD_CASE(DictionaryRequired, dictionary_id) {
                return write("preset dictionary required"_str) &&
                       f.write_fmt(fmt::Arguments::make(" (dictionary {})", dictionary_id));
            }
            RSTD_CASE(LimitExceeded) {
                return write("compression limit exceeded"_str);
            }
            RSTD_CASE(Allocation) {
                return write("compression allocation failed"_str);
            }
            RSTD_CASE(Io, cause) {
                return write("compression I/O failed"_str) &&
                       f.write_fmt(fmt::Arguments::make(": {}", cause));
            }
            RSTD_CASE(TrailingData) {
                return write("trailing compressed data"_str);
            }
        }
        return false;
    }
};
template<>
struct Impl<fmt::Debug, licompto::Error> : Impl<fmt::Display, licompto::Error> {};
template<>
struct Impl<error::Error, licompto::Error> : DefaultInImpl<error::Error, licompto::Error> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        RSTD_MATCH(this->self()) {
            RSTD_CASE(Io, cause) {
                return Some(dyn<error::Error>::from_ref(cause));
            }
        default: return None();
        }
    }
};
} // namespace rstd
