#include "hints.h"
#include "text_role.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

kbun::ElementInfo Element(std::uint64_t id, int x, int y, std::uint64_t section = 0) {
    kbun::ElementInfo result;
    result.id = id;
    result.bounds = RECT{x, y, x + 80, y + 24};
    result.sectionId = section;
    result.sectionBounds = RECT{0, 0, 500, 500};
    return result;
}

kbun::ElementInfo SectionElement(
    std::uint64_t id,
    RECT bounds,
    std::uint64_t section,
    RECT sectionBounds) {
    kbun::ElementInfo result;
    result.id = id;
    result.bounds = bounds;
    result.sectionId = section;
    result.sectionBounds = sectionBounds;
    return result;
}

}  // namespace

int main() {
    {
        kbun::HintNavigator hints;
        hints.Reset(7, {Element(1, 0, 0), Element(2, 100, 0), Element(3, 200, 0)});
        const auto shown = hints.Display();
        Check(shown.size() == 3, "three direct targets are shown");
        Check(shown[0].code.size() == 2, "direct targets use two letters");
        Check(hints.Input(shown[1].code[0]).type == kbun::HintOutcome::Type::Changed,
              "first direct-target letter changes the prefix");
        const auto selected = hints.Input(shown[1].code[1]);
        Check(selected.type == kbun::HintOutcome::Type::Selected, "second letter selects a target");
        Check(selected.element.id == 2, "the intended direct target is selected");
    }

    {
        std::vector<kbun::ElementInfo> elements;
        for (std::uint64_t id = 1; id <= 6; ++id) {
            elements.push_back(Element(id, static_cast<int>(id * 30), 20, 99));
        }
        elements.push_back(Element(10, 20, 600));

        kbun::HintNavigator hints;
        hints.Reset(9, std::move(elements));
        const auto top = hints.Display();
        Check(top.size() == 2, "section and direct target are both shown");
        const auto group = *std::ranges::find_if(top, [](const kbun::DisplayHint& hint) { return hint.isGroup; });
        Check(group.code.size() == 1, "major section uses one letter");
        Check(hints.Input(group.code[0]).type == kbun::HintOutcome::Type::Changed,
              "major section drills down");
        const auto inner = hints.Display();
        Check(inner.size() == 6, "all section targets are shown");
        for (const auto& hint : inner) Check(hint.code.size() == 1, "section targets use one letter");
        const auto selected = hints.Input(inner[4].code[0]);
        Check(selected.type == kbun::HintOutcome::Type::Selected, "single letter selects inside section");
    }

    {
        std::vector<kbun::ElementInfo> elements;
        for (std::uint64_t id = 1; id <= 70; ++id) {
            elements.push_back(Element(id, static_cast<int>((id % 10) * 90), static_cast<int>((id / 10) * 30), 88));
        }
        kbun::HintNavigator hints;
        hints.Reset(11, std::move(elements));
        const auto top = hints.Display();
        Check(top.size() == 3, "large section is split into top-level one-step groups");
        Check(std::ranges::all_of(top, [](const auto& hint) { return hint.isGroup; }),
              "large-section chunks are top-level groups");
        hints.Input(top.front().code[0]);
        const auto inner = hints.Display();
        Check(inner.size() <= 26, "one-step group fits the single-letter alphabet");
        Check(std::ranges::none_of(inner, [](const auto& hint) { return hint.isGroup; }),
              "inner targets never create another nesting level");
    }

    {
        std::vector<kbun::ElementInfo> elements;
        const RECT outerBounds{0, 0, 700, 500};
        const RECT innerBounds{300, 100, 620, 380};
        for (std::uint64_t id = 1; id <= 5; ++id) {
            elements.push_back(SectionElement(
                id,
                RECT{20, static_cast<LONG>(id * 40), 140, static_cast<LONG>(id * 40 + 28)},
                10,
                outerBounds));
            elements.push_back(SectionElement(
                id + 10,
                RECT{330, static_cast<LONG>(id * 40 + 80), 450, static_cast<LONG>(id * 40 + 108)},
                20,
                innerBounds));
        }

        kbun::HintNavigator hints;
        hints.Reset(12, std::move(elements));
        const auto top = hints.Display();
        Check(std::ranges::count_if(top, [](const auto& hint) { return hint.isGroup; }) == 1,
              "only the smaller overlapping section remains nested");
        Check(std::ranges::count_if(top, [](const auto& hint) { return !hint.isGroup; }) == 5,
              "the larger overlapping section is flattened to direct targets");
    }

    {
        std::vector<kbun::ElementInfo> elements;
        elements.push_back(SectionElement(1, RECT{0, 0, 320, 80}, 0, {}));
        elements.push_back(SectionElement(2, RECT{250, 10, 305, 45}, 0, {}));
        elements.push_back(SectionElement(3, RECT{250, 45, 305, 75}, 0, {}));

        kbun::HintNavigator hints;
        hints.Reset(13, std::move(elements));
        const auto top = hints.Display();
        Check(top.size() == 1 && top.front().isGroup,
              "an overlapping parent and its controls share one top-level hint");
        Check(hints.Input(top.front().code[0]).type == kbun::HintOutcome::Type::Changed,
              "the overlap group opens in one step");
        const auto inner = hints.Display();
        Check(inner.size() == 3, "the parent action and both child controls stay reachable");
        Check(std::ranges::count_if(inner, [](const auto& hint) { return hint.drawOutline; }) == 2,
              "the larger inner wrapper drops only its conflicting outline");
    }

    {
        const auto codexDocument = kbun::ClassifyTextRole(
            UIA_DocumentControlTypeId,
            true,
            false,
            true,
            std::nullopt,
            false);
        Check(codexDocument == kbun::ElementRole::ReadOnlyText,
              "a mixed document without explicit editability uses caret browsing");

        const auto editableDocument = kbun::ClassifyTextRole(
            UIA_DocumentControlTypeId,
            true,
            false,
            true,
            true,
            false);
        Check(editableDocument == kbun::ElementRole::EditableText,
              "range-level editability still identifies editable documents");
    }

    std::cout << "KBUN hint tests passed\n";
    return 0;
}
