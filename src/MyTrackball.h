#pragma once

// A vsg::Trackball with remappable rotate/pan/zoom buttons (see
// NavigationStyles.h) and a turntable rotation mode. Ported from
// XjetStudio's src/mytrackball.h so both apps navigate identically; the
// XjetStudio-only parts (orthographic zoom/pan, SSAO uniforms, view
// history) are left out.

#include "NavigationStyles.h"

#include <vsg/app/Trackball.h>
#include <vsg/maths/quat.h>

#include <QGuiApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <set>

class MyTrackball : public vsg::Inherit<vsg::Trackball, MyTrackball>
{
  public:
    explicit MyTrackball(vsg::ref_ptr<vsg::Camera> camera) : vsg::Inherit<vsg::Trackball, MyTrackball>(camera)
    {
        applyNavigationStyle(NavigationStyle::Netfabb);

        // As in XjetStudio: the base vsg::Trackball's w/s keys (pitch the
        // view up/down) are disabled.
        pitchUpKey = vsg::KEY_Undefined;
        pitchDownKey = vsg::KEY_Undefined;
    }

    // Swap the rotate/pan/zoom button-mask sets *and* the rotation feel
    // (trackball / turntable) to match the given preset. Safe to call at
    // any time; the next ButtonPressEvent picks up the new mapping. Wheel
    // zoom is owned by the base vsg::Trackball and is unchanged across
    // presets.
    void applyNavigationStyle(NavigationStyle style)
    {
        fillNavigationStyleMasks(style, rotateButtonMasks, panButtonMasks, zoomButtonMasks);
        _rotationModel = navigationStyleRotationModel(style);
    }

    // World-up direction used by Turntable rotation to lock out roll.
    void setWorldUp(const vsg::dvec3& worldUp) { _worldUp = vsg::normalize(worldUp); }

    void apply(vsg::ButtonPressEvent& buttonPress) override
    {
        if (buttonPress.handled || !eventRelevant(buttonPress))
        {
            _hasKeyboardFocus = false;
            return;
        }

        _hasPointerFocus = _hasKeyboardFocus = withinRenderArea(buttonPress);
        _lastPointerEventWithinRenderArea = _hasPointerFocus;

        uint16_t mask = buildMask(buttonPress.mask);

        if (rotateButtonMasks.count(mask))
            _updateMode = ROTATE;
        else if (panButtonMasks.count(mask))
            _updateMode = PAN;
        else if (zoomButtonMasks.count(mask))
            _updateMode = ZOOM;
        else
            _updateMode = INACTIVE;

        buttonPress.handled = _updateMode != INACTIVE;

        _zoomPreviousRatio = 0.0;
        _pan.set(0.0, 0.0);
        _rotateAngle = 0.0;

        _previousPointerEvent = &buttonPress;
    }

    void apply(vsg::ButtonReleaseEvent& buttonRelease) override
    {
        if (buttonRelease.handled || !eventRelevant(buttonRelease)) return;

        if (!windowOffsets.empty() && windowOffsets.count(buttonRelease.window) == 0) return;

        if (supportsThrow) _thrown = _previousPointerEvent && (buttonRelease.time == _previousPointerEvent->time);

        _lastPointerEventWithinRenderArea = withinRenderArea(buttonRelease);
        _hasPointerFocus = false;

        _previousPointerEvent = &buttonRelease;
    }

