#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mineswitch {
// Minimal Named Binary Tag tree, enough to read level.dat and to edit
// servers.dat without dropping fields this launcher does not understand.
enum class TagType : std::uint8_t {
    End, Byte, Short, Int, Long, Float, Double, ByteArray, String, List, Compound, IntArray, LongArray
};
struct Tag {
    TagType type = TagType::End;
    std::string name;
    std::int64_t integer = 0; // Byte, Short, Int, Long
    double real = 0;          // Float, Double
    std::string text;         // String
    std::vector<std::int64_t> numbers; // ByteArray, IntArray, LongArray
    TagType list_type = TagType::End;
    std::vector<Tag> children; // List items (unnamed) and Compound members

    const Tag* find(const std::string& key) const;
    Tag* find(const std::string& key);
    Tag& set(const std::string& key, TagType type); // Creates or replaces the type.
    std::string string(const std::string& key) const;
    std::int64_t number(const std::string& key, std::int64_t fallback = 0) const;
};
// Accepts raw, gzip or zlib input. Throws Error("nbt.invalid", ...).
Tag parseNbt(const std::string& bytes, std::size_t maximum_uncompressed = 32 * 1024 * 1024);
std::string writeNbt(const Tag& root); // Uncompressed, as servers.dat uses.
// gzip/zlib helpers shared by level.dat and compressed log viewing.
std::string inflateData(const std::string& compressed, std::size_t maximum);
std::string gzipData(const std::string& raw);
} // namespace mineswitch
