module licompto;
import :codec;
import licrypto;

using namespace rstd::prelude;

namespace licompto
{

using Count = rstd::uint64_t;
using Size  = rstd::size_t;

constexpr unsigned length_base[29]    = { 3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                          15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                          67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr unsigned length_extra[29]   = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
constexpr unsigned distance_base[30]  = { 1,    2,    3,    4,     5,     7,    9,    13,
                                          17,   25,   33,   49,    65,    97,   129,  193,
                                          257,  385,  513,  769,   1025,  1537, 2049, 3073,
                                          4097, 6145, 8193, 12289, 16385, 24577 };
constexpr unsigned distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
constexpr unsigned code_order[19]     = { 16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                          11, 4,  12, 3, 13, 2, 14, 1, 15 };

enum class Run
{
    Input,
    Output,
    Done,
    Error
};

struct Call {
    const unsigned char* input;
    Size                 input_size;
    unsigned char*       output;
    Size                 output_size;
    Count&               total_in;
    Count&               total_out;
    Count                max_output;
    Size                 consumed = 0;
    Size                 written  = 0;
    bool                 failed   = false;
    Error                error    = Error::InvalidData();

    void fail(Error value) {
        failed = true;
        error  = rstd::move(value);
    }
    auto byte(unsigned& value) -> bool {
        if (consumed == input_size) return false;
        if (total_in == ~Count(0)) {
            fail(Error::LimitExceeded());
            return false;
        }
        value = input[consumed++];
        ++total_in;
        return true;
    }
    auto space() -> bool {
        if (written == output_size) return false;
        if (total_out >= max_output) {
            fail(Error::LimitExceeded());
            return false;
        }
        return true;
    }
    auto emit(unsigned value) -> bool {
        if (! space()) return false;
        output[written++] = static_cast<unsigned char>(value);
        ++total_out;
        return true;
    }
};

struct Huffman {
    unsigned count[16] {};
    unsigned first_code[16] {};
    unsigned first_symbol[16] {};
    unsigned symbols[288] {};
    unsigned maximum = 0;

    auto build(const unsigned* lengths, unsigned size, bool empty, bool single, unsigned limit = 15)
        -> bool {
        for (auto& value : count) value = 0;
        maximum       = 0;
        unsigned used = 0;
        for (unsigned i = 0; i < size; ++i) {
            if (lengths[i] > limit) return false;
            if (lengths[i] != 0) {
                ++count[lengths[i]];
                ++used;
                if (lengths[i] > maximum) maximum = lengths[i];
            }
        }
        if (used == 0) return empty;
        int left = 1;
        for (unsigned i = 1; i <= limit; ++i) {
            left = left * 2 - static_cast<int>(count[i]);
            if (left < 0) return false;
        }
        if (left != 0 && ! (single && used == 1 && count[1] == 1)) return false;
        unsigned code = 0, offset = 0;
        unsigned next[16] {};
        for (unsigned i = 1; i <= 15; ++i) {
            code            = (code + count[i - 1]) << 1;
            first_code[i]   = code;
            first_symbol[i] = next[i] = offset;
            offset += count[i];
        }
        for (unsigned i = 0; i < size; ++i)
            if (lengths[i] != 0) symbols[next[lengths[i]]++] = i;
        return true;
    }
};

struct RawInflate {
    enum class State
    {
        Header,
        StoredLength,
        Stored,
        DynamicHeader,
        CodeLengths,
        Lengths,
        Repeat,
        Codes,
        Literal,
        LengthExtra,
        Distance,
        DistanceExtra,
        Copy,
        Done
    };
    State         state     = State::Header;
    Count         bits      = 0;
    unsigned      bit_count = 0;
    unsigned char window[32768] {};
    unsigned      position = 0, history = 0, window_limit = 32768;
    unsigned      literal_lengths[288] {}, distance_lengths[32] {}, lengths[320] {};
    Huffman       literal_tree, distance_tree, code_tree;
    unsigned      symbol_code = 0, symbol_length = 0;
    unsigned      hlit = 0, hdist = 0, hclen = 0, index = 0;
    unsigned      repeat = 0, length = 0, distance = 0, symbol = 0;
    bool          final_block = false;

    void reset(unsigned limit = 32768) {
        state       = State::Header;
        bits        = 0;
        bit_count   = 0;
        position    = 0;
        history     = 0;
        symbol_code = symbol_length = 0;
        window_limit                = limit;
    }
    auto take(Call& c, unsigned size, unsigned& value) -> bool {
        while (bit_count < size) {
            unsigned byte;
            if (! c.byte(byte)) return false;
            bits |= Count(byte) << bit_count;
            bit_count += 8;
        }
        value = static_cast<unsigned>(bits & ((Count(1) << size) - 1));
        bits >>= size;
        bit_count -= size;
        return true;
    }
    auto read_symbol(Call& c, const Huffman& tree) -> int {
        while (symbol_length < tree.maximum) {
            unsigned bit;
            if (! take(c, 1, bit)) return -1;
            symbol_code = (symbol_code << 1) | bit;
            ++symbol_length;
            auto offset = symbol_code - tree.first_code[symbol_length];
            if (symbol_code >= tree.first_code[symbol_length] &&
                offset < tree.count[symbol_length]) {
                auto result = tree.symbols[tree.first_symbol[symbol_length] + offset];
                symbol_code = symbol_length = 0;
                return static_cast<int>(result);
            }
        }
        c.fail(Error::InvalidData());
        return -2;
    }
    auto emit(Call& c, unsigned value) -> bool {
        if (! c.emit(value)) return false;
        window[position] = static_cast<unsigned char>(value);
        position         = (position + 1) & 32767;
        if (history < window_limit) ++history;
        return true;
    }
    void fixed() {
        for (unsigned i = 0; i < 288; ++i)
            literal_lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        for (auto& value : distance_lengths) value = 5;
        literal_tree.build(literal_lengths, 288, false, false);
        distance_tree.build(distance_lengths, 32, false, false);
    }
    auto run(Call& c) -> Run {
        unsigned value = 0;
        for (;;) {
            switch (state) {
            case State::Header:
                if (! take(c, 3, value)) return Run::Input;
                final_block = (value & 1) != 0;
                switch (value >> 1) {
                case 0:
                    bits      = 0;
                    bit_count = 0;
                    state     = State::StoredLength;
                    break;
                case 1:
                    fixed();
                    state = State::Codes;
                    break;
                case 2: state = State::DynamicHeader; break;
                default: c.fail(Error::InvalidData()); return Run::Error;
                }
                break;
            case State::StoredLength:
                if (! take(c, 32, value)) return Run::Input;
                if (((value & 65535u) ^ 65535u) != (value >> 16)) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                length = value & 65535u;
                state  = State::Stored;
                break;
            case State::Stored:
                while (length != 0) {
                    if (! c.space()) return Run::Output;
                    if (! c.byte(value)) return Run::Input;
                    if (! emit(c, value)) return Run::Output;
                    --length;
                }
                state = final_block ? State::Done : State::Header;
                break;
            case State::DynamicHeader:
                if (! take(c, 14, value)) return Run::Input;
                hlit  = (value & 31) + 257;
                hdist = ((value >> 5) & 31) + 1;
                hclen = (value >> 10) + 4;
                if (hlit > 286) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                for (unsigned i = 0; i < 19; ++i) lengths[i] = 0;
                index = 0;
                state = State::CodeLengths;
                break;
            case State::CodeLengths:
                while (index < hclen) {
                    if (! take(c, 3, value)) return Run::Input;
                    lengths[code_order[index++]] = value;
                }
                if (! code_tree.build(lengths, 19, false, false, 7)) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                index = 0;
                state = State::Lengths;
                break;
            case State::Lengths:
                if (index == hlit + hdist) {
                    if (lengths[256] == 0 || ! literal_tree.build(lengths, hlit, false, true) ||
                        ! distance_tree.build(lengths + hlit, hdist, true, true)) {
                        c.fail(Error::InvalidData());
                        return Run::Error;
                    }
                    state = State::Codes;
                    break;
                }
                {
                    auto next = read_symbol(c, code_tree);
                    if (next < 0) return next == -1 ? Run::Input : Run::Error;
                    if (next < 16) {
                        lengths[index++] = static_cast<unsigned>(next);
                        break;
                    }
                    if (next == 16 && index == 0) {
                        c.fail(Error::InvalidData());
                        return Run::Error;
                    }
                    repeat = static_cast<unsigned>(next);
                    state  = State::Repeat;
                }
                break;
            case State::Repeat:
                if (! take(c, repeat == 16 ? 2 : repeat == 17 ? 3 : 7, value)) return Run::Input;
                value += repeat == 18 ? 11 : 3;
                if (value > hlit + hdist - index) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                {
                    auto previous = repeat == 16 ? lengths[index - 1] : 0;
                    while (value-- != 0) lengths[index++] = previous;
                }
                state = State::Lengths;
                break;
            case State::Codes: {
                auto next = read_symbol(c, literal_tree);
                if (next < 0) return next == -1 ? Run::Input : Run::Error;
                symbol = static_cast<unsigned>(next);
                if (symbol < 256) {
                    state = State::Literal;
                    break;
                }
                if (symbol == 256) {
                    state = final_block ? State::Done : State::Header;
                    break;
                }
                if (symbol > 285) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                symbol -= 257;
                state = State::LengthExtra;
            } break;
            case State::Literal:
                if (! emit(c, symbol)) return Run::Output;
                state = State::Codes;
                break;
            case State::LengthExtra:
                if (! take(c, length_extra[symbol], value)) return Run::Input;
                length = length_base[symbol] + value;
                state  = State::Distance;
                break;
            case State::Distance: {
                auto next = read_symbol(c, distance_tree);
                if (next < 0) return next == -1 ? Run::Input : Run::Error;
                if (next >= 30) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                symbol = static_cast<unsigned>(next);
                state  = State::DistanceExtra;
            } break;
            case State::DistanceExtra:
                if (! take(c, distance_extra[symbol], value)) return Run::Input;
                distance = distance_base[symbol] + value;
                if (distance > history) {
                    c.fail(Error::InvalidData());
                    return Run::Error;
                }
                state = State::Copy;
                break;
            case State::Copy:
                while (length != 0) {
                    value = window[(position + 32768 - distance) & 32767];
                    if (! emit(c, value)) return Run::Output;
                    --length;
                }
                state = State::Codes;
                break;
            case State::Done:
                bits      = 0;
                bit_count = 0;
                return Run::Done;
            }
        }
    }
};

struct Token {
    unsigned short value;
    unsigned short distance;
};
struct RleCode {
    unsigned symbol;
    unsigned extra;
    unsigned size;
};

auto length_symbol(unsigned size) -> unsigned {
    unsigned i = 0;
    while (i < 28 && size >= length_base[i + 1]) ++i;
    return i;
}
auto distance_symbol(unsigned size) -> unsigned {
    unsigned i = 0;
    while (i < 29 && size >= distance_base[i + 1]) ++i;
    return i;
}

void make_lengths(const unsigned* frequencies, unsigned size, unsigned limit, unsigned* lengths) {
    unsigned weights[576] {}, parent[576] {}, used[288] {};
    unsigned count = 0;
    for (unsigned i = 0; i < size; ++i) {
        lengths[i] = 0;
        if (frequencies[i] != 0) used[count++] = i;
        weights[i] = frequencies[i];
        parent[i]  = 0;
    }
    if (count == 0) return;
    if (count == 1) {
        lengths[used[0]] = 1;
        return;
    }
    auto nodes = size;
    for (unsigned merge = 1; merge < count; ++merge) {
        unsigned a = 576, b = 576;
        for (unsigned i = 0; i < nodes; ++i) {
            if (weights[i] == 0 || parent[i] != 0) continue;
            if (a == 576 || weights[i] < weights[a]) {
                b = a;
                a = i;
            } else if (b == 576 || weights[i] < weights[b])
                b = i;
        }
        weights[nodes] = weights[a] + weights[b];
        parent[nodes]  = 0;
        parent[a] = parent[b] = nodes;
        ++nodes;
    }
    bool overflow = false;
    for (unsigned i = 0; i < count; ++i) {
        unsigned depth = 0, node = used[i];
        while (parent[node] != 0) {
            ++depth;
            node = parent[node];
        }
        lengths[used[i]] = depth;
        if (depth > limit) overflow = true;
    }
    if (! overflow) return;
    for (unsigned i = 1; i < count; ++i) {
        auto symbol = used[i];
        auto j      = i;
        while (j != 0 && frequencies[used[j - 1]] < frequencies[symbol]) {
            used[j] = used[j - 1];
            --j;
        }
        used[j] = symbol;
    }
    unsigned depth = 0;
    while ((1u << (depth + 1)) <= count) ++depth;
    auto shorter = (1u << (depth + 1)) - count;
    for (unsigned i = 0; i < count; ++i) lengths[used[i]] = i < shorter ? depth : depth + 1;
}

void make_codes(const unsigned* lengths, unsigned size, unsigned* codes) {
    unsigned counts[16] {}, next[16] {}, code = 0;
    for (unsigned i = 0; i < size; ++i) ++counts[lengths[i]];
    counts[0] = 0;
    for (unsigned i = 1; i <= 15; ++i) {
        code    = (code + counts[i - 1]) << 1;
        next[i] = code;
    }
    for (unsigned i = 0; i < size; ++i) {
        auto     value    = next[lengths[i]]++;
        unsigned reversed = 0;
        for (unsigned bit = 0; bit < lengths[i]; ++bit) {
            reversed = (reversed << 1) | (value & 1);
            value >>= 1;
        }
        codes[i] = reversed;
    }
}

struct RawDeflate {
    unsigned char data[65536] {};
    unsigned      history = 0, filled = 0;
    Token         tokens[32768] {};
    unsigned      token_count = 0;
    int           heads[32768] {}, previous[65536] {};
    unsigned      literal_freq[286] {}, distance_freq[30] {};
    unsigned      literal_lengths[288] {}, distance_lengths[32] {}, code_lengths[19] {};
    unsigned      literal_codes[288] {}, distance_codes[32] {}, code_codes[19] {};
    RleCode       rle[640] {};
    unsigned      rle_count = 0, hlit = 0, hdist = 0, hclen = 0;
    unsigned char pending[65568] {};
    unsigned      pending_size = 0, pending_position = 0;
    Count         bits      = 0;
    unsigned      bit_count = 0;

    void put(unsigned value, unsigned size) {
        bits |= Count(value) << bit_count;
        bit_count += size;
        while (bit_count >= 8) {
            pending[pending_size++] = static_cast<unsigned char>(bits);
            bits >>= 8;
            bit_count -= 8;
        }
    }
    void align() {
        if (bit_count != 0) pending[pending_size++] = static_cast<unsigned char>(bits);
        bits      = 0;
        bit_count = 0;
    }
    void byte(unsigned value) { pending[pending_size++] = static_cast<unsigned char>(value); }
    void integer(unsigned value, unsigned size, bool big = false) {
        for (unsigned i = 0; i < size; ++i) byte(value >> ((big ? size - 1 - i : i) * 8));
    }
    auto drain(Call& c) -> bool {
        while (pending_position < pending_size) {
            if (! c.emit(pending[pending_position])) return false;
            ++pending_position;
        }
        pending_position = pending_size = 0;
        return true;
    }
    auto hash(unsigned position) const -> unsigned {
        return ((unsigned(data[position]) * 251u + data[position + 1]) * 251u +
                data[position + 2]) &
               32767u;
    }
    void insert(unsigned position) {
        auto key           = hash(position);
        previous[position] = heads[key];
        heads[key]         = static_cast<int>(position);
    }
    void tokenize(Compression compression) {
        for (auto& head : heads) head = -1;
        for (auto& value : literal_freq) value = 0;
        for (auto& value : distance_freq) value = 0;
        token_count = 0;
        auto end    = history + filled;
        for (unsigned i = 0; i < history && i + 2 < end; ++i) insert(i);
        auto probes = compression == Compression::Fast   ? 16u
                      : compression == Compression::Best ? 256u
                                                         : 64u;
        for (unsigned i = history; i < end;) {
            unsigned best = 0, distance = 0;
            if (i + 2 < end) {
                auto candidate = heads[hash(i)];
                auto maximum   = end - i < 258 ? end - i : 258;
                auto remaining = probes;
                while (candidate >= 0 && i - static_cast<unsigned>(candidate) <= 32768 &&
                       remaining-- != 0) {
                    unsigned count = 0;
                    while (count < maximum &&
                           data[static_cast<unsigned>(candidate) + count] == data[i + count])
                        ++count;
                    if (count >= 3 && count > best) {
                        best     = count;
                        distance = i - static_cast<unsigned>(candidate);
                    }
                    if (best == maximum) break;
                    candidate = previous[candidate];
                }
            }
            if (best >= 3) {
                tokens[token_count++] = Token { static_cast<unsigned short>(best),
                                                static_cast<unsigned short>(distance) };
                ++literal_freq[257 + length_symbol(best)];
                ++distance_freq[distance_symbol(distance)];
            } else {
                best                  = 1;
                tokens[token_count++] = Token { data[i], 0 };
                ++literal_freq[data[i]];
            }
            for (unsigned j = 0; j < best; ++j)
                if (i + j + 2 < end) insert(i + j);
            i += best;
        }
        ++literal_freq[256];
    }
    auto cost(const unsigned* ll, const unsigned* dd) const -> Count {
        Count bits = ll[256];
        for (unsigned i = 0; i < token_count; ++i) {
            const auto& token = tokens[i];
            if (token.distance == 0)
                bits += ll[token.value];
            else {
                auto l = length_symbol(token.value), d = distance_symbol(token.distance);
                bits += ll[257 + l] + length_extra[l] + dd[d] + distance_extra[d];
            }
        }
        return bits;
    }
    void dynamic() {
        make_lengths(literal_freq, 286, 15, literal_lengths);
        if (distance_freq[0] == 0) {
            bool empty = true;
            for (auto f : distance_freq)
                if (f != 0) empty = false;
            if (empty) distance_freq[0] = 1;
        }
        make_lengths(distance_freq, 30, 15, distance_lengths);
        hlit = 286;
        while (hlit > 257 && literal_lengths[hlit - 1] == 0) --hlit;
        hdist = 30;
        while (hdist > 1 && distance_lengths[hdist - 1] == 0) --hdist;
        unsigned all[318] {}, frequencies[19] {};
        for (unsigned i = 0; i < hlit; ++i) all[i] = literal_lengths[i];
        for (unsigned i = 0; i < hdist; ++i) all[hlit + i] = distance_lengths[i];
        rle_count = 0;
        auto add  = [&](unsigned symbol, unsigned extra, unsigned size) {
            rle[rle_count++] = RleCode { symbol, extra, size };
            ++frequencies[symbol];
        };
        for (unsigned i = 0; i < hlit + hdist;) {
            auto     value = all[i];
            unsigned count = 1;
            while (i + count < hlit + hdist && all[i + count] == value) ++count;
            i += count;
            if (value != 0) {
                add(value, 0, 0);
                --count;
            }
            while (count != 0) {
                if (value == 0 && count >= 11) {
                    auto n = count < 138 ? count : 138;
                    add(18, n - 11, 7);
                    count -= n;
                } else if (value == 0 && count >= 3) {
                    auto n = count < 10 ? count : 10;
                    add(17, n - 3, 3);
                    count -= n;
                } else if (value != 0 && count >= 3) {
                    auto n = count < 6 ? count : 6;
                    add(16, n - 3, 2);
                    count -= n;
                } else {
                    add(value, 0, 0);
                    --count;
                }
            }
        }
        unsigned used = 0;
        for (auto f : frequencies)
            if (f != 0) ++used;
        if (used == 1)
            for (unsigned i = 0; i < 19; ++i)
                if (frequencies[i] == 0) {
                    frequencies[i] = 1;
                    break;
                }
        make_lengths(frequencies, 19, 7, code_lengths);
        hclen = 19;
        while (hclen > 4 && code_lengths[code_order[hclen - 1]] == 0) --hclen;
    }
    void write_tokens() {
        make_codes(literal_lengths, 288, literal_codes);
        make_codes(distance_lengths, 32, distance_codes);
        for (unsigned i = 0; i < token_count; ++i) {
            auto token = tokens[i];
            if (token.distance == 0)
                put(literal_codes[token.value], literal_lengths[token.value]);
            else {
                auto l = length_symbol(token.value), d = distance_symbol(token.distance);
                put(literal_codes[257 + l], literal_lengths[257 + l]);
                put(token.value - length_base[l], length_extra[l]);
                put(distance_codes[d], distance_lengths[d]);
                put(token.distance - distance_base[d], distance_extra[d]);
            }
        }
        put(literal_codes[256], literal_lengths[256]);
    }
    void block(Compression compression, bool final) {
        unsigned method  = 0;
        auto stored_cost = Count(3 + ((8 - ((bit_count + 3) & 7)) & 7) + 32) + Count(filled) * 8;
        if (compression != Compression::None) {
            tokenize(compression);
            for (unsigned i = 0; i < 288; ++i)
                literal_lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            for (auto& value : distance_lengths) value = 5;
            auto fixed_cost = Count(3) + cost(literal_lengths, distance_lengths);
            dynamic();
            auto dynamic_cost = Count(3 + 14 + 3 * hclen) + cost(literal_lengths, distance_lengths);
            for (unsigned i = 0; i < rle_count; ++i)
                dynamic_cost += code_lengths[rle[i].symbol] + rle[i].size;
            method = fixed_cost < stored_cost ? 1 : 0;
            if (dynamic_cost < (method == 1 ? fixed_cost : stored_cost)) method = 2;
        }
        put((method << 1) | unsigned(final), 3);
        if (method == 0) {
            align();
            integer(filled, 2);
            integer(filled ^ 65535u, 2);
            for (unsigned i = 0; i < filled; ++i) byte(data[history + i]);
        } else {
            if (method == 1) {
                for (unsigned i = 0; i < 288; ++i)
                    literal_lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                for (auto& value : distance_lengths) value = 5;
            } else {
                literal_lengths[286] = literal_lengths[287] = 0;
                distance_lengths[30] = distance_lengths[31] = 0;
                put(hlit - 257, 5);
                put(hdist - 1, 5);
                put(hclen - 4, 4);
                for (unsigned i = 0; i < hclen; ++i) put(code_lengths[code_order[i]], 3);
                make_codes(code_lengths, 19, code_codes);
                for (unsigned i = 0; i < rle_count; ++i) {
                    auto value = rle[i];
                    put(code_codes[value.symbol], code_lengths[value.symbol]);
                    put(value.extra, value.size);
                }
            }
            write_tokens();
        }
        auto end  = history + filled;
        auto keep = end < 32768 ? end : 32768;
        for (unsigned i = 0; i < keep; ++i) data[i] = data[end - keep + i];
        history = keep;
        filled  = 0;
        if (final) align();
    }
    void sync_flush() {
        put(0, 3);
        align();
        integer(0, 2);
        integer(65535, 2);
    }
};

struct EncoderState {
    Format              format;
    EncodeOptions       options;
    RawDeflate          raw;
    licrypto::Crc32Ieee crc;
    licrypto::Adler32   adler;
    Count               total_in = 0, total_out = 0;
    bool header = false, failed = false, done = false, ending = false, flushing = false;
    bool final_input = false;
    Size remaining   = 0;
    explicit EncoderState(Format f, EncodeOptions o): format(f), options(o) {}
};

struct DecoderState {
    enum class State
    {
        Header,
        GzipExtraLength,
        GzipExtra,
        GzipName,
        GzipComment,
        GzipCrc,
        Dictionary,
        Body,
        Trailer,
        Between,
        Done
    };
    Format              format;
    DecodeOptions       options;
    RawInflate          raw;
    State               state = State::Header;
    licrypto::Crc32Ieee crc, header_crc;
    licrypto::Adler32   adler;
    Count         total_in = 0, total_out = 0, header_bytes = 0, members = 0, member_output = 0;
    unsigned char fields[10] {};
    unsigned      field_size = 0, extra = 0, flags = 0;
    GzipHeader    gzip_header;
    bool          has_header = false, failed = false, final_input = false;
    Size          remaining = 0;
    explicit DecoderState(Format f, DecodeOptions o): format(f), options(o) {
        raw.window_limit = 1u << o.max_window_bits;
        if (f == Format::Deflate) state = State::Body;
    }
    auto header_byte(Call& c, unsigned& value, bool checksum = true) -> bool {
        if (! c.byte(value)) return false;
        if (header_bytes >= options.limits.max_header_bytes.to_primitive()) {
            c.fail(Error::LimitExceeded());
            return false;
        }
        ++header_bytes;
        if (checksum) {
            auto byte = array<u8, 1> { u8(value) };
            header_crc.update(byte.as_slice());
        }
        return true;
    }
    void after_extra() {
        state      = (flags & 8)    ? State::GzipName
                     : (flags & 16) ? State::GzipComment
                     : (flags & 2)  ? State::GzipCrc
                                    : State::Body;
        field_size = 0;
    }
    void after_name() {
        state      = (flags & 16) ? State::GzipComment : (flags & 2) ? State::GzipCrc : State::Body;
        field_size = 0;
    }
    void after_comment() {
        state      = (flags & 2) ? State::GzipCrc : State::Body;
        field_size = 0;
    }
    auto run(Call& c) -> Run {
        unsigned value = 0;
        for (;;) {
            switch (state) {
            case State::Header:
                if (format == Format::Zlib) {
                    while (field_size < 2) {
                        if (! c.byte(value)) return Run::Input;
                        fields[field_size++] = static_cast<unsigned char>(value);
                    }
                    auto cmf = fields[0], flg = fields[1];
                    if ((cmf & 15) != 8 || (cmf >> 4) > 7 ||
                        ((unsigned(cmf) * 256 + flg) % 31) != 0) {
                        c.fail(Error::InvalidData());
                        return Run::Error;
                    }
                    if (unsigned(cmf >> 4) + 8 > options.max_window_bits) {
                        c.fail(Error::LimitExceeded());
                        return Run::Error;
                    }
                    raw.window_limit = 1u << (unsigned(cmf >> 4) + 8);
                    state            = (flg & 32) != 0 ? State::Dictionary : State::Body;
                    field_size       = 0;
                } else {
                    if (field_size == 0 && members >= options.limits.max_members.to_primitive()) {
                        c.fail(Error::LimitExceeded());
                        return Run::Error;
                    }
                    while (field_size < 10) {
                        if (! header_byte(c, value)) return Run::Input;
                        fields[field_size++] = static_cast<unsigned char>(value);
                    }
                    if (fields[0] != 31 || fields[1] != 139 || fields[2] != 8 ||
                        (fields[3] & 224) != 0) {
                        c.fail(Error::InvalidData());
                        return Run::Error;
                    }
                    ++members;
                    flags          = fields[3];
                    unsigned mtime = 0;
                    for (unsigned i = 0; i < 4; ++i) mtime |= unsigned(fields[4 + i]) << (i * 8);
                    gzip_header = GzipHeader { u32(mtime), u8(fields[9]), u8(flags) };
                    has_header  = true;
                    field_size  = 0;
                    if ((flags & 4) != 0)
                        state = State::GzipExtraLength;
                    else
                        after_extra();
                }
                break;
            case State::GzipExtraLength:
                while (field_size < 2) {
                    if (! header_byte(c, value)) return Run::Input;
                    fields[field_size++] = static_cast<unsigned char>(value);
                }
                extra = unsigned(fields[0]) | (unsigned(fields[1]) << 8);
                state = State::GzipExtra;
                break;
            case State::GzipExtra:
                while (extra != 0) {
                    if (! header_byte(c, value)) return Run::Input;
                    --extra;
                }
                after_extra();
                break;
            case State::GzipName:
                do {
                    if (! header_byte(c, value)) return Run::Input;
                } while (value != 0);
                after_name();
                break;
            case State::GzipComment:
                do {
                    if (! header_byte(c, value)) return Run::Input;
                } while (value != 0);
                after_comment();
                break;
            case State::GzipCrc:
                while (field_size < 2) {
                    if (! header_byte(c, value, false)) return Run::Input;
                    fields[field_size++] = static_cast<unsigned char>(value);
                }
                if ((header_crc.value().to_primitive() & 65535u) !=
                    (unsigned(fields[0]) | (unsigned(fields[1]) << 8))) {
                    c.fail(Error::ChecksumMismatch());
                    return Run::Error;
                }
                field_size = 0;
                state      = State::Body;
                break;
            case State::Dictionary:
                while (field_size < 4) {
                    if (! c.byte(value)) return Run::Input;
                    fields[field_size++] = static_cast<unsigned char>(value);
                }
                value = 0;
                for (unsigned i = 0; i < 4; ++i) value = (value << 8) | fields[i];
                c.fail(Error::DictionaryRequired(u32(value)));
                return Run::Error;
            case State::Body: {
                auto before = c.written;
                auto status = raw.run(c);
                auto bytes  = slice<u8>::from_raw_parts(
                    reinterpret_cast<const rstd::byte*>(c.output + before),
                    usize(c.written - before));
                crc.update(bytes);
                adler.update(bytes);
                member_output += c.written - before;
                if (status != Run::Done) return status;
                if (format == Format::Deflate) {
                    state = State::Done;
                    break;
                }
                state      = State::Trailer;
                field_size = 0;
            } break;
            case State::Trailer: {
                auto required = format == Format::Zlib ? 4u : 8u;
                while (field_size < required) {
                    if (! c.byte(value)) return Run::Input;
                    fields[field_size++] = static_cast<unsigned char>(value);
                }
                unsigned check = 0;
                if (format == Format::Zlib)
                    for (unsigned i = 0; i < 4; ++i) check = (check << 8) | fields[i];
                else
                    for (unsigned i = 0; i < 4; ++i) check |= unsigned(fields[i]) << (i * 8);
                if (check != (format == Format::Zlib ? adler.value().to_primitive()
                                                     : crc.value().to_primitive())) {
                    c.fail(Error::ChecksumMismatch());
                    return Run::Error;
                }
                if (required == 8) {
                    unsigned size = 0;
                    for (unsigned i = 0; i < 4; ++i) size |= unsigned(fields[4 + i]) << (i * 8);
                    if (size != static_cast<unsigned>(member_output)) {
                        c.fail(Error::ChecksumMismatch());
                        return Run::Error;
                    }
                }
                state = format == Format::MultiGzip ? State::Between : State::Done;
            } break;
            case State::Between:
                if (c.consumed == c.input_size) return Run::Input;
                raw.reset(1u << options.max_window_bits);
                crc.reset();
                adler.reset();
                header_crc.reset();
                member_output = 0;
                field_size    = 0;
                state         = State::Header;
                break;
            case State::Done: return Run::Done;
            }
        }
    }
};

auto make_encoder(Format format, EncodeOptions options) -> CodecResult<EncoderState*> {
    if (format == Format::MultiGzip || static_cast<unsigned>(options.compression) > 3)
        return Err(Error::InvalidOptions());
    auto state = Box<EncoderState>::try_make(format, options);
    if (state.is_err()) return Err(Error::Allocation());
    return Ok(rstd::move(state.unwrap_unchecked()).into_raw().as_raw_ptr());
}
auto make_decoder(Format format, DecodeOptions options) -> CodecResult<DecoderState*> {
    if (options.max_window_bits < 8 || options.max_window_bits > 15 ||
        ((format == Format::Gzip || format == Format::MultiGzip) &&
         options.limits.max_members == u64(0)))
        return Err(Error::InvalidOptions());
    auto state = Box<DecoderState>::try_make(format, options);
    if (state.is_err()) return Err(Error::Allocation());
    return Ok(rstd::move(state.unwrap_unchecked()).into_raw().as_raw_ptr());
}
void destroy_encoder(EncoderState* state) noexcept {
    if (state) {
        auto box = Box<EncoderState>::from_raw(rstd::mut_ptr<EncoderState>::from_raw_parts(state));
    }
}
void destroy_decoder(DecoderState* state) noexcept {
    if (state) {
        auto box = Box<DecoderState>::from_raw(rstd::mut_ptr<DecoderState>::from_raw_parts(state));
    }
}
auto encoder_total_in(const EncoderState* state) noexcept -> u64 {
    return u64(state ? state->total_in : 0);
}
auto encoder_total_out(const EncoderState* state) noexcept -> u64 {
    return u64(state ? state->total_out : 0);
}
auto decoder_total_in(const DecoderState* state) noexcept -> u64 {
    return u64(state ? state->total_in : 0);
}
auto decoder_total_out(const DecoderState* state) noexcept -> u64 {
    return u64(state ? state->total_out : 0);
}
auto decoder_header(const DecoderState* state) noexcept -> Option<GzipHeader> {
    return state && state->has_header ? Some(GzipHeader(state->gzip_header)) : None();
}

auto encode(EncoderState* s, slice<u8> input, mut_ref<u8[]> output, EncodeAction action)
    -> CodecResult<Progress> {
    if (! s || s->failed || s->done || static_cast<unsigned>(action) > 2 ||
        (s->final_input &&
         (action != EncodeAction::Finish || input.len().to_primitive() != s->remaining)) ||
        (s->flushing && action != EncodeAction::SyncFlush))
        return Err(Error::InvalidState());
    if (action == EncodeAction::Finish) s->final_input = true;
    Call c { reinterpret_cast<const unsigned char*>(input.as_raw_ptr()),
             input.len().to_primitive(),
             reinterpret_cast<unsigned char*>(output.as_raw_ptr()),
             output.len().to_primitive(),
             s->total_in,
             s->total_out,
             s->options.max_output_bytes.to_primitive() };
    auto result = [&](StreamStatus status) -> CodecResult<Progress> {
        s->remaining = c.input_size - c.consumed;
        if (c.failed) {
            s->failed = true;
            return Err(rstd::move(c.error).at(u64(c.total_in), u64(c.total_out)));
        }
        return Ok(Progress { usize(c.consumed), usize(c.written), status });
    };
    for (;;) {
        if (! s->raw.drain(c)) return result(StreamStatus::NeedOutput);
        if (s->ending) {
            s->done = true;
            return result(StreamStatus::StreamEnd);
        }
        if (s->flushing) {
            s->flushing = false;
            return result(StreamStatus::FlushComplete);
        }
        if (! s->header) {
            s->header = true;
            if (s->format == Format::Zlib) {
                unsigned flag = s->options.compression == Compression::Fast   ? 0
                                : s->options.compression == Compression::Best ? 192
                                                                              : 128;
                flag += 31 - ((0x7800u + flag) % 31);
                s->raw.byte(0x78);
                s->raw.byte(flag);
            } else if (s->format == Format::Gzip) {
                s->raw.byte(31);
                s->raw.byte(139);
                s->raw.byte(8);
                s->raw.byte(0);
                s->raw.integer(s->options.gzip_mtime.to_primitive(), 4);
                s->raw.byte(0);
                s->raw.byte(s->options.gzip_os.to_primitive());
            }
            if (s->raw.pending_size != 0) continue;
        }
        auto before    = c.consumed;
        auto room      = 32768u - s->raw.filled;
        auto available = c.input_size - c.consumed;
        auto count     = available < room ? static_cast<unsigned>(available) : room;
        if (Count(count) > ~Count(0) - s->total_in) {
            c.fail(Error::LimitExceeded());
            return result(StreamStatus::Progress);
        }
        for (unsigned i = 0; i < count; ++i)
            s->raw.data[s->raw.history + s->raw.filled + i] = c.input[c.consumed + i];
        auto accepted = slice<u8>::from_raw_parts(
            reinterpret_cast<const rstd::byte*>(c.input + before), usize(count));
        s->crc.update(accepted);
        s->adler.update(accepted);
        s->raw.filled += count;
        c.consumed += count;
        s->total_in += count;
        bool exhausted = c.consumed == c.input_size;
        if (s->raw.filled == 32768 || (exhausted && action != EncodeAction::Continue)) {
            bool final = exhausted && action == EncodeAction::Finish;
            if (s->raw.filled != 0 || final) s->raw.block(s->options.compression, final);
            if (final) {
                if (s->format == Format::Zlib)
                    s->raw.integer(s->adler.value().to_primitive(), 4, true);
                else if (s->format == Format::Gzip) {
                    s->raw.integer(s->crc.value().to_primitive(), 4);
                    s->raw.integer(static_cast<unsigned>(s->total_in), 4);
                }
                s->ending = true;
            } else if (exhausted && action == EncodeAction::SyncFlush) {
                s->raw.sync_flush();
                s->flushing = true;
            }
            continue;
        }
        return result(c.consumed != 0 || c.written != 0 ? StreamStatus::Progress
                                                        : StreamStatus::NeedInput);
    }
}

auto decode(DecoderState* s, slice<u8> input, mut_ref<u8[]> output, InputEnd end)
    -> CodecResult<Progress> {
    if (! s || s->failed || s->state == DecoderState::State::Done ||
        static_cast<unsigned>(end) > 1 ||
        (s->final_input && (end != InputEnd::End || input.len().to_primitive() != s->remaining)))
        return Err(Error::InvalidState());
    if (end == InputEnd::End) s->final_input = true;
    Call c { reinterpret_cast<const unsigned char*>(input.as_raw_ptr()),
             input.len().to_primitive(),
             reinterpret_cast<unsigned char*>(output.as_raw_ptr()),
             output.len().to_primitive(),
             s->total_in,
             s->total_out,
             s->options.limits.max_output_bytes.to_primitive() };
    auto status  = s->run(c);
    s->remaining = c.input_size - c.consumed;
    if (! c.failed && status == Run::Input && end == InputEnd::End) {
        if (s->state == DecoderState::State::Between) {
            s->state = DecoderState::State::Done;
            status   = Run::Done;
        } else
            c.fail(Error::UnexpectedEof());
    }
    if (c.failed || status == Run::Error) {
        s->failed = true;
        return Err(rstd::move(c.error).at(u64(c.total_in), u64(c.total_out)));
    }
    return Ok(Progress { usize(c.consumed),
                         usize(c.written),
                         status == Run::Done     ? StreamStatus::StreamEnd
                         : status == Run::Output ? StreamStatus::NeedOutput
                                                 : StreamStatus::NeedInput });
}

} // namespace licompto
