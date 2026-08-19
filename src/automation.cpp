#include "automation.h"

#include "com_ptr.h"

#include <UIAutomation.h>
#include <dwmapi.h>
#include <oleauto.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kbun {
namespace {

using Clock = std::chrono::steady_clock;

struct WindowCandidate {
    HWND handle = nullptr;
    RECT bounds{};
};

struct RawElement {
    ElementInfo info;
    ComPtr<IUIAutomationElement> automationElement;
    HWND ownerWindow = nullptr;
    CONTROLTYPEID controlType = 0;
};

struct SectionCandidate {
    RECT bounds{};
    CONTROLTYPEID controlType = 0;
};

struct StoredElement {
    ElementRole role = ElementRole::Action;
    ComPtr<IUIAutomationElement> automationElement;
};

struct GenerationStore {
    std::uint64_t generation = 0;
    std::unordered_map<std::uint64_t, StoredElement> elements;
};

struct AutomationState {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationCondition> controlViewCondition;
    ComPtr<IUIAutomationCacheRequest> cacheRequest;
    std::deque<GenerationStore> generations;
    std::uint64_t nextElementId = 1;
    std::uint64_t nextSectionId = 1;

    ComPtr<IUIAutomationElement> caretElement;
    ComPtr<IUIAutomationTextRange> documentRange;
    ComPtr<IUIAutomationTextRange> anchorRange;
    ComPtr<IUIAutomationTextRange> caretRange;
};

bool IsTaskbarWindow(HWND window) {
    if (!window) return false;
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return std::wcscmp(className, L"Shell_TrayWnd") == 0 ||
           std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsWindowCandidate(HWND window, DWORD ownProcessId, RECT& bounds) {
    if (!IsWindowVisible(window) || IsIconic(window) || window == GetShellWindow() || IsTaskbarWindow(window)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == ownProcessId) {
        wchar_t className[128]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (std::wcscmp(className, L"KBUN.Overlay") == 0 ||
            std::wcscmp(className, L"KBUN.Caret") == 0 ||
            std::wcscmp(className, L"KBUN.App") == 0) {
            return false;
        }
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }

    if (!GetWindowRect(window, &bounds) || RectWidth(bounds) < 2 || RectHeight(bounds) < 2) return false;

    const RECT virtualScreen{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    RECT intersection{};
    return IntersectRect(&intersection, &bounds, &virtualScreen) != FALSE;
}

BOOL CALLBACK CollectWindowsCallback(HWND window, LPARAM parameter) {
    auto* result = reinterpret_cast<std::vector<WindowCandidate>*>(parameter);
    RECT bounds{};
    if (IsWindowCandidate(window, GetCurrentProcessId(), bounds)) result->push_back({window, bounds});
    return TRUE;
}

std::vector<WindowCandidate> CollectWindows() {
    std::vector<WindowCandidate> result;
    EnumWindows(CollectWindowsCallback, reinterpret_cast<LPARAM>(&result));
    return result;
}

bool PointIsUnoccluded(
    POINT point,
    const std::vector<WindowCandidate>& windows,
    std::size_t ownerIndex) {
    if (ownerIndex >= windows.size() || !ContainsPoint(windows[ownerIndex].bounds, point)) return false;
    for (std::size_t index = 0; index < ownerIndex; ++index) {
        if (ContainsPoint(windows[index].bounds, point)) return false;
    }
    return true;
}

bool ElementCenterIsVisible(
    const RECT& bounds,
    const std::vector<WindowCandidate>& windows,
    std::size_t ownerIndex) {
    if (RectArea(bounds) <= 0) return false;
    return PointIsUnoccluded(RectCenter(bounds), windows, ownerIndex);
}

bool WindowHasVisibleRegion(const std::vector<WindowCandidate>& windows, std::size_t ownerIndex) {
    if (ownerIndex >= windows.size()) return false;
    HRGN visible = CreateRectRgnIndirect(&windows[ownerIndex].bounds);
    if (!visible) return true;
    for (std::size_t index = 0; index < ownerIndex; ++index) {
        HRGN occluder = CreateRectRgnIndirect(&windows[index].bounds);
        if (!occluder) continue;
        const int regionType = CombineRgn(visible, visible, occluder, RGN_DIFF);
        DeleteObject(occluder);
        if (regionType == NULLREGION) {
            DeleteObject(visible);
            return false;
        }
    }
    DeleteObject(visible);
    return true;
}

bool VariantBool(IUIAutomationElement* element, PROPERTYID property, bool fallback = false) {
    VARIANT value;
    VariantInit(&value);
    const HRESULT result = element->GetCachedPropertyValueEx(property, TRUE, &value);
    const bool answer = SUCCEEDED(result) && value.vt == VT_BOOL ? value.boolVal == VARIANT_TRUE : fallback;
    VariantClear(&value);
    return answer;
}

bool IsActionControl(CONTROLTYPEID type, bool hasInvokePattern) {
    switch (type) {
        case UIA_ButtonControlTypeId:
        case UIA_CalendarControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_ComboBoxControlTypeId:
        case UIA_DataItemControlTypeId:
        case UIA_HeaderItemControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_RadioButtonControlTypeId:
        case UIA_ScrollBarControlTypeId:
        case UIA_SliderControlTypeId:
        case UIA_SpinnerControlTypeId:
        case UIA_SplitButtonControlTypeId:
        case UIA_TabItemControlTypeId:
        case UIA_ThumbControlTypeId:
        case UIA_TreeItemControlTypeId:
            return true;
        case UIA_CustomControlTypeId:
            return hasInvokePattern;
        default:
            return false;
    }
}

bool IsSectionControl(CONTROLTYPEID type) {
    switch (type) {
        case UIA_DataGridControlTypeId:
        case UIA_GroupControlTypeId:
        case UIA_ListControlTypeId:
        case UIA_MenuBarControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_TabControlTypeId:
        case UIA_TableControlTypeId:
        case UIA_ToolBarControlTypeId:
        case UIA_TreeControlTypeId:
            return true;
        default:
            return false;
    }
}

std::optional<bool> TextRangeReportsEditable(IUIAutomationElement* element, bool hasTextPattern) {
    if (!hasTextPattern) return std::nullopt;
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(element->GetCurrentPatternAs(
            UIA_TextPatternId,
            IID_IUIAutomationTextPattern,
            pattern.PutVoid())) ||
        !pattern) {
        return std::nullopt;
    }
    ComPtr<IUIAutomationTextRange> document;
    if (FAILED(pattern->get_DocumentRange(document.Put())) || !document) return std::nullopt;

    VARIANT readOnly;
    VariantInit(&readOnly);
    const HRESULT result = document->GetAttributeValue(UIA_IsReadOnlyAttributeId, &readOnly);
    std::optional<bool> editable;
    if (SUCCEEDED(result) && readOnly.vt == VT_BOOL) {
        editable = readOnly.boolVal == VARIANT_FALSE;
    }
    VariantClear(&readOnly);
    return editable;
}

ElementRole ClassifyElement(
    CONTROLTYPEID type,
    bool hasTextPattern,
    bool hasTextEditPattern,
    bool hasValuePattern,
    bool valueReadOnly,
    std::optional<bool> textRangeEditable,
    bool isPassword,
    bool hasInvokePattern) {
    if (type == UIA_EditControlTypeId) {
        if (hasValuePattern) {
            return valueReadOnly ? ElementRole::ReadOnlyText : ElementRole::EditableText;
        }
        if (textRangeEditable.has_value()) {
            return *textRangeEditable ? ElementRole::EditableText : ElementRole::ReadOnlyText;
        }
        return ElementRole::EditableText;
    }
    if (type == UIA_DocumentControlTypeId && hasTextPattern) {
        return hasTextEditPattern || (hasValuePattern && !valueReadOnly) || textRangeEditable.value_or(false)
            ? ElementRole::EditableText
            : ElementRole::ReadOnlyText;
    }
    if (!isPassword && hasTextPattern && (type == UIA_TextControlTypeId || type == UIA_CustomControlTypeId)) {
        if (textRangeEditable.has_value()) {
            return *textRangeEditable ? ElementRole::EditableText : ElementRole::ReadOnlyText;
        }
        return hasTextEditPattern ? ElementRole::EditableText : ElementRole::ReadOnlyText;
    }
    if (IsActionControl(type, hasInvokePattern)) return ElementRole::Action;
    return static_cast<ElementRole>(255);
}

int RolePriority(ElementRole role) {
    switch (role) {
        case ElementRole::EditableText: return 3;
        case ElementRole::ReadOnlyText: return 2;
        case ElementRole::Action: return 1;
    }
    return 0;
}

double IntersectionOverUnion(const RECT& left, const RECT& right) {
    RECT intersection{};
    if (!IntersectRect(&intersection, &left, &right)) return 0.0;
    const auto unionArea = RectArea(left) + RectArea(right) - RectArea(intersection);
    return unionArea > 0 ? static_cast<double>(RectArea(intersection)) / static_cast<double>(unionArea) : 0.0;
}

std::wstring CachedName(IUIAutomationElement* element) {
    BSTR value = nullptr;
    if (FAILED(element->get_CachedName(&value)) || value == nullptr) return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    if (result.size() > 160) result.resize(160);
    return result;
}

bool ConfigureCache(AutomationState& state) {
    if (FAILED(state.automation->get_ControlViewCondition(state.controlViewCondition.Put()))) return false;
    if (FAILED(state.automation->CreateCacheRequest(state.cacheRequest.Put()))) return false;

    const std::array<PROPERTYID, 12> properties{
        UIA_BoundingRectanglePropertyId,
        UIA_ControlTypePropertyId,
        UIA_IsEnabledPropertyId,
        UIA_IsOffscreenPropertyId,
        UIA_IsPasswordPropertyId,
        UIA_IsKeyboardFocusablePropertyId,
        UIA_NamePropertyId,
        UIA_IsInvokePatternAvailablePropertyId,
        UIA_IsTextPatternAvailablePropertyId,
        UIA_IsTextEditPatternAvailablePropertyId,
        UIA_IsValuePatternAvailablePropertyId,
        UIA_ValueIsReadOnlyPropertyId,
    };
    for (const PROPERTYID property : properties) {
        if (FAILED(state.cacheRequest->AddProperty(property))) return false;
    }
    if (FAILED(state.cacheRequest->put_TreeScope(TreeScope_Element))) return false;
    if (FAILED(state.cacheRequest->put_AutomationElementMode(AutomationElementMode_Full))) return false;
    return true;
}

bool InitializeAutomation(AutomationState& state) {
    HRESULT result = CoCreateInstance(
        CLSID_CUIAutomation8,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUIAutomation,
        state.automation.PutVoid());
    if (FAILED(result)) {
        result = CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUIAutomation,
            state.automation.PutVoid());
    }
    return SUCCEEDED(result) && ConfigureCache(state);
}

void AssignSections(
    std::vector<RawElement>& elements,
    const std::vector<SectionCandidate>& sections,
    const RECT& windowBounds,
    AutomationState& state) {
    struct QualifiedSection {
        RECT bounds{};
        std::uint64_t id = 0;
        std::int64_t area = 0;
    };
    std::vector<QualifiedSection> qualified;

    for (const SectionCandidate& section : sections) {
        const auto area = RectArea(section.bounds);
        if (area <= 0) continue;
        const auto windowArea = RectArea(windowBounds);
        if (windowArea > 0 && area * 100 >= windowArea * 78) {
            continue;
        }

        std::size_t count = 0;
        for (const RawElement& element : elements) {
            if (ContainsPoint(section.bounds, RectCenter(element.info.bounds))) ++count;
        }
        if (count <= 3) continue;

        bool duplicate = false;
        for (const QualifiedSection& existing : qualified) {
            if (IntersectionOverUnion(existing.bounds, section.bounds) > 0.96) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) qualified.push_back({section.bounds, state.nextSectionId++, area});
    }

    std::ranges::sort(qualified, [](const QualifiedSection& left, const QualifiedSection& right) {
        return left.area < right.area;
    });
    for (RawElement& element : elements) {
        const POINT center = RectCenter(element.info.bounds);
        const auto owner = std::ranges::find_if(qualified, [center](const QualifiedSection& section) {
            return ContainsPoint(section.bounds, center);
        });
        if (owner != qualified.end()) {
            element.info.sectionId = owner->id;
            element.info.sectionBounds = owner->bounds;
        }
    }
}

void CollectWindowElements(
    AutomationState& state,
    const std::vector<WindowCandidate>& windows,
    std::size_t windowIndex,
    std::vector<RawElement>& allElements) {
    const WindowCandidate& window = windows[windowIndex];
    if (!WindowHasVisibleRegion(windows, windowIndex)) return;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(state.automation->ElementFromHandle(window.handle, root.Put())) || !root) return;

    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAllBuildCache(
            TreeScope_Descendants,
            state.controlViewCondition.Get(),
            state.cacheRequest.Get(),
            found.Put())) ||
        !found) {
        return;
    }

    int length = 0;
    if (FAILED(found->get_Length(&length)) || length <= 0) return;

    std::vector<RawElement> windowElements;
    std::vector<SectionCandidate> sections;
    windowElements.reserve(static_cast<std::size_t>(length) / 2);
    sections.reserve(32);

    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(found->GetElement(index, element.Put())) || !element) continue;

        BOOL offscreen = TRUE;
        BOOL enabled = FALSE;
        RECT bounds{};
        CONTROLTYPEID controlType = 0;
        if (FAILED(element->get_CachedBoundingRectangle(&bounds)) ||
            FAILED(element->get_CachedControlType(&controlType)) ||
            FAILED(element->get_CachedIsOffscreen(&offscreen)) ||
            FAILED(element->get_CachedIsEnabled(&enabled)) ||
            offscreen || !enabled || RectWidth(bounds) < 2 || RectHeight(bounds) < 2) {
            continue;
        }

        RECT onWindow{};
        if (!IntersectRect(&onWindow, &bounds, &window.bounds) || RectArea(onWindow) == 0) continue;

        if (IsSectionControl(controlType)) sections.push_back({bounds, controlType});

        const bool hasTextPattern = VariantBool(element.Get(), UIA_IsTextPatternAvailablePropertyId);
        const bool hasTextEditPattern = VariantBool(element.Get(), UIA_IsTextEditPatternAvailablePropertyId);
        const bool hasValuePattern = VariantBool(element.Get(), UIA_IsValuePatternAvailablePropertyId);
        const bool hasInvokePattern = VariantBool(element.Get(), UIA_IsInvokePatternAvailablePropertyId);
        const bool valueReadOnly = VariantBool(element.Get(), UIA_ValueIsReadOnlyPropertyId, true);
        const std::optional<bool> textRangeEditable =
            (controlType == UIA_EditControlTypeId ||
             controlType == UIA_DocumentControlTypeId ||
             controlType == UIA_CustomControlTypeId)
            ? TextRangeReportsEditable(element.Get(), hasTextPattern)
            : std::nullopt;
        BOOL isPasswordValue = FALSE;
        element->get_CachedIsPassword(&isPasswordValue);

        const ElementRole role = ClassifyElement(
            controlType,
            hasTextPattern,
            hasTextEditPattern,
            hasValuePattern,
            valueReadOnly,
            textRangeEditable,
            isPasswordValue != FALSE,
            hasInvokePattern);
        if (static_cast<unsigned>(role) == 255U) continue;
        if (!ElementCenterIsVisible(bounds, windows, windowIndex)) continue;

        RawElement raw;
        raw.info.bounds = bounds;
        const auto windowArea = RectArea(window.bounds);
        raw.info.drawOutline = windowArea <= 0 ||
            RectArea(bounds) * 100 < windowArea * 78 ||
            RectWidth(bounds) * 100 < RectWidth(window.bounds) * 90;
        raw.info.role = role;
        raw.info.name = CachedName(element.Get());
        raw.automationElement = std::move(element);
        raw.ownerWindow = window.handle;
        raw.controlType = controlType;
        windowElements.push_back(std::move(raw));
    }

    // A document or edit is the single text target for the text nodes it geometrically owns.
    std::vector<RECT> textContainers;
    for (const RawElement& element : windowElements) {
        if (element.controlType == UIA_DocumentControlTypeId || element.controlType == UIA_EditControlTypeId) {
            textContainers.push_back(element.info.bounds);
        }
    }
    std::erase_if(windowElements, [&textContainers](const RawElement& candidate) {
        if (candidate.controlType != UIA_TextControlTypeId && candidate.controlType != UIA_CustomControlTypeId) {
            return false;
        }
        return std::ranges::any_of(textContainers, [&candidate](const RECT& container) {
            return RectArea(container) > RectArea(candidate.info.bounds) * 12 / 10 &&
                   ContainsPoint(container, RectCenter(candidate.info.bounds));
        });
    });

    AssignSections(windowElements, sections, window.bounds, state);
    std::ranges::move(windowElements, std::back_inserter(allElements));
}

