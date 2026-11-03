export module licompto:codec;
export import :types;

using namespace rstd::prelude;

namespace licompto
{
struct EncoderState;
struct DecoderState;
auto make_encoder(Format, EncodeOptions) -> CodecResult<EncoderState*>;
auto make_decoder(Format, DecodeOptions) -> CodecResult<DecoderState*>;
void destroy_encoder(EncoderState*) noexcept;
void destroy_decoder(DecoderState*) noexcept;
auto encode(EncoderState*, slice<u8>, mut_ref<u8[]>, EncodeAction) -> CodecResult<Progress>;
auto decode(DecoderState*, slice<u8>, mut_ref<u8[]>, InputEnd) -> CodecResult<Progress>;
auto encoder_total_in(const EncoderState*) noexcept -> u64;
auto encoder_total_out(const EncoderState*) noexcept -> u64;
auto decoder_total_in(const DecoderState*) noexcept -> u64;
auto decoder_total_out(const DecoderState*) noexcept -> u64;
auto decoder_header(const DecoderState*) noexcept -> Option<GzipHeader>;
} // namespace licompto

export namespace licompto
{

template<Format F>
class Encoder {
    EncoderState* state_ = nullptr;
    explicit Encoder(EncoderState* state) noexcept: state_(state) {}

public:
    Encoder(const Encoder&)                    = delete;
    auto operator=(const Encoder&) -> Encoder& = delete;
    Encoder(Encoder&& other) noexcept: state_(rstd::exchange(other.state_, nullptr)) {}
    auto operator=(Encoder&& other) noexcept -> Encoder& {
        if (this != &other) {
            destroy_encoder(state_);
            state_ = rstd::exchange(other.state_, nullptr);
        }
        return *this;
    }
    ~Encoder() { destroy_encoder(state_); }
    static auto make(EncodeOptions options = {}) -> CodecResult<Encoder> {
        auto result = make_encoder(F, options);
        if (result.is_err()) return Err(result.unwrap_err());
        return Ok(Encoder(result.unwrap()));
    }
    auto process(slice<u8>     input,
                 mut_ref<u8[]> output,
                 EncodeAction  action = EncodeAction::Continue) -> CodecResult<Progress> {
        return encode(state_, input, output, action);
    }
    auto total_in() const noexcept -> u64 { return encoder_total_in(state_); }
    auto total_out() const noexcept -> u64 { return encoder_total_out(state_); }
};

template<Format F>
class Decoder {
    DecoderState* state_ = nullptr;
    explicit Decoder(DecoderState* state) noexcept: state_(state) {}

public:
    Decoder(const Decoder&)                    = delete;
    auto operator=(const Decoder&) -> Decoder& = delete;
    Decoder(Decoder&& other) noexcept: state_(rstd::exchange(other.state_, nullptr)) {}
    auto operator=(Decoder&& other) noexcept -> Decoder& {
        if (this != &other) {
            destroy_decoder(state_);
            state_ = rstd::exchange(other.state_, nullptr);
        }
        return *this;
    }
    ~Decoder() { destroy_decoder(state_); }
    static auto make(DecodeOptions options) -> CodecResult<Decoder> {
        auto result = make_decoder(F, options);
        if (result.is_err()) return Err(result.unwrap_err());
        return Ok(Decoder(result.unwrap()));
    }
    auto process(slice<u8> input, mut_ref<u8[]> output, InputEnd end = InputEnd::More)
        -> CodecResult<Progress> {
        return decode(state_, input, output, end);
    }
    auto total_in() const noexcept -> u64 { return decoder_total_in(state_); }
    auto total_out() const noexcept -> u64 { return decoder_total_out(state_); }
    auto header() const noexcept -> Option<GzipHeader> { return decoder_header(state_); }
};

using DeflateEncoder   = Encoder<Format::Deflate>;
using ZlibEncoder      = Encoder<Format::Zlib>;
using GzipEncoder      = Encoder<Format::Gzip>;
using DeflateDecoder   = Decoder<Format::Deflate>;
using ZlibDecoder      = Decoder<Format::Zlib>;
using GzipDecoder      = Decoder<Format::Gzip>;
using MultiGzipDecoder = Decoder<Format::MultiGzip>;

template<typename E>
auto compress(slice<u8> input, EncodeOptions options = {}) -> CodecResult<Vec<u8>> {
    auto made = E::make(options);
    if (made.is_err()) return Err(made.unwrap_err());
    auto            encoder = made.unwrap();
    Vec<u8>         result;
    array<u8, 8192> output;
    for (;;) {
        auto step = encoder.process(input, output.as_mut_slice(), EncodeAction::Finish);
        if (step.is_err()) return Err(step.unwrap_err());
        auto p = step.unwrap();
        if (result.try_reserve(p.written).is_err()) return Err(Error::Allocation());
        for (usize i {}; i < p.written; ++i) result.push(output[i]);
        input = slice<u8>::from_raw_parts(input.as_raw_ptr() + p.consumed.to_primitive(),
                                          input.len() - p.consumed);
        if (p.status == StreamStatus::StreamEnd) return Ok(rstd::move(result));
    }
}

template<typename D>
auto decompress_prefix(slice<u8> input, DecodeOptions options) -> CodecResult<DecodedPrefix> {
    auto made = D::make(options);
    if (made.is_err()) return Err(made.unwrap_err());
    auto            decoder = made.unwrap();
    Vec<u8>         result;
    array<u8, 8192> output;
    usize           consumed {};
    for (;;) {
        auto step = decoder.process(input, output.as_mut_slice(), InputEnd::End);
        if (step.is_err()) return Err(step.unwrap_err());
        auto p = step.unwrap();
        if (result.try_reserve(p.written).is_err()) return Err(Error::Allocation());
        for (usize i {}; i < p.written; ++i) result.push(output[i]);
        input = slice<u8>::from_raw_parts(input.as_raw_ptr() + p.consumed.to_primitive(),
                                          input.len() - p.consumed);
        consumed += p.consumed;
        if (p.status == StreamStatus::StreamEnd)
            return Ok(DecodedPrefix { rstd::move(result), consumed });
    }
}

template<typename D>
auto decompress(slice<u8> input, DecodeOptions options) -> CodecResult<Vec<u8>> {
    auto result = decompress_prefix<D>(input, options);
    if (result.is_err()) return Err(result.unwrap_err());
    auto decoded = result.unwrap();
    if (decoded.consumed != input.len())
        return Err(Error::TrailingData().at(u64(decoded.consumed.to_primitive()),
                                            u64(decoded.bytes.len().to_primitive())));
    return Ok(rstd::move(decoded.bytes));
}

} // namespace licompto
