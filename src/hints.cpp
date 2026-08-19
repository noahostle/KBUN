#include "hints.h"

#include <cwctype>
#include <map>
#include <numeric>
#include <unordered_map>

namespace kbun {
namespace {

constexpr wchar_t kHintAlphabet[] = L"ASDFGHJKLQWERTYUIOPZXCVBNM";
constexpr std::size_t kAlphabetSize = 26;
constexpr std::size_t kMaxMajorGroups = 10;

struct MajorGroup {
    std::vector<std::size_t> members;
    RECT bounds{};
};

bool SpatialLess(const RECT& left, const RECT& right) {
    constexpr LONG rowSlop = 18;
    if (std::abs(left.top - right.top) > rowSlop) return left.top < right.top;
    return left.left < right.left;
}

}  // namespace

void HintNavigator::Reset(std::uint64_t generation, std::vector<ElementInfo> elements) {
    generation_ = generation;
    elements_ = std::move(elements);
    stack_.clear();
    prefix_.clear();
    if (!elements_.empty()) stack_.push_back(BuildTopScope());
}

void HintNavigator::Clear() {
    generation_ = 0;
    elements_.clear();
    stack_.clear();
    prefix_.clear();
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

    std::unordered_map<std::uint64_t, std::vector<std::size_t>> grouped;
    std::vector<std::size_t> direct;
    for (std::size_t index = 0; index < elements_.size(); ++index) {
        if (elements_[index].sectionId != 0) {
            grouped[elements_[index].sectionId].push_back(index);
        } else {
            direct.push_back(index);
        }
    }

    std::vector<MajorGroup> majorGroups;
    for (auto& [id, members] : grouped) {
        (void)id;
        if (members.size() > 3) {
            std::ranges::sort(members, [this](std::size_t left, std::size_t right) {
                return SpatialLess(elements_[left].bounds, elements_[right].bounds);
            });
            if (members.size() <= kAlphabetSize) {
                RECT bounds = MembersBounds(elements_, members);
                const RECT sectionBounds = elements_[members.front()].sectionBounds;
                if (RectArea(sectionBounds) > 0) bounds = sectionBounds;
                majorGroups.push_back({std::move(members), bounds});
            } else {
                for (std::size_t begin = 0; begin < members.size(); begin += kAlphabetSize) {
                    const std::size_t end = std::min(members.size(), begin + kAlphabetSize);
                    std::vector<std::size_t> chunk(
                        members.begin() + static_cast<std::ptrdiff_t>(begin),
                        members.begin() + static_cast<std::ptrdiff_t>(end));
                    const RECT bounds = MembersBounds(elements_, chunk);
                    majorGroups.push_back({std::move(chunk), bounds});
                }
            }
        } else {
            direct.insert(direct.end(), members.begin(), members.end());
        }
    }

    std::ranges::sort(majorGroups, [this](const auto& left, const auto& right) {
        return SpatialLess(left.bounds, right.bounds);
    });
    while (majorGroups.size() > kMaxMajorGroups) {
        auto tail = std::move(majorGroups.back().members);
        majorGroups.pop_back();
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
        node.code.assign(1, kHintAlphabet[index]);
        scope.nodes.push_back(std::move(node));
    }

    const std::size_t reserved = majorGroups.size();
    for (std::size_t index = 0; index < direct.size(); ++index) {
        const std::size_t first = reserved + index / kAlphabetSize;
        if (first >= kAlphabetSize) break;
        Node node;
        node.elementIndex = direct[index];
        node.bounds = elements_[node.elementIndex].bounds;
        node.drawOutline = elements_[node.elementIndex].drawOutline;
        node.code.push_back(kHintAlphabet[first]);
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
        node.drawOutline = elements_[node.elementIndex].drawOutline;
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