void Deduplicate(std::vector<RawElement>& elements) {
    std::ranges::sort(elements, [](const RawElement& left, const RawElement& right) {
        const int leftPriority = RolePriority(left.info.role);
        const int rightPriority = RolePriority(right.info.role);
        if (leftPriority != rightPriority) return leftPriority > rightPriority;
        return RectArea(left.info.bounds) > RectArea(right.info.bounds);
    });

    std::vector<RawElement> unique;
    unique.reserve(elements.size());
    for (RawElement& candidate : elements) {
        const bool duplicate = std::ranges::any_of(unique, [&candidate](const RawElement& existing) {
            if (candidate.ownerWindow != existing.ownerWindow) return false;
            const double overlap = IntersectionOverUnion(candidate.info.bounds, existing.info.bounds);
            if (overlap < 0.88) return false;
            return candidate.info.role == existing.info.role ||
                   candidate.info.role != ElementRole::Action ||
                   existing.info.role != ElementRole::Action;
        });
        if (!duplicate) unique.push_back(std::move(candidate));
    }
    elements = std::move(unique);
}

ScanResult* ScanDesktop(AutomationState& state, std::uint64_t generation) {
    const auto started = Clock::now();
    auto* result = new ScanResult;
    result->generation = generation;
    result->foreground = GetForegroundWindow();

    std::vector<RawElement> rawElements;
    const std::vector<WindowCandidate> windows = CollectWindows();
    for (std::size_t index = 0; index < windows.size(); ++index) {
        CollectWindowElements(state, windows, index, rawElements);
    }
    Deduplicate(rawElements);

    GenerationStore store;
    store.generation = generation;
    result->elements.reserve(rawElements.size());
    store.elements.reserve(rawElements.size());
    for (RawElement& raw : rawElements) {
        const std::uint64_t id = state.nextElementId++;
        raw.info.id = id;
        result->elements.push_back(raw.info);
        store.elements.emplace(id, StoredElement{raw.info.role, std::move(raw.automationElement)});
    }

    state.generations.push_back(std::move(store));
    while (state.generations.size() > 3) state.generations.pop_front();
    result->elapsedMs = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    return result;
}

