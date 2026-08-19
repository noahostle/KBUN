#pragma once

#include "common.h"

#include <unknwn.h>
#include <UIAutomation.h>

#include <optional>

namespace kbun {

inline std::optional<ElementRole> ClassifyTextRole(
    CONTROLTYPEID type,
    bool hasTextPattern,
    bool hasValuePattern,
    bool valueReadOnly,
    std::optional<bool> textRangeEditable,
    bool isPassword) {
    if (type == UIA_EditControlTypeId) {
        if (hasValuePattern) {
            return valueReadOnly ? ElementRole::ReadOnlyText : ElementRole::EditableText;
        }
        if (textRangeEditable.has_value()) {
            return *textRangeEditable ? ElementRole::EditableText : ElementRole::ReadOnlyText;
        }
        return ElementRole::EditableText;
    }

    if (isPassword || !hasTextPattern ||
        (type != UIA_DocumentControlTypeId &&
         type != UIA_TextControlTypeId &&
         type != UIA_CustomControlTypeId)) {
        return std::nullopt;
    }

    if (hasValuePattern) {
        return valueReadOnly ? ElementRole::ReadOnlyText : ElementRole::EditableText;
    }
    if (textRangeEditable.has_value()) {
        return *textRangeEditable ? ElementRole::EditableText : ElementRole::ReadOnlyText;
    }

    // TextEditPattern is commonly exposed by a document whose descendants include
    // an editor. Without range-level evidence, the document itself is still a
    // read-only caret-browsing surface.
    return ElementRole::ReadOnlyText;
}

}  // namespace kbun
