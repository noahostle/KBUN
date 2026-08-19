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

    std::vector<std::vector<std::size_t>> majorGroups;
    for (auto& [id, members] : grouped) {
        (void)id;
        if (members.size() > 3) {
            majorGroups.push_back(std::move(members));
        } else {
            direct.insert(direct.end(), members.begin(), members.end());
        }
    }

    std::ranges::sort(majorGroups, [this](const auto& left, const auto& right) {
        return SpatialLess(MembersBounds(elements_, left), MembersBounds(elements_, right));
    });
    while (majorGroups.size() > kMaxMajorGroups) {
        auto tail = std::move(majorGroups.back());
        majorGroups.pop_back();
        direct.insert(direct.end(), tail.begin(), tail.end());
    }

    std::ranges::sort(direct, [this](std::size_t left, std::size_t right) {
        return SpatialLess(elements_[left].bounds, elements_[right].bounds);
    });

    std::size_t groupCount = majorGroups.size();
    std::size_t directCapacity = (kAlphabetSize - groupCount) * kAlphabetSize;
    if (direct.size() > directCapacity && groupCount < kAlphabetSize - 1) {
        ++groupCount;
        directCapacity = (kAlphabetSize - groupCount) * kAlphabetSize;
        std::vector<std::size_t> overflow(direct.begin() + static_cast<std::ptrdiff_t>(directCapacity), direct.end());
        direct.resize(directCapacity);
        majorGroups.push_back(std::move(overflow));
    }

    for (std::size_t index = 0; index < majorGroups.size(); ++index) {
        Node node;
        node.group = true;
        node.members = std::move(majorGroups[index]);
        node.bounds = MembersBounds(elements_, node.members);
        if (!node.members.empty() && elements_[node.members.front()].sectionId != 0) {
            const RECT section = elements_[node.members.front()].sectionBounds;
            if (RectArea(section) > 0) node.bounds = section;
        }
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

    if (members.size() <= kAlphabetSize) {
        for (std::size_t index = 0; index < members.size(); ++index) {
            Node node;
            node.elementIndex = members[index];
            node.bounds = elements_[node.elementIndex].bounds;
            node.code.assign(1, kHintAlphabet[index]);
            scope.nodes.push_back(std::move(node));
        }
        return scope;
    }

    const std::size_t bucketCount = std::min(
        kAlphabetSize,
        (members.size() + kAlphabetSize - 1) / kAlphabetSize);
    const std::size_t bucketSize = (members.size() + bucketCount - 1) / bucketCount;
    for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
        const std::size_t begin = bucket * bucketSize;
        const std::size_t end = std::min(members.size(), begin + bucketSize);
        if (begin == end) break;

        Node node;
        node.group = true;
        node.members.assign(members.begin() + static_cast<std::ptrdiff_t>(begin),
                            members.begin() + static_cast<std::ptrdiff_t>(end));
        node.bounds = MembersBounds(elements_, node.members);
        node.code.assign(1, kHintAlphabet[bucket]);
        scope.nodes.push_back(std::move(node));
    }
    return scope;
}

void HintNavigator::Enter(const Node& node) {
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
            if (node.group) {
                Enter(node);
                return {HintOutcome::Type::Changed, {}};
            }
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
        hint.prefixMatch = prefix_.empty() || (!node.group && node.code.starts_with(prefix_));
        if (!node.group) hint.role = elements_[node.elementIndex].role;
        result.push_back(std::move(hint));
    }
    return result;
}

}  // namespace kbun