StoredElement* FindStoredElement(AutomationState& state, std::uint64_t generation, std::uint64_t id) {
    for (auto store = state.generations.rbegin(); store != state.generations.rend(); ++store) {
        if (store->generation != generation) continue;
        const auto found = store->elements.find(id);
        return found == store->elements.end() ? nullptr : &found->second;
    }
    return nullptr;
}

ComPtr<IUIAutomationTextRange> CloneRange(IUIAutomationTextRange* range) {
    ComPtr<IUIAutomationTextRange> result;
    if (range) range->Clone(result.Put());
    return result;
}

void PlaceEditableCaret(IUIAutomationElement* element) {
    RECT bounds{};
    if (element && SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds)) && RectArea(bounds) > 0) {
        const POINT center = RectCenter(bounds);
        SetCursorPos(center.x, center.y);
    }

    std::array<INPUT, 6> input{};
    for (INPUT& item : input) item.type = INPUT_KEYBOARD;
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    input[2].ki.wVk = VK_CONTROL;
    input[3].ki.wVk = VK_HOME;
    input[4].ki.wVk = VK_HOME;
    input[4].ki.dwFlags = KEYEVENTF_KEYUP;
    input[5].ki.wVk = VK_CONTROL;
    input[5].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
}

void CollapseTo(IUIAutomationTextRange* range, TextPatternRangeEndpoint endpoint) {
    if (!range) return;
    if (endpoint == TextPatternRangeEndpoint_Start) {
        range->MoveEndpointByRange(TextPatternRangeEndpoint_End, range, TextPatternRangeEndpoint_Start);
    } else {
        range->MoveEndpointByRange(TextPatternRangeEndpoint_Start, range, TextPatternRangeEndpoint_End);
    }
}

