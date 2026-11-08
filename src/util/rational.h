#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace loki {

// Exact rational number (num/den, int64) used for every edit time in Loki.
// Invariants: den > 0, gcd(|num|, den) == 1, num and den never equal INT64_MIN.
// Arithmetic is exact; results that do not fit in int64 throw std::overflow_error.
// Division by zero (or a zero denominator) throws std::domain_error.
class Rational {
public:
    constexpr Rational() noexcept = default;
    Rational(std::int64_t num, std::int64_t den = 1);

    [[nodiscard]] constexpr std::int64_t num() const noexcept { return m_num; }
    [[nodiscard]] constexpr std::int64_t den() const noexcept { return m_den; }

    [[nodiscard]] constexpr bool isZero() const noexcept { return m_num == 0; }
    [[nodiscard]] constexpr bool isNegative() const noexcept { return m_num < 0; }
    [[nodiscard]] constexpr bool isPositive() const noexcept { return m_num > 0; }
    [[nodiscard]] constexpr bool isInteger() const noexcept { return m_den == 1; }

    // Largest integer <= value, smallest integer >= value, nearest integer (ties away from zero).
    [[nodiscard]] std::int64_t floor() const noexcept;
    [[nodiscard]] std::int64_t ceil() const noexcept;
    [[nodiscard]] std::int64_t round() const noexcept;

    [[nodiscard]] Rational abs() const;
    [[nodiscard]] Rational reciprocal() const;

    // Presentation only (pixels, meters, UI). Never feed the result back into edit times.
    [[nodiscard]] double toDouble() const noexcept;

    // "num/den" (or "num" when den == 1).
    [[nodiscard]] std::string toString() const;
    // Accepts "num/den" or "num" with optional sign; returns nullopt on malformed input or den == 0.
    [[nodiscard]] static std::optional<Rational> parse(std::string_view text);

    Rational operator-() const;
    friend Rational operator+(const Rational& a, const Rational& b);
    friend Rational operator-(const Rational& a, const Rational& b);
    friend Rational operator*(const Rational& a, const Rational& b);
    friend Rational operator/(const Rational& a, const Rational& b);
    Rational& operator+=(const Rational& o) { return *this = *this + o; }
    Rational& operator-=(const Rational& o) { return *this = *this - o; }
    Rational& operator*=(const Rational& o) { return *this = *this * o; }
    Rational& operator/=(const Rational& o) { return *this = *this / o; }

    friend constexpr bool operator==(const Rational& a, const Rational& b) noexcept {
        return a.m_num == b.m_num && a.m_den == b.m_den; // canonical form makes this exact
    }
    friend std::strong_ordering operator<=>(const Rational& a, const Rational& b) noexcept;

private:
    std::int64_t m_num = 0;
    std::int64_t m_den = 1;
};

enum class Rounding { Floor, Ceil, Nearest };

// Number of whole frames at `frameRate` (frames per second) contained in `time`.
[[nodiscard]] std::int64_t timeToFrames(const Rational& time, const Rational& frameRate,
                                        Rounding rounding = Rounding::Floor);
// Start time of frame `frame` at `frameRate`.
[[nodiscard]] Rational framesToTime(std::int64_t frame, const Rational& frameRate);
// Snaps `time` to the frame grid of `frameRate`.
[[nodiscard]] Rational quantizeToFrame(const Rational& time, const Rational& frameRate,
                                       Rounding rounding = Rounding::Nearest);

[[nodiscard]] inline const Rational& min(const Rational& a, const Rational& b) noexcept { return b < a ? b : a; }
[[nodiscard]] inline const Rational& max(const Rational& a, const Rational& b) noexcept { return a < b ? b : a; }

} // namespace loki

template <>
struct std::hash<loki::Rational> {
    std::size_t operator()(const loki::Rational& r) const noexcept {
        const auto h1 = std::hash<std::int64_t>{}(r.num());
        const auto h2 = std::hash<std::int64_t>{}(r.den());
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};
