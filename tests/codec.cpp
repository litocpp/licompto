#include <rstd/test/gtest.hpp>
import licompto;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

template<typename E, typename D>
void check_roundtrip() {
    auto bytes   = licompto::compress<E>("hello hello hello hello hello"_bytes).unwrap();
    auto decoded = licompto::decompress<D>(bytes.as_slice(), { { u64(1024) } }).unwrap();
    EXPECT_EQ(decoded.as_slice(), "hello hello hello hello hello"_bytes);
}
TEST(Deflate, Roundtrip) {
    check_roundtrip<licompto::DeflateEncoder, licompto::DeflateDecoder>();
}
TEST(Zlib, Roundtrip) {
    check_roundtrip<licompto::ZlibEncoder, licompto::ZlibDecoder>();
}
TEST(Gzip, Roundtrip) {
    check_roundtrip<licompto::GzipEncoder, licompto::GzipDecoder>();
}

template<typename E, typename D>
void chunks(usize chunk, usize capacity) {
    Vec<u8> input;
    for (unsigned i = 0; i < 70000; ++i)
        input.push(u8(static_cast<rstd::uint8_t>((i * 31 + i / 257) % 251)));
    auto          encoder = E::make().unwrap();
    Vec<u8>       compressed;
    array<u8, 17> buffer;
    usize         position {};
    for (unsigned call = 0;; ++call) {
        ASSERT_TRUE(call < 1000000);
        auto remaining = input.len() - position;
        auto count     = remaining < chunk ? remaining : chunk;
        auto step      = encoder.process(
            slice<u8>::from_raw_parts(input.as_slice().as_raw_ptr() + position.to_primitive(),
                                      count),
            mut_ref<u8[]>::from_raw_parts(buffer.as_mut_slice().as_raw_ptr(), capacity),
            count == remaining ? licompto::EncodeAction::Finish : licompto::EncodeAction::Continue);
        ASSERT_TRUE(step.is_ok());
        auto p = step.unwrap();
        ASSERT_TRUE(p.consumed <= count && p.written <= capacity);
        for (usize i {}; i < p.written; ++i) compressed.push(buffer[i]);
        position += p.consumed;
        if (p.status == licompto::StreamStatus::StreamEnd) break;
    }
    EXPECT_EQ(encoder.total_in(), u64(input.len().to_primitive()));
    auto    decoder = D::make({ { u64(input.len().to_primitive()) } }).unwrap();
    Vec<u8> decoded;
    position = usize();
    for (unsigned call = 0;; ++call) {
        ASSERT_TRUE(call < 1000000);
        auto remaining = compressed.len() - position;
        auto count     = remaining < chunk ? remaining : chunk;
        auto step      = decoder.process(
            slice<u8>::from_raw_parts(compressed.as_slice().as_raw_ptr() + position.to_primitive(),
                                      count),
            mut_ref<u8[]>::from_raw_parts(buffer.as_mut_slice().as_raw_ptr(), capacity),
            count == remaining ? licompto::InputEnd::End : licompto::InputEnd::More);
        ASSERT_TRUE(step.is_ok());
        auto p = step.unwrap();
        ASSERT_TRUE(p.consumed <= count && p.written <= capacity);
        for (usize i {}; i < p.written; ++i) decoded.push(buffer[i]);
        position += p.consumed;
        if (p.status == licompto::StreamStatus::StreamEnd) break;
    }
    EXPECT_EQ(decoded.as_slice(), input.as_slice());
    EXPECT_EQ(decoder.total_out(), u64(input.len().to_primitive()));
}
TEST(Deflate, IncrementalAndCrossBlock) {
    chunks<licompto::DeflateEncoder, licompto::DeflateDecoder>(usize(1), usize(1));
}
TEST(Zlib, IncrementalAndCrossBlock) {
    chunks<licompto::ZlibEncoder, licompto::ZlibDecoder>(usize(31), usize(7));
}
TEST(Gzip, IncrementalAndCrossBlock) {
    chunks<licompto::GzipEncoder, licompto::GzipDecoder>(usize(53), usize(17));
}