ComPtr<IUIAutomationTextRange> OrderedSelection(AutomationState& state) {
    if (!state.anchorRange || !state.caretRange) return {};
    int comparison = 0;
    if (FAILED(state.anchorRange->CompareEndpoints(
            TextPatternRangeEndpoint_Start,
            state.caretRange.Get(),
            TextPatternRangeEndpoint_Start,
            &comparison))) {
        return {};
    }

    ComPtr<IUIAutomationTextRange> result;
    if (comparison <= 0) {
        result = CloneRange(state.anchorRange.Get());
        if (result) {
            result->MoveEndpointByRange(
                TextPatternRangeEndpoint_End,
                state.caretRange.Get(),
                TextPatternRangeEndpoint_Start);
        }
    } else {
        result = CloneRange(state.caretRange.Get());
        if (result) {
            result->MoveEndpointByRange(
                TextPatternRangeEndpoint_End,
                state.anchorRange.Get(),
                TextPatternRangeEndpoint_Start);
        }
    }
    return result;
}

void ApplyCaretSelection(AutomationState& state) {
    ComPtr<IUIAutomationTextRange> selection = OrderedSelection(state);
    if (selection) selection->Select();
}

bool CopyCaretSelection(AutomationState& state) {
    ComPtr<IUIAutomationTextRange> selection = OrderedSelection(state);
    if (!selection) return false;

    int comparison = 0;
    if (FAILED(state.anchorRange->CompareEndpoints(
            TextPatternRangeEndpoint_Start,
            state.caretRange.Get(),
            TextPatternRangeEndpoint_Start,
            &comparison)) ||
        comparison == 0) {
        return false;
    }

    BSTR text = nullptr;
    if (FAILED(selection->GetText(-1, &text)) || text == nullptr) return false;
    const std::size_t bytes = (static_cast<std::size_t>(SysStringLen(text)) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        SysFreeString(text);
        return false;
    }

    void* destination = GlobalLock(memory);
    std::memcpy(destination, text, bytes);
    GlobalUnlock(memory);
    SysFreeString(text);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool succeeded = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!succeeded) GlobalFree(memory);
    return succeeded;
}

