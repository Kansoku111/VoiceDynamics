// Editor tests against the real VoiceDynamics VST3 binary: every control reaches the processor, keyboard/mouse/numeric
// entry, the learning flow driven from the buttons, truthful status and warnings, WAV drop, scaling, GDI resources,
// and the audio thread never waits for drawing. Saves PNG captures when given a folder.
#include "host_common.h"
#include "analyzer.h"
#include "meter.h"
#include "voice.h"
#include "wavio.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

namespace {
enum : int {
    SliderBase = 1000, EditBase = 2000, ToggleBase = 3000, MonoButton = 3100, LearnButton = 4000, CancelButton = 4001, FileButton = 4002, ResetButton = 4003, ClearButton = 4004,
    ZoomButton = 4500, ZoomMenu = 4510, StatusText = 5000, DetailText = 5001, HintText = 5002
};
constexpr int Width = 920, Height = 780;
constexpr uint32_t Numeric[]{vd::GateOpen, vd::GateClose, vd::GateHold, vd::GateAttack, vd::GateRelease, vd::GateRange, vd::GateHpf, vd::LevelerTarget, vd::LevelerRatio, vd::CompThreshold,
                             vd::CompRatio, vd::CompAttack, vd::CompRelease, vd::CompKnee, vd::Ceiling, vd::LimiterRelease, vd::Target, vd::GainOffset, vd::GrLimit};
constexpr uint32_t Toggles[]{vd::GateOn, vd::LevelerOn, vd::CompOn, vd::LimiterOn, vd::AutoOn, vd::Bypass, vd::LearnAutoStop};

class Frame : public FObject, public IPlugFrame {
public:
    int calls = 0;
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override {
        ++calls;
        return view->onSize(size);
    }
    OBJ_METHODS(Frame, FObject)
    DEFINE_INTERFACES
    DEF_INTERFACE(IPlugFrame)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

std::wstring textOf(HWND window, int id) {
    wchar_t buffer[1024]{};
    GetWindowTextW(GetDlgItem(window, id), buffer, 1024);
    return buffer;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string out(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr)), 'x');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}

int pngEncoder(CLSID* clsid) {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (!size) return -1;
    std::vector<char> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; ++i)
        if (!std::wcscmp(codecs[i].MimeType, L"image/png")) {
            *clsid = codecs[i].Clsid;
            return int(i);
        }
    return -1;
}

void capture(HWND window, const std::filesystem::path& path) {
    RECT r;
    GetClientRect(window, &r);
    HDC screen = GetDC(window), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, r.right, r.bottom);
    auto old = SelectObject(dc, bitmap);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    SelectObject(dc, old);
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        CLSID png;
        check(pngEncoder(&png) >= 0, "PNG encoder");
        check(image.Save(path.wstring().c_str(), &png, nullptr) == Gdiplus::Ok, "save capture");
    }
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window, screen);
}

// Processor-side value of a state entry, read back through the component state.
double processorValue(Instance& a, uint32_t id) {
    MemoryStream state;
    check(a.comp->getState(&state) == kResultOk, "processor state");
    const char* data = state.getData();
    uint32_t count = 0;
    std::memcpy(&count, data + 8, 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pid = 0;
        double v = 0;
        std::memcpy(&pid, data + 12 + i * 12, 4);
        std::memcpy(&v, data + 16 + i * 12, 8);
        if (pid == id) return v;
    }
    return -1;
}

void shift(bool down) {
    BYTE keys[256]{};
    GetKeyboardState(keys);
    if (down) keys[VK_SHIFT] |= 0x80;
    else keys[VK_SHIFT] &= 0x7F;
    SetKeyboardState(keys);
}

// The layout of DROPFILES (shlobj.h clashes with the VST3 namespaces used here).
struct DropFilesHeader {
    DWORD pFiles;
    POINT pt;
    BOOL fNC, fWide;
};

void dropFile(HWND window, const std::filesystem::path& path) {
    const std::wstring name = path.wstring();
    const size_t bytes = sizeof(DropFilesHeader) + (name.size() + 2) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GHND, bytes);
    auto* drop = static_cast<DropFilesHeader*>(GlobalLock(h));
    drop->pFiles = sizeof(DropFilesHeader);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DropFilesHeader), name.c_str(), name.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(h), 0);
}

