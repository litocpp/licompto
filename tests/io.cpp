#include <rstd/test/gtest.hpp>
import licompto;
import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

TEST(CodecIo, FinishAndRead) {
    auto encoder = licompto::GzipEncoder::make().unwrap();
    auto writer  = licompto::Writer(rstd::move(encoder), rstd::io::Cursor(Vec<u8> {}));
    ASSERT_TRUE(writer.write_all("hello"_bytes).is_ok());
    ASSERT_TRUE(writer.finish().is_ok());
    auto cursor = rstd::move(writer).into_inner().unwrap();
    cursor.set_position(u64(0));
    auto          decoder = licompto::GzipDecoder::make({ { u64(64) } }).unwrap();
    auto          reader  = licompto::Reader(rstd::move(decoder), rstd::move(cursor));
    array<u8, 16> output;
    auto          count = reader.read(output.as_mut_slice()).unwrap();
    EXPECT_EQ(count, usize(5));
    ASSERT_TRUE(reader.finish().is_ok());
    EXPECT_TRUE(reader.verified());
}

struct ShortSink {
    Vec<u8>* bytes;
    usize    stop { 1000000 };
    bool     zero = false;
};
namespace rstd
{
template<>
struct Impl<io::Write, ShortSink> : ImplBase<ShortSink> {
    auto write(slice<u8> input) -> io::Result<usize> {
        auto& sink = this->self();
        if (sink.zero) return Ok(usize());
        if (sink.bytes->len() >= sink.stop)
            return Err(io::Error::from_kind({ io::ErrorKind::BrokenPipe }));
        auto n = input.len() < usize(3) ? input.len() : usize(3);
        if (n > sink.stop - sink.bytes->len()) n = sink.stop - sink.bytes->len();
        for (usize i {}; i < n; ++i) sink.bytes->push(input[i]);
        return Ok(n);
    }
    auto flush() -> io::Result<empty> { return Ok(empty {}); }
};
} // namespace rstd

TEST(CodecIo, SyncFlushAndShortWrites) {
    Vec<u8> bytes;
    auto writer = licompto::Writer(licompto::DeflateEncoder::make().unwrap(), ShortSink { &bytes });
    writer.write_all("hello"_bytes).unwrap();
    writer.flush().unwrap();
    auto           decoder = licompto::DeflateDecoder::make({ { u64(100) } }).unwrap();
    array<u8, 100> output;
    auto           p = decoder.process(bytes.as_slice(), output.as_mut_slice()).unwrap();
    EXPECT_EQ(p.written, usize(5));
    EXPECT_EQ(p.status, licompto::StreamStatus::NeedInput);
    writer.write_all(" world"_bytes).unwrap();
    writer.finish().unwrap();
    auto decoded =
        licompto::decompress<licompto::DeflateDecoder>(bytes.as_slice(), { { u64(100) } }).unwrap();
    EXPECT_EQ(decoded.as_slice(), "hello world"_bytes);
}

TEST(CodecIo, ZeroWritesAndFailuresStayFailed) {
    Vec<u8> bytes;
    auto    writer =
        licompto::Writer(licompto::GzipEncoder::make().unwrap(), ShortSink { &bytes, usize(5) });
    auto failed = writer.write_all("hello"_bytes);
    ASSERT_TRUE(failed.is_err());
    const auto& error = failed.unwrap_err();
    ASSERT_TRUE(error.is_Io());
    EXPECT_EQ(error.as_Io().cause.kind(), rstd::io::ErrorKind { rstd::io::ErrorKind::BrokenPipe });
    auto source = rstd::as<rstd::error::Error>(error).source();
    ASSERT_TRUE(source.is_some());
    EXPECT_TRUE(rstd::error::is<rstd::io::Error>(*source));
    EXPECT_EQ(source->as_raw_ptr(), static_cast<const void*>(&error.as_Io().cause));
    auto before = bytes.len();
    EXPECT_TRUE(writer.finish().is_err());
    EXPECT_EQ(bytes.len(), before);
    auto zero    = licompto::Writer(licompto::GzipEncoder::make().unwrap(),
                                    ShortSink { &bytes, usize(1000), true });
    auto stopped = zero.write("hello"_bytes);
    ASSERT_TRUE(stopped.is_err());
    ASSERT_TRUE(stopped.unwrap_err().is_Io());
    EXPECT_EQ(stopped.unwrap_err().as_Io().cause.kind(),
              rstd::io::ErrorKind { rstd::io::ErrorKind::WriteZero });
}

TEST(CodecIo, BufferedRemainderAndBufReadPosition) {
    auto bytes   = licompto::compress<licompto::DeflateEncoder>("abc"_bytes).unwrap();
    auto encoded = bytes.len();
    for (auto b : "TAIL"_bytes) bytes.push(u8(b.to_primitive()));
    auto reader = licompto::Reader(licompto::DeflateDecoder::make({ { u64(10) } }).unwrap(),
                                   rstd::io::Cursor(bytes.as_slice()));
    reader.finish().unwrap();
    auto remainder = rstd::move(reader).into_inner().unwrap();
    EXPECT_EQ(remainder.buffered.as_slice(), "TAIL"_bytes);
    auto buffered = licompto::BufReader(licompto::DeflateDecoder::make({ { u64(10) } }).unwrap(),
                                        rstd::io::Cursor(bytes.as_slice()));
    buffered.finish().unwrap();
    EXPECT_EQ(rstd::move(buffered).into_inner().position(), u64(encoded.to_primitive()));
}