void ClearCaret(AutomationState& state) {
    state.caretRange.Reset();
    state.anchorRange.Reset();
    state.documentRange.Reset();
    state.caretElement.Reset();
}

std::optional<RECT> CurrentCaretBounds(AutomationState& state) {
    if (!state.caretRange) return std::nullopt;
    ComPtr<IUIAutomationTextRange> probe = CloneRange(state.caretRange.Get());
    if (!probe) return std::nullopt;

    int moved = 0;
    bool useRightEdge = false;
    if (FAILED(probe->MoveEndpointByUnit(
            TextPatternRangeEndpoint_End,
            TextUnit_Character,
            1,
            &moved)) || moved == 0) {
        probe = CloneRange(state.caretRange.Get());
        if (!probe || FAILED(probe->MoveEndpointByUnit(
                TextPatternRangeEndpoint_Start,
                TextUnit_Character,
                -1,
                &moved)) || moved == 0) {
            return std::nullopt;
        }
        useRightEdge = true;
    }

    SAFEARRAY* rectangles = nullptr;
    if (FAILED(probe->GetBoundingRectangles(&rectangles)) || !rectangles) return std::nullopt;
    LONG lower = 0;
    LONG upper = -1;
    if (FAILED(SafeArrayGetLBound(rectangles, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(rectangles, 1, &upper)) ||
        upper - lower + 1 < 4) {
        SafeArrayDestroy(rectangles);
        return std::nullopt;
    }

    double* values = nullptr;
    if (FAILED(SafeArrayAccessData(rectangles, reinterpret_cast<void**>(&values))) || !values) {
        SafeArrayDestroy(rectangles);
        return std::nullopt;
    }
    const LONG count = upper - lower + 1;
    const LONG index = useRightEdge ? count - 4 : 0;
    const double left = values[index];
    const double top = values[index + 1];
    const double width = values[index + 2];
    const double height = values[index + 3];
    SafeArrayUnaccessData(rectangles);
    SafeArrayDestroy(rectangles);

    const LONG x = static_cast<LONG>(std::lround(useRightEdge ? left + width : left));
    const LONG y = static_cast<LONG>(std::lround(top));
    const LONG lineHeight = std::max<LONG>(12, static_cast<LONG>(std::lround(height)));
    if (lineHeight > 200 || x < -100000 || y < -100000) return std::nullopt;
    return RECT{x, y, x + 2, y + lineHeight};
}

void PostCaretVisual(HWND replyWindow, AutomationState& state, bool visible) {
    auto* result = new CaretVisualResult;
    if (visible) {
        const std::optional<RECT> bounds = CurrentCaretBounds(state);
        if (bounds) {
            result->visible = true;
            result->bounds = *bounds;
        } else if (state.caretElement) {
            RECT elementBounds{};
            if (SUCCEEDED(state.caretElement->get_CurrentBoundingRectangle(&elementBounds)) &&
                RectArea(elementBounds) > 0) {
                result->visible = true;
                const LONG top = elementBounds.top + std::min<LONG>(6, RectHeight(elementBounds) / 4);
                result->bounds = RECT{
                    elementBounds.left + 7,
                    top,
                    elementBounds.left + 9,
                    std::min(elementBounds.bottom - 2, top + 20),
                };
            }
        }
    }
    if (!IsWindow(replyWindow) ||
        !PostMessageW(replyWindow, kMsgCaretVisual, 0, reinterpret_cast<LPARAM>(result))) {
        delete result;
    }
}

bool MoveToBoundary(AutomationState& state, bool end, TextUnit unit) {
    if (!state.caretRange) return false;
    ComPtr<IUIAutomationTextRange> range = CloneRange(state.caretRange.Get());
    if (!range || FAILED(range->ExpandToEnclosingUnit(unit))) return false;
    CollapseTo(range.Get(), end ? TextPatternRangeEndpoint_End : TextPatternRangeEndpoint_Start);
    state.caretRange = std::move(range);
    return true;
}

void HandleCaretInput(AutomationState& state, const CaretInput& input) {
    if (input.action == CaretAction::Exit) {
        ClearCaret(state);
        return;
    }
    if (!state.caretRange || !state.documentRange) return;

    if (input.action == CaretAction::Copy) {
        CopyCaretSelection(state);
        return;
    }
    if (input.action == CaretAction::SelectAll) {
        state.anchorRange = CloneRange(state.documentRange.Get());
        state.caretRange = CloneRange(state.documentRange.Get());
        CollapseTo(state.anchorRange.Get(), TextPatternRangeEndpoint_Start);
        CollapseTo(state.caretRange.Get(), TextPatternRangeEndpoint_End);
        ApplyCaretSelection(state);
        return;
    }

    if (!input.extend) state.anchorRange = CloneRange(state.caretRange.Get());

    bool moved = false;
    if ((input.action == CaretAction::Home || input.action == CaretAction::End) && input.control) {
        state.caretRange = CloneRange(state.documentRange.Get());
        CollapseTo(
            state.caretRange.Get(),
            input.action == CaretAction::End ? TextPatternRangeEndpoint_End : TextPatternRangeEndpoint_Start);
        moved = true;
    } else if (input.action == CaretAction::Home || input.action == CaretAction::End) {
        moved = MoveToBoundary(state, input.action == CaretAction::End, TextUnit_Line);
    } else {
        TextUnit unit = TextUnit_Character;
        int count = 0;
        switch (input.action) {
            case CaretAction::Left: count = -1; break;
            case CaretAction::Right: count = 1; break;
            case CaretAction::Up: unit = TextUnit_Line; count = -1; break;
            case CaretAction::Down: unit = TextUnit_Line; count = 1; break;
            case CaretAction::PageUp: unit = TextUnit_Page; count = -1; break;
            case CaretAction::PageDown: unit = TextUnit_Page; count = 1; break;
            default: break;
        }
        int actual = 0;
        if (count != 0 && SUCCEEDED(state.caretRange->Move(unit, count, &actual))) moved = actual != 0;
    }

    if (moved && !input.extend) state.anchorRange = CloneRange(state.caretRange.Get());
    ApplyCaretSelection(state);
    state.caretRange->ScrollIntoView(FALSE);
}

ActivationResult* ActivateElement(
    AutomationState& state,
    std::uint64_t generation,
    std::uint64_t elementId) {
    auto* result = new ActivationResult;
    result->generation = generation;
    result->elementId = elementId;

    StoredElement* stored = FindStoredElement(state, generation, elementId);
    if (!stored || !stored->automationElement) return result;
    if (stored->role == ElementRole::Action) {
        result->mode = ActivationMode::Pointer;
        result->succeeded = true;
        return result;
    }

    ClearCaret(state);
    const HRESULT focusResult = stored->automationElement->SetFocus();
    ComPtr<IUIAutomationTextPattern> textPattern;
    const HRESULT patternResult = stored->automationElement->GetCurrentPatternAs(
        UIA_TextPatternId,
        IID_IUIAutomationTextPattern,
        textPattern.PutVoid());

    ComPtr<IUIAutomationTextRange> document;
    if (SUCCEEDED(patternResult) && textPattern) textPattern->get_DocumentRange(document.Put());
    if (document) {
        ComPtr<IUIAutomationTextRange> start = CloneRange(document.Get());
        CollapseTo(start.Get(), TextPatternRangeEndpoint_Start);
        const HRESULT selectResult = start ? start->Select() : E_FAIL;

        if (stored->role == ElementRole::ReadOnlyText) {
            state.caretElement = stored->automationElement;
            state.documentRange = std::move(document);
            state.anchorRange = CloneRange(start.Get());
            state.caretRange = std::move(start);
            result->mode = ActivationMode::Caret;
            result->succeeded = state.caretRange && (SUCCEEDED(focusResult) || SUCCEEDED(selectResult));
            if (!result->succeeded && state.caretRange) result->succeeded = true;
            return result;
        }

        result->mode = ActivationMode::Editable;
        result->succeeded = SUCCEEDED(focusResult) || SUCCEEDED(selectResult);
        if (result->succeeded) PlaceEditableCaret(stored->automationElement.Get());
        return result;
    }

    result->mode = stored->role == ElementRole::EditableText
        ? ActivationMode::Editable
        : ActivationMode::Pointer;
    result->succeeded = SUCCEEDED(focusResult);
    if (result->mode == ActivationMode::Editable && result->succeeded) {
        PlaceEditableCaret(stored->automationElement.Get());
    }
    return result;
}

}  // namespace

