#include "hints.h"

#include <cwctype>
#include <map>
#include <numeric>
#include <unordered_map>

namespace kbun {
namespace {

constexpr wchar_t kHintAlphabet[] = L"ASDFGHJKLQWERTYUIOPZXCVBNM";
constexpr std::size_t kAlphabetSize = 26;
constexpr std::size_t kMaxMajorGroups = 18;

struct MajorGroup {
    std::vector<std::size_t> members;
    RECT bounds{};
    HWND ownerWindow = nullptr;
    bool protectsOverlap = false;
};

bool SpatialLess(const RECT& left, const RECT& right) {
    constexpr LONG rowSlop = 18;
    if (std::abs(left.top - right.top) > rowSlop) return left.top < right.top;
    return left.left < right.left;
}

bool HasNestedOverlap(const RECT& left, const RECT& right) {
    const std::int64_t leftArea = RectArea(left);
    const std::int64_t rightArea = RectArea(right);
    if (leftArea <= 0 || rightArea <= 0) return false;

    const RECT intersection{
        std::max(left.left, right.left),
        std::max(left.top, right.top),
        std::min(left.right, right.right),
        std::min(left.bottom, right.bottom),
    };
    if (RectArea(intersection) == 0) return false;
    const std::int64_t smallerArea = std::min(leftArea, rightArea);
    return RectArea(intersection) * 100 >= smallerArea * 85;
}

bool IsLargerWrapper(const RECT& outer, const RECT& inner) {
    const std::int64_t outerArea = RectArea(outer);
    const std::int64_t innerArea = RectArea(inner);
    return outerArea > 0 && innerArea > 0 && outerArea * 100 >= innerArea * 120 &&
           HasNestedOverlap(outer, inner);
}

std::vector<MajorGroup> BuildOverlapGroups(
    const std::vector<ElementInfo>& elements,
    std::vector<bool>& claimed) {
    std::vector<std::size_t> byArea(elements.size());
    std::iota(byArea.begin(), byArea.end(), 0);
    std::ranges::sort(byArea, [&elements](std::size_t left, std::size_t right) {
        return RectArea(elements[left].bounds) > RectArea(elements[right].bounds);
    });

    std::vector<MajorGroup> groups;
    for (const std::size_t outerIndex : byArea) {
        const ElementInfo& outer = elements[outerIndex];
        if (claimed[outerIndex] || !outer.drawOutline) continue;

        std::vector<std::size_t> children;
        for (const std::size_t innerIndex : byArea) {
            if (innerIndex == outerIndex || claimed[innerIndex]) continue;
            const ElementInfo& inner = elements[innerIndex];
            if (outer.ownerWindow != inner.ownerWindow || !IsLargerWrapper(outer.bounds, inner.bounds)) {
                continue;
            }
            children.push_back(innerIndex);
            if (children.size() == kAlphabetSize) break;
        }
        if (children.empty() || children.size() == kAlphabetSize) continue;

        MajorGroup group;
        group.bounds = outer.bounds;
        group.ownerWindow = outer.ownerWindow;
        group.protectsOverlap = true;
        group.members.reserve(children.size() + 1);
        group.members.push_back(outerIndex);
        group.members.insert(group.members.end(), children.begin(), children.end());
        claimed[outerIndex] = true;
        for (const std::size_t child : children) claimed[child] = true;
        groups.push_back(std::move(group));
    }
    return groups;
}

void AppendUnique(std::vector<std::size_t>& destination, const std::vector<std::size_t>& source) {
    for (const std::size_t member : source) {
        if (std::ranges::find(destination, member) == destination.end()) destination.push_back(member);
    }
}

void ResolveOverlappingGroups(
    const std::vector<ElementInfo>& elements,
    std::vector<MajorGroup>& groups,
    std::vector<std::size_t>& direct) {
    for (;;) {
        bool changed = false;
        for (std::size_t left = 0; left < groups.size() && !changed; ++left) {
            for (std::size_t right = left + 1; right < groups.size(); ++right) {
                if (groups[left].ownerWindow != groups[right].ownerWindow ||
                    !HasNestedOverlap(groups[left].bounds, groups[right].bounds)) {
                    continue;
                }

                const std::size_t parent = RectArea(groups[left].bounds) >= RectArea(groups[right].bounds)
                    ? left
                    : right;
                const std::size_t child = parent == left ? right : left;
                std::vector<std::size_t> merged = groups[parent].members;
                AppendUnique(merged, groups[child].members);
                if (merged.size() <= kAlphabetSize) {
                    groups[parent].members = std::move(merged);
                    groups[parent].protectsOverlap = true;
                    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(child));
                } else {
                    AppendUnique(direct, groups[parent].members);
                    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(parent));
                }
                changed = true;
                break;
            }
        }
        if (changed) continue;

