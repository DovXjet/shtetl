//======================================================================
//  NavigationStyles.h - 3D mouse-navigation presets for MyTrackball
//
//  Maps a user-chosen preset (Netfabb, FreeCAD-CAD, Blender, Maya, ...)
//  onto the three button-mask sets that MyTrackball consults at
//  ButtonPressEvent / MoveEvent time. Header-only — depends only on the
//  VSG button/modifier enums, no Qt.
//
//  Copied from XjetStudio (src/NavigationStyles.h) so both apps navigate
//  identically; keep the two in sync.
//----------------------------------------------------------------------
#ifndef NAVIGATIONSTYLES_H
#define NAVIGATIONSTYLES_H

#include <vsg/ui/PointerEvent.h>
#include <vsg/ui/KeyEvent.h>
#include <set>
#include <cstdint>
#include <string>
#include <vector>

enum class NavigationStyle
{
  Netfabb,         // XjetStudio's historical default
  FreeCAD_CAD,
  Blender,
  Maya,
  Inventor,
  SolidWorks,
  OpenSCAD,
  TouchpadGesture,
};

// How drag-rotation behaves under the hood. Trackball lets the world roll
// freely around the view axis (vsg::Trackball's default); Turntable
// locks the world-up axis so models with a natural floor stay level —
// the convention every CAD app uses.
enum class RotationModel
{
  Trackball,
  Turntable,
};

inline RotationModel navigationStyleRotationModel(NavigationStyle s)
{
  switch (s)
  {
    // Trackball-feel: free roll, no preserved up.
    case NavigationStyle::Netfabb:          return RotationModel::Trackball;
    case NavigationStyle::OpenSCAD:         return RotationModel::Trackball;
    // Everything else expects CAD-style turntable orbit.
    case NavigationStyle::FreeCAD_CAD:
    case NavigationStyle::Blender:
    case NavigationStyle::Maya:
    case NavigationStyle::Inventor:
    case NavigationStyle::SolidWorks:
    case NavigationStyle::TouchpadGesture:
      return RotationModel::Turntable;
  }
  return RotationModel::Trackball;
}

// All styles in display order — used to populate the preferences combo.
inline const std::vector<NavigationStyle>& allNavigationStyles()
{
  static const std::vector<NavigationStyle> styles = {
    NavigationStyle::Netfabb,
    NavigationStyle::FreeCAD_CAD,
    NavigationStyle::Blender,
    NavigationStyle::Maya,
    NavigationStyle::Inventor,
    NavigationStyle::SolidWorks,
    NavigationStyle::OpenSCAD,
    NavigationStyle::TouchpadGesture,
  };
  return styles;
}

// Stable key persisted under QSettings "Navigation/Style". Must not be
// localised — UI labels are separate (navigationStyleDisplayName).
inline std::string navigationStyleName(NavigationStyle s)
{
  switch (s)
  {
    case NavigationStyle::Netfabb:          return "Netfabb";
    case NavigationStyle::FreeCAD_CAD:      return "FreeCAD-CAD";
    case NavigationStyle::Blender:          return "Blender";
    case NavigationStyle::Maya:             return "Maya";
    case NavigationStyle::Inventor:         return "Inventor";
    case NavigationStyle::SolidWorks:       return "SolidWorks";
    case NavigationStyle::OpenSCAD:         return "OpenSCAD";
    case NavigationStyle::TouchpadGesture:  return "Touchpad/Gesture";
  }
  return "Netfabb";
}

inline NavigationStyle navigationStyleFromName(const std::string& name)
{
  for (auto s : allNavigationStyles())
    if (navigationStyleName(s) == name) return s;
  return NavigationStyle::Netfabb;
}

// Structured per-preset bindings, intended for the Help > Key & Mouse
// Bindings dialog which renders them in a table. Each field is already
// human-readable ("MMB drag", "Shift+MMB drag", ...); notes is empty
// when there is nothing extra to say.
struct NavigationBindings
{
  std::string rotate;
  std::string pan;
  std::string zoom;
  std::string notes;
};