AutomationWorker::~AutomationWorker() {
    Stop();
}

bool AutomationWorker::Start(HWND replyWindow) {
    if (thread_.joinable()) return true;
    replyWindow_ = replyWindow;
    stopping_ = false;
    try {
        thread_ = std::thread(&AutomationWorker::ThreadMain, this);
    } catch (...) {
        replyWindow_ = nullptr;
        return false;
    }
    return true;
}

void AutomationWorker::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        commands_.push_back(Command{CommandType::Stop});
    }
    wake_.notify_one();
    thread_.join();
    replyWindow_ = nullptr;
}

void AutomationWorker::Push(Command command) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        commands_.push_back(std::move(command));
    }
    wake_.notify_one();
}

void AutomationWorker::RequestScan(std::uint64_t generation) {
    latestScan_.store(generation, std::memory_order_release);
    Push(Command{CommandType::Scan, generation});
}

void AutomationWorker::CancelScan(std::uint64_t generation) {
    std::uint64_t expected = generation;
    latestScan_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

void AutomationWorker::Activate(std::uint64_t generation, std::uint64_t elementId) {
    Command command;
    command.type = CommandType::Activate;
    command.generation = generation;
    command.elementId = elementId;
    Push(std::move(command));
}

void AutomationWorker::SendCaretInput(CaretInput input) {
    Command command;
    command.type = CommandType::Caret;
    command.caret = input;
    Push(std::move(command));
}

void AutomationWorker::ThreadMain() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    AutomationState state;
    const bool initialized = SUCCEEDED(comResult) && InitializeAutomation(state);

    for (;;) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return !commands_.empty() || stopping_; });
            if (commands_.empty() && stopping_) break;
            command = std::move(commands_.front());
            commands_.pop_front();
        }

        if (command.type == CommandType::Stop) break;
        if (!initialized) continue;

        if (command.type == CommandType::Scan) {
            if (latestScan_.load(std::memory_order_acquire) != command.generation) continue;
            ScanResult* result = ScanDesktop(state, command.generation);
            if (latestScan_.load(std::memory_order_acquire) == command.generation && IsWindow(replyWindow_)) {
                if (!PostMessageW(replyWindow_, kMsgScanComplete, 0, reinterpret_cast<LPARAM>(result))) delete result;
            } else {
                delete result;
            }
        } else if (command.type == CommandType::Activate) {
            ActivationResult* result = ActivateElement(state, command.generation, command.elementId);
            const bool showCaret = result->succeeded && result->mode == ActivationMode::Caret;
            if (!IsWindow(replyWindow_) ||
                !PostMessageW(replyWindow_, kMsgActivationComplete, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
            PostCaretVisual(replyWindow_, state, showCaret);
        } else if (command.type == CommandType::Caret) {
            HandleCaretInput(state, command.caret);
            PostCaretVisual(replyWindow_, state, command.caret.action != CaretAction::Exit);
        }
    }

    ClearCaret(state);
    state.generations.clear();
    state.cacheRequest.Reset();
    state.controlViewCondition.Reset();
    state.automation.Reset();
    if (SUCCEEDED(comResult)) CoUninitialize();
}

}  // namespace kbun
