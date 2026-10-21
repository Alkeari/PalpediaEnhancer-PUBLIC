#include <PalpediaQuery.hpp>

#include <algorithm>
#include <cwchar>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace PalpediaEnhancer::Query
{
    namespace
    {
        auto AnchoredMatch(const std::vector<Segment>& segments, size_t segIdx, const std::wstring& haystack, size_t hayIdx)
                -> bool
        {
            if (segIdx == segments.size())
            {
                return hayIdx == haystack.size();
            }
            const auto& segment = segments[segIdx];
            switch (segment.Kind)
            {
                case SegmentKind::Star:
                    for (size_t len = 0; hayIdx + len <= haystack.size(); ++len)
                    {
                        if (AnchoredMatch(segments, segIdx + 1, haystack, hayIdx + len))
                        {
                            return true;
                        }
                    }
                    return false;
                case SegmentKind::Question:
                    return hayIdx < haystack.size() && AnchoredMatch(segments, segIdx + 1, haystack, hayIdx + 1);
                case SegmentKind::Literal:
                    return haystack.compare(hayIdx, segment.Literal.size(), segment.Literal) == 0 &&
                           AnchoredMatch(segments, segIdx + 1, haystack, hayIdx + segment.Literal.size());
                case SegmentKind::Brace:
                    for (const auto& alt : segment.Alternatives)
                    {
                        if (haystack.compare(hayIdx, alt.size(), alt) == 0 &&
                            AnchoredMatch(segments, segIdx + 1, haystack, hayIdx + alt.size()))
                        {
                            return true;
                        }
                    }
                    return false;
            }
            return false;
        }
    } // namespace

    // Wrapping with an implicit leading/trailing '*' reduces "found anywhere" (unanchored search)
    // to the anchored full-string match above - the standard technique for glob-style substring
    // search.
    auto PatternMatches(const std::vector<Segment>& segments, const std::wstring& haystack) -> bool
    {
        std::vector<Segment> wrapped;
        wrapped.reserve(segments.size() + 2);
        if (segments.empty() || segments.front().Kind != SegmentKind::Star)
        {
            wrapped.push_back({SegmentKind::Star, L"", {}});
        }
        wrapped.insert(wrapped.end(), segments.begin(), segments.end());
        if (segments.empty() || segments.back().Kind != SegmentKind::Star)
        {
            wrapped.push_back({SegmentKind::Star, L"", {}});
        }
        return AnchoredMatch(wrapped, 0, haystack, 0);
    }

    namespace
    {
        auto BracesBalanced(const std::wstring& raw) -> bool
        {
            int depth = 0;
            for (const auto ch : raw)
            {
                if (ch == L'{')
                {
                    ++depth;
                }
                else if (ch == L'}')
                {
                    --depth;
                    if (depth < 0)
                    {
                        return false;
                    }
                }
            }
            return depth == 0;
        }

        auto StripDropSeparators(std::wstring value) -> std::wstring
        {
            value.erase(std::remove_if(value.begin(), value.end(),
                                        [](wchar_t c) { return c == L'_' || c == L' '; }),
                        value.end());
            return value;
        }
    } // namespace

    auto ParseTermSegments(std::wstring raw) -> Term
    {
        const bool bracesEnabled = BracesBalanced(raw);

        Term term;
        std::wstring literal;
        const auto flushLiteral = [&]() {
            if (!literal.empty())
            {
                term.Segments.push_back({SegmentKind::Literal, std::move(literal), {}});
                literal.clear();
            }
        };

        size_t i = 0;
        while (i < raw.size())
        {
            const auto ch = raw[i];
            if (ch == L'*')
            {
                flushLiteral();
                if (term.Segments.empty() || term.Segments.back().Kind != SegmentKind::Star)
                {
                    term.Segments.push_back({SegmentKind::Star, L"", {}});
                }
                ++i;
            }
            else if (ch == L'?')
            {
                flushLiteral();
                term.Segments.push_back({SegmentKind::Question, L"", {}});
                ++i;
            }
            else if (ch == L'{' && bracesEnabled)
            {
                flushLiteral();
                const auto close = raw.find(L'}', i);
                const auto body = raw.substr(i + 1, close - i - 1);
                std::vector<std::wstring> alternatives;
                size_t start = 0;
                while (true)
                {
                    const auto comma = body.find(L',', start);
                    alternatives.push_back(body.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
                    if (comma == std::wstring::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
                term.Segments.push_back({SegmentKind::Brace, L"", std::move(alternatives)});
                i = close + 1;
            }
            else
            {
                literal += ch;
                ++i;
            }
        }
        flushLiteral();

        return term;
    }

    namespace
    {
        // Nesting cap purely as a stack-safety measure: each '(' costs several recursive-descent
        // stack frames, and a pathologically long pasted string of nested parens could otherwise
        // overflow the native stack - a real crash, unlike every other malformed-input case here.
        // No realistic query approaches this depth. Past the cap, '(' stops being treated as a
        // group opener and falls back to the same literal-character treatment as an unbalanced
        // paren.
        constexpr int kMaxGroupDepth = 32;

        struct Cursor
        {
            const std::wstring& Text;
            size_t Pos{};
            uint8_t DefaultTargetMask{};
            bool ParenGroupingEnabled{true};
            int Depth{};

            auto AtEnd() const -> bool
            {
                return Pos >= Text.size();
            }

            auto Peek() const -> wchar_t
            {
                return AtEnd() ? L'\0' : Text[Pos];
            }

            auto SkipSpaces() -> void
            {
                while (!AtEnd() && Text[Pos] == L' ')
                {
                    ++Pos;
                }
            }

            auto CanOpenGroup() const -> bool
            {
                return ParenGroupingEnabled && Depth < kMaxGroupDepth;
            }
        };

        constexpr std::pair<const wchar_t*, SearchTarget> kPrefixes[] = {
            {L"name:", SearchTarget::Name},
            {L"drops:", SearchTarget::Drops},
            {L"ranch:", SearchTarget::Ranch},
            {L"desc:", SearchTarget::Description},
        };

        // Returns the target and advances pos past the keyword when text at pos starts with a
        // recognized prefix. An empty operand after the prefix is handled by the caller (the
        // dangling-prefix rule), not here.
        auto TryReadPrefix(const std::wstring& text, size_t& pos) -> std::optional<SearchTarget>
        {
            for (const auto& [keyword, target] : kPrefixes)
            {
                const auto length = std::wcslen(keyword);
                if (text.compare(pos, length, keyword) == 0)
                {
                    pos += length;
                    return target;
                }
            }
            return std::nullopt;
        }

        // True when, after skipping any spaces starting at pos, the next character begins a
        // recognized atom-starting marker: '-' (NOT), '(' (a group, only when grouping is
        // enabled for this query), or a name:/drops:/ranch:/desc: prefix. This is the ONLY thing
        // that lets a bare space end a term.
        auto StartsNewAtomAhead(const std::wstring& text, size_t pos, bool canOpenGroup) -> bool
        {
            size_t probe = pos;
            while (probe < text.size() && text[probe] == L' ')
            {
                ++probe;
            }
            if (probe >= text.size())
            {
                return false;
            }
            if (text[probe] == L'-')
            {
                return true;
            }
            if (canOpenGroup && text[probe] == L'(')
            {
                return true;
            }
            size_t discard = probe;
            return TryReadPrefix(text, discard).has_value();
        }

        // Reads one atom's raw term text, stopping at '|', ')' (when grouping is enabled), end of
        // string, or a space immediately followed by a marker (StartsNewAtomAhead). Any other
        // character - including any other space - is consumed as part of the term, which is what
        // lets "fire organ" and "applied * handbook i" each read as one contiguous term with no
        // quoting, while "flame -boss" and "flame (water|oil)" still split at the space before
        // their marker character.
        auto ReadTermRaw(Cursor& cursor) -> std::wstring
        {
            std::wstring raw;
            while (!cursor.AtEnd())
            {
                const auto ch = cursor.Peek();
                if (ch == L'|')
                {
                    break;
                }
                if (cursor.ParenGroupingEnabled && ch == L')')
                {
                    break;
                }
                if (ch == L' ' && StartsNewAtomAhead(cursor.Text, cursor.Pos, cursor.CanOpenGroup()))
                {
                    break;
                }
                raw += ch;
                ++cursor.Pos;
            }
            while (!raw.empty() && raw.back() == L' ')
            {
                raw.pop_back();
            }
            return raw;
        }

        // A prefix on a group retargets every term inside it that did not specify its own prefix;
        // an atom's own explicit prefix always wins over an enclosing one.
        auto ApplyTargetOverride(Node& node, SearchTarget target) -> void
        {
            if (node.Kind == NodeKind::Term)
            {
                if (!node.HasExplicitTarget)
                {
                    node.TargetMask = TargetBit(target);
                    node.HasExplicitTarget = true;
                }
                return;
            }
            for (auto& child : node.Children)
            {
                ApplyTargetOverride(child, target);
            }
        }

        auto ParseOrExpr(Cursor& cursor) -> std::optional<Node>;

        auto ParseAtom(Cursor& cursor) -> std::optional<Node>
        {
            cursor.SkipSpaces();
            if (cursor.AtEnd())
            {
                return std::nullopt;
            }

            size_t probePos = cursor.Pos;
            const auto prefix = TryReadPrefix(cursor.Text, probePos);
            if (prefix)
            {
                cursor.Pos = probePos;
            }
            cursor.SkipSpaces();

            if (cursor.CanOpenGroup() && !cursor.AtEnd() && cursor.Peek() == L'(')
            {
                ++cursor.Pos;
                ++cursor.Depth;
                auto inner = ParseOrExpr(cursor);
                --cursor.Depth;
                cursor.SkipSpaces();
                if (!cursor.AtEnd() && cursor.Peek() == L')')
                {
                    ++cursor.Pos;
                }
                if (!inner)
                {
                    return std::nullopt;
                }
                if (prefix)
                {
                    ApplyTargetOverride(*inner, *prefix);
                }
                return inner;
            }

            const auto raw = ReadTermRaw(cursor);
            if (raw.empty())
            {
                return std::nullopt;
            }

            Node node;
            node.Kind = NodeKind::Term;
            node.TargetMask = prefix ? TargetBit(*prefix) : cursor.DefaultTargetMask;
            node.HasExplicitTarget = prefix.has_value();
            auto term = ParseTermSegments(raw);
            node.Segments = std::move(term.Segments);
            return node;
        }

        auto ParseAndExpr(Cursor& cursor) -> std::optional<Node>
        {
            std::vector<Node> operands;
            while (true)
            {
                cursor.SkipSpaces();
                if (cursor.AtEnd() || cursor.Peek() == L'|' || (cursor.ParenGroupingEnabled && cursor.Peek() == L')'))
                {
                    break;
                }
                bool negate = false;
                if (cursor.Peek() == L'-')
                {
                    ++cursor.Pos;
                    negate = true;
                }
                auto atom = ParseAtom(cursor);
                if (!atom)
                {
                    continue;
                }
                if (negate)
                {
                    Node notNode;
                    notNode.Kind = NodeKind::Not;
                    notNode.Children.push_back(std::move(*atom));
                    operands.push_back(std::move(notNode));
                }
                else
                {
                    operands.push_back(std::move(*atom));
                }
            }
            if (operands.empty())
            {
                return std::nullopt;
            }
            if (operands.size() == 1)
            {
                return std::move(operands.front());
            }
            Node andNode;
            andNode.Kind = NodeKind::And;
            andNode.Children = std::move(operands);
            return andNode;
        }

        auto ParseOrExpr(Cursor& cursor) -> std::optional<Node>
        {
            std::vector<Node> operands;
            auto first = ParseAndExpr(cursor);
            if (first)
            {
                operands.push_back(std::move(*first));
            }
            cursor.SkipSpaces();
            while (!cursor.AtEnd() && cursor.Peek() == L'|')
            {
                ++cursor.Pos;
                auto next = ParseAndExpr(cursor);
                if (next)
                {
                    operands.push_back(std::move(*next));
                }
                cursor.SkipSpaces();
            }
            if (operands.empty())
            {
                return std::nullopt;
            }
            if (operands.size() == 1)
            {
                return std::move(operands.front());
            }
            Node orNode;
            orNode.Kind = NodeKind::Or;
            orNode.Children = std::move(operands);
            return orNode;
        }
    } // namespace

    auto ParseQuery(const std::wstring& lowered, uint8_t defaultTargetMask) -> std::optional<Node>
    {
        int depth = 0;
        bool parensBalanced = true;
        for (const auto ch : lowered)
        {
            if (ch == L'(')
            {
                ++depth;
            }
            else if (ch == L')')
            {
                --depth;
                if (depth < 0)
                {
                    parensBalanced = false;
                    break;
                }
            }
        }
        if (depth != 0)
        {
            parensBalanced = false;
        }

        Cursor cursor{lowered, 0, defaultTargetMask, parensBalanced};
        return ParseOrExpr(cursor);
    }

    namespace
    {
        auto MatchesOneHaystack(const std::vector<Segment>& segments, const std::wstring& haystack) -> bool
        {
            return PatternMatches(segments, haystack);
        }

        // A throwaway, normalized copy of a term's segments for comparing against Drops/Ranch:
        // every literal segment and brace alternative has '_' and ' ' stripped, via the
        // StripDropSeparators helper defined earlier in this file. Never mutates the node's own
        // Segments - Name/Description keep matching the original, space-preserved text, which is
        // why normalization has to happen here, per comparison, rather than once when the term
        // was parsed: a single term's TargetMask can carry Drops/Ranch alongside Name/Description
        // at once, and one segment list cannot be both space-preserved and space-stripped.
        auto NormalizedForDrops(const std::vector<Segment>& segments) -> std::vector<Segment>
        {
            auto normalized = segments;
            for (auto& segment : normalized)
            {
                if (segment.Kind == SegmentKind::Literal)
                {
                    segment.Literal = StripDropSeparators(std::move(segment.Literal));
                }
                else if (segment.Kind == SegmentKind::Brace)
                {
                    for (auto& alt : segment.Alternatives)
                    {
                        alt = StripDropSeparators(std::move(alt));
                    }
                }
            }
            return normalized;
        }

        auto TermMatchesHaystacks(const Node& node, const CompiledQuery::Haystacks& haystacks) -> bool
        {
            if ((node.TargetMask & TargetBit(SearchTarget::Name)) != 0 && haystacks.Name &&
                MatchesOneHaystack(node.Segments, *haystacks.Name))
            {
                return true;
            }
            if ((node.TargetMask & TargetBit(SearchTarget::Description)) != 0 && haystacks.Description &&
                MatchesOneHaystack(node.Segments, *haystacks.Description))
            {
                return true;
            }
            const auto checksDropsOrRanch =
                    (node.TargetMask & (TargetBit(SearchTarget::Drops) | TargetBit(SearchTarget::Ranch))) != 0;
            if (!checksDropsOrRanch)
            {
                return false;
            }
            const auto normalized = NormalizedForDrops(node.Segments);
            if ((node.TargetMask & TargetBit(SearchTarget::Drops)) != 0 && haystacks.DropIds &&
                std::any_of(haystacks.DropIds->begin(), haystacks.DropIds->end(), [&normalized](const std::wstring& id) {
                    return MatchesOneHaystack(normalized, id);
                }))
            {
                return true;
            }
            if ((node.TargetMask & TargetBit(SearchTarget::Ranch)) != 0 && haystacks.RanchItems &&
                std::any_of(haystacks.RanchItems->begin(), haystacks.RanchItems->end(), [&normalized](const std::wstring& id) {
                    return MatchesOneHaystack(normalized, id);
                }))
            {
                return true;
            }
            return false;
        }

        auto CollectReferencedTargets(const Node& node) -> uint8_t
        {
            if (node.Kind == NodeKind::Term)
            {
                return node.TargetMask;
            }
            uint8_t mask = 0;
            for (const auto& child : node.Children)
            {
                mask |= CollectReferencedTargets(child);
            }
            return mask;
        }

        auto Evaluate(const Node& node, const CompiledQuery::Haystacks& haystacks) -> bool
        {
            switch (node.Kind)
            {
                case NodeKind::Term:
                    return TermMatchesHaystacks(node, haystacks);
                case NodeKind::Not:
                    return !Evaluate(node.Children.front(), haystacks);
                case NodeKind::And:
                    return std::all_of(node.Children.begin(), node.Children.end(),
                                        [&haystacks](const Node& child) { return Evaluate(child, haystacks); });
                case NodeKind::Or:
                    return std::any_of(node.Children.begin(), node.Children.end(),
                                        [&haystacks](const Node& child) { return Evaluate(child, haystacks); });
            }
            return false;
        }
    } // namespace

    auto CompiledQuery::Parse(const std::wstring& rawQuery, uint8_t defaultTargetMask) -> CompiledQuery
    {
        auto lowered = rawQuery;
        if (!lowered.empty())
        {
            CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
        }
        CompiledQuery compiled;
        compiled.m_root = ParseQuery(lowered, defaultTargetMask);
        return compiled;
    }

    auto CompiledQuery::Matches(const Haystacks& haystacks) const -> bool
    {
        return !m_root || Evaluate(*m_root, haystacks);
    }

    auto CompiledQuery::ReferencedTargets() const -> uint8_t
    {
        return m_root ? CollectReferencedTargets(*m_root) : 0;
    }
} // namespace PalpediaEnhancer::Query
