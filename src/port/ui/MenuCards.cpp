/**
 * The Android menu of the N64 ports (handheld fork): one full-screen page, the sections in a column on the left, the
 * settings of the chosen section as a vertical list of cards on the right - "◄ value ►", no horizontal scrolling.
 * The same design is used by every N64 Android port; this is the libultraship (ImGui) one.
 *
 * Built on the menu's own data (sections -> sidebars -> widgets in PortMenu.cpp): only the drawing differs. Values
 * change in steps (checkbox On / Off, combo box: next entry, slider: one step); widgets that do not fit a row
 * (colour pickers, custom widgets, ...) are drawn as they are inside their card.
 *
 * Controller: up / down move; A flips a switch, takes a slider / list for left / right (A or B ends it), presses a
 * button or opens a page; Y the default; L / R the section; B one level up (page -> list -> sections -> closed).
 * Touch: tap a section; tap a switch, or the left / right part of a value; drag the list to scroll.
 * The "Popout ..." windows (bindings, stats, console, ...) open as pages inside the menu, in its colours.
 */
#ifdef __ANDROID__

#include "Menu.h"
#include "UIWidgets.h"
#include "port/Engine.h"
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Ship {
void Android_OpenModsManager(); // AndroidBridge.cpp
// The Back menu's pages (TouchControls' bridge, AndroidBridge.cpp): { title, then kind / title / value per item }.
std::vector<std::string> Android_MenuPage(const char* id);
void Android_MenuAct(const char* id, int index, int op); // op -1 / +1 step, 0 run, 2 default
std::string Android_MenuTakePushed();
bool Android_MenuTakeClose();

namespace {

// Colours: graphite surfaces and one accent colour - Settings > General > Menu Theme (gSettings.Menu.Accent, the
// options in PortMenu.h menuAccentOptions, same order); 0 = the game's own (SpaghettiKart: tomato red).
const ImU32 kBg = IM_COL32(18, 20, 24, 255);
const ImU32 kCard = IM_COL32(32, 35, 42, 255);
const ImU32 kText = IM_COL32(238, 238, 240, 255);
const ImU32 kTextDim = IM_COL32(152, 156, 166, 255);
const ImU32 kTextOff = IM_COL32(104, 108, 118, 255);
const ImU32 kArrow = IM_COL32(176, 180, 190, 255);
const ImU32 kAccents[] = {
    IM_COL32(236, 92, 64, 255),  IM_COL32(240, 140, 40, 255), IM_COL32(232, 184, 48, 255),
    IM_COL32(88, 180, 96, 255),  IM_COL32(40, 170, 160, 255), IM_COL32(64, 132, 230, 255),
    IM_COL32(150, 100, 220, 255), IM_COL32(226, 92, 150, 255), IM_COL32(170, 176, 186, 255),
};
constexpr int kAccentDefault = 0; // Tomato
ImU32 kAccent = kAccents[kAccentDefault];
ImU32 kAccentSoft = 0;  // the accent, faint (the focused card's rim)
ImU32 kCardFocus = 0;   // the card colour tinted with the accent
ImU32 kTabActive = 0;

void UpdateAccent() {
    int i = kAccentDefault; // the game's own colour (no theme choice)
    if (i < 0 || i >= static_cast<int>(IM_ARRAYSIZE(kAccents))) {
        i = 0;
    }
    kAccent = kAccents[i];
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(kAccent), c = ImGui::ColorConvertU32ToFloat4(kCard);
    kAccentSoft = ImGui::ColorConvertFloat4ToU32({ a.x, a.y, a.z, 0.24f });
    kCardFocus = ImGui::ColorConvertFloat4ToU32({ c.x + (a.x - c.x) * 0.13f, c.y + (a.y - c.y) * 0.13f,
                                                  c.z + (a.z - c.z) * 0.13f, 1.0f });
    kTabActive = kCardFocus;
}
#define kGold kAccent

const ImGuiID kOwner = ImHashStr("MenuCards");

enum class TabKind { Page, Mods, BackToGame, Exit };

struct Tab {
    std::string label;
    std::string header;
    std::string sidebar;
    TabKind kind = TabKind::Page;
    bool groupStart = false; // a small gap above (a new group of sections)
};

struct State {
    int tab = -1;          // selected tab (index into the tab list)
    int focus = 0;         // focused card on the page
    bool focusOnTabs = false; // the controller is in the section column (B from the list goes there)
    int tabCursor = 0;        // the controller's place in the section column
    bool scrollToFocus = false;
    float dragY = 0.0f;    // touch drag of the list
    bool dragging = false;
    bool resetScroll = false;
    bool editing = false;     // a slider / list taken with A: left / right change it
    std::string subWindow;    // a page of another window (bindings, stats, ...) shown in the list's place
    std::string subTitle;
    bool subFocus = false;    // give that page the controller on its first frame
    std::vector<std::string> javaStack; // pages of the Back menu opened from a card (shader folders, ...)
    std::unordered_map<std::string, std::pair<int, std::vector<std::string>>> javaCache; // id -> (frame, data)
    int frame = 0;
};
State s;

bool PadPressed(ImGuiKey key, bool repeat) {
    ImGui::SetKeyOwner(key, kOwner, ImGuiInputFlags_LockUntilRelease);
    return ImGui::IsKeyPressed(key, repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None, kOwner);
}

// Readable section names: the Settings sections keep their names, the other areas prefix theirs.
std::string TabLabel(const std::string& header, const std::string& sidebar) {
    if (header == "Settings") {
        return sidebar;
    }
    if (sidebar == "General") {
        return header;
    }
    return sidebar;
}

// Slider names carry their value format ("Master Volume: %.0f%%"); the card shows the value on its own.
std::string DisplayName(const std::string& name) {
    size_t pct = name.find('%');
    if (pct == std::string::npos) {
        return name;
    }
    std::string n = name.substr(0, pct);
    while (!n.empty() && (n.back() == ' ' || n.back() == ':')) {
        n.pop_back();
    }
    return n;
}

bool IsValueWidget(WidgetType t) {
    switch (t) {
        case WIDGET_CHECKBOX:
        case WIDGET_CVAR_CHECKBOX:
        case WIDGET_COMBOBOX:
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_INT:
        case WIDGET_SLIDER_FLOAT:
        case WIDGET_CVAR_SLIDER_FLOAT:
        case WIDGET_AUDIO_BACKEND:
        case WIDGET_VIDEO_BACKEND:
            return true;
        default:
            return false;
    }
}

bool IsFocusable(const WidgetInfo& w) {
    switch (w.type) {
        case WIDGET_SEPARATOR:
        case WIDGET_SEPARATOR_TEXT:
        case WIDGET_TEXT:
        case WIDGET_SEARCH:
            return false;
        default:
            return true;
    }
}

std::vector<int32_t> SortedKeys(const std::unordered_map<int32_t, const char*>& map) {
    std::vector<int32_t> keys;
    keys.reserve(map.size());
    for (auto& [k, v] : map) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

// The value shown between the arrows.
std::string ValueText(WidgetInfo& w) {
    switch (w.type) {
        case WIDGET_VIDEO_BACKEND: // shown only: Android has one renderer (OpenGL ES)
            return Ship::Context::GetRawInstance()->GetConfig()->GetString("Window.Backend.Name", "OpenGL");
        case WIDGET_AUDIO_BACKEND: {
            auto b = Ship::Context::GetRawInstance()->GetAudio()->GetCurrentAudioBackend();
            auto it = audioBackendsMap.find(b);
            return it != audioBackendsMap.end() ? it->second : "?";
        }
        case WIDGET_CHECKBOX:
            return *std::get<bool*>(w.valuePointer) ? "On" : "Off";
        case WIDGET_CVAR_CHECKBOX: {
            auto o = std::static_pointer_cast<UIWidgets::CheckboxOptions>(w.options);
            return CVarGetInteger(w.cVar, o->defaultValue) ? "On" : "Off";
        }
        case WIDGET_COMBOBOX:
        case WIDGET_CVAR_COMBOBOX: {
            auto o = std::static_pointer_cast<UIWidgets::ComboboxOptions>(w.options);
            int32_t v = w.type == WIDGET_COMBOBOX ? *std::get<int32_t*>(w.valuePointer)
                                                  : CVarGetInteger(w.cVar, o->defaultIndex);
            auto it = o->comboMap.find(v);
            return it != o->comboMap.end() ? it->second : fmt::format("{}", v);
        }
        case WIDGET_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_INT: {
            auto o = std::static_pointer_cast<UIWidgets::IntSliderOptions>(w.options);
            int32_t v = w.type == WIDGET_SLIDER_INT ? *std::get<int32_t*>(w.valuePointer)
                                                    : CVarGetInteger(w.cVar, o->defaultValue);
            char buf[64];
            snprintf(buf, sizeof buf, o->format, v);
            return buf;
        }
        case WIDGET_SLIDER_FLOAT:
        case WIDGET_CVAR_SLIDER_FLOAT: {
            auto o = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(w.options);
            float v = w.type == WIDGET_SLIDER_FLOAT ? *std::get<float*>(w.valuePointer)
                                                    : CVarGetFloat(w.cVar, o->defaultValue);
            char buf[64];
            snprintf(buf, sizeof buf, o->format, o->isPercentage ? v * 100.0f : v);
            return buf;
        }
        default:
            return "";
    }
}

// One step of a value widget, -1 / +1; `wrap` for A / a tap on the card (On / Off and lists go round).
bool StepValue(WidgetInfo& w, int dir, bool wrap) {
    if (w.type == WIDGET_VIDEO_BACKEND) {
        return false;
    }
    if (w.options && w.options->disabled) {
        return false;
    }
    bool changed = false;
    switch (w.type) {
        case WIDGET_AUDIO_BACKEND: {
            auto audio = Ship::Context::GetRawInstance()->GetAudio();
            auto list = audio->GetAvailableAudioBackends();
            if (!list || list->size() <= 1) {
                break;
            }
            auto cur = audio->GetCurrentAudioBackend();
            int i = static_cast<int>(std::find(list->begin(), list->end(), cur) - list->begin());
            int n = (i + dir + static_cast<int>(list->size())) % static_cast<int>(list->size());
            audio->SetCurrentAudioBackend(list->at(n));
            changed = true;
        } break;
        case WIDGET_CHECKBOX: {
            bool* p = std::get<bool*>(w.valuePointer);
            *p = !*p;
            changed = true;
        } break;
        case WIDGET_CVAR_CHECKBOX: {
            auto o = std::static_pointer_cast<UIWidgets::CheckboxOptions>(w.options);
            CVarSetInteger(w.cVar, !CVarGetInteger(w.cVar, o->defaultValue));
            changed = true;
        } break;
        case WIDGET_COMBOBOX:
        case WIDGET_CVAR_COMBOBOX: {
            auto o = std::static_pointer_cast<UIWidgets::ComboboxOptions>(w.options);
            auto keys = SortedKeys(o->comboMap);
            if (keys.empty()) {
                break;
            }
            int32_t v = w.type == WIDGET_COMBOBOX ? *std::get<int32_t*>(w.valuePointer)
                                                  : CVarGetInteger(w.cVar, o->defaultIndex);
            int i = static_cast<int>(std::find(keys.begin(), keys.end(), v) - keys.begin());
            if (i >= static_cast<int>(keys.size())) {
                i = 0;
            }
            int n = i + dir;
            if (n < 0 || n >= static_cast<int>(keys.size())) {
                if (!wrap) {
                    break;
                }
                n = (n + static_cast<int>(keys.size())) % static_cast<int>(keys.size());
            }
            if (w.type == WIDGET_COMBOBOX) {
                *std::get<int32_t*>(w.valuePointer) = keys[n];
            } else {
                CVarSetInteger(w.cVar, keys[n]);
            }
            changed = n != i;
        } break;
        case WIDGET_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_INT: {
            auto o = std::static_pointer_cast<UIWidgets::IntSliderOptions>(w.options);
            int32_t v = w.type == WIDGET_SLIDER_INT ? *std::get<int32_t*>(w.valuePointer)
                                                    : CVarGetInteger(w.cVar, o->defaultValue);
            int32_t n = v + dir * std::max(1, o->step);
            if (n > o->max) {
                n = wrap ? o->min : o->max;
            }
            if (n < o->min) {
                n = wrap ? o->max : o->min;
            }
            if (w.type == WIDGET_SLIDER_INT) {
                *std::get<int32_t*>(w.valuePointer) = n;
            } else {
                CVarSetInteger(w.cVar, n);
            }
            changed = n != v;
        } break;
        case WIDGET_SLIDER_FLOAT:
        case WIDGET_CVAR_SLIDER_FLOAT: {
            auto o = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(w.options);
            float v = w.type == WIDGET_SLIDER_FLOAT ? *std::get<float*>(w.valuePointer)
                                                    : CVarGetFloat(w.cVar, o->defaultValue);
            float step = o->step > 0 ? o->step : 0.01f;
            float n = std::round((v + dir * step) / step) * step;
            if (n > o->max + step * 0.5f) {
                n = wrap ? o->min : o->max;
            }
            if (n < o->min - step * 0.5f) {
                n = wrap ? o->max : o->min;
            }
            n = std::clamp(n, o->min, o->max);
            if (w.type == WIDGET_SLIDER_FLOAT) {
                *std::get<float*>(w.valuePointer) = n;
            } else {
                CVarSetFloat(w.cVar, n);
            }
            changed = n != v;
        } break;
        default:
            break;
    }
    if (changed) {
        CVarSave();
        if (w.callback != nullptr) {
            w.callback(w);
        }
    }
    return changed;
}


bool IsToggle(WidgetType t) {
    return t == WIDGET_CHECKBOX || t == WIDGET_CVAR_CHECKBOX;
}

// A value chosen with A first, then left / right (sliders and lists), A or B to finish.
bool IsEditable(const WidgetInfo& w) {
    return IsValueWidget(w.type) && !IsToggle(w.type) && w.type != WIDGET_VIDEO_BACKEND;
}

// Y: the setting back to its default.
bool ResetDefault(WidgetInfo& w) {
    if (!IsValueWidget(w.type) || w.type == WIDGET_VIDEO_BACKEND || w.type == WIDGET_AUDIO_BACKEND ||
        (w.options && w.options->disabled)) {
        return false;
    }
    switch (w.type) {
        case WIDGET_CVAR_CHECKBOX:
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_CVAR_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_FLOAT:
            if (w.cVar == nullptr) {
                return false;
            }
            CVarClear(w.cVar); // the widget's own default applies again
            break;
        case WIDGET_CHECKBOX:
            *std::get<bool*>(w.valuePointer) = std::static_pointer_cast<UIWidgets::CheckboxOptions>(w.options)->defaultValue;
            break;
        case WIDGET_COMBOBOX: {
            auto o = std::static_pointer_cast<UIWidgets::ComboboxOptions>(w.options);
            if (!o->hasDefault) {
                return false;
            }
            *std::get<int32_t*>(w.valuePointer) = static_cast<int32_t>(o->defaultIndex);
        } break;
        case WIDGET_SLIDER_INT:
            *std::get<int32_t*>(w.valuePointer) = std::static_pointer_cast<UIWidgets::IntSliderOptions>(w.options)->defaultValue;
            break;
        case WIDGET_SLIDER_FLOAT:
            *std::get<float*>(w.valuePointer) = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(w.options)->defaultValue;
            break;
        default:
            return false;
    }
    CVarSave();
    if (w.callback != nullptr) {
        w.callback(w);
    }
    return true;
}

// "Popout Bindings Window" -> "Bindings": the window opens inside this menu, nothing pops out any more.
std::string PageName(const std::string& name) {
    std::string n = name;
    if (n.rfind("Popout ", 0) == 0) {
        n = n.substr(7);
    }
    if (n.size() > 7 && n.compare(n.size() - 7, 7, " Window") == 0) {
        n = n.substr(0, n.size() - 7);
    }
    return n;
}

// ImGui's own widgets (inside a card, or a page of another window) in the menu's colours.
// padX: the inner left / right margin of buttons and fields. Pages of other windows (the bindings editor) size their
// buttons as text + a small margin themselves, so they get a narrow one (a wide one cut their labels off).
int PushTheme(float unit, float padX = 12.0f) {
    auto c = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };
    const ImU32 cols[][2] = {
        { ImGuiCol_Text, kText },          { ImGuiCol_TextDisabled, kTextOff },
        { ImGuiCol_WindowBg, 0 },          { ImGuiCol_ChildBg, 0 },
        { ImGuiCol_PopupBg, IM_COL32(26, 28, 34, 252) },
        { ImGuiCol_Border, IM_COL32(255, 255, 255, 26) },
        { ImGuiCol_FrameBg, kCard },       { ImGuiCol_FrameBgHovered, kCardFocus },
        { ImGuiCol_FrameBgActive, kCardFocus },
        { ImGuiCol_Button, kCard },        { ImGuiCol_ButtonHovered, kCardFocus },
        { ImGuiCol_ButtonActive, kAccent },
        { ImGuiCol_Header, kCardFocus },   { ImGuiCol_HeaderHovered, kCardFocus },
        { ImGuiCol_HeaderActive, kAccent },
        { ImGuiCol_CheckMark, kAccent },   { ImGuiCol_SliderGrab, kAccent },
        { ImGuiCol_SliderGrabActive, kAccent },
        { ImGuiCol_Separator, IM_COL32(255, 255, 255, 26) },
        { ImGuiCol_Tab, kCard },           { ImGuiCol_TabHovered, kCardFocus },
        { ImGuiCol_TabSelected, kCardFocus },
        { ImGuiCol_ScrollbarBg, 0 },       { ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 40) },
        { ImGuiCol_NavCursor, kAccent },
    };
    for (auto& p : cols) {
        ImGui::PushStyleColor(static_cast<ImGuiCol>(p[0]), c(p[1]));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * unit);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f * unit);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f * unit);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX * unit, 9.0f * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16.0f * unit, 14.0f * unit));
    return IM_ARRAYSIZE(cols);
}

