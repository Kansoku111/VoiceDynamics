// Win32 Japanese editor (no JUCE). Slider, numeric-entry and scaling mechanics follow Glaze's editor in this repository.
#include "editor.h"
#include "parameters.h"
#include "version.h"
#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
// Layout follows Adobe Audition's Dynamics effect: a dark window, sections side by side, knobs with blue values.
constexpr int Width = 920, Height = 780;
constexpr wchar_t ClassName[] = L"VoiceDynamics.Editor.020";
constexpr COLORREF Background = RGB(34, 34, 34), Paper = RGB(46, 46, 46), Ink = RGB(222, 222, 222), Muted = RGB(150, 150, 150), Accent = RGB(64, 148, 255),
                   Line = RGB(78, 78, 78), Night = RGB(22, 22, 22), NightMuted = RGB(140, 140, 140), NightGrid = RGB(58, 58, 58), Good = RGB(80, 200, 110),
                   Warn = RGB(240, 160, 60), Bad = RGB(235, 85, 75), GateColor = RGB(96, 160, 255), CompColor = RGB(255, 170, 70), LimitColor = RGB(255, 100, 90),
                   Face = RGB(60, 60, 60), Rim = RGB(98, 98, 98), Disabled = RGB(100, 100, 100);
enum Ids : int {
    SliderBase = 1000, EditBase = 2000, ToggleBase = 3000, MonoButton = 3100, LearnButton = 4000, CancelButton = 4001, FileButton = 4002, ResetButton = 4003,
    ClearButton = 4004, ZoomButton = 4500, ZoomMenu = 4510, StatusText = 5000, DetailText = 5001, HintText = 5002
};
// One knob cell: the knob on top, the value (blue, editable) under it, the name at the bottom.
constexpr int CellWidth = 110, KnobWidth = 60, KnobHeight = 46;
struct Place {
    uint32_t id;
    int x, y;
};
// Sections: three columns (gate | compressor | leveler + limiter) under the target panel, as in Audition.
constexpr int Column[]{8, 312, 616}, ColumnWidth = 296, ColumnTop = 212, ColumnBottom = 620, PanelTop = 52, PanelBottom = 204, MeterTop = 628, MeterBottom = 748;
constexpr int Left(int c) { return Column[c] + 25; }
constexpr int Right(int c) { return Column[c] + 160; }
constexpr int LimiterTop = 384;
constexpr Place Sliders[]{
    {vd::Target, 20, 80},
    {vd::GateOpen, Left(0), 256},      {vd::GateClose, Right(0), 256},      {vd::GateAttack, Left(0), 346}, {vd::GateRelease, Right(0), 346},
    {vd::GateHold, Left(0), 436},      {vd::GateRange, Right(0), 436},      {vd::GateHpf, Left(0), 526},
    {vd::CompThreshold, Left(1), 286}, {vd::CompRatio, Right(1), 286},      {vd::CompAttack, Left(1), 376}, {vd::CompRelease, Right(1), 376},
    {vd::CompKnee, Left(1), 466},      {vd::GainOffset, Right(1), 466},
    {vd::LevelerTarget, Left(2), 284}, {vd::LevelerRatio, Right(2), 284},
    {vd::Ceiling, Left(2), 444},       {vd::LimiterRelease, Right(2), 444}, {vd::GrLimit, Left(2), 530},
};
const wchar_t* label(uint32_t id) {
    switch (id) {
    case vd::GateOpen: return L"しきい値";
    case vd::GateClose: return L"閉じるしきい値";
    case vd::GateHold: return L"ホールド";
    case vd::GateAttack: return L"アタック";
    case vd::GateRelease: return L"リリース";
    case vd::GateRange: return L"範囲";
    case vd::GateHpf: return L"検出の低域カット";
    case vd::LevelerTarget: return L"基準レベル";
    case vd::LevelerRatio: return L"比率";
    case vd::CompThreshold: return L"しきい値";
    case vd::CompRatio: return L"比率";
    case vd::CompAttack: return L"アタック";
    case vd::CompRelease: return L"リリース";
    case vd::CompKnee: return L"ニー";
    case vd::Ceiling: return L"上限（True Peak）";
    case vd::LimiterRelease: return L"リリース";
    case vd::Target: return L"目標";
    case vd::GainOffset: return L"補正 微調整";
    case vd::GrLimit: return L"削減の警告";
    default: return L"";
    }
}
const wchar_t* help(uint32_t id) {
    switch (id) {
    case vd::GateOn: return L"声がない部分を静かにします。息や部屋の音を抑えます。";
    case vd::GateOpen: return L"この音量（声の短い平均レベル）を超えるとゲートが開きます。";
    case vd::GateClose: return L"この音量を下回るとゲートが閉じ始めます。開く閾値より上には設定できません。";
    case vd::GateHold: return L"閾値を下回ってもこの時間は開いたままにします。語尾が切れるときは長くします。";
    case vd::GateAttack: return L"開くときの速さです。短いほど子音が立ちます。";
    case vd::GateRelease: return L"閉じるときの速さです。長いほど自然に消えます。";
    case vd::GateRange: return L"閉じたときに下げる量です。−80 dBで完全に無音にします。";
    case vd::GateHpf: return L"ゲートの判定に使う信号だけ、この周波数より下をカットします。出力の音には影響しません。最小値でオフです。";
    case vd::LevelerOn: return L"ゆっくりした音量のばらつき（Aメロとサビ、マイクとの距離）をならします。コンプの前段です。";
    case vd::LevelerTarget: return L"レベラーがそろえる声の音量です。";
    case vd::LevelerRatio: return L"大きいほど基準レベルに強く寄せます。補正は±6 dBまでです。";
    case vd::CompOn: return L"声の大小の差を縮めます。";
    case vd::CompThreshold: return L"この音量を超えた分を圧縮します。";
    case vd::CompRatio: return L"圧縮の強さです。3:1なら、閾値を3 dB超えるごとに1 dBだけ出力が増えます。";
    case vd::CompAttack: return L"圧縮が効き始める速さです。短いと音の立ち上がりも抑えます。";
    case vd::CompRelease: return L"圧縮が戻る速さです。";
    case vd::CompKnee: return L"閾値付近の曲がり方です。大きいほど自然に効き始めます。";
    case vd::LimiterOn: return L"ピークが上限（True Peak）を超えないようにします。補正ゲインの後ろで動きます。";
    case vd::Ceiling: return L"出力のTrue Peakの上限です。書き出しがこの値を超えないようにします。";
    case vd::LimiterRelease: return L"リミッターが戻る速さです。深く効いているときは自動で長くなります。";
    case vd::Target: return L"全体の平均ラウドネス（LUFS）の目標です。配信の基準に合わせます。";
    case vd::GainOffset: return L"学習で決まった補正ゲインに足す微調整です。学習し直す必要はありません（リミッターに当たる場合は予測とずれます）。";
    case vd::GrLimit: return L"リミッターがこれ以上深く効いたら、画面で警告します。";
    case vd::AutoOn: return L"オン：学習で決まった補正ゲインを使います。オフ：補正ゲインは微調整の値だけです。";
    case vd::Bypass: return L"処理を通さず、原音を同じ遅延で出します。";
    case vd::LearnAutoStop: return L"オン：DAWの再生を止めたときに学習を終えます。";
    default: return L"";
    }
}
void format(uint32_t id, double normalized, wchar_t* out, size_t size) {
    const double v = vd::plain(id, normalized);
    if (id == vd::GateRange && v <= vd::GateRangeMute + .05) std::swprintf(out, size, L"ミュート");
    else if (id == vd::GateHpf && v <= vd::GateHpfOff + .05) std::swprintf(out, size, L"オフ");
    else if (id == vd::GateHold || id == vd::GateRelease || id == vd::CompRelease || id == vd::LimiterRelease || id == vd::GateHpf || (id == vd::CompAttack && v >= 10))
        std::swprintf(out, size, L"%.0f", v);
    else std::swprintf(out, size, L"%.1f", v);
}
const wchar_t* unit(uint32_t id) {
    switch (id) {
    case vd::GateHold: case vd::GateAttack: case vd::GateRelease: case vd::CompAttack: case vd::CompRelease: case vd::LimiterRelease: return L"ms";
    case vd::GateHpf: return L"Hz";
    case vd::CompRatio: case vd::LevelerRatio: return L": 1";
    case vd::Target: return L"LUFS";
    case vd::Ceiling: return L"dBTP";
    default: return L"dB";
    }
}
double stepFor(uint32_t id) {  // 0: 1 % of the range
    switch (id) {
    case vd::GateOpen: case vd::GateClose: case vd::GateRange: case vd::LevelerTarget: case vd::CompThreshold: case vd::CompKnee: return 1;
    case vd::Ceiling: case vd::GainOffset: return .1;
    case vd::Target: case vd::GrLimit: case vd::LevelerRatio: case vd::CompRatio: return .5;
    default: return 0;
    }
}
double zero(double v) { return std::abs(v) < .05 ? 0.0 : v; }  // no "-0.0" on screen
Gdiplus::Color color(COLORREF c, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

class Editor final : public CPluginView, public IPlugViewContentScaleSupport {
    struct Item {
        HWND h;
        int x, y, w, hgt;
    };
    IPtr<EditController> controller_;
    vd::EditorLink* link_;
    vd::EditorPreferences* preferences_;
    HWND window_ = nullptr, status_ = nullptr, detail_ = nullptr, help_ = nullptr, lastClick_ = nullptr;
    HINSTANCE instance_ = nullptr;
    ULONG_PTR graphics_ = 0;
    HFONT font_ = nullptr, smallFont_ = nullptr, sectionFont_ = nullptr, titleFont_ = nullptr, valueFont_ = nullptr, bigFont_ = nullptr;
    HBRUSH paper_ = nullptr, background_ = nullptr;
    std::vector<Item> items_;
    std::array<HWND, vd::ParamCount> sliders_{}, edits_{};
    std::array<bool, vd::ParamCount> dirty_{};
    vd::MeterFrame meter_{};
    vd::FileStatus file_;
    std::u16string note_;
    static constexpr int HistoryLength = 200;
    struct Sample {
        float gate = 0, comp = 0, limiter = 0;
    };
    std::array<Sample, HistoryLength> history_{};
    int historyHead_ = 0;
    ULONGLONG meterTime_ = 0, lastClickTime_ = 0, historyTime_ = 0;
    POINT lastClickPoint_{};
    float scale_ = 1, dpi_ = 1, zoom_ = 1, pendingDpi_ = 1, pendingZoom_ = 1;
    bool refreshing_ = false, closing_ = false, haveMeter_ = false, dragging_ = false, dragFine_ = false, resizing_ = false, wasLive_ = false;
    double dragAnchorValue_ = 0;
    int gesture_ = -1, dragAnchorX_ = 0, dragAnchorY_ = 0;
    uint64_t lastGeneration_ = 0;
    std::wstring shownButtons_;

    int px(int x) const { return int(std::lround(x * scale_)); }
    double value(uint32_t id) const { return controller_->getParamNormalized(id); }
    bool live() const { return meterTime_ && GetTickCount64() - meterTime_ < 600; }
    int learnState() const { return haveMeter_ ? int(meter_[vd::MLearnState]) : int(vd::Learner::Idle); }
    bool learning() const { return learnState() == vd::Learner::Armed || learnState() == vd::Learner::Capturing; }
    bool computing() const { return learnState() == vd::Learner::Computing; }
    void text(HWND h, const wchar_t* s) {
        if (!h) return;
        wchar_t old[512]{};
        GetWindowTextW(h, old, 512);
        if (std::wcscmp(old, s)) SetWindowTextW(h, s);
    }
    void hint(const wchar_t* s) { text(help_, s); }
    void finish() {
        if (gesture_ >= 0) {
            controller_->endEdit(gesture_);
            gesture_ = -1;
        }
    }
    void edit(uint32_t id, double v, bool hold = false) {
        v = vd::normalized(v);
        if (v == value(id)) {
            if (!hold) finish();
            return;
        }
        if (gesture_ != int(id)) {
            finish();
            controller_->beginEdit(id);
            gesture_ = int(id);
        }
        controller_->setParamNormalized(id, v);
        controller_->performEdit(id, v);
        if (!hold) finish();
        if (*help(id)) hint(help(id));
        refresh();
    }
    void commit(uint32_t id) {
        if (closing_ || refreshing_ || !dirty_[id]) return;
        dirty_[id] = false;
        wchar_t s[64]{};
        GetWindowTextW(edits_[id], s, 64);
        const auto& spec = vd::specs[id];
        if ((id == vd::GateHpf && (!std::wcscmp(s, L"オフ") || !std::wcscmp(s, L"off"))) || (id == vd::GateRange && !std::wcscmp(s, L"ミュート"))) {
            edit(id, 0);
            refresh(true);
            return;
        }
        wchar_t* end = nullptr;
        const double v = std::wcstod(s, &end);
        while (end && std::iswspace(*end)) ++end;
        if (end != s && end && !*end && std::isfinite(v) && v >= spec.lo && v <= spec.hi) edit(id, vd::norm(id, v));
        else {
            wchar_t message[200];
            std::swprintf(message, 200, L"%sは %g ～ %g %s で入力してください。入力を取り消し、元の値に戻しました。", label(id), spec.lo, spec.hi, unit(id));
            hint(message);
        }
        refresh(true);
    }
    // Knob drag: up or right turns it up; 180 px (scaled) cover the whole range, Shift is ten times finer.
    void position(HWND h, uint32_t id, int x, int y) {
        const double span = std::max(1, px(180));
        const bool fine = GetKeyState(VK_SHIFT) < 0;
        if (fine != dragFine_) {
            dragFine_ = fine;
            dragAnchorX_ = x;
            dragAnchorY_ = y;
            dragAnchorValue_ = value(id);
            return;
        }
        edit(id, dragAnchorValue_ + (fine ? .1 : 1.) * double((x - dragAnchorX_) - (y - dragAnchorY_)) / span, true);
        dragAnchorX_ = x;
        dragAnchorY_ = y;
        dragAnchorValue_ = value(id);
        InvalidateRect(h, nullptr, FALSE);
    }
    void nudge(uint32_t id, double direction, bool fine, bool page) {
        const double step = stepFor(id);
        const double factor = (fine ? .1 : 1.) * (page ? 10. : 1.);
        if (step > 0) edit(id, vd::norm(id, vd::plain(id, value(id)) + direction * step * factor));
        else edit(id, value(id) + direction * .01 * factor);
    }
    static LRESULT CALLBACK childProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR idRef, DWORD_PTR data) {
        auto* s = reinterpret_cast<Editor*>(data);
        const int control = GetDlgCtrlID(h);
        const bool slider = control >= SliderBase && control < SliderBase + int(vd::ParamCount);
        const bool editBox = control >= EditBase && control < EditBase + int(vd::ParamCount);
        const uint32_t id = uint32_t(idRef);
        if (m == WM_SETFOCUS && (slider || editBox) && *help(id)) s->hint(help(id));
        if (slider && (m == WM_SETFOCUS || m == WM_KILLFOCUS)) InvalidateRect(h, nullptr, FALSE);
        if (m == WM_GETDLGCODE) {
            const auto* key = reinterpret_cast<MSG*>(l);
            return DefSubclassProc(h, m, w, l) | DLGC_WANTTAB | DLGC_WANTARROWS | ((key && editBox && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE)) ? DLGC_WANTMESSAGE : 0);
        }
        if (m == WM_KEYDOWN && w == VK_TAB) {
            if (HWND next = GetNextDlgTabItem(s->window_, h, GetKeyState(VK_SHIFT) < 0)) SetFocus(next);
            return 0;
        }
        // Space belongs to the host's transport: a focused button must not fire on it (it used to start learning).
        if (!editBox && (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) && w == VK_SPACE) {
            if (m != WM_CHAR) s->toHost(m, w, l);
            return 0;
        }
        if (editBox && m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
            if (w == VK_RETURN) s->commit(id);
            else {
                s->dirty_[id] = false;
                s->refresh(true);
                s->hint(L"数値の入力を取り消しました。");
            }
            SetFocus(s->sliders_[id]);
            return 0;
        }
        if (slider) {
            if (m == WM_PAINT || m == WM_PRINTCLIENT) {
                s->paintSlider(h, id, m == WM_PRINTCLIENT ? reinterpret_cast<HDC>(w) : nullptr);
                return 0;
            }
            if (m == WM_ERASEBKGND) return 1;
            if (m == WM_LBUTTONDOWN) {
                SetFocus(h);
                const auto now = GetTickCount64();
                const POINT at{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                // Trackbars do not emit WM_LBUTTONDBLCLK; detect double clicks locally.
                if (s->lastClick_ == h && s->lastClickTime_ && now - s->lastClickTime_ <= GetDoubleClickTime() &&
                    std::abs(at.x - s->lastClickPoint_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 && std::abs(at.y - s->lastClickPoint_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2) {
                    s->lastClickTime_ = 0;
                    s->finish();
                    s->edit(id, vd::norm(id, vd::specs[id].def));
                    return 0;
                }
                s->lastClick_ = h;
                s->lastClickTime_ = now;
                s->lastClickPoint_ = at;
                SetCapture(h);
                s->dragging_ = true;
                s->dragFine_ = GetKeyState(VK_SHIFT) < 0;
                // A knob never jumps to the click; it turns with the drag.
                s->dragAnchorX_ = at.x;
                s->dragAnchorY_ = at.y;
                s->dragAnchorValue_ = s->value(id);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (m == WM_MOUSEMOVE && s->dragging_ && GetCapture() == h) {
                if (std::abs(GET_X_LPARAM(l) - s->lastClickPoint_.x) > GetSystemMetrics(SM_CXDOUBLECLK) / 2 ||
                    std::abs(GET_Y_LPARAM(l) - s->lastClickPoint_.y) > GetSystemMetrics(SM_CYDOUBLECLK) / 2)
                    s->lastClickTime_ = 0;
                s->position(h, id, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                return 0;
            }
            if (m == WM_LBUTTONUP && GetCapture() == h) {
                s->position(h, id, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                s->dragging_ = false;
                ReleaseCapture();
                s->finish();
                return 0;
            }
            if (m == WM_CAPTURECHANGED || m == WM_CANCELMODE) {
                if (s->dragging_) s->lastClickTime_ = 0;
                s->dragging_ = false;
                s->finish();
                return 0;
            }
            if (m == WM_LBUTTONDBLCLK) {
                s->dragging_ = false;
                if (GetCapture() == h) ReleaseCapture();
                s->finish();
                s->edit(id, vd::norm(id, vd::specs[id].def));
                return 0;
            }
            if (m == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT)) {
                if (w == VK_HOME) s->edit(id, 0.);
                else if (w == VK_END) s->edit(id, 1.);
                else s->nudge(id, (w == VK_LEFT || w == VK_DOWN || w == VK_NEXT) ? -1. : 1., GetKeyState(VK_SHIFT) < 0, w == VK_PRIOR || w == VK_NEXT);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (m == WM_MOUSEWHEEL) {
                if (GetFocus() == h) s->nudge(id, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1. : -1., GetKeyState(VK_SHIFT) < 0, false);
                return 0;
            }
        }
        return DefSubclassProc(h, m, w, l);
    }
    void toHost(UINT m, WPARAM w, LPARAM l) {
        if (HWND parent = window_ ? GetParent(window_) : nullptr) PostMessageW(parent, m, w, l);
    }
    HWND add(const wchar_t* cls, const wchar_t* title, DWORD style, int x, int y, int w, int h, int id, UINT_PTR ref = 0) {
        HWND child = CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), window_, reinterpret_cast<HMENU>(INT_PTR(id)), instance_, nullptr);
        if (child) {
            items_.push_back({child, x, y, w, h});
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SetWindowSubclass(child, childProc, ref, reinterpret_cast<DWORD_PTR>(this));
        }
        return child;
    }
    void fonts() {
        auto make = [this](HFONT& f, int size, int weight, const wchar_t* face) {
            if (f) DeleteObject(f);
            f = CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        };
        make(font_, 15, FW_NORMAL, L"Yu Gothic UI");
        make(smallFont_, 13, FW_NORMAL, L"Yu Gothic UI");
        make(sectionFont_, 16, FW_SEMIBOLD, L"Yu Gothic UI");
        make(titleFont_, 24, FW_SEMIBOLD, L"Segoe UI");
        make(valueFont_, 20, FW_SEMIBOLD, L"Segoe UI");
        make(bigFont_, 34, FW_SEMIBOLD, L"Segoe UI");
        for (const auto& i : items_) {
            const int id = GetDlgCtrlID(i.h);
            SendMessageW(i.h, WM_SETFONT, reinterpret_cast<WPARAM>(id == DetailText || id == HintText ? smallFont_ : font_), TRUE);
        }
    }
    void control(uint32_t id, int x, int y) {
        edits_[id] = add(L"EDIT", L"", ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP, x + 22, y + 48, 54, 20, EditBase + int(id), id);
        sliders_[id] = add(TRACKBAR_CLASSW, label(id), TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + (CellWidth - KnobWidth) / 2, y, KnobWidth, KnobHeight, SliderBase + int(id), id);
        SendMessageW(sliders_[id], TBM_SETRANGE, TRUE, MAKELONG(0, 10000));
    }
    bool build() {
        fonts();
        add(L"BUTTON", L"バイパス", BS_OWNERDRAW | WS_TABSTOP, 716, 14, 100, 26, ToggleBase + int(vd::Bypass));
        add(L"BUTTON", L"表示 100%", BS_OWNERDRAW | WS_TABSTOP, 822, 13, 90, 28, ZoomButton);
        // Target panel: the target, the learning buttons and the result.
        add(L"BUTTON", L"1ch", BS_OWNERDRAW | WS_TABSTOP, 76, 176, 40, 22, MonoButton + 0);
        add(L"BUTTON", L"デュアル", BS_OWNERDRAW | WS_TABSTOP, 120, 176, 60, 22, MonoButton + 1);
        add(L"BUTTON", L"学習開始", BS_OWNERDRAW | WS_TABSTOP, 196, 62, 128, 34, LearnButton);
        add(L"BUTTON", L"取り消し", BS_OWNERDRAW | WS_TABSTOP, 330, 62, 76, 34, CancelButton);
        add(L"BUTTON", L"停止で自動終了", BS_OWNERDRAW | WS_TABSTOP, 412, 66, 134, 26, ToggleBase + int(vd::LearnAutoStop));
        status_ = add(L"STATIC", L"", SS_LEFT, 196, 104, 344, 24, StatusText);
        detail_ = add(L"STATIC", L"", SS_LEFT, 196, 130, 344, 70, DetailText);
        add(L"BUTTON", L"WAVで解析…（補助）", BS_OWNERDRAW | WS_TABSTOP, 562, 172, 150, 22, FileButton);
        add(L"BUTTON", L"結果を消す", BS_OWNERDRAW | WS_TABSTOP, 718, 172, 96, 22, ClearButton);
        // Section switches (check boxes, as in Audition)
        add(L"BUTTON", L"ゲート", BS_OWNERDRAW | WS_TABSTOP, Column[0] + 12, 220, 150, 24, ToggleBase + int(vd::GateOn));
        add(L"BUTTON", L"コンプレッサー", BS_OWNERDRAW | WS_TABSTOP, Column[1] + 12, 220, 150, 24, ToggleBase + int(vd::CompOn));
        add(L"BUTTON", L"補正を学習で自動に決める", BS_OWNERDRAW | WS_TABSTOP, Left(1), 564, 250, 24, ToggleBase + int(vd::AutoOn));
        add(L"BUTTON", L"レベラー", BS_OWNERDRAW | WS_TABSTOP, Column[2] + 12, 220, 150, 24, ToggleBase + int(vd::LevelerOn));
        add(L"BUTTON", L"リミッター", BS_OWNERDRAW | WS_TABSTOP, Column[2] + 12, LimiterTop, 150, 24, ToggleBase + int(vd::LimiterOn));
        add(L"BUTTON", L"測り直し", BS_OWNERDRAW | WS_TABSTOP, 812, MeterTop + 4, 92, 22, ResetButton);
        for (const auto& p : Sliders) control(p.id, p.x, p.y);
        help_ = add(L"STATIC", L"トラックにインサートして［学習開始］を押し、先頭から最後まで再生してから［学習終了］を押します。", SS_LEFT, 12, 754, 896, 22, HintText);
        SendMessageW(help_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        SendMessageW(detail_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        DragAcceptFiles(window_, TRUE);
        SetTimer(window_, 1, 33, nullptr);
        refresh(true);
        return status_ && detail_ && help_;
    }
    static std::wstring flagsText(uint32_t flags) {
        std::wstring s;
        auto add = [&](uint32_t bit, const wchar_t* t) {
            if (flags & bit) s += (s.empty() ? L"" : L"\n") + std::wstring(L"・") + t;
        };
        add(vd::LfStartedMidway, L"途中から再生しました。先頭から学習し直してください。");
        add(vd::LfBackJump, L"巻き戻し（ループ）を検出しました。");
        add(vd::LfForwardJump, L"早送り（シーク）を検出しました。");
        add(vd::LfDropped, L"音を取りこぼしました（処理が追いつきません）。");
        add(vd::LfSettingsChanged, L"学習中にゲート/レベラー/コンプの設定を変えました。");
        add(vd::LfRateChanged, L"サンプルレートが変わりました。");
        add(vd::LfNoTransport, L"DAWが再生位置を教えないため、巻き戻しなどは検出できません。");
        add(vd::LfTruncated, L"長すぎるため後半を記録できませんでした。");
        return s;
    }
    struct Verdict {
        const wchar_t* title;
        COLORREF c;
    };
    // Result card: what the learned result says about the target.
    Verdict verdict() const {
        if (!haveMeter_ || meter_[vd::MRecValid] < .5) return {L"未学習", Muted};
        if (meter_[vd::MRecStale] > .5) return {L"設定が変わりました", Warn};
        if (meter_[vd::MRecUnreached] > .5 || ((uint32_t(meter_[vd::MRecWarnings]) & vd::WarnUnreached) != 0)) return {L"目標に届かない", Bad};
        if (meter_[vd::MRecFlags] != 0 || ((uint32_t(meter_[vd::MRecWarnings]) & (vd::WarnTruePeak | vd::WarnShort | vd::WarnLimiterOff)) != 0)) return {L"完了（注意あり）", Warn};
        return {L"完了", Good};
    }
    void refresh(bool force = false) {
        if (!window_ || refreshing_) return;
        refreshing_ = true;
        vd::MeterFrame f{};
        bool updated = false;
        if (link_ && link_->pollMeter(f) && vd::validMeter(f)) {
            if (!haveMeter_ || std::memcmp(&f, &meter_, sizeof(f)) != 0) {
                updated = true;
                if (f[vd::MGeneration] != meter_[vd::MGeneration] && haveMeter_) {
                    meterTime_ = GetTickCount64();
                    pushHistory(f);
                }
                meter_ = f;
            }
            haveMeter_ = true;
        }
        if (link_) {
            note_ = link_->note();
            file_ = link_->fileStatus();
        }
        for (uint32_t id = 0; id < vd::ParamCount; ++id) {
            if (sliders_[id] && gesture_ != int(id)) {
                const int pos = int(std::lround(value(id) * 10000));
                if (SendMessageW(sliders_[id], TBM_GETPOS, 0, 0) != pos) {
                    SendMessageW(sliders_[id], TBM_SETPOS, TRUE, pos);
                    InvalidateRect(sliders_[id], nullptr, FALSE);
                }
            }
            if (edits_[id] && (force || !(GetFocus() == edits_[id] && dirty_[id]))) {
                wchar_t s[40];
                format(id, value(id), s, 40);
                text(edits_[id], s);
                dirty_[id] = false;
            }
        }
        // Buttons repaint only when what they show changes.
        wchar_t state[200];
        std::swprintf(state, 200, L"%d %d %d %d %d %d %d %d %d %d %d %d %d", learnState(), int(meter_[vd::MRecValid]), file_.running ? 1 : 0, int(std::lround(zoom_ * 100)),
                      int(vd::index(vd::GateOn, value(vd::GateOn))), int(vd::index(vd::CompOn, value(vd::CompOn))), int(vd::index(vd::LevelerOn, value(vd::LevelerOn))),
                      int(vd::index(vd::LimiterOn, value(vd::LimiterOn))), int(vd::index(vd::AutoOn, value(vd::AutoOn))), int(vd::index(vd::Bypass, value(vd::Bypass))),
                      int(vd::index(vd::LearnAutoStop, value(vd::LearnAutoStop))), int(vd::index(vd::MonoMode, value(vd::MonoMode))), int(meter_[vd::MRecStale]));
        if (force || shownButtons_ != state) {
            shownButtons_ = state;
            for (const auto& i : items_)
                if (GetDlgCtrlID(i.h) >= ToggleBase && GetDlgCtrlID(i.h) < StatusText) InvalidateRect(i.h, nullptr, FALSE);
            updated = true;
        }
        const bool isLive = live();
        wchar_t title[40];
        std::swprintf(title, 40, learning() ? L"学習終了" : L"学習開始");
        text(GetDlgItem(window_, LearnButton), title);
        std::swprintf(title, 40, L"表示 %.0f%%", zoom_ * 100);
        text(GetDlgItem(window_, ZoomButton), title);
        statusTexts();
        if (!learning() && !computing() && help_) {
            wchar_t shown[200]{};
            GetWindowTextW(help_, shown, 200);
            if (!std::wcscmp(shown, L"学習を終えました。補正ゲインを計算しています。")) hint(L"補正ゲインが決まりました。目標のラウドネスに合わせて処理しています。");
        }
        if (updated || isLive != wasLive_ || force) InvalidateRect(window_, nullptr, FALSE);  // the child controls are clipped out
        wasLive_ = isLive;
        refreshing_ = false;
    }
    void pushHistory(const vd::MeterFrame& f) {
        historyHead_ = (historyHead_ + 1) % HistoryLength;
        history_[size_t(historyHead_)] = {float(f[vd::MGateDb]), float(f[vd::MCompDb]), float(f[vd::MLimiterDb])};
    }
    void statusTexts() {
        const wchar_t* status = L"";
        wchar_t detail[600]{};
        const auto st = learnState();
        const uint32_t flags = haveMeter_ ? uint32_t(meter_[vd::MLearnFlags]) : 0;
        std::wstring more;
        if (file_.running) {
            status = L"WAVを解析しています";
            std::swprintf(detail, 600, L"%ls　%.0f %%", reinterpret_cast<const wchar_t*>(file_.name.c_str()), file_.progress * 100);
        } else if (value(vd::Bypass) >= .5) {
            status = L"DAWのバイパスが有効です";
            std::swprintf(detail, 600, L"原音をそのまま出力しています（遅延は維持）。");
        } else if (!haveMeter_) {
            status = L"再生を待っています";
            std::swprintf(detail, 600, L"DAWで再生すると、レベルとゲイン変化を表示します。");
        } else if (st == vd::Learner::Armed) {
            status = L"学習待機中";
            std::swprintf(detail, 600, L"トラックの先頭から再生してください（REAPERは書き出しやドライランで高速に流せます）。最後まで再生したら［学習終了］を押します。");
        } else if (st == vd::Learner::Capturing) {
            const double t = meter_[vd::MLearnSeconds];
            wchar_t s[80];
            std::swprintf(s, 80, L"学習中 %d:%02d.%d", int(t) / 60, int(t) % 60, int(t * 10) % 10);
            more = s;
            status = more.c_str();
            const std::wstring warnings = flagsText(flags);
            std::swprintf(detail, 600, L"再生中の音を記録しています。最後まで再生したら［学習終了］を押します。%ls%ls", warnings.empty() ? L"" : L"\n", warnings.c_str());
        } else if (st == vd::Learner::Computing) {
            wchar_t s[80];
            std::swprintf(s, 80, L"補正ゲインを計算中 %.0f %%", meter_[vd::MLearnProgress] * 100);
            more = s;
            status = more.c_str();
            std::swprintf(detail, 600, L"記録した音にゲインとリミッターを当てて、目標に合うまで測り直しています。");
        } else if (st == vd::Learner::Failed) {
            status = L"学習できませんでした";
            const std::wstring warnings = flagsText(flags);
            std::swprintf(detail, 600, L"%ls%ls%ls", reinterpret_cast<const wchar_t*>(note_.c_str()), warnings.empty() ? L"" : L"\n", warnings.c_str());
        } else {
            const auto v = verdict();
            status = v.title;
            std::wstring warnings = flagsText(uint32_t(meter_[vd::MRecFlags]));
            if (meter_[vd::MRecValid] > .5) {
                const uint32_t w = uint32_t(meter_[vd::MRecWarnings]);
                auto add = [&](uint32_t bit, const wchar_t* t) {
                    if (w & bit) warnings += (warnings.empty() ? L"" : L"\n") + std::wstring(L"・") + t;
                };
                add(vd::WarnUnreached, L"リミッターの上限のため、目標のラウドネスに届きません。上限を上げるか、目標を下げてください。");
                add(vd::WarnTruePeak, L"出力のTrue Peakが上限を超えています。");
                add(vd::WarnShort, L"3秒未満の素材です。ラウドネスが安定しません。");
                add(vd::WarnLimiterOff, L"リミッターがオフでピークが上限を超えます。");
                if (meter_[vd::MRecStale] > .5) warnings += (warnings.empty() ? L"" : L"\n") + std::wstring(L"・設定が変わりました。目標とリミッターだけの変更なら、記録した音で自動的に計算し直します。ゲート・レベラー・コンプを変えたときは学習し直してください。");
                if (live() && -meter_[vd::MLimiterDb] > vd::plain(vd::GrLimit, value(vd::GrLimit)))
                    warnings += (warnings.empty() ? L"" : L"\n") + std::wstring(L"・リミッターが深く効いています。補正ゲインが大きすぎる可能性があります。");
                if (warnings.empty()) warnings = L"補正ゲインを使って処理しています。";
                if (meter_[vd::MRecSource] == vd::SourceFile) warnings = L"WAVの解析結果です。\n" + warnings;
            } else {
                warnings = L"［学習開始］を押して、トラックを先頭から最後まで再生してください。";
            }
            std::swprintf(detail, 600, L"%ls", warnings.c_str());
            if (!file_.message.empty() && file_.done && !file_.ok) std::swprintf(detail, 600, L"WAV: %ls", reinterpret_cast<const wchar_t*>(file_.message.c_str()));
        }
        text(status_, status);
        text(detail_, detail);
    }
    void drawText(HDC dc, const wchar_t* s, int x, int y, int w, int h, COLORREF c, HFONT font = nullptr, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
        auto old = SelectObject(dc, font ? font : font_);
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        RECT r{px(x), px(y), px(x + w), px(y + h)};
        DrawTextW(dc, s, -1, &r, flags);
        SelectObject(dc, old);
    }
    void round(Gdiplus::Graphics& g, float x, float y, float w, float h, float radius, COLORREF c, COLORREF border = CLR_INVALID) {
        x *= scale_, y *= scale_, w *= scale_, h *= scale_, radius *= scale_;
        const float d = radius * 2;
        Gdiplus::GraphicsPath p;
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
        Gdiplus::SolidBrush b(color(c));
        g.FillPath(&b, &p);
        if (border != CLR_INVALID) {
            Gdiplus::Pen pen(color(border), scale_);
            g.DrawPath(&pen, &p);
        }
    }
    void line(Gdiplus::Graphics& g, float x1, float y1, float x2, float y2, COLORREF c, float width = 1, BYTE alpha = 255) {
        Gdiplus::Pen p(color(c, alpha), width * scale_);
        g.DrawLine(&p, x1 * scale_, y1 * scale_, x2 * scale_, y2 * scale_);
    }
    void paintSlider(HWND h, uint32_t id, HDC supplied) {
        PAINTSTRUCT ps{};
        HDC target = supplied ? supplied : BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, r.right, r.bottom);
        auto old = SelectObject(dc, bitmap);
        FillRect(dc, &r, paper_);
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            // A 270 degree knob: minimum at the lower left, maximum at the lower right (GDI+ angles run clockwise from 3 o'clock).
            const float cx = r.right / 2.f, cy = r.bottom / 2.f + scale_, radius = 19 * scale_;
            const float v = float(value(id));
            const bool bipolar = id == vd::GainOffset;
            const float start = 135, at = start + 270 * v, from = bipolar ? 270.f : start;
            const Gdiplus::RectF arc(cx - radius, cy - radius, 2 * radius, 2 * radius);
            Gdiplus::Pen track(color(NightGrid), 3 * scale_), lit(color(Accent), 3 * scale_);
            g.DrawArc(&track, arc, start, 270);
            if (std::abs(at - from) > .2f) g.DrawArc(&lit, arc, std::min(from, at), std::abs(at - from));
            const float face = radius - 5 * scale_;
            Gdiplus::SolidBrush fill(color(Face));
            Gdiplus::Pen rim(color(GetFocus() == h ? Accent : Rim), scale_);
            g.FillEllipse(&fill, cx - face, cy - face, 2 * face, 2 * face);
            g.DrawEllipse(&rim, cx - face, cy - face, 2 * face, 2 * face);
            const double a = at * 3.14159265358979 / 180;
            Gdiplus::Pen pointer(color(Ink), 2 * scale_);
            pointer.SetStartCap(Gdiplus::LineCapRound);
            pointer.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pointer, cx + float(std::cos(a)) * face * .25f, cy + float(std::sin(a)) * face * .25f, cx + float(std::cos(a)) * (face - 2 * scale_),
                       cy + float(std::sin(a)) * (face - 2 * scale_));
        }
        BitBlt(target, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (!supplied) EndPaint(h, &ps);
    }
    bool selected(int id) const {
        if (id >= ToggleBase && id < ToggleBase + int(vd::ParamCount)) return vd::index(uint32_t(id - ToggleBase), value(uint32_t(id - ToggleBase))) == 1;
        if (id == MonoButton || id == MonoButton + 1) return vd::index(vd::MonoMode, value(vd::MonoMode)) == id - MonoButton;
        if (id == LearnButton) return learning();
        return false;
    }
    bool enabled(int id) const {
        if (id == LearnButton) return !computing() && !file_.running;
        if (id == CancelButton) return learning() || computing() || learnState() == vd::Learner::Failed;
        if (id == FileButton) return !file_.running && !learning() && !computing();
        if (id == ClearButton) return haveMeter_ && meter_[vd::MRecValid] > .5 && !learning() && !computing();
        return true;
    }
    static bool checkBox(int id) { return id >= ToggleBase && id < ToggleBase + int(vd::ParamCount); }
    COLORREF behind(int id) const {
        if (id == ZoomButton || id == ToggleBase + int(vd::Bypass)) return Background;
        if (id == FileButton || id == ClearButton || id == ResetButton) return Night;
        return Paper;
    }
    void paintButton(const DRAWITEMSTRUCT& d) {
        const int id = int(d.CtlID);
        const bool on = selected(id), down = (d.itemState & ODS_SELECTED) != 0, usable = enabled(id);
        RECT r = d.rcItem;
        HBRUSH b = CreateSolidBrush(behind(id));
        FillRect(d.hDC, &r, b);
        DeleteObject(b);
        wchar_t title[64];
        GetWindowTextW(d.hwndItem, title, 64);
        if (checkBox(id)) {
            // A check box: the section switches of Audition's Dynamics.
            {
                Gdiplus::Graphics g(d.hDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float y = (r.bottom / scale_ - 14) / 2;
                round(g, 2, y, 14, 14, 2, on ? Accent : Night, on ? Accent : Rim);
                if (on) {
                    Gdiplus::Pen tick(color(RGB(255, 255, 255)), 2 * scale_);
                    const Gdiplus::PointF pts[]{{5 * scale_, (y + 7) * scale_}, {8 * scale_, (y + 10) * scale_}, {13 * scale_, (y + 4) * scale_}};
                    g.DrawLines(&tick, pts, 3);
                }
            }
            RECT t = r;
            t.left += px(22);
            auto old = SelectObject(d.hDC, id == ToggleBase + int(vd::GateOn) || id == ToggleBase + int(vd::CompOn) || id == ToggleBase + int(vd::LevelerOn) ||
                                               id == ToggleBase + int(vd::LimiterOn)
                                           ? sectionFont_
                                           : font_);
            SetBkMode(d.hDC, TRANSPARENT);
            SetTextColor(d.hDC, usable ? Ink : Disabled);
            DrawTextW(d.hDC, title, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(d.hDC, old);
        } else {
            const bool primary = id == LearnButton;
            COLORREF bg = down ? Line : Face;
            if (on) bg = primary ? Bad : Accent;
            else if (primary && usable) bg = Accent;
            const COLORREF fg = !usable ? Disabled : (on || primary) ? RGB(255, 255, 255) : Ink;
            {
                Gdiplus::Graphics g(d.hDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                round(g, 1, 1, r.right / scale_ - 2, r.bottom / scale_ - 2, 4, bg, on || primary ? bg : Rim);
            }
            auto old = SelectObject(d.hDC, primary ? sectionFont_ : (r.bottom < px(26) ? smallFont_ : font_));
            SetBkMode(d.hDC, TRANSPARENT);
            SetTextColor(d.hDC, fg);
            DrawTextW(d.hDC, title, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(d.hDC, old);
        }
        if (d.itemState & ODS_FOCUS) {
            InflateRect(&r, -px(2), -px(2));
            DrawFocusRect(d.hDC, &r);
        }
    }
    void cellLabel(HDC dc, uint32_t id, int x, int y) {
        drawText(dc, unit(id), x + 78, y + 48, 32, 20, Muted, smallFont_);
        drawText(dc, label(id), x - 6, y + 70, CellWidth + 12, 18, Muted, smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    void paintResult(HDC dc, Gdiplus::Graphics& g) {
        const float x0 = 548, y0 = 60, w = 356, h = 138;
        round(g, x0, y0, w, h, 6, Night);
        const bool valid = haveMeter_ && meter_[vd::MRecValid] > .5;
        const auto v = verdict();
        drawText(dc, L"補正ゲイン（自動）", 562, 64, 160, 20, NightMuted, smallFont_);
        drawText(dc, v.title, 732, 64, 160, 22, v.c == Muted ? NightMuted : v.c, sectionFont_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        wchar_t s[120];
        if (valid) {
            const double total = meter_[vd::MRecGain] + vd::plain(vd::GainOffset, value(vd::GainOffset));
            std::swprintf(s, 120, L"%+.1f dB", meter_[vd::MRecGain]);
            drawText(dc, s, 562, 84, 150, 44, Accent, bigFont_);
            std::swprintf(s, 120, L"微調整込み %+.1f dB", total);
            drawText(dc, s, 562, 130, 150, 18, NightMuted, smallFont_);
            const double target = vd::plain(vd::Target, value(vd::Target));
            std::swprintf(s, 120, L"学習した音  %.1f LUFS", meter_[vd::MRecPre]);
            drawText(dc, s, 718, 90, 180, 18, Ink, smallFont_);
            std::swprintf(s, 120, L"予測 %.2f / 目標 %.1f", meter_[vd::MRecPred], target);
            drawText(dc, s, 718, 110, 180, 18, Ink, smallFont_);
            std::swprintf(s, 120, L"True Peak  %.1f dBTP", meter_[vd::MRecTp]);
            drawText(dc, s, 718, 130, 180, 18, Ink, smallFont_);
            const double sec = meter_[vd::MRecDuration];
            std::swprintf(s, 120, L"削減最大 %.1f dB・%d:%02d・%ls", meter_[vd::MRecMaxGr], int(sec) / 60, int(sec) % 60, meter_[vd::MRecSource] == vd::SourceFile ? L"WAV" : L"再生");
            drawText(dc, s, 718, 150, 180, 18, NightMuted, smallFont_);
        } else {
            drawText(dc, L"まだ学習していません", 562, 92, 330, 28, NightMuted, valueFont_);
            drawText(dc, L"補正は 0 dB のままです。", 562, 126, 330, 20, NightMuted, smallFont_);
        }
    }
    // A small horizontal meter at the top of a section, like the meters of Audition's Dynamics.
    // lo..hi in dB; the bar grows from `origin` to the value.
    void sectionMeter(HDC dc, Gdiplus::Graphics& g, int c, int y, double lo, double hi, double origin, double v, COLORREF fill, const wchar_t* name) {
        const float x = float(Left(c)), w = float(Right(c) + CellWidth - Left(c));
        auto xOf = [&](double db) { return x + w * float((std::clamp(db, lo, hi) - lo) / (hi - lo)); };
        drawText(dc, name, int(x), y, 60, 14, Muted, smallFont_);
        for (int i = 1; i <= 4; ++i) {  // the leftmost value is where the name stands
            const double db = lo + (hi - lo) * i / 4;
            wchar_t t[8];
            std::swprintf(t, 8, L"%.0f", zero(db));
            drawText(dc, t, int(xOf(db)) - 16, y, 32, 14, Muted, smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        round(g, x, float(y + 16), w, 10, 1, Night, Rim);
        if (live() && haveMeter_) {
            const float a = std::min(xOf(origin), xOf(v)), b = std::max(xOf(origin), xOf(v));
            if (b - a > .5f) round(g, a, float(y + 17), b - a, 8, 1, fill);
        }
    }
    void paintMeters(HDC dc, Gdiplus::Graphics& g) {
        const float x0 = 8, y0 = float(MeterTop), w = 904, h = float(MeterBottom - MeterTop);
        const int top = MeterTop;
        round(g, x0, y0, w, h, 6, Night);
        drawText(dc, L"出力のラウドネス", 24, top + 6, 200, 20, NightMuted, smallFont_);
        const double target = vd::plain(vd::Target, value(vd::Target));
        const float bx = 96, bw = 300;
        auto xOf = [&](double lufs) { return bx + bw * float(std::clamp((lufs + 50.) / 50., 0., 1.)); };
        for (double db : {-40., -30., -20., -10.}) {
            line(g, xOf(db), float(top + 26), xOf(db), float(top + 96), NightGrid);
            wchar_t s[8];
            std::swprintf(s, 8, L"%.0f", db);
            drawText(dc, s, int(xOf(db)) - 14, top + 98, 28, 16, NightMuted, smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const bool isLive = live() && haveMeter_;
        const struct {
            const wchar_t* name;
            int field;
        } rows[]{{L"Momentary", vd::MMomentary}, {L"Short-term", vd::MShortTerm}, {L"Integrated", vd::MIntegrated}};
        for (int i = 0; i < 3; ++i) {
            const float y = float(top + 26 + i * 24);
            drawText(dc, rows[i].name, 24, int(y), 70, 20, NightMuted, smallFont_);
            const double lufs = haveMeter_ ? meter_[size_t(rows[i].field)] : vd::NoLevel;
            const bool has = isLive && lufs > -150;
            round(g, bx, y + 4, bw, 12, 2, NightGrid);
            if (has) {
                const bool pass = i == 2 && std::abs(lufs - target) <= 1.0;
                round(g, bx, y + 4, std::max(2.f, xOf(lufs) - bx), 12, 2, i == 2 ? (pass ? Good : Warn) : Accent);
            }
            wchar_t s[32];
            if (has) std::swprintf(s, 32, L"%.1f", lufs);
            else std::swprintf(s, 32, L"--");
            drawText(dc, s, int(bx + bw) + 8, int(y), 56, 20, Ink, smallFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        line(g, xOf(target), float(top + 24), xOf(target), float(top + 96), RGB(255, 214, 90), 1.5f);
        wchar_t s[96];
        std::swprintf(s, 96, L"目標 %.1f LUFS", target);
        drawText(dc, s, int(xOf(target)) - 50, top + 6, 100, 18, RGB(255, 214, 90), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (isLive) {
            const double tp = meter_[vd::MOutTruePeak];
            std::swprintf(s, 96, L"TP %.1f dBTP", tp);
            drawText(dc, s, 24, top + 98, 112, 16, tp > vd::plain(vd::Ceiling, value(vd::Ceiling)) + .05 && value(vd::LimiterOn) > .5 ? LimitColor : Ink, smallFont_);
            std::swprintf(s, 96, L"遅延 %.1f ms", 1000. * meter_[vd::MLatency] / meter_[vd::MRate]);
            drawText(dc, s, 384, top + 6, 100, 18, NightMuted, smallFont_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            drawText(dc, L"再生すると表示します", 24, top + 98, 112, 16, NightMuted, smallFont_);
        }
        // Gain-reduction history
        const float gx = 512, gy = float(top + 26), gw = 280, gh = 82;
        drawText(dc, L"ゲイン変化（約6秒）", int(gx), top + 6, 130, 20, NightMuted, smallFont_);
        drawText(dc, L"━ゲート", int(gx) + 130, top + 6, 56, 20, GateColor, smallFont_);
        drawText(dc, L"━コンプ", int(gx) + 186, top + 6, 56, 20, CompColor, smallFont_);
        round(g, gx, gy, gw, gh, 2, Night, NightGrid);
        constexpr double Range = 24;
        for (double db : {-6., -12., -18.}) {
            const float y = gy + gh * float(-db / Range);
            line(g, gx, y, gx + gw, y, NightGrid);
            wchar_t t[8];
            std::swprintf(t, 8, L"%.0f", db);
            drawText(dc, t, int(gx) - 30, int(y) - 8, 26, 16, NightMuted, smallFont_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        auto trace = [&](float Sample::*member, COLORREF c) {
            Gdiplus::PointF pts[HistoryLength];
            for (int i = 0; i < HistoryLength; ++i) {
                const auto& sample = history_[size_t((historyHead_ + 1 + i) % HistoryLength)];
                const double v = std::clamp(double(sample.*member), -Range, 0.);
                pts[i] = {(gx + gw * float(i) / (HistoryLength - 1)) * scale_, (gy + gh * float(-v / Range)) * scale_};
            }
            Gdiplus::Pen pen(color(c), 1.4f * scale_);
            g.DrawLines(&pen, pts, HistoryLength);
        };
        trace(&Sample::gate, GateColor);
        trace(&Sample::comp, CompColor);
        trace(&Sample::limiter, LimitColor);
        drawText(dc, L"━リミッター", 806, top + 36, 96, 18, LimitColor, smallFont_);
        if (isLive) {
            std::swprintf(s, 96, L"レベラー %+.1f dB", zero(meter_[vd::MLevelerDb]));
            drawText(dc, s, 806, top + 56, 100, 18, NightMuted, smallFont_);
            std::swprintf(s, 96, L"ゲート %.1f", zero(meter_[vd::MGateDb]));
            drawText(dc, s, 806, top + 74, 100, 18, GateColor, smallFont_);
            std::swprintf(s, 96, L"コンプ %.1f", zero(meter_[vd::MCompDb]));
            drawText(dc, s, 806, top + 92, 100, 18, CompColor, smallFont_);
        }
    }
    void paint(HDC supplied = nullptr) {
        PAINTSTRUCT ps{};
        HDC target = supplied ? supplied : BeginPaint(window_, &ps);
        RECT client;
        GetClientRect(window_, &client);
        HDC dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        auto old = SelectObject(dc, bitmap);
        FillRect(dc, &client, background_);
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            drawText(dc, L"VoiceDynamics", 16, 8, 200, 36, Ink, titleFont_);
            drawText(dc, VD_VERSION_W L"（試作）　ゲート・コンプ・リミッター／目標ラウドネス", 196, 18, 500, 22, Muted, smallFont_);
            // Target panel
            round(g, 8, float(PanelTop), 904, float(PanelBottom - PanelTop), 6, Paper, Line);
            drawText(dc, L"目標ラウドネス", 20, 58, 160, 20, Ink, sectionFont_);
            drawText(dc, L"モノラル", 20, 176, 56, 22, Muted, smallFont_);
            paintResult(dc, g);
            // Sections
            for (int c = 0; c < 3; ++c) round(g, float(Column[c]), float(ColumnTop), float(ColumnWidth), float(ColumnBottom - ColumnTop), 6, Paper, Line);
            line(g, float(Column[2] + 12), float(LimiterTop - 8), float(Column[2] + ColumnWidth - 12), float(LimiterTop - 8), Line);
            sectionMeter(dc, g, 1, 250, -24, 0, 0, haveMeter_ ? meter_[vd::MCompDb] : 0, CompColor, L"削減");
            sectionMeter(dc, g, 2, 248, -6, 6, 0, haveMeter_ ? meter_[vd::MLevelerDb] : 0, Accent, L"補正");
            sectionMeter(dc, g, 2, LimiterTop + 30, -12, 0, 0, haveMeter_ ? meter_[vd::MLimiterDb] : 0, LimitColor, L"削減");
            wchar_t s[120];
            if (haveMeter_ && meter_[vd::MRecValid] > .5 && value(vd::AutoOn) >= .5)
                std::swprintf(s, 120, L"補正の合計 %+.1f dB（自動 %+.1f）", zero(meter_[vd::MRecGain] + vd::plain(vd::GainOffset, value(vd::GainOffset))), meter_[vd::MRecGain]);
            else if (value(vd::AutoOn) >= .5) std::swprintf(s, 120, L"学習するまで補正は微調整の値だけです");
            else std::swprintf(s, 120, L"補正は微調整の値だけです（自動はオフ）");
            drawText(dc, s, Left(1), 592, 260, 18, Muted, smallFont_);
            for (const auto& p : Sliders) cellLabel(dc, p.id, p.x, p.y);
            paintMeters(dc, g);
        }
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (!supplied) EndPaint(window_, &ps);
    }
    void chooseFile() {
        wchar_t path[MAX_PATH * 2]{};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = window_;
        ofn.lpstrFilter = L"WAVファイル (*.wav)\0*.wav\0すべてのファイル\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 2;
        ofn.lpstrTitle = L"解析するWAV（書き出したトラックの音）";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        addRef();
        const HWND owner = window_;
        const bool ok = GetOpenFileNameW(&ofn) != 0;
        if (ok && window_ == owner && !closing_) startFile(path);
        release();
    }
    void startFile(const std::wstring& path) {
        if (!link_ || !enabled(FileButton)) {
            hint(L"学習中は使えません。先に学習を終えるか取り消してください。");
            return;
        }
        hint(link_->analyzeFile(path) ? L"WAVを解析しています。結果は学習結果と同じ場所に表示されます。" : L"解析を始められませんでした。");
        refresh(true);
    }
    void command(int id) {
        if (id >= ToggleBase && id < ToggleBase + int(vd::ParamCount)) {
            const uint32_t p = uint32_t(id - ToggleBase);
            edit(p, vd::index(p, value(p)) ? 0. : 1.);
            return;
        }
        if (id == MonoButton || id == MonoButton + 1) {
            edit(vd::MonoMode, vd::norm(vd::MonoMode, id - MonoButton));
            hint(id == MonoButton ? L"モノラルのトラックを1チャンネルとして測ります（初期値）。" : L"モノラルのトラックを左右に同じ音を出す前提（デュアルモノ）で測ります。+3 dB 大きく測ります。");
            return;
        }
        if (!link_) return;
        if (id == LearnButton) {
            if (!enabled(id)) return;
            if (learning()) {
                link_->command(vd::CmdLearnFinish);
                hint(L"学習を終えました。補正ゲインを計算しています。");
            } else {
                link_->command(vd::CmdLearnStart);
                hint(L"学習を始めました。トラックの先頭から最後まで再生してください。");
            }
            refresh(true);
        } else if (id == CancelButton) {
            if (enabled(id)) link_->command(vd::CmdLearnCancel);
            refresh(true);
        } else if (id == ClearButton) {
            if (enabled(id)) link_->command(vd::CmdClearResult);
            hint(L"学習結果を消しました。補正ゲインは 0 dB です。");
            refresh(true);
        } else if (id == ResetButton) {
            link_->command(vd::CmdResetMeter);
            for (auto& s : history_) s = {};
            hint(L"出力のラウドネス（Integrated）とTrue Peakを測り直します。");
        } else if (id == FileButton) {
            chooseFile();
        } else if (id == ZoomButton) {
            menu(ZoomButton);
        } else if (id >= ZoomMenu && id < ZoomMenu + 5) {
            constexpr float levels[]{.75f, .85f, 1.f, 1.15f, 1.25f};
            const auto result = requestScale(dpi_, levels[id - ZoomMenu]);
            hint(result != kResultOk ? L"DAWが表示サイズの変更を受け付けませんでした。現在の表示を維持します。" : L"表示サイズを調整しました。音と設定は変わりません。");
        }
    }
    void menu(int button) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        constexpr int levels[]{75, 85, 100, 115, 125};
        for (int i = 0; i < 5; ++i) {
            wchar_t title[32];
            std::swprintf(title, 32, L"%d%%", levels[i]);
            AppendMenuW(menu, MF_STRING | (std::abs(zoom_ * 100 - levels[i]) < .5 ? MF_CHECKED : 0), ZoomMenu + i, title);
        }
        RECT r;
        GetWindowRect(GetDlgItem(window_, button), &r);
        // The popup runs a nested loop; the host may close the view there.
        addRef();
        const HWND owner = window_;
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, owner, nullptr);
        DestroyMenu(menu);
        if (chosen && window_ == owner && !closing_) command(chosen);
        release();
    }
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* s = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            s = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            s->window_ = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        if (!s) return DefWindowProcW(h, msg, w, l);
        switch (msg) {
        case WM_PAINT: s->paint(); return 0;
        case WM_PRINTCLIENT: s->paint(reinterpret_cast<HDC>(w)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (IsWindowVisible(h)) s->refresh();
            return 0;
        case WM_DRAWITEM: s->paintButton(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_DROPFILES: {
            wchar_t path[MAX_PATH * 2]{};
            const auto drop = reinterpret_cast<HDROP>(w);
            if (DragQueryFileW(drop, 0, path, MAX_PATH * 2)) s->startFile(path);
            DragFinish(drop);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            const bool number = id >= EditBase && id < EditBase + int(vd::ParamCount);
            SetTextColor(reinterpret_cast<HDC>(w), number ? Accent : id == DetailText || id == HintText ? Muted : Ink);
            const bool onBackground = id == HintText;
            SetBkColor(reinterpret_cast<HDC>(w), onBackground ? Background : Paper);
            return reinterpret_cast<LRESULT>(onBackground ? s->background_ : s->paper_);
        }
        case WM_HSCROLL:
            if (!s->refreshing_) {
                const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l)) - SliderBase;
                if (id >= 0 && id < int(vd::ParamCount)) {
                    if (LOWORD(w) == TB_ENDTRACK) s->finish();
                    else s->edit(uint32_t(id), double(SendMessageW(reinterpret_cast<HWND>(l), TBM_GETPOS, 0, 0)) / 10000., LOWORD(w) == TB_THUMBTRACK);
                    return 0;
                }
            }
            break;
        case WM_COMMAND:
            if (!s->refreshing_ && !s->closing_) {
                const int id = LOWORD(w), code = HIWORD(w);
                if (id >= EditBase && id < EditBase + int(vd::ParamCount)) {
                    if (code == EN_CHANGE) s->dirty_[id - EditBase] = true;
                    if (code == EN_KILLFOCUS) s->commit(uint32_t(id - EditBase));
                    return 0;
                }
                if (code == BN_CLICKED) {
                    s->command(id);
                    return 0;
                }
            }
            break;
        case WM_KEYUP:
        case WM_KEYDOWN:
            if (w == VK_SPACE) {
                s->toHost(msg, w, l);
                return 0;
            }
            if (msg == WM_KEYDOWN && w == VK_TAB) {
                if (HWND next = GetNextDlgTabItem(h, nullptr, GetKeyState(VK_SHIFT) < 0)) SetFocus(next);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            KillTimer(h, 1);
            s->finish();
            s->window_ = nullptr;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(h, msg, w, l);
    }
    tresult requestScale(float dpi, float zoom) {
        finish();
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        lastClickTime_ = 0;
        pendingDpi_ = dpi;
        pendingZoom_ = zoom;
        resizing_ = true;
        ViewRect requested(0, 0, int(std::lround(Width * dpi * zoom)), int(std::lround(Height * dpi * zoom)));
        if (!window_ || !plugFrame || (requested.getWidth() == rect.getWidth() && requested.getHeight() == rect.getHeight())) return onSize(&requested);
        // Keep the current size until the host calls onSize.
        const auto result = plugFrame->resizeView(this, &requested);
        if (result != kResultOk) resizing_ = false;
        return result;
    }

public:
    Editor(EditController* c, vd::EditorLink* link, vd::EditorPreferences* preferences) : controller_(c), link_(link), preferences_(preferences) {
        zoom_ = std::clamp(preferences_->zoom, .75f, 1.25f);
        scale_ = zoom_;
        rect = ViewRect(0, 0, px(Width), px(Height));
        background_ = CreateSolidBrush(Background);
        paper_ = CreateSolidBrush(Paper);
        Gdiplus::GdiplusStartupInput startup;
        Gdiplus::GdiplusStartup(&graphics_, &startup, nullptr);
    }
    ~Editor() override {
        removed();
        for (auto f : {font_, smallFont_, sectionFont_, titleFont_, valueFont_, bigFont_})
            if (f) DeleteObject(f);
        DeleteObject(background_);
        DeleteObject(paper_);
        if (graphics_) Gdiplus::GdiplusShutdown(graphics_);
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return type && !std::strcmp(type, kPlatformTypeHWND) ? kResultTrue : kResultFalse; }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (window_ || !graphics_ || !IsWindow(static_cast<HWND>(parent)) || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&proc), &instance_);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = ClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return kResultFalse;
        INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_BAR_CLASSES};
        InitCommonControlsEx(&ic);
        closing_ = false;
        haveMeter_ = wasLive_ = dragging_ = false;
        meterTime_ = lastClickTime_ = 0;
        lastClick_ = nullptr;
        meter_ = {};
        file_ = {};
        history_ = {};
        shownButtons_.clear();
        dirty_.fill(false);
        if (!CreateWindowExW(WS_EX_CONTROLPARENT, ClassName, L"VoiceDynamics", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, rect.getWidth(), rect.getHeight(),
                             static_cast<HWND>(parent), nullptr, instance_, this))
            return kResultFalse;
        if (!build()) {
            removed();
            return kResultFalse;
        }
        return CPluginView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override {
        closing_ = true;
        resizing_ = false;
        finish();
        if (window_) DestroyWindow(window_);
        items_.clear();
        sliders_ = {};
        edits_ = {};
        status_ = detail_ = help_ = nullptr;
        return CPluginView::removed();
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) override {
        if (!r) return kInvalidArgument;
        const float dpi = resizing_ ? pendingDpi_ : dpi_;
        const float wanted = r->getWidth() != rect.getWidth() ? float(r->getWidth()) / Width : float(r->getHeight()) / Height;
        const float s = std::clamp(wanted, dpi * .75f, dpi * 1.25f);
        r->right = r->left + int(std::lround(Width * s));
        r->bottom = r->top + int(std::lround(Height * s));
        return kResultTrue;
    }
    tresult PLUGIN_API onSize(ViewRect* r) override {
        if (!r || r->getWidth() <= 0 || r->getHeight() <= 0) return kInvalidArgument;
        const float dpi = resizing_ ? pendingDpi_ : dpi_, s = float(r->getWidth()) / Width;
        if (s < dpi * .75f - .001f || s > dpi * 1.25f + .001f || std::abs(r->getHeight() - int(std::lround(Height * s))) > 1) return kResultFalse;
        const bool requested = resizing_ && r->getWidth() == int(std::lround(Width * pendingDpi_ * pendingZoom_)) && r->getHeight() == int(std::lround(Height * pendingDpi_ * pendingZoom_));
        finish();
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        scale_ = requested ? pendingDpi_ * pendingZoom_ : s;
        zoom_ = scale_ / dpi;
        dpi_ = dpi;
        resizing_ = false;
        preferences_->zoom = zoom_;
        CPluginView::onSize(r);
        if (window_) {
            fonts();
            for (const auto& i : items_) MoveWindow(i.h, px(i.x), px(i.y), px(i.w), px(i.hgt), TRUE);
            MoveWindow(window_, 0, 0, r->getWidth(), r->getHeight(), TRUE);
            refresh(true);
            InvalidateRect(window_, nullptr, TRUE);
        }
        return kResultTrue;
    }
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor f) override {
        if (!std::isfinite(f) || f < .5f || f > 4.f) return kInvalidArgument;
        return requestScale(f, zoom_);
    }
    OBJ_METHODS(Editor, CPluginView)
    DEFINE_INTERFACES
    DEF_INTERFACE(IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)
};
}  // namespace

IPlugView* createVoiceDynamicsEditor(EditController* c, vd::EditorLink* link, vd::EditorPreferences* preferences) { return new Editor(c, link, preferences); }
void unregisterVoiceDynamicsEditor() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&unregisterVoiceDynamicsEditor), &module);
    if (module) UnregisterClassW(ClassName, module);
}