        for (std::size_t groupIndex = 0; groupIndex < groups.size() && !changed; ++groupIndex) {
            for (std::size_t directIndex = 0; directIndex < direct.size(); ++directIndex) {
                const ElementInfo& target = elements[direct[directIndex]];
                if (groups[groupIndex].ownerWindow != target.ownerWindow ||
                    !HasNestedOverlap(groups[groupIndex].bounds, target.bounds)) {
                    continue;
                }

                if (groups[groupIndex].members.size() < kAlphabetSize) {
                    groups[groupIndex].members.push_back(direct[directIndex]);
                    groups[groupIndex].protectsOverlap = true;
                    direct.erase(direct.begin() + static_cast<std::ptrdiff_t>(directIndex));
                } else {
                    AppendUnique(direct, groups[groupIndex].members);
                    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(groupIndex));
                }
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
}

}  // namespace

void HintNavigator::Reset(
    std::uint64_t generation,
    std::vector<ElementInfo> elements,
    HintOptions options) {
    generation_ = generation;
    elements_ = std::move(elements);
    options_ = std::move(options);
    stack_.clear();
    prefix_.clear();
    if (elements_.empty()) return;
    if (options_.singleLetter) {
        std::vector<std::size_t> members(elements_.size());
        std::iota(members.begin(), members.end(), 0);
        stack_.push_back(BuildInnerScope(std::move(members)));
    } else {
        stack_.push_back(BuildTopScope());
    }
}

void HintNavigator::Clear() {
    generation_ = 0;
    elements_.clear();
    stack_.clear();
    prefix_.clear();
    options_ = {};
}

RECT HintNavigator::MembersBounds(
    const std::vector<ElementInfo>& elements,
    const std::vector<std::size_t>& members) {
    RECT result{};
    for (const std::size_t index : members) result = UnionRects(result, elements[index].bounds);
    return result;
}

HintNavigator::Scope HintNavigator::BuildTopScope() const {
    Scope scope;
    scope.topLevel = true;

    std::vector<bool> overlapClaimed(elements_.size(), false);
    std::vector<MajorGroup> majorGroups = BuildOverlapGroups(elements_, overlapClaimed);
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> grouped;
    std::vector<std::size_t> direct;
    for (std::size_t index = 0; index < elements_.size(); ++index) {
        if (overlapClaimed[index]) continue;
        if (elements_[index].sectionId != 0) {
            grouped[elements_[index].sectionId].push_back(index);
        } else {
            direct.push_back(index);
        }
    }

    for (auto& [id, members] : grouped) {
        (void)id;
        if (members.size() > 3) {
            std::ranges::sort(members, [this](std::size_t left, std::size_t right) {
                return SpatialLess(elements_[left].bounds, elements_[right].bounds);
            });
            if (members.size() <= kAlphabetSize) {
                RECT bounds = MembersBounds(elements_, members);
                const RECT sectionBounds = elements_[members.front()].sectionBounds;
                const HWND ownerWindow = elements_[members.front()].ownerWindow;
                if (RectArea(sectionBounds) > 0) bounds = sectionBounds;
                majorGroups.push_back({
                    std::move(members),
                    bounds,
                    ownerWindow,
                    false});
            } else {
                for (std::size_t begin = 0; begin < members.size(); begin += kAlphabetSize) {
                    const std::size_t end = std::min(members.size(), begin + kAlphabetSize);
                    std::vector<std::size_t> chunk(
                        members.begin() + static_cast<std::ptrdiff_t>(begin),
                        members.begin() + static_cast<std::ptrdiff_t>(end));
                    const RECT bounds = MembersBounds(elements_, chunk);
                    const HWND ownerWindow = elements_[chunk.front()].ownerWindow;
                    majorGroups.push_back({
                        std::move(chunk),
                        bounds,
                        ownerWindow,
                        false});
                }
            }
        } else {
            direct.insert(direct.end(), members.begin(), members.end());
        }
    }

    // Keep overlapping targets in one flat, one-step scope whenever the
    // alphabet can hold them. Over-capacity parents fall back to direct hints.
    ResolveOverlappingGroups(elements_, majorGroups, direct);

    std::wstring topAlphabet = kHintAlphabet;
    std::erase_if(topAlphabet, [this](wchar_t letter) {
        return std::ranges::any_of(options_.reservedTopLetters, [letter](wchar_t reserved) {
            return std::towupper(reserved) == letter;
        });
    });

    std::ranges::sort(majorGroups, [this](const auto& left, const auto& right) {
        return SpatialLess(left.bounds, right.bounds);
    });
    const std::size_t maximumGroups = std::min(kMaxMajorGroups, topAlphabet.size());
    while (majorGroups.size() > maximumGroups) {
        auto candidate = std::ranges::find_if(
            majorGroups.rbegin(),
            majorGroups.rend(),
            [](const MajorGroup& group) { return !group.protectsOverlap; });
        if (candidate == majorGroups.rend()) candidate = majorGroups.rbegin();
        auto tail = std::move(candidate->members);
        majorGroups.erase(std::next(candidate).base());
        direct.insert(direct.end(), tail.begin(), tail.end());
    }

    std::ranges::sort(direct, [this](std::size_t left, std::size_t right) {
        return SpatialLess(elements_[left].bounds, elements_[right].bounds);
    });

    for (std::size_t index = 0; index < majorGroups.size(); ++index) {
        Node node;
        node.group = true;
        node.members = std::move(majorGroups[index].members);
        node.bounds = majorGroups[index].bounds;
        node.code.assign(1, topAlphabet[index]);
        scope.nodes.push_back(std::move(node));
    }

    const std::size_t reserved = majorGroups.size();
    for (std::size_t index = 0; index < direct.size(); ++index) {
        const std::size_t first = reserved + index / kAlphabetSize;
        if (first >= topAlphabet.size()) break;
        Node node;
        node.elementIndex = direct[index];
        node.bounds = elements_[node.elementIndex].bounds;
        node.drawOutline = elements_[node.elementIndex].drawOutline;
        node.code.push_back(topAlphabet[first]);
        node.code.push_back(kHintAlphabet[index % kAlphabetSize]);
        scope.nodes.push_back(std::move(node));
    }

    return scope;
}

HintNavigator::Scope HintNavigator::BuildInnerScope(std::vector<std::size_t> members) const {
    Scope scope;
    std::ranges::sort(members, [this](std::size_t left, std::size_t right) {
        return SpatialLess(elements_[left].bounds, elements_[right].bounds);
    });

    const std::size_t shown = std::min(members.size(), kAlphabetSize);
    for (std::size_t index = 0; index < shown; ++index) {
        Node node;
        node.elementIndex = members[index];
        node.bounds = elements_[node.elementIndex].bounds;
        const bool wrapsAnotherTarget = std::ranges::any_of(members, [this, &node](std::size_t other) {
            return other != node.elementIndex &&
                   IsLargerWrapper(elements_[node.elementIndex].bounds, elements_[other].bounds);
        });
        node.drawOutline = elements_[node.elementIndex].drawOutline && !wrapsAnotherTarget;
        node.code.assign(1, kHintAlphabet[index]);
        scope.nodes.push_back(std::move(node));
    }
    return scope;
}

void HintNavigator::Enter(const Node& node) {
    if (stack_.size() != 1) return;
    stack_.push_back(BuildInnerScope(node.members));
    prefix_.clear();
}

HintOutcome HintNavigator::Input(wchar_t letter) {
    if (stack_.empty()) return {};
    letter = static_cast<wchar_t>(std::towupper(letter));
    if (std::wcschr(kHintAlphabet, letter) == nullptr) return {};

    Scope& scope = stack_.back();
    if (!scope.topLevel) {
        for (const Node& node : scope.nodes) {
            if (node.code.front() != letter) continue;
            return {HintOutcome::Type::Selected, elements_[node.elementIndex]};
        }
        return {};
    }

    if (prefix_.empty()) {
        for (const Node& node : scope.nodes) {
            if (node.group && node.code.front() == letter) {
                Enter(node);
                return {HintOutcome::Type::Changed, {}};
            }
        }
        prefix_.push_back(letter);
        const bool hasPrefix = std::ranges::any_of(scope.nodes, [letter](const Node& node) {
            return !node.group && node.code.front() == letter;
        });
        if (!hasPrefix) {
            prefix_.clear();
            return {};
        }
        return {HintOutcome::Type::Changed, {}};
    }

    const std::wstring code = prefix_ + letter;
    prefix_.clear();
    for (const Node& node : scope.nodes) {
        if (!node.group && node.code == code) {
            return {HintOutcome::Type::Selected, elements_[node.elementIndex]};
        }
    }
    return {HintOutcome::Type::Changed, {}};
}

bool HintNavigator::Back() {
    if (stack_.empty()) return false;
    if (!prefix_.empty()) {
        prefix_.clear();
        return true;
    }
    if (stack_.size() > 1) {
        stack_.pop_back();
        return true;
    }
    return false;
}

std::vector<DisplayHint> HintNavigator::Display() const {
    std::vector<DisplayHint> result;
    if (stack_.empty()) return result;
    result.reserve(stack_.back().nodes.size());
    for (const Node& node : stack_.back().nodes) {
        DisplayHint hint;
        hint.bounds = node.bounds;
        hint.code = node.code;
        hint.isGroup = node.group;
        hint.drawOutline = node.drawOutline;
        hint.prefixMatch = prefix_.empty() || (!node.group && node.code.starts_with(prefix_));
        if (!node.group) hint.role = elements_[node.elementIndex].role;
        result.push_back(std::move(hint));
    }
    return result;
}

}  // namespace kbun
