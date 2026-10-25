// Built only with Dear ImGui (third_party/imgui), like overlay.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "nav.h"

#include <cfloat>

#include "imgui_internal.h"

namespace sr2::ui::nav {

bool Button(const char* strId, ImVec2 sizeArg, ImGuiButtonFlags flags) {
    // ImGui::InvisibleButton without the NoNav item flag and without its RenderNavCursor (drawn before
    // the caller's own drawing, square-ish, and covered by neighbours); the caller draws Ring().
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    const ImVec2 size = ImGui::CalcItemSize(ImVec2(sizeArg.x != 0.0f ? sizeArg.x : -FLT_MIN, sizeArg.y != 0.0f ? sizeArg.y : -FLT_MIN),
                                            0.0f, 0.0f);
    const ImGuiID id = window->GetID(strId);
    const ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered = false, held = false;
    return ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);
}

bool Focused() {
    const ImGuiContext& g = *GImGui;
    return g.NavCursorVisible && g.NavId != 0 && g.NavId == g.LastItemData.ID &&
           (g.LastItemData.ItemFlags & ImGuiItemFlags_NoNav) == 0;
}

void Ring(float rounding) {
    if (!Focused()) return;
    Ring(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), rounding);
}

void Ring(ImVec2 a, ImVec2 b, float rounding) {
    if (!Focused()) return;
    const ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    if (window->DC.NavHideHighlightOneFrame) return;
    // As ImGui's RenderNavCursor (its other widgets keep theirs): 2 px, 3 px + half the line outside the item.
    const float th = static_cast<float>(static_cast<int>(ImMax(2.0f, 1.5f * ImGui::GetScale())));
    const float d = static_cast<float>(static_cast<int>(3.0f + th * 0.5f));
    const ImVec2 ra(a.x - d, a.y - d), rb(b.x + d, b.y + d);
    window->DrawList->PushClipRect(window->ClipRect.Min, window->ClipRect.Max, true);
    window->DrawList->AddRect(ra, rb, ImGui::GetColorU32(ImGuiCol_NavCursor), rounding > 0.0f ? rounding + d : 0.0f, 0, th);
    window->DrawList->PopClipRect();
}

bool ContextKey() {
    if (!ImGui::IsItemFocused()) return false;
    return ImGui::IsKeyPressed(ImGuiKey_Menu, false) || (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false));
}

void PushNoTabStop() { ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true); }
void PopNoTabStop() { ImGui::PopItemFlag(); }

TabGroup::TabGroup(const char* id) {
    st_ = ImGui::GetStateStorage();
    id_ = ImGui::GetID(id);
    cur_ = static_cast<ImGuiID>(st_->GetInt(id_, 0));
}

TabGroup::~TabGroup() {
    if (pushed_) PopNoTabStop();
    End();
}

void TabGroup::Item(uint64_t key) {
    item_ = ImHashData(&key, sizeof(key), 0x7AB5u);
    if (item_ == 0) item_ = 1;
    const bool firstItem = first_ == 0;
    if (firstItem) first_ = item_;
    if (item_ == cur_) seen_ = true;
    const bool stop = item_ == cur_ || (cur_ == 0 && firstItem);
    if (!stop) {
        PushNoTabStop();
        pushed_ = true;
    }
}

void TabGroup::Done() {
    if (pushed_) {
        PopNoTabStop();
        pushed_ = false;
    }
    // The focused item (by Tab, the arrows or a click) becomes the group's Tab stop.
    if (ImGui::IsItemFocused() && item_ != cur_) {
        cur_ = item_;
        seen_ = true;
        st_->SetInt(id_, static_cast<int>(cur_));
    }
}

void TabGroup::End() {
    if (ended_) return;
    ended_ = true;
    // The stop is gone (filtered out by a search, a tab switch): the first item takes over next frame.
    if (!seen_ && cur_ != 0) st_->SetInt(id_, static_cast<int>(first_));
}

}  // namespace sr2::ui::nav

#endif  // SR2_HAVE_IMGUI