void PopTheme(int colours) {
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(colours);
}

// A round key symbol and its meaning; returns the width used.
float Hint(ImDrawList* dl, ImFont* font, float size, float unit, float x, float y, const char* key, const char* text) {
    const float pad = 10.0f * unit;
    const ImVec2 ks = font->CalcTextSizeA(size, FLT_MAX, 0, key);
    const float kw = std::max(ks.x + pad * 2, size + 6.0f * unit);
    dl->AddRectFilled({ x, y - 3.0f * unit }, { x + kw, y + size + 3.0f * unit }, IM_COL32(60, 64, 74, 255),
                      (size + 6.0f * unit) / 2);
    dl->AddText(font, size, { x + (kw - ks.x) / 2, y }, kText, key);
    dl->AddText(font, size, { x + kw + 8.0f * unit, y }, kTextDim, text);
    return kw + 8.0f * unit + font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
}

float HintWidth(ImFont* font, float size, float unit, const char* key, const char* text) {
    if (key == nullptr) {
        return 0.0f;
    }
    const float pad = 10.0f * unit;
    const float kw = std::max(font->CalcTextSizeA(size, FLT_MAX, 0, key).x + pad * 2, size + 6.0f * unit);
    return kw + 8.0f * unit + font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
}

// The controls in two rows of two, flush right at `right`; each column as wide as its widest entry, so the rows line up.
void Hints(ImDrawList* dl, ImFont* font, float size, float unit, float right, float y1, float y2,
           const char* const k[4], const char* const t[4]) {
    const float w1 = std::max(HintWidth(font, size, unit, k[0], t[0]), HintWidth(font, size, unit, k[2], t[2]));
    const float w2 = std::max(HintWidth(font, size, unit, k[1], t[1]), HintWidth(font, size, unit, k[3], t[3]));
    const float x2 = right - w2, x1 = x2 - 36.0f * unit - w1;
    if (k[0]) Hint(dl, font, size, unit, x1, y1, k[0], t[0]);
    if (k[1]) Hint(dl, font, size, unit, x2, y1, k[1], t[1]);
    if (k[2]) Hint(dl, font, size, unit, x1, y2, k[2], t[2]);
    if (k[3]) Hint(dl, font, size, unit, x2, y2, k[3], t[3]);
}

