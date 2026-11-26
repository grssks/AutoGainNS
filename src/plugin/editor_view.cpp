#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "plugin/editor_view.hpp"
#include "plugin/controller.hpp"
#include "plugin/ids.hpp"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace autogain {

namespace {

constexpr const wchar_t* kWndClassName = L"AutoGainNS_VST3_View";

struct ControlRow {
    ParamID id;
    const wchar_t* label;
    const wchar_t* desc;
    const wchar_t* section; // non-null starts a section header inside the card
    int y;
    bool isToggle;
};

// 540x610 logical layout:
// Header:             0..54
// Preset bar:        62..88
// Input Routing bar: 94..120
// Card 1 (Noise):    128..296
// Card 2 (AutoVol):  304..472
// Card 3 (Protect):  480..574
// Footer tip bar:    580..604
constexpr ControlRow kRows[] = {
    { kParamHpfHz,            L"Low-Cut Rumble Filter",  L"Removes desk bumps, AC hum & wind below cutoff",     L"NOISE & ROOM CLEANUP", 154, false },
    { kParamNsStrength,       L"AI Noise Removal",       L"Suppresses fans, keyboard & background hiss",        nullptr,                 188, false },
    { kParamVadThreshold,     L"Voice Gate Sensitivity", L"Higher = stricter gate; cuts noise between words",   nullptr,                 222, false },
    { kParamGateFloorDb,      L"Silence Reduction",      L"How much background drops when you stop talking",    nullptr,                 256, false },
    { kParamAgcEnable,        L"Auto Volume",            L"Automatically evens out quiet and loud speech",      L"AUTOMATIC VOLUME",     330, true  },
    { kParamTargetLufs,       L"Target Voice Volume",    L"Overall speaking loudness (-16 LUFS = standard)",    nullptr,                 364, false },
    { kParamAgcMaxBoostDb,    L"Max Volume Boost",       L"Maximum extra gain applied to quiet speech",         nullptr,                 398, false },
    { kParamAgcSpeed,         L"Leveling Speed",         L"How fast volume adapts between phrases",             nullptr,                 432, false },
    { kParamLimiterCeilingDb, L"Clip Protection",        L"Stops loud shouts from distorting or clipping",      L"PROTECTION & OUTPUT",  506, false },
    { kParamOutputGainDb,     L"Final Output Trim",      L"Manual volume adjustment after all processing",      nullptr,                 540, false },
};

constexpr RECT kBypassRect = { 370, 13, 452, 39 };
constexpr RECT kResetRect  = { 460, 13, 524, 39 };

constexpr RECT kPresetRects[kNumPresets] = {
    { 88,  62, 174, 88 },
    { 182, 62, 290, 88 },
    { 298, 62, 407, 88 },
    { 415, 62, 524, 88 },
};

struct InputModeBtn {
    const wchar_t* name;
    const wchar_t* tip;
    RECT rect;
};

constexpr InputModeBtn kInputModes[] = {
    {
        L"Auto",
        L"Input Auto: Matches host channel layout (recommended for standard 1-ch USB mics)",
        { 88, 94, 174, 120 }
    },
    {
        L"L+R Mono",
        L"L+R Mono Sum: Mixes stereo input equally into mono (fixes phase/balance mismatch)",
        { 182, 94, 290, 120 }
    },
    {
        L"Left -> Both",
        L"Left -> Both: Uses Channel 1 (Left) for both ears (fixes 2-ch audio interfaces!)",
        { 298, 94, 407, 120 }
    },
    {
        L"Right -> Both",
        L"Right -> Both: Uses Channel 2 (Right) for both ears (if mic is in Input 2)",
        { 415, 94, 524, 120 }
    },
};

constexpr int kLabelLeft   = 24;
constexpr int kSliderLeft  = 258;
constexpr int kSliderRight = 442;
constexpr int kRowHeight   = 30;

inline bool ptInRect(const RECT& r, int x, int y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline int sc(int v, float s) noexcept {
    return static_cast<int>(std::lround(static_cast<float>(v) * s));
}

inline RECT scRect(const RECT& r, float s) noexcept {
    return RECT{ sc(r.left, s), sc(r.top, s), sc(r.right, s), sc(r.bottom, s) };
}

bool isRowDimmed(ParamID id, bool bypassed, bool agcOn) noexcept {
    if (bypassed) return true;
    if (!agcOn && (id == kParamTargetLufs || id == kParamAgcMaxBoostDb || id == kParamAgcSpeed)) {
        return true;
    }
    return false;
}

bool isPresetActive(AutoGainNSController* ctrl, int idx) noexcept {
    if (!ctrl || idx < 0 || idx >= kNumPresets) return false;
    const auto& pr = kPresets[idx];
    auto nearNorm = [&](ParamID id, double targetNorm) {
        return std::fabs(ctrl->getParamNormalized(id) - targetNorm) < 0.015;
    };
    return nearNorm(kParamHpfHz, rangeToNorm(pr.hpfHz, 20.0, 160.0)) &&
           nearNorm(kParamNsStrength, pr.nsStrength) &&
           nearNorm(kParamVadThreshold, rangeToNorm(pr.vadThreshold, 0.0, 0.95)) &&
           nearNorm(kParamGateFloorDb, rangeToNorm(pr.gateFloorDb, -60.0, 0.0)) &&
           nearNorm(kParamAgcEnable, pr.agcEnable) &&
           nearNorm(kParamTargetLufs, rangeToNorm(pr.targetLufs, -28.0, -10.0)) &&
           nearNorm(kParamAgcMaxBoostDb, rangeToNorm(pr.agcMaxBoostDb, 0.0, 30.0)) &&
           nearNorm(kParamAgcSpeed, pr.agcSpeed) &&
           nearNorm(kParamLimiterCeilingDb, rangeToNorm(pr.limiterCeilingDb, -6.0, -0.1)) &&
           nearNorm(kParamOutputGainDb, rangeToNorm(pr.outputGainDb, -12.0, 12.0));
}

void formatParamValue(ParamID id, double norm, wchar_t* out, size_t cap) {
    switch (id) {
        case kParamHpfHz:
            std::swprintf(out, cap, L"%.0f Hz", normToRange(norm, 20.0, 160.0));
            break;
        case kParamNsStrength:
            std::swprintf(out, cap, L"%.0f %%", norm * 100.0);
            break;
        case kParamVadThreshold:
            std::swprintf(out, cap, L"%.0f %%", normToRange(norm, 0.0, 95.0));
            break;
        case kParamGateFloorDb:
            std::swprintf(out, cap, L"%.1f dB", normToRange(norm, -60.0, 0.0));
            break;
        case kParamAgcEnable:
            std::swprintf(out, cap, L"%ls", norm >= 0.5 ? L"ON" : L"OFF");
            break;
        case kParamTargetLufs:
            std::swprintf(out, cap, L"%.1f LUFS", normToRange(norm, -28.0, -10.0));
            break;
        case kParamAgcMaxBoostDb:
            std::swprintf(out, cap, L"+%.1f dB", normToRange(norm, 0.0, 30.0));
            break;
        case kParamAgcSpeed:
            std::swprintf(out, cap, L"%.0f %%", norm * 100.0);
            break;
        case kParamLimiterCeilingDb:
            std::swprintf(out, cap, L"%.1f dBFS", normToRange(norm, -6.0, -0.1));
            break;
        case kParamOutputGainDb: {
            double v = normToRange(norm, -12.0, 12.0);
            std::swprintf(out, cap, L"%+.1f dB", v);
            break;
        }
        default:
            out[0] = L'\0';
            break;
    }
}

void fillRoundRect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
}

void drawFocusRing(HDC hdc, const RECT& r, int radius, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, radius + 2, radius + 2);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void resetParamToDefault(AutoGainNSController* ctrl, ParamID id) {
    if (!ctrl) return;
    const VoiceParams def{};
    double defNorm = 0.0;
    switch (id) {
        case kParamHpfHz:            defNorm = rangeToNorm(def.hpfHz, 20.0, 160.0); break;
        case kParamNsStrength:       defNorm = def.nsStrength; break;
        case kParamVadThreshold:     defNorm = rangeToNorm(def.vadThreshold, 0.0, 0.95); break;
        case kParamGateFloorDb:      defNorm = rangeToNorm(def.gateFloorDb, -60.0, 0.0); break;
        case kParamTargetLufs:       defNorm = rangeToNorm(def.targetLufs, -28.0, -10.0); break;
        case kParamAgcMaxBoostDb:    defNorm = rangeToNorm(def.agcMaxBoostDb, 0.0, 30.0); break;
        case kParamAgcSpeed:         defNorm = def.agcSpeed; break;
        case kParamLimiterCeilingDb: defNorm = rangeToNorm(def.limiterCeilingDb, -6.0, -0.1); break;
        case kParamOutputGainDb:     defNorm = rangeToNorm(def.outputGainDb, -12.0, 12.0); break;
        default: return;
    }
    ctrl->beginEdit(id);
    ctrl->setParamNormalized(id, defNorm);
    ctrl->performEdit(id, defNorm);
    ctrl->endEdit(id);
}

LRESULT CALLBACK ViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* view = reinterpret_cast<AutoGainNSView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTTAB;
        case WM_KEYDOWN:
            if (view) view->handleKeyDown(static_cast<unsigned int>(wParam));
            return 0;
        case WM_KILLFOCUS:
            if (view) view->handleKillFocus();
            return 0;
        case WM_SETCURSOR:
            if (view && LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, view->isOverInteractive() ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_PAINT:
            if (view) view->handlePaint();
            return 0;
        case WM_LBUTTONDOWN:
            if (view) view->handleMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONDBLCLK:
            if (view) view->handleMouseDoubleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            if (view) view->handleMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            if (view) view->handleMouseLeave();
            return 0;
        case WM_LBUTTONUP:
            if (view) view->handleMouseUp();
            return 0;
        case WM_CAPTURECHANGED:
            if (view) view->handleCaptureLost();
            return 0;
        case WM_MOUSEWHEEL: {
            if (view) {
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                view->handleMouseWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wParam));
            }
            return 0;
        }
        case WM_DESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ensureWindowClassRegistered() {
    static bool registered = false;
    if (registered) return;
    HINSTANCE hInst = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ViewWndProc), &hInst);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = ViewWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClassName;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

} // namespace

