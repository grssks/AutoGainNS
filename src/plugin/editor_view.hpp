#pragma once

#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

namespace autogain {

class AutoGainNSController;

class AutoGainNSView : public Steinberg::CPluginView,
                       public Steinberg::IPlugViewContentScaleSupport {
public:
    static constexpr int kLogicalWidth = 540;
    static constexpr int kLogicalHeight = 640;

    explicit AutoGainNSView(AutoGainNSController* controller);
    ~AutoGainNSView() override;

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, Steinberg::IPlugViewContentScaleSupport::iid, Steinberg::IPlugViewContentScaleSupport)
        return CPluginView::queryInterface(_iid, obj);
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return CPluginView::addRef(); }
    Steinberg::uint32 PLUGIN_API release() override { return CPluginView::release(); }

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultFalse; }
    Steinberg::tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override;

    void refresh() noexcept;

    // Win32 message handlers (physical coordinates)
    void handlePaint();
    void handleMouseDown(int x, int y);
    void handleMouseDoubleClick(int x, int y);
    void handleMouseMove(int x, int y);
    void handleMouseUp();
    void handleMouseWheel(int x, int y, int delta);
    void handleMouseLeave();
    void handleCaptureLost();
    void handleTimer();
    bool isOverInteractive() const noexcept { return overInteractive_; }

private:
    enum class HoverKind { None, Bypass, Reset, Preset, InputMode, Row };

    void createFonts();
    void destroyFonts();
    void applyScaleToRect();

    AutoGainNSController* controller_ = nullptr;
    void* hwnd_ = nullptr;
    float scaleFactor_ = 1.0f;
    bool hostScaled_ = false;
    int activeSliderParam_ = -1;
    HoverKind hoverKind_ = HoverKind::None;
    int hoverIndex_ = -1;
    bool trackingMouse_ = false;
    bool overInteractive_ = false;

    void* fontTitle_ = nullptr;
    void* fontSub_ = nullptr;
    void* fontSec_ = nullptr;
    void* fontRow_ = nullptr;
    void* fontDesc_ = nullptr;
};

} // namespace autogain