// Per-preset selection bindings. The base "Select" is always LMB click/drag
// because the rect selector hard-codes BUTTON_MASK_1 — what varies is whether
// the Shift / Ctrl modifier-selects survive (in TouchpadGesture they are
// reused by navigation). `notes` is shown verbatim under the table.
struct SelectionBindings
{
  std::string base;       // e.g. "LMB click or drag"
  std::string addToSel;   // e.g. "Shift+LMB"      ("" if unavailable)
  std::string toggleSel;  // e.g. "Ctrl+LMB"       ("" if unavailable)
  std::string notes;
};

inline SelectionBindings navigationStyleSelectionBindings(NavigationStyle s)
{
  // All presets except TouchpadGesture leave the LMB+modifier combinations
  // free for multi-select. In TouchpadGesture every Shift/Ctrl+LMB drag is
  // taken by navigation, so modifier-drag selection is unavailable; users
  // multi-select via the Parts panel (Shift/Ctrl-click).
  if (s == NavigationStyle::TouchpadGesture)
  {
    return {
      "LMB click or drag",
      "",
      "",
      "Shift / Ctrl multi-select via mouse is unavailable in this preset "
      "(the modifiers are used by navigation). Use the Parts panel "
      "Shift/Ctrl-click for multi-select."
    };
  }
  return {
    "LMB click or drag",
    "Shift+LMB",
    "Ctrl+LMB",
    ""
  };
}

inline NavigationBindings navigationStyleBindings(NavigationStyle s)
{
  switch (s)
  {
    case NavigationStyle::Netfabb:
      return {
        "MMB+Shift drag, RMB drag",
        "MMB drag, Ctrl+RMB drag",
        "Ctrl+MMB drag, Shift+RMB drag",
        ""
      };
    case NavigationStyle::FreeCAD_CAD:
      return {
        "LMB+MMB drag (both buttons)",
        "MMB drag",
        "RMB+MMB drag (both buttons)",
        ""
      };
    case NavigationStyle::Blender:
      return {
        "MMB drag",
        "Shift+MMB drag",
        "Ctrl+MMB drag",
        ""
      };
    case NavigationStyle::Maya:
      return {
        "Alt+LMB drag",
        "Alt+MMB drag",
        "Alt+RMB drag",
        "Hold Alt to tumble / track / dolly, the Maya viewport convention."
      };
    case NavigationStyle::Inventor:
      return {
        "Shift+MMB drag",
        "MMB drag",
        "Ctrl+MMB drag",
        ""
      };
    case NavigationStyle::SolidWorks:
      return {
        "MMB drag",
        "Ctrl+MMB drag",
        "Shift+MMB drag",
        ""
      };
    case NavigationStyle::OpenSCAD:
      return {
        "Shift+LMB drag",
        "RMB drag",
        "(use wheel — no drag-zoom in this preset)",
        "OpenSCAD itself uses bare LMB for rotate, but XjetStudio reserves "
        "LMB click/drag for part selection. The preset shifts rotate onto "
        "Shift+LMB so both work."
      };
    case NavigationStyle::TouchpadGesture:
      return {
        "Shift+LMB drag",
        "Ctrl+LMB drag",
        "Ctrl+Shift+LMB drag",
        "Trackpad-friendly: every action uses LMB so no MMB/RMB is required."
      };
  }
  return {};
}

// Human-readable hint shown next to the combo. Mentions rotation feel
// because that is what users notice first when switching presets; wheel
// always zooms regardless of preset.
inline std::string navigationStyleDescription(NavigationStyle s)
{
  switch (s)
  {
    case NavigationStyle::Netfabb:
      return "Rotate: MMB+Shift, RMB   "
             "Pan: MMB, RMB+Ctrl   "
             "Zoom: MMB+Ctrl, RMB+Shift   "
             "(trackball, free roll)";
    case NavigationStyle::FreeCAD_CAD:
      return "Rotate: LMB+MMB   Pan: MMB   Zoom: RMB+MMB   "
             "(turntable, Z-up preserved)";
    case NavigationStyle::Blender:
      return "Rotate: MMB   Pan: Shift+MMB   Zoom: Ctrl+MMB   "
             "(turntable, Z-up preserved)";
    case NavigationStyle::Maya:
      return "Rotate: Alt+LMB   Pan: Alt+MMB   Zoom: Alt+RMB   "
             "(turntable, Z-up preserved)";
    case NavigationStyle::Inventor:
      return "Rotate: Shift+MMB   Pan: MMB   Zoom: Ctrl+MMB   "
             "(turntable, Z-up preserved)";
    case NavigationStyle::SolidWorks:
      return "Rotate: MMB   Pan: Ctrl+MMB   Zoom: Shift+MMB   "
             "(turntable, Z-up preserved)";
    case NavigationStyle::OpenSCAD:
      return "Rotate: Shift+LMB   Pan: RMB   Zoom: wheel only   "
             "(trackball, free roll; LMB reserved for part selection)";
    case NavigationStyle::TouchpadGesture:
      return "Rotate: Shift+LMB   Pan: Ctrl+LMB   "
             "Zoom: Ctrl+Shift+LMB   (turntable, Z-up preserved)";
  }
  return "";
}

