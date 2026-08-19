#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kbun {

struct DisplayHint {
    RECT bounds{};
    std::wstring code;
    ElementRole role = ElementRole::Action;
    bool isGroup = false;
    bool prefixMatch = true;
};

struct HintOutcome {
    enum class Type {
        None,
        Changed,
        Selected,
    } type = Type::None;

    ElementInfo element;
};

class HintNavigator {
public:
    void Reset(std::uint64_t generation, std::vector<ElementInfo> elements);
    void Clear();

    HintOutcome Input(wchar_t letter);
    bool Back();

    [[nodiscard]] std::vector<DisplayHint> Display() const;
    [[nodiscard]] bool Empty() const noexcept { return elements_.empty(); }
    [[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }
    [[nodiscard]] const std::wstring& Prefix() const noexcept { return prefix_; }

private:
    struct Node {
        bool group = false;
        std::size_t elementIndex = 0;
        std::vector<std::size_t> members;
        RECT bounds{};
        std::wstring code;
    };

    struct Scope {
        bool topLevel = false;
        std::vector<Node> nodes;
    };

    Scope BuildTopScope() const;
    Scope BuildInnerScope(std::vector<std::size_t> members) const;
    static RECT MembersBounds(const std::vector<ElementInfo>& elements, const std::vector<std::size_t>& members);
    void Enter(const Node& node);

    std::uint64_t generation_ = 0;
    std::vector<ElementInfo> elements_;
    std::vector<Scope> stack_;
    std::wstring prefix_;
};

}  // namespace kbun

