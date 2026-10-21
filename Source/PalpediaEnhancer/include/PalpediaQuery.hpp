#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace PalpediaEnhancer
{
    // What the search box matches; any combination via the toggles, or one via a prefix on an
    // individual atom of the query (name:, drops:, ranch:, desc:) - a query can carry more than
    // one prefix at once, each scoped to just the word or group it precedes. Shared between the
    // query parser (this file) and the row filter (PalpediaFilter.hpp) so a prefix bit always
    // means the same target in both.
    enum class SearchTarget : uint8_t
    {
        Name = 0,
        Drops,       // items in the defeat/capture drop table
        Ranch,       // items in the partner skill's "Sometimes drops # when assigned to Ranch" sentence only
        Description, // free text over partner skill name + text and both Palpedia descriptions
    };

    constexpr auto TargetBit(SearchTarget target) -> uint8_t
    {
        return static_cast<uint8_t>(1u << static_cast<uint8_t>(target));
    }

    namespace Query
    {
        enum class SegmentKind : uint8_t
        {
            Literal,
            Star,     // '*': zero or more characters
            Question, // '?': exactly one character
            Brace,    // '{a,b,c}': one of the listed literal alternatives
        };

        struct Segment
        {
            SegmentKind Kind{SegmentKind::Literal};
            std::wstring Literal{};                   // Kind == Literal
            std::vector<std::wstring> Alternatives{};  // Kind == Brace
        };

        // Unanchored: matches if the segment sequence is found anywhere in haystack, same as
        // today's plain substring search when segments is a single Literal. Both segments and
        // haystack are expected to already be lowercased by the caller.
        auto PatternMatches(const std::vector<Segment>& segments, const std::wstring& haystack) -> bool;

        struct Term
        {
            std::vector<Segment> Segments{};
        };

        // Parses one atom's literal text into wildcard/brace segments. raw is expected to already
        // be lowercased and to have had its enclosing operators ('|', '-', '(', ')', a prefix
        // keyword) stripped by the caller - this function only sees the text between those
        // boundaries. Always returns raw, unnormalized segments; Drops/Ranch normalization
        // (stripping '_' and ' ') happens at match time in CompiledQuery, not here - see
        // NormalizedForDrops in PalpediaQuery.cpp.
        auto ParseTermSegments(std::wstring raw) -> Term;

        enum class NodeKind : uint8_t
        {
            Or,
            And,
            Not,
            Term,
        };

        struct Node
        {
            NodeKind Kind{NodeKind::Term};
            std::vector<Node> Children{};    // Or/And: operands. Not: exactly one child.
            uint8_t TargetMask{};            // Term only: which haystacks this term checks.
            bool HasExplicitTarget{};        // Term only: true if TargetMask came from a prefix, not the default.
            std::vector<Segment> Segments{}; // Term only.
        };

        // Parses a full query string (already lowercased by the caller) into an AST. Returns
        // std::nullopt when the query reduces to no constraints (an empty string, or every
        // construct in it was dropped per the malformed-input rules), meaning "match everything".
        // defaultTargetMask is the toggle-button mask used by any atom without an explicit prefix.
        auto ParseQuery(const std::wstring& lowered, uint8_t defaultTargetMask) -> std::optional<Node>;

        class CompiledQuery
        {
        public:
            struct Haystacks
            {
                const std::wstring* Name{};
                const std::wstring* Description{};
                const std::vector<std::wstring>* DropIds{};
                const std::vector<std::wstring>* RanchItems{};
            };

            // rawQuery is the search box's text as typed (any case); defaultTargetMask is the
            // toggle-button mask used by atoms without an explicit prefix.
            static auto Parse(const std::wstring& rawQuery, uint8_t defaultTargetMask) -> CompiledQuery;

            // True (matches everything) when the query is empty or reduced to no constraints.
            auto Matches(const Haystacks& haystacks) const -> bool;

            // The union of every Term's TargetMask anywhere in the AST - which haystacks this
            // compiled query could possibly need, independent of the default toggle-button mask
            // that produced any individual atom's default target. 0 when the query is empty.
            // PalpediaFilter's haystack construction is lazy, gated on whichever toggle buttons
            // are lit - a query with an explicit drops:/ranch:/desc: prefix needs this to force
            // that haystack to build even when the corresponding toggle button is off.
            auto ReferencedTargets() const -> uint8_t;

            auto IsEmpty() const -> bool
            {
                return !m_root.has_value();
            }

        private:
            std::optional<Node> m_root{};
        };
    } // namespace Query
} // namespace PalpediaEnhancer