TEST(Deflate, InvalidTruncatedAndLimits) {
    auto invalid = licompto::decompress<licompto::DeflateDecoder>("\x07"_bytes, { { u64(100) } });
    ASSERT_TRUE(invalid.is_err());
    EXPECT_TRUE(invalid.unwrap_err().is_InvalidData());
    EXPECT_EQ(invalid.unwrap_err().input_offset(), u64(1));
    EXPECT_EQ(invalid.unwrap_err().output_offset(), u64());
    auto bad_length = licompto::decompress<licompto::DeflateDecoder>("\x01\x01\x00\x00\x00"_bytes,
                                                                     { { u64(100) } });
    ASSERT_TRUE(bad_length.is_err());
    auto compressed = licompto::compress<licompto::DeflateEncoder>("123456789"_bytes).unwrap();
    auto short_input =
        slice<u8>::from_raw_parts(compressed.as_slice().as_raw_ptr(), compressed.len() - usize(1));
    auto truncated = licompto::decompress<licompto::DeflateDecoder>(short_input, { { u64(100) } });
    ASSERT_TRUE(truncated.is_err());
    EXPECT_TRUE(truncated.unwrap_err().is_UnexpectedEof());
    auto limited =
        licompto::decompress<licompto::DeflateDecoder>(compressed.as_slice(), { { u64(8) } });
    ASSERT_TRUE(limited.is_err());
    EXPECT_TRUE(limited.unwrap_err().is_LimitExceeded());
    EXPECT_EQ(limited.unwrap_err().output_offset(), u64(8));
    licompto::EncodeOptions options;
    options.max_output_bytes = u64(1);
    EXPECT_TRUE(licompto::compress<licompto::GzipEncoder>("abc"_bytes, options).is_err());
}

TEST(Zlib, DictionaryRequestCarriesIdAndPositions) {
    auto result = licompto::decompress<licompto::ZlibDecoder>("\x78\x20\x12\x34\x56\x78"_bytes,
                                                              { { u64(100) } });
    ASSERT_TRUE(result.is_err());
    const auto& error = result.unwrap_err();
    ASSERT_TRUE(error.is_DictionaryRequired());
    EXPECT_EQ(error.as_DictionaryRequired().dictionary_id, u32(0x12345678));
    EXPECT_EQ(error.input_offset(), u64(6));
    EXPECT_EQ(error.output_offset(), u64());
    EXPECT_TRUE(rstd::as<rstd::error::Error>(error).source().is_none());
    EXPECT_EQ(
        rstd::format("{}", error),
        "compression error preset dictionary required at input 6, output 0 (dictionary 305419896)"_str);
}

TEST(Gzip, TailChecksumAndMembers) {
    auto bytes    = licompto::compress<licompto::GzipEncoder>("first"_bytes).unwrap();
    auto original = bytes.len();
    bytes.push(u8('!'));
    EXPECT_TRUE(
        licompto::decompress<licompto::GzipDecoder>(bytes.as_slice(), { { u64(100) } }).is_err());
    auto prefix =
        licompto::decompress_prefix<licompto::GzipDecoder>(bytes.as_slice(), { { u64(100) } })
            .unwrap();
    EXPECT_EQ(prefix.consumed, original);
    bytes.pop();
    auto second = licompto::compress<licompto::GzipEncoder>("second"_bytes).unwrap();
    for (auto b : second) bytes.push(b);
    auto both = licompto::decompress<licompto::MultiGzipDecoder>(bytes.as_slice(), { { u64(100) } })
                    .unwrap();
    EXPECT_EQ(both.as_slice(), "firstsecond"_bytes);
    auto limited = licompto::decompress<licompto::MultiGzipDecoder>(
        bytes.as_slice(), { { u64(100), u64(1000), u64(1) } });
    ASSERT_TRUE(limited.is_err());
    EXPECT_TRUE(limited.unwrap_err().is_LimitExceeded());
    bytes[bytes.len() - usize(8)] = bytes[bytes.len() - usize(8)] ^ u8(1);
    EXPECT_TRUE(licompto::decompress<licompto::MultiGzipDecoder>(bytes.as_slice(), { { u64(100) } })
                    .is_err());
}

TEST(Deflate, ZeroOutputAndMoveState) {
    auto encoded = licompto::compress<licompto::DeflateEncoder>("abc"_bytes).unwrap();
    auto decoder = licompto::DeflateDecoder::make({ { u64(3) } }).unwrap();
    auto blocked = decoder.process(encoded.as_slice(), {}, licompto::InputEnd::End).unwrap();
    EXPECT_EQ(blocked.written, usize());
    EXPECT_EQ(blocked.status, licompto::StreamStatus::NeedOutput);
    auto state = rstd::move(decoder);
    EXPECT_TRUE(decoder.process({}, {}).is_err());
    auto input =
        slice<u8>::from_raw_parts(encoded.as_slice().as_raw_ptr() + blocked.consumed.to_primitive(),
                                  encoded.len() - blocked.consumed);
    array<u8, 3> output;
    auto complete = state.process(input, output.as_mut_slice(), licompto::InputEnd::End).unwrap();
    EXPECT_EQ(complete.status, licompto::StreamStatus::StreamEnd);
    EXPECT_EQ(output.as_slice(), "abc"_bytes);
    auto empty         = licompto::compress<licompto::DeflateEncoder>({}).unwrap();
    auto empty_decoder = licompto::DeflateDecoder::make({ { u64(0) } }).unwrap();
    EXPECT_EQ(empty_decoder.process(empty.as_slice(), {}, licompto::InputEnd::End).unwrap().status,
              licompto::StreamStatus::StreamEnd);
}