AutoGainNSView::AutoGainNSView(AutoGainNSController* controller)
    : CPluginView(nullptr), controller_(controller) {
    applyScaleToRect();
}

AutoGainNSView::~AutoGainNSView() {
    if (controller_) {
        controller_->clearActiveView(this);
    }
    if (hwnd_) {
        DestroyWindow(static_cast<HWND>(hwnd_));
        hwnd_ = nullptr;
    }
    destroyFonts();
}

void AutoGainNSView::createFonts() {
    destroyFonts();
    auto mk = [this](int h, int weight) -> HFONT {
        return CreateFontW(
            sc(h, scaleFactor_), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    fontTitle_ = mk(20, FW_BOLD);
    fontSub_   = mk(12, FW_NORMAL);
    fontSec_   = mk(11, FW_BOLD);
    fontRow_   = mk(14, FW_SEMIBOLD);
    fontDesc_  = mk(11, FW_NORMAL);
}

void AutoGainNSView::destroyFonts() {
    auto del = [](void*& f) {
        if (f) { DeleteObject(static_cast<HFONT>(f)); f = nullptr; }
    };
    del(fontTitle_);
    del(fontSub_);
    del(fontSec_);
    del(fontRow_);
    del(fontDesc_);
}

void AutoGainNSView::applyScaleToRect() {
    ViewRect r(0, 0, sc(kLogicalWidth, scaleFactor_), sc(kLogicalHeight, scaleFactor_));
    setRect(r);
}

void AutoGainNSView::refresh() noexcept {
    if (hwnd_) {
        InvalidateRect(static_cast<HWND>(hwnd_), nullptr, FALSE);
    }
}

tresult PLUGIN_API AutoGainNSView::isPlatformTypeSupported(FIDString type) {
    if (type && std::strcmp(type, kPlatformTypeHWND) == 0) {
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API AutoGainNSView::setContentScaleFactor(ScaleFactor factor) {
    const float clamped = std::clamp(static_cast<float>(factor), 0.75f, 3.0f);
    hostScaled_ = true;
    if (std::abs(clamped - scaleFactor_) < 0.01f) {
        return kResultTrue;
    }
    scaleFactor_ = clamped;
    applyScaleToRect();
    createFonts();

    ViewRect target(0, 0, sc(kLogicalWidth, scaleFactor_), sc(kLogicalHeight, scaleFactor_));
    if (plugFrame) {
        plugFrame->resizeView(this, &target);
    } else if (hwnd_) {
        MoveWindow(static_cast<HWND>(hwnd_), 0, 0, target.getWidth(), target.getHeight(), TRUE);
    }
    refresh();
    return kResultTrue;
}

tresult PLUGIN_API AutoGainNSView::attached(void* parent, FIDString type) {
    if (!parent || isPlatformTypeSupported(type) != kResultTrue) {
        return kResultFalse;
    }
    ensureWindowClassRegistered();

    if (!hostScaled_) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            auto pGetDpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(u32, "GetDpiForWindow"));
            if (pGetDpi) {
                const UINT dpi = pGetDpi(static_cast<HWND>(parent));
                if (dpi >= 96) {
                    scaleFactor_ = std::clamp(static_cast<float>(dpi) / 96.0f, 1.0f, 3.0f);
                    applyScaleToRect();
                }
            }
        }
    }

    createFonts();

    HINSTANCE hInst = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ViewWndProc), &hInst);

    const int physW = sc(kLogicalWidth, scaleFactor_);
    const int physH = sc(kLogicalHeight, scaleFactor_);

    HWND hwnd = CreateWindowExW(
        0,
        kWndClassName,
        L"AutoGainNS",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, physW, physH,
        static_cast<HWND>(parent),
        nullptr,
        hInst,
        nullptr);

    if (!hwnd) {
        return kResultFalse;
    }

    hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (controller_) {
        controller_->setActiveView(this);
    }
    if (!hostScaled_ && plugFrame && std::abs(scaleFactor_ - 1.0f) >= 0.01f) {
        ViewRect target(0, 0, physW, physH);
        plugFrame->resizeView(this, &target);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API AutoGainNSView::removed() {
    if (controller_) {
        controller_->clearActiveView(this);
    }
    if (hwnd_) {
        DestroyWindow(static_cast<HWND>(hwnd_));
        hwnd_ = nullptr;
    }
    destroyFonts();
    return CPluginView::removed();
}

tresult PLUGIN_API AutoGainNSView::onSize(ViewRect* newSize) {
    tresult res = CPluginView::onSize(newSize);
    if (hwnd_ && newSize) {
        MoveWindow(static_cast<HWND>(hwnd_), 0, 0,
                   newSize->getWidth(), newSize->getHeight(), TRUE);
    }
    return res;
}

void AutoGainNSView::handlePaint() {
    HWND hwnd = static_cast<HWND>(hwnd_);
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    const float s = scaleFactor_;
    const int physW = sc(kLogicalWidth, s);
    const int physH = sc(kLogicalHeight, s);

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, physW, physH);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    // Palette
    constexpr COLORREF colBg        = RGB(19, 22, 28);
    constexpr COLORREF colHeader    = RGB(24, 28, 36);
    constexpr COLORREF colCard       = RGB(28, 32, 40);
    constexpr COLORREF colCardHover  = RGB(33, 38, 48);
    constexpr COLORREF colBorder     = RGB(46, 53, 66);
    constexpr COLORREF colCtrlBorder = RGB(100, 114, 138);
    constexpr COLORREF colBtnHover   = RGB(54, 62, 78);
    constexpr COLORREF colAccent     = RGB(96, 165, 250);
    constexpr COLORREF colAccentDim  = RGB(45, 72, 114);
    constexpr COLORREF colPresetOn   = RGB(30, 48, 76);
    constexpr COLORREF colTrack      = RGB(42, 48, 60);
    constexpr COLORREF colText       = RGB(238, 242, 248);
    constexpr COLORREF colTextDim    = RGB(115, 126, 144);
    constexpr COLORREF colMuted      = RGB(150, 163, 182);
    constexpr COLORREF colDesc       = RGB(144, 156, 176);
    constexpr COLORREF colDescDim    = RGB(82, 90, 104);
    constexpr COLORREF colGreen      = RGB(34, 197, 94);
    constexpr COLORREF colRedFill    = RGB(178, 38, 38);
    constexpr COLORREF colRedBorder  = RGB(239, 68, 68);
    constexpr COLORREF colRedText    = RGB(248, 113, 113);

    RECT full{ 0, 0, physW, physH };
    HBRUSH bgBr = CreateSolidBrush(colBg);
    FillRect(memDc, &full, bgBr);
    DeleteObject(bgBr);

    // Header bar
    RECT hdr{ 0, 0, physW, sc(54, s) };
    HBRUSH hdrBr = CreateSolidBrush(colHeader);
    FillRect(memDc, &hdr, hdrBr);
    DeleteObject(hdrBr);

    HPEN divPen = CreatePen(PS_SOLID, 1, colBorder);
    HGDIOBJ oldPen = SelectObject(memDc, divPen);
    MoveToEx(memDc, 0, sc(54, s), nullptr);
    LineTo(memDc, physW, sc(54, s));
    SelectObject(memDc, oldPen);
    DeleteObject(divPen);

    if (!fontTitle_) createFonts();
    HFONT fontTitle = static_cast<HFONT>(fontTitle_);
    HFONT fontSub   = static_cast<HFONT>(fontSub_);
    HFONT fontSec   = static_cast<HFONT>(fontSec_);
    HFONT fontRow   = static_cast<HFONT>(fontRow_);
    HFONT fontDesc  = static_cast<HFONT>(fontDesc_);

    SetBkMode(memDc, TRANSPARENT);

    SelectObject(memDc, fontTitle);
    SetTextColor(memDc, colText);
    TextOutW(memDc, sc(16, s), sc(8, s), L"AutoGainNS", 10);

    SelectObject(memDc, fontSub);
    SetTextColor(memDc, colMuted);
    constexpr const wchar_t* kSubtitle = L"Voice Noise Removal & Auto Volume  \x2022  12 ms Latency";
    TextOutW(memDc, sc(16, s), sc(31, s), kSubtitle, static_cast<int>(std::wcslen(kSubtitle)));

    const bool bypassed = controller_ ? (controller_->getParamNormalized(kParamBypass) >= 0.5) : false;
    const bool agcOn = controller_ ? (controller_->getParamNormalized(kParamAgcEnable) >= 0.5) : true;
    const int inputMode = controller_
        ? std::clamp(static_cast<int>(std::llround(controller_->getParamNormalized(kParamInputMode) * 3.0)), 0, 3)
        : 0;

    const COLORREF bypassFill = bypassed
        ? colRedFill
        : (hoverKind_ == HoverKind::Bypass ? RGB(28, 102, 58) : RGB(22, 82, 48));
    const COLORREF bypassBorder = bypassed
        ? colRedBorder
        : (hoverKind_ == HoverKind::Bypass ? colText : colGreen);
    RECT bypassPhys = scRect(kBypassRect, s);
    fillRoundRect(memDc, bypassPhys, sc(8, s), bypassFill, bypassBorder);
    SelectObject(memDc, fontSec);
    SetTextColor(memDc, colText);
    DrawTextW(memDc, bypassed ? L"DISABLED" : L"ENABLED", -1, &bypassPhys, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (hasKeyFocus_ && focusIndex_ == 0) drawFocusRing(memDc, bypassPhys, sc(8, s), colAccent);

    const COLORREF resetFill = (hoverKind_ == HoverKind::Reset) ? colBtnHover : colCard;
    const COLORREF resetBorder = (hoverKind_ == HoverKind::Reset) ? colAccent : colCtrlBorder;
    RECT resetPhys = scRect(kResetRect, s);
    fillRoundRect(memDc, resetPhys, sc(8, s), resetFill, resetBorder);
    DrawTextW(memDc, L"RESET", -1, &resetPhys, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (hasKeyFocus_ && focusIndex_ == 1) drawFocusRing(memDc, resetPhys, sc(8, s), colAccent);

    SelectObject(memDc, fontSec);
    SetTextColor(memDc, colMuted);
    RECT presetLbl = scRect(RECT{ 16, 62, 84, 88 }, s);
    DrawTextW(memDc, L"PRESETS", -1, &presetLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < kNumPresets; ++i) {
        const bool active = isPresetActive(controller_, i);
        const bool hovered = (hoverKind_ == HoverKind::Preset && hoverIndex_ == i);
        const COLORREF fill = active
            ? (bypassed ? colTrack : colPresetOn)
            : (hovered ? colBtnHover : colCard);
        const COLORREF border = active
            ? (bypassed ? colAccentDim : colAccent)
            : (hovered ? colAccent : colCtrlBorder);
        RECT pPhys = scRect(kPresetRects[i], s);
        fillRoundRect(memDc, pPhys, sc(8, s), fill, border);
        SelectObject(memDc, fontDesc);
        SetTextColor(memDc, active ? (bypassed ? colMuted : colText) : colMuted);
        DrawTextW(memDc, kPresets[i].name, -1, &pPhys, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hasKeyFocus_ && focusIndex_ == 2 + i) drawFocusRing(memDc, pPhys, sc(8, s), colAccent);
    }

    // Input Routing bar (y = 94..120)
    SelectObject(memDc, fontSec);
    SetTextColor(memDc, colMuted);
    RECT inLbl = scRect(RECT{ 16, 94, 84, 120 }, s);
    DrawTextW(memDc, L"MIC INPUT", -1, &inLbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 4; ++i) {
        const bool active = (inputMode == i);
        const bool hovered = (hoverKind_ == HoverKind::InputMode && hoverIndex_ == i);
        const COLORREF fill = active ? colPresetOn : (hovered ? colBtnHover : colCard);
        const COLORREF border = (active || hovered) ? colAccent : colCtrlBorder;
        RECT mPhys = scRect(kInputModes[i].rect, s);
        fillRoundRect(memDc, mPhys, sc(8, s), fill, border);
        SelectObject(memDc, fontDesc);
        SetTextColor(memDc, active ? colText : colMuted);
        DrawTextW(memDc, kInputModes[i].name, -1, &mPhys, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hasKeyFocus_ && focusIndex_ == 6 + i) drawFocusRing(memDc, mPhys, sc(8, s), colAccent);
    }

    // Section cards
    fillRoundRect(memDc, scRect(RECT{ 12, 128, kLogicalWidth - 12, 296 }, s), sc(10, s), colCard, colBorder);
    fillRoundRect(memDc, scRect(RECT{ 12, 304, kLogicalWidth - 12, 472 }, s), sc(10, s), colCard, colBorder);
    fillRoundRect(memDc, scRect(RECT{ 12, 480, kLogicalWidth - 12, 574 }, s), sc(10, s), colCard, colBorder);

    // Rows
    const int numRows = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
    for (int i = 0; i < numRows; ++i) {
        const auto& row = kRows[i];
        if (row.section) {
            SelectObject(memDc, fontSec);
            SetTextColor(memDc, bypassed ? colAccentDim : colAccent);
            TextOutW(memDc, sc(kLabelLeft, s), sc(row.y - 18, s), row.section, static_cast<int>(std::wcslen(row.section)));
        }

        const bool dimmed = isRowDimmed(row.id, bypassed, agcOn);
        const bool hovered = (hoverKind_ == HoverKind::Row && hoverIndex_ == i) ||
                             (activeSliderParam_ == static_cast<int>(row.id));

        if (hovered && !dimmed) {
            RECT hl = scRect(RECT{ 16, row.y - 2, kLogicalWidth - 16, row.y + kRowHeight + 1 }, s);
            fillRoundRect(memDc, hl, sc(6, s), colCardHover, colCardHover);
        }

        const double norm = controller_ ? controller_->getParamNormalized(row.id) : 0.0;

        // Label + 1-line description
        SelectObject(memDc, fontRow);
        SetTextColor(memDc, dimmed ? colTextDim : colText);
        RECT lblR = scRect(RECT{ kLabelLeft, row.y - 1, kSliderLeft - 10, row.y + 15 }, s);
        DrawTextW(memDc, row.label, -1, &lblR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDc, fontDesc);
        SetTextColor(memDc, dimmed ? colDescDim : colDesc);
        RECT descR = scRect(RECT{ kLabelLeft, row.y + 15, kSliderLeft - 10, row.y + 29 }, s);
        DrawTextW(memDc, row.desc, -1, &descR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (row.isToggle) {
            const bool isOn = (norm >= 0.5);
            RECT togR = scRect(RECT{ kSliderLeft, row.y + 4, kSliderLeft + 62, row.y + 26 }, s);
            const COLORREF togFill = bypassed ? colTrack : (isOn ? RGB(22, 82, 48) : colTrack);
            const COLORREF togBorder = (hovered && !bypassed) ? colText : (isOn && !bypassed ? colGreen : colCtrlBorder);
            fillRoundRect(memDc, togR, sc(10, s), togFill, togBorder);
            SelectObject(memDc, fontSec);
            SetTextColor(memDc, dimmed ? colTextDim : colText);
            DrawTextW(memDc, isOn ? L"ON" : L"OFF", -1, &togR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            // Slider track
            RECT trk = scRect(RECT{ kSliderLeft, row.y + 12, kSliderRight, row.y + 18 }, s);
            fillRoundRect(memDc, trk, sc(4, s), colTrack, colTrack);

            const int trkW = trk.right - trk.left;
            const int fillW = static_cast<int>(std::clamp(norm, 0.0, 1.0) * trkW);
            const COLORREF fillCol = dimmed ? colAccentDim : colAccent;
            if (fillW > 2) {
                RECT fl{ trk.left, trk.top, trk.left + fillW, trk.bottom };
                fillRoundRect(memDc, fl, sc(4, s), fillCol, fillCol);
            }

            // Thumb
            const int tx = trk.left + fillW;
            const int ty = (trk.top + trk.bottom) / 2;
            const int thumbR = sc((hovered && !dimmed) ? 7 : 6, s);
            RECT thumb{ tx - thumbR, ty - thumbR, tx + thumbR, ty + thumbR };
            fillRoundRect(memDc, thumb, thumbR, dimmed ? colMuted : colText, fillCol);
        }

        // Value text (omit duplicate ON/OFF next to toggle pill)
        if (!row.isToggle) {
            wchar_t valBuf[32]{};
            formatParamValue(row.id, norm, valBuf, 32);
            SelectObject(memDc, fontRow);
            SetTextColor(memDc, dimmed ? colTextDim : colMuted);
            RECT valR = scRect(RECT{ kSliderRight + 8, row.y, kLogicalWidth - 22, row.y + kRowHeight }, s);
            DrawTextW(memDc, valBuf, -1, &valR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (hasKeyFocus_ && focusIndex_ == 10 + i) {
            RECT rowPhys = scRect(RECT{ 16, row.y - 2, kLogicalWidth - 16, row.y + kRowHeight + 1 }, s);
            drawFocusRing(memDc, rowPhys, sc(6, s), colAccent);
        }
    }

    // Footer tip bar
    const wchar_t* tip = L"Drag slider to adjust  \x2022  Scroll wheel to fine-tune  \x2022  Double-click slider to reset";
    if (bypassed) {
        tip = L"Plugin is DISABLED (raw mic passing through)  \x2022  Click DISABLED to turn back on";
    } else if (hoverKind_ == HoverKind::Bypass) {
        tip = L"Click to turn AutoGainNS ON or OFF (compare processed vs raw mic)";
    } else if (hoverKind_ == HoverKind::Preset && hoverIndex_ >= 0 && hoverIndex_ < kNumPresets) {
        tip = kPresets[hoverIndex_].tip;
    } else if (hoverKind_ == HoverKind::InputMode && hoverIndex_ >= 0 && hoverIndex_ < 4) {
        tip = kInputModes[hoverIndex_].tip;
    } else if (hoverKind_ == HoverKind::Row && hoverIndex_ >= 0 && hoverIndex_ < numRows) {
        const auto& r = kRows[hoverIndex_];
        if (!agcOn && isRowDimmed(r.id, false, false)) {
            tip = L"Turn Auto Volume ON above to enable automatic loudness controls";
        } else if (r.isToggle) {
            tip = L"Click to toggle Auto Volume ON/OFF";
        } else {
            tip = L"Drag slider to adjust  \x2022  Scroll wheel to fine-tune  \x2022  Double-click to reset default";
        }
    } else if (hoverKind_ == HoverKind::Reset) {
        tip = L"Reset all sliders and switches back to default settings";
    }

    SelectObject(memDc, fontDesc);
    SetTextColor(memDc, bypassed ? colRedText : colMuted);
    RECT tipR = scRect(RECT{ 16, 580, kLogicalWidth - 16, 604 }, s);
    DrawTextW(memDc, tip, -1, &tipR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(hdc, 0, 0, physW, physH, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

void AutoGainNSView::handleMouseDown(int x, int y) {
    if (!controller_) return;
    if (hwnd_) SetFocus(static_cast<HWND>(hwnd_));
    if (hasKeyFocus_) {
        hasKeyFocus_ = false;
        refresh();
    }
    const int lx = static_cast<int>(std::lround(static_cast<float>(x) / scaleFactor_));
    const int ly = static_cast<int>(std::lround(static_cast<float>(y) / scaleFactor_));

    if (ptInRect(kBypassRect, lx, ly)) {
        const double cur = controller_->getParamNormalized(kParamBypass);
        const double next = (cur >= 0.5) ? 0.0 : 1.0;
        controller_->beginEdit(kParamBypass);
        controller_->setParamNormalized(kParamBypass, next);
        controller_->performEdit(kParamBypass, next);
        controller_->endEdit(kParamBypass);
        refresh();
        return;
    }

    if (ptInRect(kResetRect, lx, ly)) {
        controller_->resetDefaults();
        refresh();
        return;
    }

    for (int i = 0; i < kNumPresets; ++i) {
        if (ptInRect(kPresetRects[i], lx, ly)) {
            controller_->applyPreset(i);
            refresh();
            return;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (ptInRect(kInputModes[i].rect, lx, ly)) {
            const double norm = static_cast<double>(i) / 3.0;
            controller_->beginEdit(kParamInputMode);
            controller_->setParamNormalized(kParamInputMode, norm);
            controller_->performEdit(kParamInputMode, norm);
            controller_->endEdit(kParamInputMode);
            refresh();
            return;
        }
    }

    for (const auto& row : kRows) {
        if (ly >= row.y && ly <= row.y + kRowHeight) {
            if (row.isToggle) {
                RECT togR{ kSliderLeft, row.y + 4, kSliderLeft + 62, row.y + 26 };
                if (ptInRect(togR, lx, ly)) {
                    const double cur = controller_->getParamNormalized(row.id);
                    const double next = (cur >= 0.5) ? 0.0 : 1.0;
                    controller_->beginEdit(row.id);
                    controller_->setParamNormalized(row.id, next);
                    controller_->performEdit(row.id, next);
                    controller_->endEdit(row.id);
                    refresh();
                }
                return;
            }
            if (lx >= kSliderLeft - 8 && lx <= kSliderRight + 8) {
                activeSliderParam_ = static_cast<int>(row.id);
                SetCapture(static_cast<HWND>(hwnd_));
                controller_->beginEdit(row.id);
                const double norm = std::clamp(
                    static_cast<double>(lx - kSliderLeft) / static_cast<double>(kSliderRight - kSliderLeft),
                    0.0, 1.0);
                controller_->setParamNormalized(row.id, norm);
                controller_->performEdit(row.id, norm);
                refresh();
                return;
            }
        }
    }
}

void AutoGainNSView::handleMouseDoubleClick(int x, int y) {
    if (!controller_) return;
    const int lx = static_cast<int>(std::lround(static_cast<float>(x) / scaleFactor_));
    const int ly = static_cast<int>(std::lround(static_cast<float>(y) / scaleFactor_));
    for (const auto& row : kRows) {
        if (!row.isToggle && ly >= row.y && ly <= row.y + kRowHeight &&
            lx >= kSliderLeft - 8 && lx <= kSliderRight + 8) {
            resetParamToDefault(controller_, row.id);
            refresh();
            return;
        }
    }
}

void AutoGainNSView::handleMouseMove(int x, int y) {
    if (!trackingMouse_ && hwnd_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = static_cast<HWND>(hwnd_);
        if (TrackMouseEvent(&tme)) {
            trackingMouse_ = true;
        }
    }

    const int lx = static_cast<int>(std::lround(static_cast<float>(x) / scaleFactor_));
    const int ly = static_cast<int>(std::lround(static_cast<float>(y) / scaleFactor_));

    if (controller_ && activeSliderParam_ >= 0) {
        const ParamID id = static_cast<ParamID>(activeSliderParam_);
        const double norm = std::clamp(
            static_cast<double>(lx - kSliderLeft) / static_cast<double>(kSliderRight - kSliderLeft),
            0.0, 1.0);
        controller_->setParamNormalized(id, norm);
        controller_->performEdit(id, norm);
        overInteractive_ = true;
        refresh();
        return;
    }

    HoverKind nextKind = HoverKind::None;
    int nextIndex = -1;
    bool handCursor = false;

    if (ptInRect(kBypassRect, lx, ly)) {
        nextKind = HoverKind::Bypass;
        handCursor = true;
    } else if (ptInRect(kResetRect, lx, ly)) {
        nextKind = HoverKind::Reset;
        handCursor = true;
    } else {
        for (int i = 0; i < kNumPresets; ++i) {
            if (ptInRect(kPresetRects[i], lx, ly)) {
                nextKind = HoverKind::Preset;
                nextIndex = i;
                handCursor = true;
                break;
            }
        }
        if (nextKind == HoverKind::None) {
            for (int i = 0; i < 4; ++i) {
                if (ptInRect(kInputModes[i].rect, lx, ly)) {
                    nextKind = HoverKind::InputMode;
                    nextIndex = i;
                    handCursor = true;
                    break;
                }
            }
        }
    }

    if (nextKind == HoverKind::None) {
        const int numRows = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
        for (int i = 0; i < numRows; ++i) {
            const auto& row = kRows[i];
            if (ly >= row.y && ly <= row.y + kRowHeight) {
                nextKind = HoverKind::Row;
                nextIndex = i;
                if (row.isToggle) {
                    RECT togR{ kSliderLeft, row.y + 4, kSliderLeft + 62, row.y + 26 };
                    handCursor = ptInRect(togR, lx, ly);
                } else {
                    handCursor = (lx >= kSliderLeft - 8 && lx <= kSliderRight + 8);
                }
                break;
            }
        }
    }

    overInteractive_ = handCursor;

    if (nextKind != hoverKind_ || nextIndex != hoverIndex_) {
        hoverKind_ = nextKind;
        hoverIndex_ = nextIndex;
        refresh();
    }
}

void AutoGainNSView::handleMouseLeave() {
    trackingMouse_ = false;
    overInteractive_ = false;
    if (hoverKind_ != HoverKind::None || hoverIndex_ != -1) {
        hoverKind_ = HoverKind::None;
        hoverIndex_ = -1;
        refresh();
    }
}

void AutoGainNSView::handleMouseUp() {
    if (controller_ && activeSliderParam_ >= 0) {
        controller_->endEdit(static_cast<ParamID>(activeSliderParam_));
        activeSliderParam_ = -1;
        ReleaseCapture();
        refresh();
    }
}

void AutoGainNSView::handleCaptureLost() {
    if (controller_ && activeSliderParam_ >= 0) {
        controller_->endEdit(static_cast<ParamID>(activeSliderParam_));
        activeSliderParam_ = -1;
        refresh();
    }
}

void AutoGainNSView::handleMouseWheel(int x, int y, int delta) {
    (void)x;
    if (!controller_ || delta == 0) return;
    const int ly = static_cast<int>(std::lround(static_cast<float>(y) / scaleFactor_));
    for (const auto& row : kRows) {
        if (!row.isToggle && ly >= row.y && ly <= row.y + kRowHeight) {
            const double step = (delta > 0 ? 0.02 : -0.02);
            const double cur = controller_->getParamNormalized(row.id);
            const double next = std::clamp(cur + step, 0.0, 1.0);
            controller_->beginEdit(row.id);
            controller_->setParamNormalized(row.id, next);
            controller_->performEdit(row.id, next);
            controller_->endEdit(row.id);
            refresh();
            return;
        }
    }
}

void AutoGainNSView::handleKeyDown(unsigned int vk) {
    if (!controller_) return;
    constexpr int kFocusCount = 20;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (vk == VK_TAB) {
        if (focusIndex_ < 0) {
            focusIndex_ = shift ? (kFocusCount - 1) : 0;
        } else {
            focusIndex_ = (focusIndex_ + (shift ? kFocusCount - 1 : 1)) % kFocusCount;
        }
        hasKeyFocus_ = true;
        refresh();
        return;
    }

    if (focusIndex_ < 0) {
        focusIndex_ = 0;
        hasKeyFocus_ = true;
    }

    if (vk == VK_SPACE || vk == VK_RETURN) {
        if (focusIndex_ == 0) {
            const double next = (controller_->getParamNormalized(kParamBypass) >= 0.5) ? 0.0 : 1.0;
            controller_->beginEdit(kParamBypass);
            controller_->setParamNormalized(kParamBypass, next);
            controller_->performEdit(kParamBypass, next);
            controller_->endEdit(kParamBypass);
        } else if (focusIndex_ == 1) {
            controller_->resetDefaults();
        } else if (focusIndex_ >= 2 && focusIndex_ < 2 + kNumPresets) {
            controller_->applyPreset(focusIndex_ - 2);
        } else if (focusIndex_ >= 6 && focusIndex_ < 10) {
            const double norm = static_cast<double>(focusIndex_ - 6) / 3.0;
            controller_->beginEdit(kParamInputMode);
            controller_->setParamNormalized(kParamInputMode, norm);
            controller_->performEdit(kParamInputMode, norm);
            controller_->endEdit(kParamInputMode);
        } else if (focusIndex_ >= 10 && focusIndex_ < 20) {
            const auto& row = kRows[focusIndex_ - 10];
            if (row.isToggle) {
                const double next = (controller_->getParamNormalized(row.id) >= 0.5) ? 0.0 : 1.0;
                controller_->beginEdit(row.id);
                controller_->setParamNormalized(row.id, next);
                controller_->performEdit(row.id, next);
                controller_->endEdit(row.id);
            }
        }
        hasKeyFocus_ = true;
        refresh();
        return;
    }

    if (focusIndex_ >= 10 && focusIndex_ < 20) {
        const auto& row = kRows[focusIndex_ - 10];
        if (!row.isToggle) {
            double next = controller_->getParamNormalized(row.id);
            bool changed = false;
            const double step = shift ? 0.10 : 0.02;
            if (vk == VK_RIGHT || vk == VK_UP) {
                next = std::clamp(next + step, 0.0, 1.0);
                changed = true;
            } else if (vk == VK_LEFT || vk == VK_DOWN) {
                next = std::clamp(next - step, 0.0, 1.0);
                changed = true;
            } else if (vk == VK_HOME) {
                next = 0.0;
                changed = true;
            } else if (vk == VK_END) {
                next = 1.0;
                changed = true;
            } else if (vk == VK_DELETE || vk == VK_BACK) {
                resetParamToDefault(controller_, row.id);
                hasKeyFocus_ = true;
                refresh();
                return;
            }
            if (changed) {
                controller_->beginEdit(row.id);
                controller_->setParamNormalized(row.id, next);
                controller_->performEdit(row.id, next);
                controller_->endEdit(row.id);
                hasKeyFocus_ = true;
                refresh();
            }
        }
    }
}

void AutoGainNSView::handleKillFocus() {
    if (hasKeyFocus_) {
        hasKeyFocus_ = false;
        refresh();
    }
}

} // namespace autogain