// Text wrapped to `width`, each line flush right at `right`.
void TextRight(ImDrawList* dl, ImFont* font, float size, float right, float y, float width, ImU32 col, const std::string& text) {
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end) {
        const char* brk = font->CalcWordWrapPositionA(size / font->FontSize, p, end, width);
        if (brk == p) {
            brk = p + 1;
        }
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, p, brk);
        dl->AddText(font, size, { right - sz.x, y }, col, p, brk);
        y += size + 2.0f;
        p = brk;
        while (p < end && (*p == ' ' || *p == '\n')) {
            p++;
        }
    }
}

} // namespace

// A page of the Back menu, read again every few frames (and right after a change).
static const std::vector<std::string>& JavaPage(const std::string& id) {
    auto& c = s.javaCache[id];
    if (c.second.empty() || s.frame - c.first > 10 || c.first > s.frame) {
        c.second = Android_MenuPage(id.c_str());
        c.first = s.frame;
    }
    return c.second;
}

static void JavaAct(const std::string& id, int index, int op) {
    Android_MenuAct(id.c_str(), index, op);
    for (auto& [k, v] : s.javaCache) {
        v.first = -100; // read every page again (a step may change others, e.g. the shader's name)
    }
}

// Areas whose sections are one tab here (one page, the sections as headings): the developer tools.
static bool MergedArea(const std::string& header) {
    return header == "Developer";
}

// Headings the engine's pages don't have: put above these options (by their shown name), so every tab is grouped
// like the Controls tab (the same in all N64 ports).
static const char* HeadingBefore(const std::string& name) {
    static const std::vector<std::pair<const char*, const char*>> table = {
        { "Startup Behaviour", "Game" }, { "Internal Resolution", "Picture" }, { "Current FPS", "Frame rate" },
        { "Texture Filter (Needs reload)", "Quality" }, { "Master Volume", "Volume" }, { "Audio API", "Output" },
    };
    for (auto& [n, h] : table) {
        if (name == n) {
            return h;
        }
    }
    return nullptr;
}

