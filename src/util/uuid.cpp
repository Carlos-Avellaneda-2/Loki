#include "util/uuid.h"

#include <random>

namespace loki {

namespace {

constexpr std::array<std::size_t, 4> kDashPositions = {8, 13, 18, 23};

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

Uuid Uuid::generate() {
    std::random_device device;
    Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const auto r = static_cast<std::uint32_t>(device());
        for (std::size_t j = 0; j < 4; ++j) {
            bytes[i + j] = static_cast<std::uint8_t>((r >> (8 * j)) & 0xFFU);
        }
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U); // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U); // RFC 4122 variant
    return Uuid(bytes);
}

std::optional<Uuid> Uuid::parse(std::string_view text) noexcept {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text = text.substr(1, 36);
    }
    if (text.size() != 36) {
        return std::nullopt;
    }
    Bytes bytes{};
    std::size_t byteIndex = 0;
    std::size_t dash = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (dash < kDashPositions.size() && i == kDashPositions[dash]) {
            if (text[i] != '-') {
                return std::nullopt;
            }
            ++dash;
            ++i;
            continue;
        }
        const int hi = hexValue(text[i]);
        const int lo = i + 1 < text.size() ? hexValue(text[i + 1]) : -1;
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        bytes[byteIndex++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return Uuid(bytes);
}

std::string Uuid::toString() const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < m_bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[m_bytes[i] >> 4]);
        out.push_back(kHex[m_bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace loki
