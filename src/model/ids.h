#pragma once

#include "util/uuid.h"

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace loki::model {

// Strongly typed identifier: a MediaId cannot be passed where a ClipId is expected.
template <typename Tag>
class Id {
public:
    constexpr Id() noexcept = default; // null id
    explicit constexpr Id(const Uuid& uuid) noexcept : m_uuid(uuid) {}

    [[nodiscard]] static Id generate() { return Id(Uuid::generate()); }
    [[nodiscard]] static std::optional<Id> parse(std::string_view text) noexcept {
        if (auto uuid = Uuid::parse(text)) {
            return Id(*uuid);
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr const Uuid& uuid() const noexcept { return m_uuid; }
    [[nodiscard]] constexpr bool isNull() const noexcept { return m_uuid.isNull(); }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return !isNull(); }
    [[nodiscard]] std::string toString() const { return m_uuid.toString(); }

    friend constexpr bool operator==(const Id&, const Id&) noexcept = default;
    friend constexpr auto operator<=>(const Id&, const Id&) noexcept = default;

private:
    Uuid m_uuid;
};

using MediaId = Id<struct MediaTag>;
using BinId = Id<struct BinTag>;
using SequenceId = Id<struct SequenceTag>;
using TrackId = Id<struct TrackTag>;
using ClipId = Id<struct ClipTag>;
using EffectId = Id<struct EffectTag>;
using LinkId = Id<struct LinkTag>;
using TransitionId = Id<struct TransitionTag>;

} // namespace loki::model

template <typename Tag>
struct std::hash<loki::model::Id<Tag>> {
    std::size_t operator()(const loki::model::Id<Tag>& id) const noexcept { return std::hash<loki::Uuid>{}(id.uuid()); }
};
