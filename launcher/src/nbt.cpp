#include "mineswitch/nbt.hpp"
#include "mineswitch/model.hpp"
#include <cstring>
#include <zlib.h>

namespace mineswitch {
namespace {
[[noreturn]] void invalid(const char* message) { throw Error("nbt.invalid", message); }

} // namespace

std::string inflateData(const std::string& input, std::size_t maximum) {
    z_stream stream{};
    // 15 + 32 detects gzip and zlib headers automatically.
    if (inflateInit2(&stream, 15 + 32) != Z_OK) invalid("Cannot start decompression");
    std::string output;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    char buffer[65536];
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            invalid("Corrupt compressed NBT");
        }
        output.append(buffer, sizeof(buffer) - stream.avail_out);
        if (output.size() > maximum) {
            inflateEnd(&stream);
            invalid("NBT exceeds its size limit");
        }
        if (result == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) {
            inflateEnd(&stream);
            invalid("Truncated compressed NBT");
        }
    }
    inflateEnd(&stream);
    return output;
}

std::string gzipData(const std::string& input) {
    z_stream stream{};
    // 15 + 16 writes a gzip wrapper, the format Minecraft uses for level.dat.
    if (deflateInit2(&stream, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw Error("nbt.compress", "Cannot start compression");
    std::string output(deflateBound(&stream, input.size()) + 32, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(&output[0]);
    stream.avail_out = static_cast<uInt>(output.size());
    int result = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) throw Error("nbt.compress", "Cannot compress NBT");
    output.resize(stream.total_out);
    return output;
}

namespace {
class Reader {
    const std::string& data_;
    std::size_t at_ = 0;
    unsigned depth_ = 0;

    void need(std::size_t bytes) {
        if (bytes > data_.size() - at_) invalid("Truncated NBT");
    }
    std::uint64_t unsigned_value(unsigned bytes) {
        need(bytes);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | static_cast<unsigned char>(data_[at_++]);
        return value;
    }
    std::int64_t signed_value(unsigned bytes) {
        auto value = unsigned_value(bytes);
        if (bytes < 8 && (value >> (bytes * 8 - 1))) value |= ~0ULL << (bytes * 8);
        return static_cast<std::int64_t>(value);
    }
    std::size_t length() {
        auto value = signed_value(4);
        if (value < 0 || static_cast<std::uint64_t>(value) > data_.size()) invalid("Invalid NBT length");
        return static_cast<std::size_t>(value);
    }

  public:
    explicit Reader(const std::string& data) : data_(data) {}
    std::string utf() {
        auto bytes = static_cast<std::size_t>(unsigned_value(2));
        need(bytes);
        auto value = data_.substr(at_, bytes);
        at_ += bytes;
        return value;
    }
    TagType type() {
        auto value = unsigned_value(1);
        if (value > static_cast<unsigned>(TagType::LongArray)) invalid("Unknown NBT tag type");
        return static_cast<TagType>(value);
    }
    void payload(Tag& tag) {
        if (++depth_ > 512) invalid("NBT nesting is too deep");
        switch (tag.type) {
        case TagType::Byte: tag.integer = signed_value(1); break;
        case TagType::Short: tag.integer = signed_value(2); break;
        case TagType::Int: tag.integer = signed_value(4); break;
        case TagType::Long: tag.integer = signed_value(8); break;
        case TagType::Float: {
            auto bits = static_cast<std::uint32_t>(unsigned_value(4));
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            tag.real = value;
            break;
        }
        case TagType::Double: {
            auto bits = unsigned_value(8);
            std::memcpy(&tag.real, &bits, sizeof(tag.real));
            break;
        }
        case TagType::String: tag.text = utf(); break;
        case TagType::ByteArray:
        case TagType::IntArray:
        case TagType::LongArray: {
            unsigned width = tag.type == TagType::ByteArray ? 1 : tag.type == TagType::IntArray ? 4 : 8;
            auto count = length();
            need(count * width);
            tag.numbers.reserve(count);
            for (std::size_t i = 0; i < count; ++i) tag.numbers.push_back(signed_value(width));
            break;
        }
        case TagType::List: {
            tag.list_type = type();
            auto count = length();
            if (tag.list_type == TagType::End && count) invalid("Typed NBT list has no element type");
            for (std::size_t i = 0; i < count; ++i) {
                Tag item;
                item.type = tag.list_type;
                payload(item);
                tag.children.push_back(std::move(item));
            }
            break;
        }
        case TagType::Compound:
            while (true) {
                Tag item;
                item.type = type();
                if (item.type == TagType::End) break;
                item.name = utf();
                payload(item);
                tag.children.push_back(std::move(item));
            }
            break;
        case TagType::End: invalid("Unexpected NBT end tag");
        }
        --depth_;
    }
};

void put(std::string& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i = bytes; i-- > 0;) out += static_cast<char>((value >> (i * 8)) & 255);
}
void putUtf(std::string& out, const std::string& text) {
    if (text.size() > 65535) throw Error("nbt.invalid", "NBT string is too long");
    put(out, text.size(), 2);
    out += text;
}
void writePayload(std::string& out, const Tag& tag) {
    switch (tag.type) {
    case TagType::Byte: put(out, static_cast<std::uint64_t>(tag.integer), 1); break;
    case TagType::Short: put(out, static_cast<std::uint64_t>(tag.integer), 2); break;
    case TagType::Int: put(out, static_cast<std::uint64_t>(tag.integer), 4); break;
    case TagType::Long: put(out, static_cast<std::uint64_t>(tag.integer), 8); break;
    case TagType::Float: {
        float value = static_cast<float>(tag.real);
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        put(out, bits, 4);
        break;
    }
    case TagType::Double: {
        std::uint64_t bits;
        std::memcpy(&bits, &tag.real, sizeof(bits));
        put(out, bits, 8);
        break;
    }
    case TagType::String: putUtf(out, tag.text); break;
    case TagType::ByteArray:
    case TagType::IntArray:
    case TagType::LongArray: {
        unsigned width = tag.type == TagType::ByteArray ? 1 : tag.type == TagType::IntArray ? 4 : 8;
        put(out, tag.numbers.size(), 4);
        for (auto value : tag.numbers) put(out, static_cast<std::uint64_t>(value), width);
        break;
    }
    case TagType::List: {
        auto element = tag.children.empty() ? tag.list_type : tag.children.front().type;
        put(out, static_cast<unsigned>(element), 1);
        put(out, tag.children.size(), 4);
        for (const auto& item : tag.children) {
            if (item.type != element) throw Error("nbt.invalid", "NBT list mixes element types");
            writePayload(out, item);
        }
        break;
    }
    case TagType::Compound:
        for (const auto& item : tag.children) {
            put(out, static_cast<unsigned>(item.type), 1);
            putUtf(out, item.name);
            writePayload(out, item);
        }
        put(out, 0, 1);
        break;
    case TagType::End: throw Error("nbt.invalid", "Cannot write an NBT end tag");
    }
}
} // namespace

const Tag* Tag::find(const std::string& key) const {
    for (const auto& child : children)
        if (child.name == key) return &child;
    return nullptr;
}
Tag* Tag::find(const std::string& key) {
    for (auto& child : children)
        if (child.name == key) return &child;
    return nullptr;
}
Tag& Tag::set(const std::string& key, TagType kind) {
    auto* existing = find(key);
    if (!existing) {
        children.emplace_back();
        existing = &children.back();
        existing->name = key;
    }
    if (existing->type != kind) {
        *existing = Tag{};
        existing->name = key;
        existing->type = kind;
    }
    return *existing;
}
std::string Tag::string(const std::string& key) const {
    auto* value = find(key);
    return value && value->type == TagType::String ? value->text : std::string();
}
std::int64_t Tag::number(const std::string& key, std::int64_t fallback) const {
    auto* value = find(key);
    if (!value) return fallback;
    switch (value->type) {
    case TagType::Byte:
    case TagType::Short:
    case TagType::Int:
    case TagType::Long: return value->integer;
    default: return fallback;
    }
}

Tag parseNbt(const std::string& bytes, std::size_t maximum) {
    if (bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0x1f && static_cast<unsigned char>(bytes[1]) == 0x8b) ||
                              static_cast<unsigned char>(bytes[0]) == 0x78)) {
        auto raw = inflateData(bytes, maximum);
        return parseNbt(raw, maximum);
    }
    if (bytes.size() > maximum) invalid("NBT exceeds its size limit");
    Reader reader(bytes);
    Tag root;
    root.type = reader.type();
    if (root.type != TagType::Compound) invalid("NBT root must be a compound");
    root.name = reader.utf();
    reader.payload(root);
    return root;
}

std::string writeNbt(const Tag& root) {
    if (root.type != TagType::Compound) throw Error("nbt.invalid", "NBT root must be a compound");
    std::string out;
    put(out, static_cast<unsigned>(TagType::Compound), 1);
    putUtf(out, root.name);
    writePayload(out, root);
    return out;
}
} // namespace mineswitch