    void apply(vsg::MoveEvent& moveEvent) override
    {
        if (!eventRelevant(moveEvent)) return;

        _lastPointerEventWithinRenderArea = withinRenderArea(moveEvent);

        if (moveEvent.handled || !_hasPointerFocus) return;

        vsg::dvec2 new_ndc = ndc(moveEvent);
        vsg::dvec3 new_tbc = tbc(moveEvent);

        if (!_previousPointerEvent) _previousPointerEvent = &moveEvent;

        vsg::dvec2 prev_ndc = ndc(*_previousPointerEvent);
        vsg::dvec3 prev_tbc = tbc(*_previousPointerEvent);

        double dt = std::chrono::duration<double, std::chrono::seconds::period>(moveEvent.time - _previousPointerEvent->time).count();
        _previousDelta = dt;
        _previousTime = moveEvent.time;

        uint16_t mask = buildMask(moveEvent.mask);

        if (rotateButtonMasks.count(mask))
        {
            _updateMode = ROTATE;
            moveEvent.handled = true;

            if (_rotationModel == RotationModel::Turntable)
            {
                // Yaw around world-up, pitch around the post-yaw camera-right.
                // The eye orbits the lookAt center; up is forced to worldUp every
                // frame so the world can never roll. Drag right → world rotates
                // right under the cursor; drag down → tilt the top of the model
                // toward the viewer.
                vsg::dvec2 delta = new_ndc - prev_ndc;
                const double rotGain = 3.14159265358979323846; // ~one screen width = π
                double yawAngle = -delta.x * rotGain;
                double pitchAngle = -delta.y * rotGain;

                vsg::dvec3 toEye = _lookAt->eye - _lookAt->center;
                toEye = vsg::dquat(yawAngle, _worldUp) * toEye;

                // At the poles (looking straight along worldUp) right collapses
                // to zero — skip pitch for that frame; the user can yaw out of
                // the singularity.
                vsg::dvec3 right = vsg::cross(-toEye, _worldUp);
                double rightLen = vsg::length(right);
                if (rightLen > 1e-9) toEye = vsg::dquat(pitchAngle, right / rightLen) * toEye;

                _lookAt->eye = _lookAt->center + toEye;
                _lookAt->up = _worldUp;

                _rotateAngle = std::abs(yawAngle) + std::abs(pitchAngle);
                _rotateAxis = _worldUp;
            }
            else
            {
                vsg::dvec3 xp = vsg::cross(vsg::normalize(new_tbc), vsg::normalize(prev_tbc));
                double xp_len = vsg::length(xp);
                if (xp_len > 0.0)
                {
                    _rotateAngle = std::asin(xp_len);
                    _rotateAxis = xp / xp_len;
                    rotate(_rotateAngle, _rotateAxis);
                }
                else
                {
                    _rotateAngle = 0.0;
                }
            }
        }
        else if (panButtonMasks.count(mask))
        {
            _updateMode = PAN;
            moveEvent.handled = true;

            vsg::dvec2 delta = new_ndc - prev_ndc;
            _pan = delta;
            pan(delta);
        }
        else if (zoomButtonMasks.count(mask))
        {
            _updateMode = ZOOM;
            moveEvent.handled = true;

            vsg::dvec2 delta = new_ndc - prev_ndc;
            if (delta.y != 0.0)
            {
                _zoomPreviousRatio = zoomScale * 2.0 * delta.y;
                zoom(_zoomPreviousRatio);
            }
        }

        _thrown = false;

        _previousPointerEvent = &moveEvent;
    }

    // vsg::Trackball::setViewpoint() (hidden, not virtual) starts its animation
    // clock from _previousTime, which is only refreshed by MoveEvent/FrameEvent
    // processing — under on-demand rendering that can be arbitrarily stale. By
    // the time the first frame after setViewpoint() renders, the elapsed time
    // already exceeds the requested duration and the base class jumps straight
    // to the end pose instead of interpolating. Refresh the clock immediately
    // before arming the animation so it always starts from "now".
    void setViewpoint(vsg::ref_ptr<vsg::LookAt> lookAt, double duration = 1.0)
    {
        _zoomActive = false;
        _previousTime = vsg::clock::now();
        vsg::Inherit<vsg::Trackball, MyTrackball>::setViewpoint(lookAt, duration);
    }

    // True while a setViewpoint()-driven animation (orientation cube, view
    // keys) is still interpolating toward its target LookAt.
    bool isAnimatingViewpoint() const { return _animationDuration > 0.0; }

