#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace loki {

// RFC 4122 version-4 UUID. Used as stable identity for model objects (survives save/load and
// copy/paste between projects).
class Uuid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    constexpr Uuid() noexcept = default; // null UUID
    explicit constexpr Uuid(const Bytes& bytes) noexcept : m_bytes(bytes) {}

    // Cryptographically random v4 UUID (std::random_device; OS entropy on Windows/Linux/macOS).
    [[nodiscard]] static Uuid generate();
    // Accepts the canonical 8-4-4-4-12 hex form, case-insensitive, optionally wrapped in braces.
    [[nodiscard]] static std::optional<Uuid> parse(std::string_view text) noexcept;

    [[nodiscard]] std::string toString() const; // lowercase 8-4-4-4-12
    [[nodiscard]] constexpr bool isNull() const noexcept { return m_bytes == Bytes{}; }
    [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return m_bytes; }

    friend constexpr bool operator==(const Uuid&, const Uuid&) noexcept = default;
    friend constexpr auto operator<=>(const Uuid&, const Uuid&) noexcept = default;

private:
    Bytes m_bytes{};
};

} // namespace loki

template <>
struct std::hash<loki::Uuid> {
    std::size_t operator()(const loki::Uuid& id) const noexcept {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            a = (a << 8) | id.bytes()[i];
            b = (b << 8) | id.bytes()[i + 8];
        }
        return std::hash<std::uint64_t>{}(a ^ (b * 0x9e3779b97f4a7c15ULL));
    }
};