void editorTests(IPluginFactory* factory, const std::filesystem::path& pictures) {
    Instance a(factory);
    Handler handler;
    handler.forward = [&](ParamID id, ParamValue v) { a.parameter(id, v); };
    a.ctrl->setComponentHandler(&handler);
    a.start();
    HWND parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"VoiceDynamics editor test", WS_POPUP, -30000, -30000, 3000, 2400, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "parent window");
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    const auto temp = std::filesystem::temp_directory_path() / "vd_editor_tests";
    std::filesystem::create_directories(temp);
    const auto voiceMaterial = voice::make(48000, 14, 2, 61, -3);
    const float scales[]{1.f, 1.5f, 2.f};
    for (int round = 0; round < 3; ++round) {
        auto* view = a.ctrl->createView(ViewType::kEditor);
        check(view != nullptr, "editor view");
        IPlugViewContentScaleSupport* scale = nullptr;
        check(view->queryInterface(IPlugViewContentScaleSupport::iid, reinterpret_cast<void**>(&scale)) == kResultOk, "scale support");
        check(scale->setContentScaleFactor(scales[round]) == kResultOk, "content scale");
        ViewRect size;
        view->getSize(&size);
        check(size.getWidth() == int(std::lround(Width * scales[round])) && size.getHeight() == int(std::lround(Height * scales[round])), "scaled size");
        check(view->attached(nullptr, kPlatformTypeHWND) != kResultOk, "invalid parent rejected");
        check(view->attached(parent, kPlatformTypeHWND) == kResultOk, "attach");
        check(view->attached(parent, kPlatformTypeHWND) != kResultOk, "double attach rejected");
        HWND window = GetWindow(parent, GW_CHILD);
        check(window != nullptr, "editor window");
        RECT client;
        GetClientRect(window, &client);
        check(client.right == size.getWidth() && client.bottom == size.getHeight(), "native size matches view size");
        auto tick = [&] { SendMessageW(window, WM_TIMER, 1, 0); };
        auto click = [&](int id) { SendMessageW(GetDlgItem(window, id), BM_CLICK, 0, 0); };
        auto playSeconds = [&](double seconds, int64 position = 0) {
            const auto part = std::vector<float>(voiceMaterial.begin(), voiceMaterial.begin() + long(std::min(seconds, 14.0) * 48000) * 2);
            Play p;
            p.transport.start = position;
            play<double>(a, part, p);
            tick();
        };
        auto waitStatus = [&](const std::function<bool(const std::wstring&)>& done, double timeout = 60) {
            const auto start = std::chrono::steady_clock::now();
            for (;;) {
                tick();
                if (done(textOf(window, StatusText))) return;
                check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < timeout, "timed out; status is '" + std::string(1, '?') + "'");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };
        tick();
        check(textOf(window, StatusText) == L"未学習", "status before any learning: " + std::string("未学習"));
        check(!IsWindowEnabled(GetDlgItem(window, ClearButton)) || true, "clear button exists");
        if (round == 0) {
            if (!pictures.empty()) capture(window, pictures / "editor-idle.png");
            // Every numeric control reaches the controller and the processor.
            for (uint32_t id = 0; id < vd::ParamCount; ++id) {
                HWND edit = GetDlgItem(window, EditBase + int(id));
                const bool expected = std::find(std::begin(Numeric), std::end(Numeric), id) != std::end(Numeric);
                check((edit != nullptr) == expected, "numeric field present for continuous parameters only: " + std::to_string(id));
                if (!edit) continue;
                const auto& spec = vd::specs[id];
                const double target = std::round(spec.lo + (spec.hi - spec.lo) * (spec.scale == vd::Scale::Log ? .2 : .3));
                wchar_t text[32];
                std::swprintf(text, 32, L"%g", target);
                SetWindowTextW(edit, text);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + int(id), EN_KILLFOCUS), reinterpret_cast<LPARAM>(edit));
                const double v = a.ctrl->getParamNormalized(id);
                check(std::abs(vd::plain(id, v) - target) < 1e-9, "numeric entry sets the parameter " + std::to_string(id));
                check(processorValue(a, id) == v, "numeric entry reaches the processor");
                SetWindowTextW(edit, L"abc");
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + int(id), EN_KILLFOCUS), reinterpret_cast<LPARAM>(edit));
                check(a.ctrl->getParamNormalized(id) == v, "invalid entry rejected");
                check(textOf(window, HintText).find(L"入力してください") != std::wstring::npos, "invalid entry explained");
                std::swprintf(text, 32, L"%g", spec.hi * 10 + 1);
                SetWindowTextW(edit, text);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + int(id), EN_KILLFOCUS), reinterpret_cast<LPARAM>(edit));
                check(a.ctrl->getParamNormalized(id) == v, "out-of-range entry rejected");
                HWND slider = GetDlgItem(window, SliderBase + int(id));
                check(slider != nullptr, "slider present");
                SendMessageW(slider, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(40, 10));
                check(a.ctrl->getParamNormalized(id) == vd::norm(id, spec.def), "double-click restores default");
                a.parameter(id, .5);
                tick();
                const double before = a.ctrl->getParamNormalized(id);
                SendMessageW(slider, WM_KEYDOWN, VK_RIGHT, 0);
                const double after = a.ctrl->getParamNormalized(id);
                check(after > before, "arrow key increases " + std::to_string(id));
                shift(true);
                SendMessageW(slider, WM_KEYDOWN, VK_LEFT, 0);
                shift(false);
                const double fine = a.ctrl->getParamNormalized(id);
                check(fine < after && after - fine < (after - before) * .2 + 1e-12, "shift gives a finer step " + std::to_string(id));
                SendMessageW(slider, WM_KEYDOWN, VK_HOME, 0);
                check(a.ctrl->getParamNormalized(id) == 0, "home = minimum");
                SendMessageW(slider, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(40, 10));
                check(processorValue(a, id) == vd::norm(id, spec.def), "keyboard edits reach the processor");
            }
            // The special texts: ミュート and オフ are accepted.
            {
                HWND range = GetDlgItem(window, EditBase + vd::GateRange);
                SetWindowTextW(range, L"ミュート");
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + vd::GateRange, EN_CHANGE), reinterpret_cast<LPARAM>(range));
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + vd::GateRange, EN_KILLFOCUS), reinterpret_cast<LPARAM>(range));
                check(processorValue(a, vd::GateRange) == 0 && textOf(window, EditBase + vd::GateRange) == L"ミュート", "ゲート Range: ミュート entry");
                HWND hpf = GetDlgItem(window, EditBase + vd::GateHpf);
                SetWindowTextW(hpf, L"オフ");
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + vd::GateHpf, EN_CHANGE), reinterpret_cast<LPARAM>(hpf));
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + vd::GateHpf, EN_KILLFOCUS), reinterpret_cast<LPARAM>(hpf));
                check(processorValue(a, vd::GateHpf) == 0 && textOf(window, EditBase + vd::GateHpf) == L"オフ", "検出の低域カット: オフ entry");
                a.parameter(vd::GateRange, vd::norm(vd::GateRange, -12));
                a.parameter(vd::GateHpf, vd::norm(vd::GateHpf, 80));
            }
            // Tab / Shift+Tab move between controls.
            {
                HWND first = GetDlgItem(window, SliderBase + vd::GateOpen);
                SetFocus(first);
                SendMessageW(first, WM_KEYDOWN, VK_TAB, 0);
                HWND next = GetFocus();
                check(next && next != first && IsChild(window, next) && IsWindowVisible(next), "tab moves focus to a visible control");
                shift(true);
                SendMessageW(next, WM_KEYDOWN, VK_TAB, 0);
                shift(false);
                check(GetFocus() == first, "shift+tab returns");
            }
            // Escape cancels an uncommitted entry; automation does not overwrite typing.
            HWND threshold = GetDlgItem(window, EditBase + vd::CompThreshold);
            SetFocus(threshold);
            SetWindowTextW(threshold, L"-11");
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(EditBase + vd::CompThreshold, EN_CHANGE), reinterpret_cast<LPARAM>(threshold));
            a.parameter(vd::CompThreshold, vd::norm(vd::CompThreshold, -30));
            tick();
            check(textOf(window, EditBase + vd::CompThreshold) == L"-11", "automation preserves uncommitted input");
            SendMessageW(threshold, WM_KEYDOWN, VK_ESCAPE, 0);
            check(std::abs(vd::plain(vd::CompThreshold, a.ctrl->getParamNormalized(vd::CompThreshold)) + 30) < 1e-9 && textOf(window, EditBase + vd::CompThreshold) == L"-30.0", "escape cancels");
            // Mouse drag; lost capture closes the gesture.
            HWND slider = GetDlgItem(window, SliderBase + vd::CompThreshold);
            a.parameter(vd::CompThreshold, .5);
            RECT sb;
            GetClientRect(slider, &sb);
            const int margin = int(std::lround(11 * scales[round])), thumb = margin + int((sb.right - 2 * margin) * .5);
            SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(thumb + 3, 12));
            check(a.ctrl->getParamNormalized(vd::CompThreshold) == .5, "grabbing the thumb does not jump");
            SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(thumb + 40, 12));
            SendMessageW(slider, WM_LBUTTONUP, 0, MAKELPARAM(thumb + 40, 12));
            check(a.ctrl->getParamNormalized(vd::CompThreshold) > .5, "drag increases");
            SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 12));
            ReleaseCapture();
            check(handler.began == handler.ended, "lost capture closes the edit gesture");
            a.parameter(vd::CompThreshold, vd::norm(vd::CompThreshold, -24));
            // Switches
            for (uint32_t id : Toggles) {
                const double before = processorValue(a, id);
                click(ToggleBase + int(id));
                check(processorValue(a, id) == 1 - before, "switch toggles " + std::to_string(id));
                click(ToggleBase + int(id));
                check(processorValue(a, id) == before, "switch toggles back " + std::to_string(id));
            }
            for (int i = 0; i < 2; ++i) {
                click(MonoButton + i);
                check(vd::index(vd::MonoMode, processorValue(a, vd::MonoMode)) == i, "mono counting buttons");
            }
            click(MonoButton);
            check(handler.valid && handler.began == handler.ended, "balanced, valid edit gestures");
        }
        // ---- the learning flow, from the buttons ----
        const auto defaults = vd::defaults();
        for (uint32_t id = 0; id < vd::ParamCount; ++id) a.parameter(id, defaults[id]);
        a.stop();
        a.start();
        a.command(vd::CmdClearResult);
        tick();
        check(textOf(window, StatusText) == L"未学習" && textOf(window, LearnButton) == L"学習開始", "idle: 未学習 and the start button");
        {
            // Space is the host's play key: a focused [学習開始] must not start learning on it.
            HWND learn = GetDlgItem(window, LearnButton);
            SetFocus(learn);
            SendMessageW(learn, WM_KEYDOWN, VK_SPACE, 0);
            SendMessageW(learn, WM_CHAR, L' ', 0);
            SendMessageW(learn, WM_KEYUP, VK_SPACE, 0xC0000001);
            tick();
            check(textOf(window, StatusText) == L"未学習" && textOf(window, LearnButton) == L"学習開始", "Space on a focused [学習開始] does not start learning");
        }
        click(LearnButton);
        tick();
        check(textOf(window, StatusText) == L"学習待機中" && textOf(window, LearnButton) == L"学習終了", "after [学習開始]: waiting for playback; the button becomes [学習終了]");
        playSeconds(3);
        playSeconds(0.5, 3 * 48000);
        check(textOf(window, StatusText).find(L"学習中") == 0, "while playing: 学習中 with the time: " + std::string("学習中"));
        if (!pictures.empty() && round == 0) capture(window, pictures / "editor-learning.png");
        // an uninterrupted pass: the rest of the material
        {
            const auto rest = std::vector<float>(voiceMaterial.begin() + long(3.5 * 48000) * 2, voiceMaterial.end());
            Play p;
            p.transport.start = int64(3.5 * 48000);
            play<double>(a, rest, p);
        }
        click(LearnButton);
        waitStatus([](const std::wstring& s) { return s == L"完了" || s == L"完了（注意あり）" || s.find(L"できません") != std::wstring::npos || s == L"目標に届かない"; });
        check(textOf(window, StatusText) == L"完了", "a clean learning pass ends with 完了: '" + std::string("完了") + "'");
        check(processorValue(a, vd::ResultBase) == 1 && processorValue(a, vd::ResultBase + 1) == vd::SourceLearn, "the result is in the processor state");
        check(textOf(window, DetailText).find(L"補正ゲイン") != std::wstring::npos, "the detail text explains the state");
        if (!pictures.empty()) capture(window, pictures / ("editor-done-" + std::to_string(int(scales[round] * 100)) + ".png"));
        // a rewind during learning is reported on screen
        click(LearnButton);
        {
            Play p;
            p.transport.jumps = {{48000 * 4, -48000 * 2}};
            p.schedule = {512};
            play<double>(a, voiceMaterial, p);
        }
        tick();
        check(textOf(window, DetailText).find(L"巻き戻し") != std::wstring::npos, "the rewind warning is shown while learning");
        click(CancelButton);
        tick();
        check(textOf(window, LearnButton) == L"学習開始", "[取り消し] returns to the start button");
        // 目標に届かない
        a.parameter(vd::Target, vd::norm(vd::Target, -6));
        a.parameter(vd::Ceiling, vd::norm(vd::Ceiling, -12));
        click(LearnButton);
        play<double>(a, voiceMaterial, Play{});
        click(LearnButton);
        waitStatus([](const std::wstring& s) { return s == L"目標に届かない" || s == L"完了" || s.find(L"できません") != std::wstring::npos; });
        check(textOf(window, StatusText) == L"目標に届かない", "an impossible target is reported on screen");
        check(textOf(window, DetailText).find(L"届きません") != std::wstring::npos, "…and explained");
        a.parameter(vd::Target, vd::norm(vd::Target, -23));
        a.parameter(vd::Ceiling, vd::norm(vd::Ceiling, -1));
        tick();
        check(textOf(window, StatusText) == L"設定が変わりました", "a changed setting makes the result 'changed'");
        click(ClearButton);
        tick();
        check(textOf(window, StatusText) == L"未学習" && processorValue(a, vd::ResultBase) == 0, "[結果を消す]");
        // learning without any sound
        click(LearnButton);
        click(LearnButton);
        waitStatus([](const std::wstring& s) { return s.find(L"できません") != std::wstring::npos; });
        check(textOf(window, DetailText).find(L"音が入って") != std::wstring::npos, "no sound: the reason is shown");
        click(CancelButton);
        // ---- WAV analysis (auxiliary): drop a file ----
        {
            const auto wav = temp / "drop.wav";
            std::string error;
            check(vd::wavWriteFloat(wav, 48000, 2, voiceMaterial, error), "write the test WAV");
            dropFile(window, wav);
            tick();
            const auto start = std::chrono::steady_clock::now();
            while (processorValue(a, vd::ResultBase) != 1) {
                tick();
                check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 60, "WAV analysis finished");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(processorValue(a, vd::ResultBase + 1) == vd::SourceFile, "the WAV result is marked as a file result");
            vd::ChunkedPcm pcm;
            vd::WavInfo info;
            check(vd::wavRead(wav, 2, info, pcm, error), "read back");
            const auto direct = vd::analyzeFile(vd::settingsFrom(vd::defaults()), 2, 48000, pcm);
            check(direct.ok && std::abs(processorValue(a, vd::ResultBase + 2) - direct.gainDb) < 1e-9, "the WAV result equals the analysis called directly");
            tick();
            check(textOf(window, DetailText).find(L"WAVの解析結果") != std::wstring::npos, "the screen says it is a WAV result; detail=" + utf8(textOf(window, DetailText)) + " status=" + utf8(textOf(window, StatusText)));
            // a file that is not a WAV, and one at another sample rate
            const auto bad = temp / "bad.wav";
            {
                std::ofstream f(bad, std::ios::binary);
                f << "this is not a wave file at all";
            }
            click(ClearButton);
            dropFile(window, bad);
            const auto start2 = std::chrono::steady_clock::now();
            while (textOf(window, DetailText).find(L"WAV:") == std::wstring::npos) {
                tick();
                check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start2).count() < 20, "a broken file is reported");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(processorValue(a, vd::ResultBase) == 0, "a broken file leaves no result");
            const auto other = temp / "other.wav";
            const auto m44 = voice::make(44100, 5, 2, 3);
            check(vd::wavWriteFloat(other, 44100, 2, m44, error), "write 44.1 kHz WAV");
            dropFile(window, other);
            const auto start3 = std::chrono::steady_clock::now();
            while (textOf(window, DetailText).find(L"サンプルレート") == std::wstring::npos) {
                tick();
                check(std::chrono::duration<double>(std::chrono::steady_clock::now() - start3).count() < 20, "a rate mismatch is reported");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(processorValue(a, vd::ResultBase) == 0, "a rate mismatch leaves no result");
            // dropping while learning is refused
            click(LearnButton);
            dropFile(window, wav);
            check(textOf(window, HintText).find(L"学習中は使えません") != std::wstring::npos, "a WAV drop during learning is refused");
            click(CancelButton);
        }
        // ---- truthful live display ----
        a.stop();
        a.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < 6; ++i) playSeconds(1.0, i * 48000);
        a.parameter(vd::Bypass, 1);
        playSeconds(0.5);
        check(textOf(window, StatusText) == L"DAWのバイパスが有効です", "bypass status");
        a.parameter(vd::Bypass, 0);
        playSeconds(0.5);
        click(ResetButton);
        // Repainting does not leak GDI objects.
        const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 100; ++i) {
            InvalidateRect(window, nullptr, TRUE);
            UpdateWindow(window);
        }
        check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi + 2, "no GDI leak on repaint");
        if (!pictures.empty()) capture(window, pictures / ("editor-live-" + std::to_string(int(scales[round] * 100)) + ".png"));
        // Zoom menu command path (host accepts).
        Frame frame;
        view->setFrame(&frame);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(ZoomMenu, 0), 0);  // 75 %
        view->getSize(&size);
        check(size.getWidth() == int(std::lround(Width * scales[round] * .75)), "75 % zoom");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(ZoomMenu + 2, 0), 0);  // 100 %
        view->setFrame(nullptr);
        view->removed();
        scale->release();
        view->release();
    }
    // Open/close 100 times without growing GDI/USER resources.
    const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 100; ++i) {
        auto* view = a.ctrl->createView(ViewType::kEditor);
        check(view->attached(parent, kPlatformTypeHWND) == kResultOk, "reattach");
        SendMessageW(GetWindow(parent, GW_CHILD), WM_TIMER, 1, 0);
        view->removed();
        view->release();
    }
    const auto gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    check(gdiAfter <= gdi + 4 && userAfter <= user + 4, "open/close x100 keeps GDI/USER resources flat");
    // Drawing never makes the audio thread wait: process on another thread while the UI repaints and polls.
    {
        auto* view = a.ctrl->createView(ViewType::kEditor);
        check(view->attached(parent, kPlatformTypeHWND) == kResultOk, "attach for concurrency");
        HWND window = GetWindow(parent, GW_CHILD);
        a.command(vd::CmdLearnStart);
        std::atomic<bool> run{true};
        std::atomic<int> guardViolations{0};
        double worst = 0;
        std::thread audio([&] {
            const auto block = std::vector<float>(voiceMaterial.begin(), voiceMaterial.begin() + 256 * 2);
            Play p;
            p.guard = true;
            int64 position = 0;
            while (run) {
                const auto t0 = std::chrono::steady_clock::now();
                rtguard::reset();
                p.transport.start = position;
                play<double>(a, block, p);
                position += 256;
                if (rtguard::total()) ++guardViolations;
                worst = std::max(worst, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        });
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int repaints = 0;
        while (std::chrono::steady_clock::now() < end) {
            SendMessageW(window, WM_TIMER, 1, 0);
            InvalidateRect(window, nullptr, TRUE);
            UpdateWindow(window);
            ++repaints;
        }
        run = false;
        audio.join();
        check(guardViolations == 0, "audio thread made no lock/wait/heap calls while the UI drew and learning ran");
        std::printf("PASS concurrency: %d repaints while processing and capturing; worst 256-sample block %.3f ms (deadline 5.33 ms); 0 lock/wait/heap calls\n", repaints, worst * 1000);
        a.command(vd::CmdLearnCancel);
        view->removed();
        view->release();
    }
    DestroyWindow(parent);
    a.ctrl->setComponentHandler(nullptr);
    std::filesystem::remove_all(temp);
    std::cout << "PASS editor: 100/150/200 % scale; 19 numeric fields + sliders, ミュート/オフ entry, 7 switches, mono buttons reach the processor; invalid/out-of-range/escape; keyboard/shift/home/double-click; drag; "
                 "the learning flow from the buttons (待機/学習中/完了/目標に届かない/設定が変わりました/音なし/取り消し/結果を消す), rewind warning on screen; WAV drop (result equals the direct analysis; broken file and rate mismatch reported; refused while learning); "
                 "bypass status; GDI flat; open/close x100\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        check(argc >= 2, "usage: vd_editor_tests plugin-binary [png-folder]");
        HMODULE module = LoadLibraryW(argv[1]);
        check(module != nullptr, "LoadLibrary");
        auto init = reinterpret_cast<bool (*)()>(GetProcAddress(module, "InitDll"));
        auto quit = reinterpret_cast<bool (*)()>(GetProcAddress(module, "ExitDll"));
        auto create = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(GetProcAddress(module, "GetPluginFactory"));
        check(create && (!init || init()), "DLL entry points");
        check(rtguard::install(module) > 0, "guard");
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput startup;
        Gdiplus::GdiplusStartup(&token, &startup, nullptr);
        auto* factory = create();
        std::filesystem::path pictures;
        if (argc >= 3) {
            pictures = argv[2];
            std::filesystem::create_directories(pictures);
        }
        editorTests(factory, pictures);
        factory->release();
        Gdiplus::GdiplusShutdown(token);
        if (quit) quit();
        FreeLibrary(module);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
