export module licompto:io;
export import :codec;

using namespace rstd::prelude;

export namespace licompto
{

template<typename R>
struct ReaderRemainder {
    R       reader;
    Vec<u8> buffered;
};

template<typename D, typename R>
class Reader {
    D               decoder_;
    R               reader_;
    array<u8, 8192> buffer_;
    usize           position_ {}, length_ {};
    bool            eof_ = false, done_ = false, failed_ = false;

public:
    Reader(D decoder, R reader): decoder_(rstd::move(decoder)), reader_(rstd::move(reader)) {}
    auto read(mut_ref<u8[]> output) -> CodecResult<usize> {
        if (failed_) return Err(Error::InvalidState());
        if (output.is_empty() || done_) return Ok(usize());
        for (;;) {
            if (position_ == length_ && ! eof_) {
                auto buffer = rstd::io::BorrowedBuf::from_initialized(buffer_.as_mut_slice());
                auto read   = rstd::as<rstd::io::Read>(reader_).read_buf(buffer.unfilled());
                position_   = usize();
                length_     = buffer.len();
                if (read.is_err()) {
                    auto cause = read.unwrap_err();
                    if (length_ == usize() &&
                        cause.kind() == rstd::io::ErrorKind { rstd::io::ErrorKind::Interrupted })
                        continue;
                    failed_ = true;
                    return Err(Error::Io(rstd::move(cause)));
                }
                eof_ = length_ == usize();
            }
            auto input = slice<u8>::from_raw_parts(
                buffer_.as_slice().as_raw_ptr() + position_.to_primitive(), length_ - position_);
            auto step = decoder_.process(input, output, eof_ ? InputEnd::End : InputEnd::More);
            if (step.is_err()) {
                failed_ = true;
                return Err(step.unwrap_err());
            }
            auto p = step.unwrap();
            position_ += p.consumed;
            done_ = p.status == StreamStatus::StreamEnd;
            if (p.written != usize() || done_) return Ok(p.written);
        }
    }
    auto finish() -> CodecResult<empty> {
        array<u8, 8192> bytes;
        while (! done_) {
            auto read = this->read(bytes.as_mut_slice());
            if (read.is_err()) return Err(read.unwrap_err());
        }
        return Ok(empty {});
    }
    auto verified() const noexcept -> bool { return done_; }
    auto into_inner() && -> CodecResult<ReaderRemainder<R>> {
        Vec<u8> buffered;
        if (buffered.try_reserve(length_ - position_).is_err()) return Err(Error::Allocation());
        for (auto i = position_; i < length_; ++i) buffered.push(buffer_[i]);
        return Ok(ReaderRemainder<R> { rstd::move(reader_), rstd::move(buffered) });
    }
};

template<typename D, typename R>
class BufReader {
    D    decoder_;
    R    reader_;
    bool done_ = false, failed_ = false;

public:
    BufReader(D decoder, R reader): decoder_(rstd::move(decoder)), reader_(rstd::move(reader)) {}
    auto read(mut_ref<u8[]> output) -> CodecResult<usize> {
        if (failed_) return Err(Error::InvalidState());
        if (output.is_empty() || done_) return Ok(usize());
        for (;;) {
            auto filled = rstd::as<rstd::io::BufRead>(reader_).fill_buf();
            if (filled.is_err()) {
                failed_ = true;
                return Err(Error::Io(filled.unwrap_err()));
            }
            auto input = filled.unwrap();
            auto step =
                decoder_.process(input, output, input.is_empty() ? InputEnd::End : InputEnd::More);
            if (step.is_err()) {
                failed_ = true;
                return Err(step.unwrap_err());
            }
            auto p = step.unwrap();
            rstd::as<rstd::io::BufRead>(reader_).consume(p.consumed);
            done_ = p.status == StreamStatus::StreamEnd;
            if (p.written != usize() || done_) return Ok(p.written);
        }
    }
    auto finish() -> CodecResult<empty> {
        array<u8, 8192> bytes;
        while (! done_) {
            auto read = this->read(bytes.as_mut_slice());
            if (read.is_err()) return Err(read.unwrap_err());
        }
        return Ok(empty {});
    }
    auto verified() const noexcept -> bool { return done_; }
    auto into_inner() && -> R { return rstd::move(reader_); }
};

template<typename E, typename W>
class Writer {
    E    encoder_;
    W    writer_;
    bool done_ = false, failed_ = false;
    auto send(slice<u8> bytes) -> CodecResult<empty> {
        while (! bytes.is_empty()) {
            auto result = rstd::as<rstd::io::Write>(writer_).write(bytes);
            if (result.is_err()) {
                failed_ = true;
                return Err(Error::Io(result.unwrap_err()));
            }
            auto n = result.unwrap();
            if (n == usize() || n > bytes.len()) {
                failed_ = true;
                return Err(Error::Io(rstd::io::Error::from_kind(
                    { n == usize() ? rstd::io::ErrorKind::WriteZero
                                   : rstd::io::ErrorKind::InvalidData })));
            }
            bytes =
                slice<u8>::from_raw_parts(bytes.as_raw_ptr() + n.to_primitive(), bytes.len() - n);
        }
        return Ok(empty {});
    }
    auto advance(slice<u8> input, EncodeAction action) -> CodecResult<usize> {
        if (done_ || failed_) return Err(Error::InvalidState());
        usize           accepted {};
        array<u8, 8192> bytes;
        for (;;) {
            auto step = encoder_.process(input, bytes.as_mut_slice(), action);
            if (step.is_err()) {
                failed_ = true;
                return Err(step.unwrap_err());
            }
            auto p    = step.unwrap();
            auto sent = send(slice<u8>::from_raw_parts(bytes.as_slice().as_raw_ptr(), p.written));
            if (sent.is_err()) return Err(sent.unwrap_err());
            accepted += p.consumed;
            input = slice<u8>::from_raw_parts(input.as_raw_ptr() + p.consumed.to_primitive(),
                                              input.len() - p.consumed);
            if (p.status == StreamStatus::StreamEnd) {
                done_ = true;
                return Ok(accepted);
            }
            if (p.status == StreamStatus::FlushComplete ||
                (action == EncodeAction::Continue && input.is_empty() &&
                 p.status != StreamStatus::NeedOutput))
                return Ok(accepted);
        }
    }
    auto flush_sink() -> CodecResult<empty> {
        auto flushed = rstd::as<rstd::io::Write>(writer_).flush();
        if (flushed.is_err()) {
            failed_ = true;
            return Err(Error::Io(flushed.unwrap_err()));
        }
        return Ok(empty {});
    }

public:
    Writer(E encoder, W writer): encoder_(rstd::move(encoder)), writer_(rstd::move(writer)) {}
    auto write(slice<u8> input) -> CodecResult<usize> {
        return advance(input, EncodeAction::Continue);
    }
    auto write_all(slice<u8> input) -> CodecResult<empty> {
        auto result = write(input);
        if (result.is_err()) return Err(result.unwrap_err());
        return Ok(empty {});
    }
    auto flush() -> CodecResult<empty> {
        auto result = advance({}, EncodeAction::SyncFlush);
        if (result.is_err()) return Err(result.unwrap_err());
        return flush_sink();
    }
    auto finish() -> CodecResult<empty> {
        if (failed_) return Err(Error::InvalidState());
        if (done_) return Ok(empty {});
        auto result = advance({}, EncodeAction::Finish);
        if (result.is_err()) return Err(result.unwrap_err());
        return flush_sink();
    }
    auto into_inner() && -> CodecResult<W> {
        if (! done_ || failed_) return Err(Error::InvalidState());
        failed_ = true;
        return Ok(rstd::move(writer_));
    }
    auto abort() && -> W {
        failed_ = true;
        return rstd::move(writer_);
    }
};

} // namespace licompto