// True when the preset itself binds Ctrl+Shift+LMB to a nav action. Used
// to suppress the universal Ctrl+Shift+LMB transient box-zoom so the
// preset's own zoom binding wins. The sticky Shift+B path is unaffected.
inline bool navigationStyleClaimsCtrlShiftLmb(NavigationStyle s)
{
  return s == NavigationStyle::TouchpadGesture;
}

// Build the mask uint16_t exactly the way MyTrackball::buildMask() does:
// button-bit OR'd with modifier bits. The two enum ranges don't overlap
// (buttons start at 256, modifiers fit in the low byte).
namespace navstyle_detail
{
  constexpr uint16_t lmb = vsg::BUTTON_MASK_1;
  constexpr uint16_t mmb = vsg::BUTTON_MASK_2;
  constexpr uint16_t rmb = vsg::BUTTON_MASK_3;
  constexpr uint16_t kShift = vsg::MODKEY_Shift;
  constexpr uint16_t kCtrl  = vsg::MODKEY_Control;
  constexpr uint16_t kAlt   = vsg::MODKEY_Alt;
}

// Replace `rotate`, `pan`, `zoom` with the masks for the given style.
inline void fillNavigationStyleMasks(NavigationStyle s,
                                     std::set<uint32_t>& rotate,
                                     std::set<uint32_t>& pan,
                                     std::set<uint32_t>& zoom)
{
  using namespace navstyle_detail;
  rotate.clear();
  pan.clear();
  zoom.clear();

  switch (s)
  {
    case NavigationStyle::Netfabb:
      // Current XjetStudio default — preserved exactly.
      pan.insert(mmb);
      rotate.insert(mmb | kShift);
      zoom.insert(mmb | kCtrl);
      rotate.insert(rmb);
      pan.insert(rmb | kCtrl);
      zoom.insert(rmb | kShift);
      break;

    case NavigationStyle::FreeCAD_CAD:
      // FreeCAD's "CAD" style uses chords (two buttons at once).
      rotate.insert(lmb | mmb);
      pan.insert(mmb);
      zoom.insert(rmb | mmb);
      break;

    case NavigationStyle::Blender:
      rotate.insert(mmb);
      pan.insert(mmb | kShift);
      zoom.insert(mmb | kCtrl);
      break;

    case NavigationStyle::Maya:
      // The "Alt + button" tumble/track/dolly convention.
      rotate.insert(lmb | kAlt);
      pan.insert(mmb | kAlt);
      zoom.insert(rmb | kAlt);
      break;

    case NavigationStyle::Inventor:
      rotate.insert(mmb | kShift);
      pan.insert(mmb);
      zoom.insert(mmb | kCtrl);
      break;

    case NavigationStyle::SolidWorks:
      rotate.insert(mmb);
      pan.insert(mmb | kCtrl);
      zoom.insert(mmb | kShift);
      break;

    case NavigationStyle::OpenSCAD:
      // Rotate is on Shift+LMB rather than bare LMB so XjetStudio's
      // LMB-click/drag part selection still works. RMB pans. Drag-zoom
      // is intentionally absent — OpenSCAD users zoom with the wheel.
      rotate.insert(lmb | kShift);
      pan.insert(rmb);
      break;

    case NavigationStyle::TouchpadGesture:
      // Trackpad-friendly: every action is LMB + a modifier, so users
      // without an MMB/RMB can still navigate fully.
      rotate.insert(lmb | kShift);
      pan.insert(lmb | kCtrl);
      zoom.insert(lmb | kShift | kCtrl);
      break;
  }
}

#endif /* NAVIGATIONSTYLES_H */
