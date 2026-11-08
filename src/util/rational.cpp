#include "util/rational.h"

#include <charconv>
#include <limits>
#include <numeric>
#include <stdexcept>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace loki {

namespace {

constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Signed 128-bit product split in high (signed) and low (unsigned) halves.
struct Wide {
    std::int64_t hi;
    std::uint64_t lo;
};

Wide mulWide(std::int64_t a, std::int64_t b) noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
    std::int64_t hi = 0;
    const std::int64_t lo = _mul128(a, b, &hi);
    return {hi, static_cast<std::uint64_t>(lo)};
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
    return {__mulh(a, b), static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)};
#else
    const __int128 p = static_cast<__int128>(a) * static_cast<__int128>(b);
    return {static_cast<std::int64_t>(p >> 64), static_cast<std::uint64_t>(p)};
#endif
}

std::strong_ordering compareWide(const Wide& a, const Wide& b) noexcept {
    if (a.hi != b.hi) {
        return a.hi <=> b.hi;
    }
    return a.lo <=> b.lo;
}

[[noreturn]] void throwOverflow() {
    throw std::overflow_error("Rational arithmetic overflow");
}

std::int64_t checkedMul(std::int64_t a, std::int64_t b) {
    const Wide w = mulWide(a, b);
    // Fits in int64 iff the high half is the sign extension of the low half.
    const auto lo = static_cast<std::int64_t>(w.lo);
    if (w.hi != (lo < 0 ? -1 : 0) || lo == kInt64Min) {
        throwOverflow();
    }
    return lo;
}

std::int64_t checkedAdd(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > kInt64Max - b) || (b < 0 && a < kInt64Min - b)) {
        throwOverflow();
    }
    const std::int64_t r = a + b;
    if (r == kInt64Min) {
        throwOverflow();
    }
    return r;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept { // b > 0
    std::int64_t q = a / b;
    if ((a % b) != 0 && a < 0) {
        --q;
    }
    return q;
}

} // namespace

Rational::Rational(std::int64_t num, std::int64_t den) {
    if (den == 0) {
        throw std::domain_error("Rational denominator is zero");
    }
    if (num == kInt64Min || den == kInt64Min) {
        throwOverflow();
    }
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const std::int64_t g = std::gcd(num, den);
    m_num = num / g;
    m_den = den / g;
}

std::int64_t Rational::floor() const noexcept {
    return floorDiv(m_num, m_den);
}

std::int64_t Rational::ceil() const noexcept {
    return -floorDiv(-m_num, m_den);
}

std::int64_t Rational::round() const noexcept {
    // |num| < 2^63 and den >= 1, so 2*|num| + den may exceed int64: work on quotient/remainder.
    const std::int64_t q = m_num / m_den;
    const std::int64_t r = m_num % m_den;
    const std::int64_t absR = r < 0 ? -r : r;
    if (absR >= m_den - absR) { // |r|/den >= 1/2
        return m_num < 0 ? q - 1 : q + 1;
    }
    return q;
}

Rational Rational::abs() const {
    return m_num < 0 ? -*this : *this;
}

Rational Rational::reciprocal() const {
    if (m_num == 0) {
        throw std::domain_error("Reciprocal of zero");
    }
    return Rational(m_den, m_num);
}

double Rational::toDouble() const noexcept {
    return static_cast<double>(m_num) / static_cast<double>(m_den);
}

std::string Rational::toString() const {
    if (m_den == 1) {
        return std::to_string(m_num);
    }
    return std::to_string(m_num) + "/" + std::to_string(m_den);
}

std::optional<Rational> Rational::parse(std::string_view text) {
    const auto parseInt = [](std::string_view s) -> std::optional<std::int64_t> {
        if (!s.empty() && s.front() == '+') {
            s.remove_prefix(1);
        }
        std::int64_t value = 0;
        const auto* end = s.data() + s.size();
        const auto [ptr, ec] = std::from_chars(s.data(), end, value);
        if (s.empty() || ec != std::errc() || ptr != end) {
            return std::nullopt;
        }
        return value;
    };

    const auto slash = text.find('/');
    const auto num = parseInt(text.substr(0, slash));
    const auto den = slash == std::string_view::npos ? std::optional<std::int64_t>(1) : parseInt(text.substr(slash + 1));
    if (!num || !den || *den == 0) {
        return std::nullopt;
    }
    try {
        return Rational(*num, *den);
    } catch (const std::overflow_error&) {
        return std::nullopt;
    }
}

Rational Rational::operator-() const {
    Rational r;
    r.m_num = -m_num; // safe: m_num != INT64_MIN by invariant
    r.m_den = m_den;
    return r;
}

Rational operator+(const Rational& a, const Rational& b) {
    // a/b + c/d = (a*(d/g) + c*(b/g)) / (b/g*d), g = gcd(b, d): keeps intermediates small.
    const std::int64_t g = std::gcd(a.m_den, b.m_den);
    const std::int64_t num = checkedAdd(checkedMul(a.m_num, b.m_den / g), checkedMul(b.m_num, a.m_den / g));
    const std::int64_t den = checkedMul(a.m_den / g, b.m_den);
    return Rational(num, den);
}

Rational operator-(const Rational& a, const Rational& b) {
    return a + (-b);
}

Rational operator*(const Rational& a, const Rational& b) {
    // Cross-reduce before multiplying to avoid spurious overflow.
    const std::int64_t g1 = std::gcd(a.m_num, b.m_den);
    const std::int64_t g2 = std::gcd(b.m_num, a.m_den);
    const std::int64_t n1 = g1 == 0 ? a.m_num : a.m_num / g1;
    const std::int64_t d2 = g1 == 0 ? b.m_den : b.m_den / g1;
    const std::int64_t n2 = g2 == 0 ? b.m_num : b.m_num / g2;
    const std::int64_t d1 = g2 == 0 ? a.m_den : a.m_den / g2;
    return Rational(checkedMul(n1, n2), checkedMul(d1, d2));
}

Rational operator/(const Rational& a, const Rational& b) {
    if (b.m_num == 0) {
        throw std::domain_error("Rational division by zero");
    }
    return a * b.reciprocal();
}

std::strong_ordering operator<=>(const Rational& a, const Rational& b) noexcept {
    // Denominators are positive, so a/b <=> c/d  <=>  a*d <=> c*b (exact in 128 bits).
    return compareWide(mulWide(a.m_num, b.m_den), mulWide(b.m_num, a.m_den));
}

std::int64_t timeToFrames(const Rational& time, const Rational& frameRate, Rounding rounding) {
    if (!frameRate.isPositive()) {
        throw std::domain_error("Frame rate must be positive");
    }
    const Rational frames = time * frameRate;
    switch (rounding) {
    case Rounding::Floor: return frames.floor();
    case Rounding::Ceil: return frames.ceil();
    case Rounding::Nearest: return frames.round();
    }
    return frames.floor();
}

Rational framesToTime(std::int64_t frame, const Rational& frameRate) {
    if (!frameRate.isPositive()) {
        throw std::domain_error("Frame rate must be positive");
    }
    return Rational(frame) / frameRate;
}

Rational quantizeToFrame(const Rational& time, const Rational& frameRate, Rounding rounding) {
    return framesToTime(timeToFrames(time, frameRate, rounding), frameRate);
}

} // namespace loki