    // Invoked from apply(FrameEvent&) while a viewpoint animation is still in
    // progress, so the host can request another frame. Under on-demand
    // rendering a FrameEvent is only dispatched once per explicit request; the
    // animation would otherwise advance a single step and then freeze.
    void setRedrawRequestCallback(std::function<void()> cb) { m_RedrawRequestCallback = std::move(cb); }

    void apply(vsg::FrameEvent& frame) override
    {
        vsg::Inherit<vsg::Trackball, MyTrackball>::apply(frame);

        advanceZoom();

        if ((isAnimatingViewpoint() || _zoomActive) && m_RedrawRequestCallback) m_RedrawRequestCallback();
    }

    // Wheel zoom as a short eased motion (profile copied from vgiv's
    // PanZoomHandler): one notch is the base class's 10% step, shift makes
    // it 5x. A notch arriving mid-motion starts a new motion from the
    // present view to the previous destination zoomed once more.
    void apply(vsg::ScrollWheelEvent& scrollWheel) override
    {
        if (scrollWheel.handled || !eventRelevant(scrollWheel) || !_lastPointerEventWithinRenderArea) return;
        scrollWheel.handled = true;

        double amount = scrollWheel.delta.y;
        if (amount == 0.0) return;
        double factor = std::pow(0.9, amount * (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier ? 5.0 : 1.0));

        _zoomTotal = (_zoomActive ? _zoomTotal / _zoomApplied : 1.0) * factor;
        _zoomApplied = 1.0;
        _zoomStart = std::chrono::steady_clock::now();
        _zoomActive = true;
        if (m_RedrawRequestCallback) m_RedrawRequestCallback();
    }

    std::set<uint32_t> zoomButtonMasks;
    std::set<uint32_t> panButtonMasks;
    std::set<uint32_t> rotateButtonMasks;

  private:
    // vsgQt's pointer events carry only the button bits; OR in the Qt
    // keyboard modifiers so masks like MMB+Shift can be matched.
    static uint16_t buildMask(vsg::ButtonMask mask)
    {
        uint16_t result = uint16_t(mask);
        Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
        if (modifiers & Qt::ShiftModifier) result |= vsg::MODKEY_Shift;
        if (modifiers & Qt::ControlModifier) result |= vsg::MODKEY_Control;
        if (modifiers & Qt::AltModifier) result |= vsg::MODKEY_Alt;
        return result;
    }

    // Trapezoidal-velocity ease: accelerate over the first quarter, cruise,
    // decelerate over the last quarter. u and result are in [0,1].
    static double zoomProfile(double u)
    {
        constexpr double a = 0.25;
        constexpr double v = 1.0 / (1.0 - a);
        if (u <= 0.0) return 0.0;
        if (u >= 1.0) return 1.0;
        if (u < a) return v * u * u / (2.0 * a);
        if (u > 1.0 - a) return 1.0 - v * (1.0 - u) * (1.0 - u) / (2.0 * a);
        return v * (0.5 * a + (u - a));
    }

    void advanceZoom()
    {
        if (!_zoomActive) return;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - _zoomStart).count();
        const double u = std::min(elapsed / kZoomDuration, 1.0);
        const double wanted = std::pow(_zoomTotal, zoomProfile(u));
        // The base zoom(r) scales the eye distance by (1 - r).
        zoom(1.0 - wanted / _zoomApplied);
        _zoomApplied = wanted;
        if (u >= 1.0) _zoomActive = false;
    }

    static constexpr double kZoomDuration = 0.25; // seconds

    bool _zoomActive = false;
    double _zoomTotal = 1.0;   // distance factor of the whole motion
    double _zoomApplied = 1.0; // part of it already applied
    std::chrono::steady_clock::time_point _zoomStart;

    std::function<void()> m_RedrawRequestCallback;

    RotationModel _rotationModel = RotationModel::Trackball;
    vsg::dvec3 _worldUp = vsg::dvec3(0.0, 0.0, 1.0);
};