void Menu::DrawCardMenu() {
    UpdateAccent();
    // ---- the tabs: every section of every area (merged areas as one), then the app's own entries -------------
    struct PageTab : Tab {
        std::vector<std::string> sidebars;
        std::vector<std::string> headersOf; // regrouped: each sidebar's own area (empty: all of `header`)
        std::vector<std::string> headings;  // regrouped: each sidebar's heading on the tab
        bool regrouped = false;
        std::vector<std::pair<std::string, std::string>> java; // (heading, Back menu page id) after the widgets
    };
    s.frame++;
    if (Android_MenuTakeClose()) { // "Move and resize buttons": the layout editor is on the game screen
        s.javaStack.clear();
        ToggleVisibility();
    }
    {
        std::string pushed = Android_MenuTakePushed();
        if (!pushed.empty()) {
            s.javaStack.push_back(pushed);
            s.focus = 0;
            s.editing = false;
            s.resetScroll = true;
        }
    }
    std::vector<PageTab> tabs;
    for (auto& header : menuOrder) {
        auto& entry = menuEntries.at(header);
        bool first = true;
        if (MergedArea(header)) {
            PageTab t;
            t.label = header;
            t.header = header;
            t.groupStart = !tabs.empty();
            for (auto& sidebar : entry.sidebarOrder) {
                if (sidebar != "Search") {
                    t.sidebars.push_back(sidebar);
                }
            }
            if (!t.sidebars.empty()) {
                t.sidebar = t.sidebars[0];
                tabs.push_back(t);
            }
            continue;
        }
        for (auto& sidebar : entry.sidebarOrder) {
            if (sidebar == "Search") {
                continue;
            }
            PageTab t;
            t.label = TabLabel(header, sidebar);
            t.header = header;
            t.sidebar = sidebar;
            t.sidebars = { sidebar };
            t.groupStart = first && !tabs.empty();
            tabs.push_back(t);
            first = false;
        }
    }
    // ---- regroup: similar sections on one tab, each as a heading (nothing is dropped - a section not named here
    // keeps its own tab). The same shape in all N64 ports: General, Gameplay, Graphics, Shaders, Audio, Controls, ...
    {
        struct Src { const char* header; const char* sidebar; const char* heading; };
        struct Group { const char* label; std::vector<Src> srcs; };
        static const std::vector<Group> groups = {
            { "General", { { "Settings", "General", "General" } } },
            { "Gameplay", { { "Enhancements", "General", "Enhancements" }, { "Enhancements", "Rulesets", "Rulesets" },
                            { "Enhancements", "Cheats", "Cheats" }, { "Enhancements", "Freecam", "Freecam" } } },
        };
        for (const Group& g : groups) {
            int at = -1;
            PageTab merged;
            for (const Src& src : g.srcs) {
                for (size_t i = 0; i < tabs.size(); i++) {
                    PageTab& t = tabs[i];
                    if (t.header != src.header || t.sidebars.size() != 1 || t.sidebars[0] != src.sidebar || t.regrouped) {
                        continue;
                    }
                    if (at < 0) {
                        merged = t;
                        merged.label = g.label;
                        merged.sidebars.clear();
                        merged.headersOf.clear();
                        merged.headings.clear();
                        at = static_cast<int>(i);
                    }
                    merged.sidebars.push_back(t.sidebars[0]);
                    merged.headersOf.push_back(t.header);
                    merged.headings.push_back(src.heading);
                    if (static_cast<int>(i) != at) {
                        tabs.erase(tabs.begin() + i);
                    }
                    break;
                }
            }
            if (at >= 0) {
                merged.regrouped = true;
                tabs[at] = merged;
            }
        }
    }
    auto action = [&](const char* label, TabKind kind, bool gap) {
        PageTab t;
        t.label = label;
        t.kind = kind;
        t.groupStart = gap;
        tabs.push_back(t);
    };
    // the Back menu's pages where they belong: touch controls under Controls, the FPS readout under Graphics, the
    // shaders as a tab of their own after Graphics
    for (size_t i = 0; i < tabs.size(); i++) {
        if (tabs[i].header == "Settings" && tabs[i].sidebar == "Controls") {
            tabs[i].java.push_back({ "Touch controls", "touch" });
        }
        if (tabs[i].header == "Settings" && tabs[i].sidebar == "Graphics") {
            tabs[i].java.push_back({ "Display", "display" });
            if (JavaPage("shader").size() > 1) {
                PageTab t;
                t.label = "Shaders";
                t.java.push_back({ "", "shader" });
                tabs.insert(tabs.begin() + i + 1, t);
                i++;
            }
        }
    }
    // the same section order in all N64 ports' menus: General, the game's own areas, Graphics, Shaders, Audio,
    // Controls, the developer tools, Mods (DK64 / Banjo / dinotale build theirs the same way)
    {
        auto rank = [](const PageTab& t) {
            if (t.label == "Mods") return 90;
            if (t.header == "Developer" || t.header == "Dev Tools") return 60;
            if (t.label == "Shaders" || t.header == "Shaders") return 30;
            if (t.header == "Settings") {
                if (t.sidebar == "General") return 0;
                if (t.sidebar == "Graphics") return 20;
                if (t.sidebar == "Audio") return 40;
                if (t.sidebar == "Controls") return 50;
                return 15;
            }
            return 10;
        };
        std::stable_sort(tabs.begin(), tabs.end(), [&](const PageTab& a, const PageTab& b) { return rank(a) < rank(b); });
        int prev = -1;
        for (auto& t : tabs) {
            const int r = rank(t);
            t.groupStart = (r >= 60) && r != prev;
            prev = r;
        }
    }
    action("Mods", TabKind::Mods, true);
    if (s.tab < 0 || s.tab >= static_cast<int>(tabs.size()) || tabs[s.tab].kind != TabKind::Page) {
        s.tab = 0;
        s.focus = 0;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin = vp->WorkPos;
    const ImVec2 size = vp->WorkSize;
    const float unit = std::max(1.0f, size.y / 540.0f); // laid out for 960 x 540

    ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(kBg));
    const bool open = ImGui::Begin("Main Menu", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    if (s.subWindow.empty()) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // the game picture is an ImGui window too
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* fontBig = GameEngine::Instance->fontStandardLargest;
    ImFont* font = GameEngine::Instance->fontStandardLarger;
    ImFont* fontSmall = GameEngine::Instance->fontStandard;
    const float big = 30.0f * unit, mid = 20.0f * unit, small = 15.0f * unit;
    ImGuiIO& io = ImGui::GetIO();

    // ---- controller ------------------------------------------------------------------------------------------
    // Up / down move; A: a switch flips, a slider or list goes into editing (left / right change it, A or B ends
    // it), a button runs, a page opens; Y: the default; L / R: the section; B: one level up (MenuCards_Back).
    int move = 0, change = 0, section = 0;
    bool press = false, reset = false;
    if ((io.BackendFlags & ImGuiBackendFlags_HasGamepad) && s.subWindow.empty()) {
        move = (PadPressed(ImGuiKey_GamepadDpadDown, true) || PadPressed(ImGuiKey_GamepadLStickDown, true)) -
               (PadPressed(ImGuiKey_GamepadDpadUp, true) || PadPressed(ImGuiKey_GamepadLStickUp, true));
        change = (PadPressed(ImGuiKey_GamepadDpadRight, true) || PadPressed(ImGuiKey_GamepadLStickRight, true)) -
                 (PadPressed(ImGuiKey_GamepadDpadLeft, true) || PadPressed(ImGuiKey_GamepadLStickLeft, true));
        section = PadPressed(ImGuiKey_GamepadR1, true) - PadPressed(ImGuiKey_GamepadL1, true);
        press = PadPressed(ImGuiKey_GamepadFaceDown, false);
        reset = PadPressed(ImGuiKey_GamepadFaceUp, false);
        for (ImGuiKey k : { ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadL2, ImGuiKey_GamepadR2 }) {
            ImGui::SetKeyOwner(k, kOwner, ImGuiInputFlags_LockUntilRelease);
        }
    }
    auto runTab = [&](int i) {
        s.editing = false;
        s.javaStack.clear();
        switch (tabs[i].kind) {
            case TabKind::Page:
                if (s.tab != i) {
                    s.tab = i;
                    s.focus = 0;
                    s.resetScroll = true;
                }
                break;
            case TabKind::Mods:
                Android_OpenModsManager();
                break;
            case TabKind::BackToGame:
                ToggleVisibility();
                break;
            case TabKind::Exit:
                ToggleVisibility();
                Ship::Context::GetRawInstance()->GetWindow()->Close();
                break;
        }
    };
    if (section != 0) {
        int n = s.tab;
        do {
            n = (n + section + static_cast<int>(tabs.size())) % static_cast<int>(tabs.size());
        } while (tabs[n].kind != TabKind::Page);
        runTab(n);
        s.tabCursor = n;
    }
    if (s.focusOnTabs) {
        if (move != 0) {
            s.tabCursor = std::clamp(s.tabCursor + move, 0, static_cast<int>(tabs.size()) - 1);
            if (tabs[s.tabCursor].kind == TabKind::Page) {
                runTab(s.tabCursor);
            }
            s.scrollToFocus = true;
        }
        if (press || change > 0) {
            if (tabs[s.tabCursor].kind == TabKind::Page) {
                s.focusOnTabs = false;
            } else if (press) {
                runTab(s.tabCursor);
            }
        }
        move = change = 0;
        press = reset = false;
    }

    // ---- left: title and sections ------------------------------------------------------------------------------
    const float leftX = origin.x + 24.0f * unit;
    const float leftW = 260.0f * unit;
    float y = origin.y + 14.0f * unit;
    dl->AddText(fontBig, big, { leftX + 10.0f * unit, y }, kText, "Settings");
    dl->AddRectFilled({ leftX + 10.0f * unit, y + big + 4.0f * unit }, { leftX + 50.0f * unit, y + big + 7.0f * unit },
                      kAccent, 2.0f * unit);
    y += big + 14.0f * unit;
    const float tabH = 40.0f * unit;
    const float tabsTop = y;
    const float tabsBottom = origin.y + size.y - 12.0f * unit;
    ImGui::SetCursorScreenPos({ leftX, tabsTop });
    ImGui::BeginChild("##tabs", { leftW, tabsBottom - tabsTop }, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    // touch: the column scrolls with a finger too (a drag is no tap on a section)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (ImGui::IsMouseClicked(0)) {
            s.dragY = 0.0f;
            s.dragging = false;
        }
        if (ImGui::IsMouseDown(0)) {
            s.dragY += io.MouseDelta.y;
            if (std::fabs(s.dragY) > 12.0f * unit) {
                s.dragging = true;
            }
            if (s.dragging) {
                ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
            }
        }
    }
    for (int i = 0; i < static_cast<int>(tabs.size()); i++) {
        if (tabs[i].groupStart) {
            ImGui::Dummy({ 1, 10.0f * unit });
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", { leftW - 4.0f, tabH }) && !s.dragging) {
            s.subWindow.clear();
            runTab(i);
            s.tabCursor = i;
        }
        ImGui::PopID();
        const bool active = i == s.tab;
        if (active) {
            tdl->AddRectFilled(p, { p.x + leftW - 4.0f, p.y + tabH }, IM_COL32(255, 255, 255, 10), 8.0f * unit);
            tdl->AddRectFilled({ p.x, p.y + 8.0f * unit }, { p.x + 4.0f * unit, p.y + tabH - 8.0f * unit }, kAccent,
                               2.0f * unit);
        }
        if (s.focusOnTabs && i == s.tabCursor) {
            tdl->AddRect(p, { p.x + leftW - 4.0f, p.y + tabH }, kAccent, 10.0f * unit, 0, 3.0f * unit);
        }
        if ((s.focusOnTabs ? i == s.tabCursor : active) && s.scrollToFocus) {
            ImGui::SetScrollHereY(0.5f);
        }
        const ImU32 col = active ? kText : (tabs[i].kind == TabKind::Page ? kTextDim : kTextOff);
        tdl->AddText(font, mid, { p.x + 20.0f * unit, p.y + (tabH - mid) / 2 }, col, tabs[i].label.c_str());
    }
    ImGui::EndChild();

    // ---- right: the page's items ------------------------------------------------------------------------------
    PageTab& tab = tabs[s.tab];
    struct Item {
        WidgetInfo* w = nullptr; // null and no page: a heading (a section of a merged page)
        std::string heading;
        std::string page;        // a Back menu item: its page, index, kind ("v" / "s" a switch / "a"), title, value
        int index = -1;
        std::string kind, title, value;
        int app = 0;             // 1 Back to game, 2 Exit game (the first cards of General)
    };
    std::vector<Item> items;
    std::string javaTitle;
    auto addJava = [&](const std::string& heading, const std::string& id) {
        const auto& d = JavaPage(id);
        if (d.size() < 4) {
            return;
        }
        javaTitle = d[0];
        if (!heading.empty()) {
            items.push_back({ nullptr, heading });
        }
        for (size_t k = 1; k + 2 < d.size(); k += 3) {
            Item it;
            it.page = id;
            it.index = static_cast<int>((k - 1) / 3);
            it.kind = d[k];
            it.title = d[k + 1];
            it.value = d[k + 2];
            items.push_back(it);
        }
    };
    if (s.javaStack.empty() && tab.header == "Settings" && tab.sidebar == "General") {
        Item b, e;
        b.app = 1;
        b.title = "Back to game";
        e.app = 2;
        e.title = "Exit game";
        items.push_back({ nullptr, "Session" });
        items.push_back(b);
        items.push_back(e);
    }
    if (!s.javaStack.empty()) {
        addJava("", s.javaStack.back());
    } else for (size_t si = 0; si < tab.sidebars.size(); si++) {
        const std::string& area = si < tab.headersOf.size() ? tab.headersOf[si] : tab.header;
        SidebarEntry& page = menuEntries.at(area).sidebars.at(tab.sidebars[si]);
        if (tab.sidebars.size() > 1) {
            items.push_back({ nullptr, si < tab.headings.size() ? tab.headings[si] : tab.sidebars[si] });
        }
        for (auto& column : page.columnWidgets) {
            for (auto& w : column) {
                if (w.type == WIDGET_SEARCH) {
                    continue;
                }
                if (w.preFunc != nullptr) {
                    w.ResetDisables();
                    w.preFunc(w);
                    if (w.isHidden) {
                        continue;
                    }
                    if (!w.activeDisables.empty()) {
                        w.options->disabled = true;
                    }
                }
                // a page's own heading right after the same section heading of a merged tab: once is enough
                if (w.type == WIDGET_SEPARATOR_TEXT && !items.empty() && items.back().w == nullptr &&
                    items.back().index < 0 && items.back().app == 0 && items.back().heading == w.name) {
                    continue;
                }
                if (const char* h = HeadingBefore(DisplayName(w.name))) {
                    items.push_back({ nullptr, h });
                }
                items.push_back({ &w, "" });
            }
        }
    }
    if (s.javaStack.empty()) {
        for (auto& [heading, id] : tab.java) {
            addJava(heading, id);
        }
    }
    std::vector<int> focusable;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if ((items[i].w != nullptr && IsFocusable(*items[i].w)) || items[i].index >= 0 || items[i].app > 0) {
            focusable.push_back(i);
        }
    }
    if (!focusable.empty()) {
        s.focus = std::clamp(s.focus, 0, static_cast<int>(focusable.size()) - 1);
        if (move != 0 && !s.editing) {
            s.focus = std::clamp(s.focus + move, 0, static_cast<int>(focusable.size()) - 1);
            s.scrollToFocus = true;
        }
    }
    Item* focusedItem = focusable.empty() ? nullptr : &items[focusable[s.focus]];
    WidgetInfo* focused = focusedItem ? focusedItem->w : nullptr;
    auto runApp = [&](int app) {
        if (app == 1) {
            ToggleVisibility();
        } else if (app == 2) {
            ToggleVisibility();
            Ship::Context::GetRawInstance()->GetWindow()->Close();
        }
    };
    if (focusedItem != nullptr && focusedItem->app > 0) {
        s.editing = false;
        if (press) {
            runApp(focusedItem->app);
        }
    }
    if (focusedItem != nullptr && focusedItem->index >= 0) {
        Item& it = *focusedItem;
        const bool isValue = it.kind == "v" || it.kind == "s", toggle = it.kind == "s";
        if (!isValue || toggle) {
            s.editing = false;
        }
        if (s.editing && change != 0) {
            JavaAct(it.page, it.index, change);
        }
        if (press) {
            if (toggle) {
                JavaAct(it.page, it.index, 1);
            } else if (isValue) {
                s.editing = !s.editing;
            } else {
                JavaAct(it.page, it.index, 0);
            }
        }
        if (reset) {
            JavaAct(it.page, it.index, 2);
        }
    }
    if (focused != nullptr && !IsEditable(*focused)) {
        s.editing = false;
    }
    auto openPage = [&](WidgetInfo& w) {
        if (w.windowName && w.windowName[0] &&
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow(w.windowName)) {
            s.subWindow = w.windowName;
            s.subTitle = PageName(w.name);
            s.subFocus = true;
            s.editing = false;
        }
    };
    if (focused != nullptr) {
        const bool disabled = focused->options && focused->options->disabled;
        if (s.editing && change != 0) {
            StepValue(*focused, change, false);
        }
        if (press) {
            if (IsToggle(focused->type)) {
                StepValue(*focused, 1, true);
            } else if (IsEditable(*focused)) {
                s.editing = !s.editing && !disabled;
            } else if (focused->type == WIDGET_BUTTON && !disabled && focused->callback) {
                focused->callback(*focused);
            } else if (focused->type == WIDGET_WINDOW_BUTTON) {
                openPage(*focused);
            }
        }
        if (reset) {
            ResetDefault(*focused);
        }
    }

    const float rightX = leftX + leftW + 24.0f * unit;
    const float rightW = origin.x + size.x - 24.0f * unit - rightX;
    const float titleY = origin.y + 14.0f * unit;
    std::string title = s.subWindow.empty() ? tab.label : tab.label + "  /  " + s.subTitle;
    if (!s.javaStack.empty()) {
        title = tab.label + "  /  " + javaTitle;
    }
    dl->AddText(font, mid + 2.0f * unit, { rightX + 4.0f * unit, titleY + (big - mid) / 2 }, kText, title.c_str());
    const float listTop = titleY + big + 14.0f * unit;
    const float footerH = (small + 14.0f * unit) * 2;
    const float descH = small * 2 + 12.0f * unit;
    const float listBottom = origin.y + size.y - footerH - descH - 10.0f * unit;

    if (!s.subWindow.empty()) {
        // ---- a page of another window (bindings, stats, ...), drawn inside the menu in its colours -----------
        ImGui::End(); // the page is its own window, so the controller can move through its widgets
        ImGui::SetNextWindowPos({ rightX, listTop }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ rightW, listBottom - listTop }, ImGuiCond_Always);
        if (s.subFocus) {
            ImGui::SetNextWindowFocus();
        }
        const int themed = PushTheme(unit, 3.0f);
        // the larger font: the window sizes its buttons by it, so they get finger-sized and further apart
        ImGui::PushFont(font);
        if (ImGui::Begin("##menu_page", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            // touch: drag to scroll; a drag lets go of the button it started on, so it does not press it
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                if (ImGui::IsMouseClicked(0)) {
                    s.dragY = 0.0f;
                    s.dragging = false;
                }
                if (ImGui::IsMouseDown(0)) {
                    s.dragY += io.MouseDelta.y;
                    if (std::fabs(s.dragY) > 12.0f * unit) {
                        s.dragging = true;
                    }
                    if (s.dragging) {
                        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
                        ImGui::ClearActiveID();
                    }
                }
            }
            if (ImGui::IsMouseReleased(0)) {
                s.dragging = false;
            }
            auto win = Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow(s.subWindow);
            if (win) {
                win->DrawElement();
            }
            if (s.subFocus) {
                ImGui::NavInitWindow(ImGui::GetCurrentWindow(), true);
                ImGui::SetNavCursorVisible(true);
                s.subFocus = false;
            }
        }
        ImGui::End();
        ImGui::PopFont();
        PopTheme(themed);
        // the hints, on the full-screen window's draw list
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float y1 = origin.y + size.y - footerH + 4.0f * unit;
        {
            const char* k[4] = { "D-Pad", "A", nullptr, "B" };
            const char* t[4] = { "Move", "Select", "", "Back to the list" };
            Hints(fg, fontSmall, small, unit, rightX + rightW - 4.0f * unit, y1, y1 + small + 14.0f * unit, k, t);
        }
        return;
    }

    ImGui::SetCursorScreenPos({ rightX, listTop });
    ImGui::BeginChild("##cards", { rightW, listBottom - listTop }, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    if (s.resetScroll) {
        ImGui::SetScrollY(0.0f);
        s.resetScroll = false;
    }
    // touch: drag to scroll (a tap still changes a value; a drag does not)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseClicked(0)) {
        s.dragY = 0.0f;
        s.dragging = false;
    }
    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                                        ImGuiHoveredFlags_ChildWindows)) {
        s.dragY += io.MouseDelta.y;
        if (std::fabs(s.dragY) > 12.0f * unit) {
            s.dragging = true;
        }
        if (s.dragging) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        }
    }
    const float cardH = 58.0f * unit;
    const float gap = 10.0f * unit;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (items[i].app > 0) { // Back to game / Exit game
            const int fi = static_cast<int>(std::find(focusable.begin(), focusable.end(), i) - focusable.begin());
            const bool foc = !s.focusOnTabs && fi == s.focus;
            const ImVec2 q = { p.x + rightW - 4.0f, p.y + cardH };
            const float cy = p.y + cardH / 2, r = 10.0f * unit;
            ImGui::PushID("app");
            if (ImGui::InvisibleButton("card", { rightW - 4.0f, cardH }) && !s.dragging) {
                s.focus = fi;
                runApp(items[i].app);
            }
            ImGui::PopID();
            cdl->AddRectFilled(p, q, foc ? kCardFocus : kCard, r);
            if (foc) {
                cdl->AddRectFilled(p, { p.x + 6.0f * unit, q.y }, kAccent, r, ImDrawFlags_RoundCornersLeft);
                cdl->AddRect(p, q, kAccentSoft, r, 0, 1.5f * unit);
                if (s.scrollToFocus) {
                    ImGui::SetScrollHereY(0.5f);
                }
            }
            cdl->AddText(font, mid, { p.x + 24.0f * unit, cy - mid / 2 }, kText, items[i].title.c_str());
            // a small mark on the right: a "play" triangle back into the game, a cross for leaving it
            const float mx = q.x - 34.0f * unit, h = 8.0f * unit;
            if (items[i].app == 1) {
                cdl->AddTriangleFilled({ mx - h * 0.7f, cy - h }, { mx - h * 0.7f, cy + h }, { mx + h, cy }, kAccent);
            } else {
                cdl->AddLine({ mx - h, cy - h }, { mx + h, cy + h }, kArrow, 2.4f * unit);
                cdl->AddLine({ mx - h, cy + h }, { mx + h, cy - h }, kArrow, 2.4f * unit);
            }
            ImGui::Dummy({ 1, gap - ImGui::GetStyle().ItemSpacing.y });
            ImGui::PopID();
            if (items[i].app == 2) { // a little room before the settings
                ImGui::Dummy({ 1, 6.0f * unit });
            }
            continue;
        }
        if (items[i].w == nullptr && items[i].index < 0) { // a section heading
            ImGui::Dummy({ rightW, 8.0f * unit });
            p = ImGui::GetCursorScreenPos();
            cdl->AddText(font, mid, { p.x + 6.0f * unit, p.y }, kAccent, items[i].heading.c_str());
            ImGui::Dummy({ rightW, mid + 10.0f * unit });
            ImGui::PopID();
            continue;
        }
        if (items[i].index >= 0) { // a Back menu item (touch controls, shaders, FPS)
            Item& it = items[i];
            const int fi = static_cast<int>(std::find(focusable.begin(), focusable.end(), i) - focusable.begin());
            const bool foc = !s.focusOnTabs && fi == s.focus;
            const bool isValue = it.kind == "v" || it.kind == "s", toggle = it.kind == "s";
            std::string name = it.title;
            bool link = false, chosen = false;
            std::string linkValue; // "Shader selection:  crt-royale  ›": the part after ":  " shown on the right
            if (name.size() > 3 && name.compare(name.size() - 3, 3, "\xe2\x80\xba") == 0) { // "›": opens a page
                name = name.substr(0, name.size() - 3);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                link = true;
                const size_t colon = name.find(":  ");
                if (colon != std::string::npos) {
                    linkValue = name.substr(colon + 3);
                    name = name.substr(0, colon);
                }
            }
            if (name.rfind("\xe2\x80\xa2 ", 0) == 0) { // "• ": the chosen one
                name = name.substr(4);
                chosen = true;
            }
            const ImVec2 q = { p.x + rightW - 4.0f, p.y + cardH };
            const float cy = p.y + cardH / 2;
            const float pillR = q.x - 18.0f * unit;
            const float pillW = toggle ? 64.0f * unit : rightW * 0.34f;
            const float pillL = pillR - pillW;
            ImGui::PushID("java");
            if (ImGui::InvisibleButton("card", { rightW - 4.0f, cardH }) && !s.dragging) {
                s.focus = fi;
                s.focusOnTabs = false;
                if (toggle) {
                    JavaAct(it.page, it.index, 1);
                } else if (isValue) {
                    if (io.MousePos.x >= pillL) {
                        JavaAct(it.page, it.index, io.MousePos.x < pillL + pillW / 3 ? -1 : 1);
                    }
                } else {
                    JavaAct(it.page, it.index, 0);
                }
            }
            ImGui::PopID();
            const float r = 10.0f * unit;
            cdl->AddRectFilled(p, q, foc ? kCardFocus : kCard, r);
            if (foc) {
                cdl->AddRectFilled(p, { p.x + 6.0f * unit, q.y }, kAccent, r, ImDrawFlags_RoundCornersLeft);
                cdl->AddRect(p, q, kAccentSoft, r, 0, 1.5f * unit);
                if (s.scrollToFocus) {
                    ImGui::SetScrollHereY(0.5f);
                }
            }
            float nx = p.x + 24.0f * unit;
            if (chosen) {
                cdl->AddCircleFilled({ nx + 5.0f * unit, cy }, 5.0f * unit, kAccent);
                nx += 20.0f * unit;
            }
            cdl->PushClipRect(p, { (isValue ? pillL : q.x - 40.0f * unit) - 16.0f * unit, q.y }, true);
            cdl->AddText(font, mid, { nx, cy - mid / 2 }, chosen ? kAccent : kText, name.c_str());
            cdl->PopClipRect();
            const bool editing = foc && s.editing;
            auto chevron = [&](float x, int dir) {
                const float h = 6.0f * unit, wdt = 4.0f * unit;
                ImVec2 pts[3] = { { x - dir * wdt, cy - h }, { x + dir * wdt, cy }, { x - dir * wdt, cy + h } };
                cdl->AddPolyline(pts, 3, editing ? kAccent : kArrow, ImDrawFlags_None, 2.2f * unit);
            };
            if (toggle) {
                const bool on = it.value == "On" || it.value == "shown";
                const float th = 30.0f * unit;
                const ImVec2 t0 = { pillL, cy - th / 2 }, t1 = { pillR, cy + th / 2 };
                cdl->AddRectFilled(t0, t1, on ? kAccent : IM_COL32(60, 64, 74, 255), th / 2);
                cdl->AddCircleFilled({ on ? t1.x - th / 2 : t0.x + th / 2, cy }, th / 2 - 4.0f * unit, kText);
            } else if (isValue) {
                const float ph = 38.0f * unit;
                cdl->AddRectFilled({ pillL, cy - ph / 2 }, { pillR, cy + ph / 2 }, IM_COL32(22, 24, 29, 255), ph / 2);
                if (editing) {
                    cdl->AddRect({ pillL, cy - ph / 2 }, { pillR, cy + ph / 2 }, kAccent, ph / 2, 0, 2.0f * unit);
                }
                const ImVec2 vs = font->CalcTextSizeA(mid, FLT_MAX, 0.0f, it.value.c_str());
                cdl->PushClipRect({ pillL + 30.0f * unit, p.y }, { pillR - 30.0f * unit, q.y }, true);
                cdl->AddText(font, mid, { (pillL + pillR) / 2 - vs.x / 2, cy - mid / 2 }, kAccent, it.value.c_str());
                cdl->PopClipRect();
                chevron(pillL + 18.0f * unit, -1);
                chevron(pillR - 18.0f * unit, 1);
            } else if (link) {
                chevron(q.x - 30.0f * unit, 1);
                if (!linkValue.empty()) {
                    const ImVec2 vs = font->CalcTextSizeA(mid, FLT_MAX, 0.0f, linkValue.c_str());
                    cdl->AddText(font, mid, { q.x - 56.0f * unit - vs.x, cy - mid / 2 }, kAccent, linkValue.c_str());
                }
            }
            ImGui::Dummy({ 1, gap - ImGui::GetStyle().ItemSpacing.y });
            ImGui::PopID();
            continue;
        }
        WidgetInfo& w = *items[i].w;
        const int fIndex = static_cast<int>(std::find(focusable.begin(), focusable.end(), i) - focusable.begin());
        const bool isFocused = !s.focusOnTabs && fIndex < static_cast<int>(focusable.size()) && fIndex == s.focus;
        const bool disabled = (w.options && w.options->disabled) || w.type == WIDGET_VIDEO_BACKEND;
        switch (w.type) {
            case WIDGET_SEPARATOR:
                ImGui::Dummy({ rightW, 8.0f * unit });
                break;
            case WIDGET_SEPARATOR_TEXT:
                ImGui::Dummy({ rightW, 6.0f * unit });
                p = ImGui::GetCursorScreenPos();
                cdl->AddText(font, mid, { p.x + 6.0f * unit, p.y }, kAccent, w.name.c_str());
                ImGui::Dummy({ rightW, mid + 8.0f * unit });
                break;
            case WIDGET_TEXT: {
                ImGui::PushFont(fontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextDim));
                ImGui::PushTextWrapPos(rightW - 16.0f * unit); // local to the list (it starts at its left edge)
                ImGui::SetCursorScreenPos({ p.x + 6.0f * unit, p.y });
                ImGui::TextWrapped("%s", w.name.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::Dummy({ rightW, 4.0f * unit });
            } break;
            default: {
                const bool valueRow = IsValueWidget(w.type);
                const bool simpleRow = valueRow || w.type == WIDGET_BUTTON || w.type == WIDGET_WINDOW_BUTTON;
                if (simpleRow) {
                    const ImVec2 q = { p.x + rightW - 4.0f, p.y + cardH };
                    const float cy = p.y + cardH / 2;
                    const bool toggle = IsToggle(w.type);
                    const bool editing = isFocused && s.editing;
                    const float pillR = q.x - 18.0f * unit;
                    const float pillW = toggle ? 64.0f * unit : rightW * 0.34f;
                    const float pillL = pillR - pillW;
                    if (ImGui::InvisibleButton("card", { rightW - 4.0f, cardH }) && !s.dragging) {
                        s.focus = fIndex;
                        s.focusOnTabs = false;
                        const float mx = io.MousePos.x;
                        if (valueRow) {
                            if (toggle) {
                                StepValue(w, 1, true);
                            } else if (mx >= pillL) {
                                // touch on the pill: its left third steps back, the rest forward
                                StepValue(w, mx < pillL + pillW / 3 ? -1 : 1, false);
                            }
                        } else if (w.type == WIDGET_BUTTON && !disabled && w.callback) {
                            w.callback(w);
                        } else if (w.type == WIDGET_WINDOW_BUTTON) {
                            openPage(w);
                        }
                    }
                    const float r = 10.0f * unit;
                    cdl->AddRectFilled(p, q, isFocused ? kCardFocus : kCard, r);
                    if (isFocused) {
                        cdl->AddRectFilled(p, { p.x + 6.0f * unit, q.y }, kAccent, r, ImDrawFlags_RoundCornersLeft);
                        cdl->AddRect(p, q, kAccentSoft, r, 0, 1.5f * unit);
                        if (s.scrollToFocus) {
                            ImGui::SetScrollHereY(0.5f);
                        }
                    }
                    const ImU32 nameCol = disabled ? kTextOff : kText;
                    const std::string name = w.type == WIDGET_WINDOW_BUTTON ? PageName(w.name) : DisplayName(w.name);
                    const float nameRight = (valueRow ? pillL : q.x - 40.0f * unit) - 16.0f * unit;
                    cdl->PushClipRect(p, { nameRight, q.y }, true);
                    cdl->AddText(font, mid, { p.x + 24.0f * unit, cy - mid / 2 }, nameCol, name.c_str());
                    cdl->PopClipRect();
                    const ImU32 ac = editing ? kAccent : kArrow;
                    auto chevron = [&](float x, int dir) {
                        const float h = 6.0f * unit, wdt = 4.0f * unit;
                        ImVec2 pts[3] = { { x - dir * wdt, cy - h }, { x + dir * wdt, cy }, { x - dir * wdt, cy + h } };
                        cdl->AddPolyline(pts, 3, ac, ImDrawFlags_None, 2.2f * unit);
                    };
                    if (valueRow && toggle) {
                        const bool on = ValueText(w) == "On";
                        const float th = 30.0f * unit;
                        const ImVec2 t0 = { pillL, cy - th / 2 }, t1 = { pillR, cy + th / 2 };
                        cdl->AddRectFilled(t0, t1, on && !disabled ? kAccent : IM_COL32(60, 64, 74, 255), th / 2);
                        const float kx = on ? t1.x - th / 2 : t0.x + th / 2;
                        cdl->AddCircleFilled({ kx, cy }, th / 2 - 4.0f * unit, disabled ? kTextOff : kText);
                    } else if (valueRow) {
                        const float ph = 38.0f * unit;
                        cdl->AddRectFilled({ pillL, cy - ph / 2 }, { pillR, cy + ph / 2 }, IM_COL32(22, 24, 29, 255),
                                           ph / 2);
                        if (editing) {
                            cdl->AddRect({ pillL, cy - ph / 2 }, { pillR, cy + ph / 2 }, kAccent, ph / 2, 0, 2.0f * unit);
                        }
                        const std::string v = ValueText(w);
                        const ImVec2 vs = font->CalcTextSizeA(mid, FLT_MAX, 0.0f, v.c_str());
                        cdl->PushClipRect({ pillL + 30.0f * unit, p.y }, { pillR - 30.0f * unit, q.y }, true);
                        cdl->AddText(font, mid, { (pillL + pillR) / 2 - vs.x / 2, cy - mid / 2 },
                                     disabled ? kTextOff : kAccent, v.c_str());
                        cdl->PopClipRect();
                        if (!disabled) { // read-only rows: no arrows
                            chevron(pillL + 18.0f * unit, -1);
                            chevron(pillR - 18.0f * unit, 1);
                        }
                    } else if (!disabled) {
                        chevron(q.x - 30.0f * unit, 1);
                    }
                } else {
                    // a widget that does not fit a row (colour picker, custom): drawn as it is, in its card
                    const ImVec2 start = p;
                    const int themed = PushTheme(unit);
                    cdl->ChannelsSplit(2);
                    cdl->ChannelsSetCurrent(1);
                    ImGui::SetCursorScreenPos({ p.x + 18.0f * unit, p.y + 12.0f * unit });
                    ImGui::BeginGroup();
                    ImGui::PushItemWidth(rightW - 60.0f * unit);
                    MenuDrawItem(w, 90, UIWidgets::Colors::Red);
                    ImGui::PopItemWidth();
                    ImGui::EndGroup();
                    const float end = ImGui::GetItemRectMax().y + 12.0f * unit;
                    cdl->ChannelsSetCurrent(0);
                    cdl->AddRectFilled(start, { start.x + rightW - 4.0f, end }, isFocused ? kCardFocus : kCard,
                                       10.0f * unit);
                    if (isFocused) {
                        cdl->AddRectFilled(start, { start.x + 6.0f * unit, end }, kAccent, 10.0f * unit,
                                           ImDrawFlags_RoundCornersLeft);
                        if (s.scrollToFocus) {
                            ImGui::SetScrollHereY(0.5f);
                        }
                    }
                    cdl->ChannelsMerge();
                    PopTheme(themed);
                    ImGui::SetCursorScreenPos({ start.x, end });
                    ImGui::Dummy({ rightW - 4.0f, 0.0f });
                }
                ImGui::Dummy({ 1, gap - ImGui::GetStyle().ItemSpacing.y });
            } break;
        }
        ImGui::PopID();
    }
    ImGui::Dummy({ 1, 8.0f * unit });
    if (ImGui::IsMouseReleased(0)) {
        s.dragging = false;
    }
    ImGui::EndChild();
    s.scrollToFocus = false;

    // ---- the focused setting's description and the controls (two rows) --------------------------------------
    const float descY = listBottom + 8.0f * unit;
    if (focused != nullptr && focused->options) {
        std::string d = focused->options->tooltip ? focused->options->tooltip : "";
        if (focused->type == WIDGET_WINDOW_BUTTON) {
            d = "Opens " + PageName(focused->name) + " here in the menu.";
        }
        if (focused->options->disabled && !focused->activeDisables.empty()) {
            std::string why;
            for (auto r : focused->activeDisables) {
                why += (why.empty() ? "" : ", ") + std::string(disabledMap.at(r).reason);
            }
            d = "Not available: " + why + (d.empty() ? "" : ". " + d);
        }
        TextRight(dl, fontSmall, small, rightX + rightW - 4.0f * unit, descY, rightW - 8.0f * unit, kTextDim, d);
    }
    {
        const float rowH = small + 14.0f * unit;
        const float y1 = origin.y + size.y - footerH + 4.0f * unit, y2 = y1 + rowH;
        const float right = rightX + rightW - 4.0f * unit;
        if (s.editing) {
            const char* k[4] = { "D-Pad", "A", "Y", "B" };
            const char* t[4] = { "Change", "Done", "Default", "Done" };
            Hints(dl, fontSmall, small, unit, right, y1, y2, k, t);
        } else {
            const char* k[4] = { "A", "B", "Y", "L / R" };
            const char* t[4] = { "Select", "Back", "Default", "Section" };
            Hints(dl, fontSmall, small, unit, right, y1, y2, k, t);
        }
    }

    ImGui::End();
}

// B / Back while the menu is up, one level at a time (true = taken): a page of another window -> the list,
// editing a value -> done, the list -> the section column; from the column the menu closes (false: SpaghettiGui
// closes it as before).
bool MenuCards_Back() {
    if (s.editing) {
        s.editing = false;
        return true;
    }
    if (!s.javaStack.empty()) {
        s.javaStack.pop_back();
        s.focus = 0;
        s.resetScroll = true;
        return true;
    }
    if (!s.subWindow.empty()) {
        s.subWindow.clear();
        return true;
    }
    if (s.editing) {
        s.editing = false;
        return true;
    }
    if (s.focusOnTabs) {
        return false;
    }
    s.focusOnTabs = true;
    s.tabCursor = std::max(0, s.tab);
    s.scrollToFocus = true;
    return true;
}

} // namespace Ship

#endif // __ANDROID__
