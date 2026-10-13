#pragma once

// Clickable orientation cube drawn in the top-right corner of the viewport.
// It tracks the scene camera's rotation, and clicking one of its faces,
// edges or corners animates the scene camera to look from that direction.
// Ported from XjetStudio's src/cube-gizmo.{h,cpp}; the cube geometry is the
// same set of STL files, embedded here as Qt resources under :/cube-gizmo.

#include <vsg/app/Camera.h>
#include <vsg/app/View.h>
#include <vsg/core/Visitor.h>
#include <vsg/io/Options.h>

#include <functional>

class MyTrackball;

class CCubeGizmo : public vsg::Inherit<vsg::Object, CCubeGizmo>
{
  public:
    // Throws std::runtime_error if the embedded cube meshes can't be read.
    CCubeGizmo(vsg::ref_ptr<vsg::Options> options, vsg::ref_ptr<vsg::Camera> sceneCamera, vsg::ref_ptr<MyTrackball> trackball);

    // The overlay view; add it to the render graph after the scene's view.
    vsg::ref_ptr<vsg::View> GetView() const { return m_View; }

    // Handles clicks on the cube. Add it to the viewer's event handlers
    // after the trackball.
    vsg::ref_ptr<vsg::Visitor> GetIntersectionHandler() const { return m_IntersectionHandler; }

    // Handles right-clicks on the cube by invoking the context-menu callback.
    // Add it to the viewer's event handlers *before* the trackball, since
    // some navigation presets bind the right button.
    vsg::ref_ptr<vsg::Visitor> GetContextMenuHandler() const { return m_ContextMenuHandler; }
    void SetContextMenuCallback(std::function<void()> cb) { m_ContextMenuCallback = std::move(cb); }

  private:
    vsg::ref_ptr<vsg::Visitor> m_ContextMenuHandler;
    std::function<void()> m_ContextMenuCallback;
    vsg::ref_ptr<vsg::View> m_View;
    vsg::ref_ptr<vsg::Visitor> m_IntersectionHandler;
};
