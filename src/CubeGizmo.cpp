#include "CubeGizmo.h"

#include <functional>

#include "MyTrackball.h"

#include <vsg/app/ProjectionMatrix.h>
#include <vsg/app/ViewMatrix.h>
#include <vsg/commands/Command.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/maths/transform.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/VertexDraw.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/material.h>
#include <vsg/state/ViewportState.h>
#include <vsg/ui/PointerEvent.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/LineSegmentIntersector.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/CommandBuffer.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace
{
    // Camera direction for every clickable cube part. The key is the part
    // name (the STL file name minus "face_"): one letter per axis X,Y,Z -
    // M(inus), P(lus) or Z(ero) - giving the side of the cube the camera
    // looks from.
    const std::map<std::string, vsg::ref_ptr<vsg::LookAt>>& getLookAtMatrices()
    {
        static const vsg::dvec3 center(0, 0, 0);
        static const std::map<std::string, vsg::ref_ptr<vsg::LookAt>> lookAtMatrices = {
            {"MZZ", vsg::LookAt::create(vsg::dvec3(-1, 0, 0), center, vsg::dvec3(0, 0, 1))}, // left
            {"PZZ", vsg::LookAt::create(vsg::dvec3(1, 0, 0), center, vsg::dvec3(0, 0, 1))},  // right
            {"ZMZ", vsg::LookAt::create(vsg::dvec3(0, -1, 0), center, vsg::dvec3(0, 0, 1))}, // front
            {"ZPZ", vsg::LookAt::create(vsg::dvec3(0, 1, 0), center, vsg::dvec3(0, 0, 1))},  // back
            {"ZZM", vsg::LookAt::create(vsg::dvec3(0, 0, -1), center, vsg::dvec3(0, -1, 0))}, // bottom
            {"ZZP", vsg::LookAt::create(vsg::dvec3(0, 0, 1), center, vsg::dvec3(0, 1, 0))},  // top

            // edges
            {"MMZ", vsg::LookAt::create(vsg::dvec3(-1, -1, 0), center, vsg::dvec3(0, 0, 1))},
            {"MPZ", vsg::LookAt::create(vsg::dvec3(-1, 1, 0), center, vsg::dvec3(0, 0, 1))},
            {"PMZ", vsg::LookAt::create(vsg::dvec3(1, -1, 0), center, vsg::dvec3(0, 0, 1))},
            {"PPZ", vsg::LookAt::create(vsg::dvec3(1, 1, 0), center, vsg::dvec3(0, 0, 1))},

            {"ZMM", vsg::LookAt::create(vsg::dvec3(0, -1, -1), center, vsg::dvec3(0, 0, 1))},
            {"ZMP", vsg::LookAt::create(vsg::dvec3(0, -1, 1), center, vsg::dvec3(0, 0, 1))},
            {"ZPM", vsg::LookAt::create(vsg::dvec3(0, 1, -1), center, vsg::dvec3(0, 0, 1))},
            {"ZPP", vsg::LookAt::create(vsg::dvec3(0, 1, 1), center, vsg::dvec3(0, 0, 1))},

            {"MZM", vsg::LookAt::create(vsg::dvec3(-1, 0, -1), center, vsg::dvec3(0, 0, 1))},
            {"PZM", vsg::LookAt::create(vsg::dvec3(1, 0, -1), center, vsg::dvec3(0, 0, 1))},
            {"MZP", vsg::LookAt::create(vsg::dvec3(-1, 0, 1), center, vsg::dvec3(0, 0, 1))},
            {"PZP", vsg::LookAt::create(vsg::dvec3(1, 0, 1), center, vsg::dvec3(0, 0, 1))},

            // corners
            {"PPP", vsg::LookAt::create(vsg::dvec3(1, 1, 1), center, vsg::dvec3(0, 0, 1))},
            {"PPM", vsg::LookAt::create(vsg::dvec3(1, 1, -1), center, vsg::dvec3(0, 0, 1))},
            {"PMP", vsg::LookAt::create(vsg::dvec3(1, -1, 1), center, vsg::dvec3(0, 0, 1))},
            {"PMM", vsg::LookAt::create(vsg::dvec3(1, -1, -1), center, vsg::dvec3(0, 0, 1))},
            {"MPP", vsg::LookAt::create(vsg::dvec3(-1, 1, 1), center, vsg::dvec3(0, 0, 1))},
            {"MPM", vsg::LookAt::create(vsg::dvec3(-1, 1, -1), center, vsg::dvec3(0, 0, 1))},
            {"MMP", vsg::LookAt::create(vsg::dvec3(-1, -1, 1), center, vsg::dvec3(0, 0, 1))},
            {"MMM", vsg::LookAt::create(vsg::dvec3(-1, -1, -1), center, vsg::dvec3(0, 0, 1))},
        };
        return lookAtMatrices;
    }

    // The rotation part of another view matrix, so the cube turns with the
    // scene camera but stays put in its corner.
    class RotationTrackingMatrix : public vsg::Inherit<vsg::ViewMatrix, RotationTrackingMatrix>
    {
      public:
        explicit RotationTrackingMatrix(vsg::ref_ptr<vsg::ViewMatrix> parent) : m_Parent(parent) {}

        vsg::dmat4 transform(const vsg::dvec3& /*offset*/ = {}) const override
        {
            vsg::dvec3 translation, scale;
            vsg::dquat rotation;
            vsg::decompose(m_Parent->transform(), translation, rotation, scale);
            return vsg::rotate(rotation);
        }

      private:
        vsg::ref_ptr<vsg::ViewMatrix> m_Parent;
    };

    // Orthographic projection that places the cube in the top-right corner
    // of the viewport. It reads the aspect ratio from the (shared, resized)
    // viewport on every use, so window resizes need no extra bookkeeping;
    // vsg's WindowResizeHandler leaves non-standard projections alone.
    class GizmoProjection : public vsg::Inherit<vsg::ProjectionMatrix, GizmoProjection>
    {
      public:
        explicit GizmoProjection(vsg::ref_ptr<vsg::ViewportState> viewportState) : m_ViewportState(viewportState) {}

        vsg::dmat4 transform() const override
        {
            // Same frustum as XjetStudio: the cube spans 1/15 of it.
            const double camWidth = 15;
            const double camXOffs = -14;
            const double camYOffs = -14;
            auto viewport = m_ViewportState->getViewport();
            double aspectRatio = viewport.height > 0 ? viewport.width / viewport.height : 1.0;
            return vsg::orthographic(camXOffs, camXOffs + camWidth, camYOffs / aspectRatio, (camYOffs + camWidth) / aspectRatio, -10.0, 1000.0);
        }

      private:
        vsg::ref_ptr<vsg::ViewportState> m_ViewportState;
    };

    // Clears the depth buffer before the cube is drawn, so the machine model
    // behind the top-right corner can never hide it.
    class ClearDepth : public vsg::Inherit<vsg::Command, ClearDepth>
    {
      public:
        explicit ClearDepth(vsg::ref_ptr<vsg::ViewportState> viewportState) : m_ViewportState(viewportState) {}

        void record(vsg::CommandBuffer& commandBuffer) const override
        {
            VkClearAttachment attachment{};
            attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            attachment.clearValue.depthStencil = {0.0f, 0}; // vsg uses reversed depth
            VkClearRect rect{};
            rect.rect = m_ViewportState->scissors.empty() ? VkRect2D{} : m_ViewportState->scissors[0];
            rect.layerCount = 1;
            if (rect.rect.extent.width == 0 || rect.rect.extent.height == 0) return;
            vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &rect);
        }

      private:
        vsg::ref_ptr<vsg::ViewportState> m_ViewportState;
    };

    // Reads a binary STL from the Qt resource system into a flat-shaded,
    // single-color mesh node.
    vsg::ref_ptr<vsg::Node> loadStlNode(const QString& path, const vsg::vec4& color, vsg::ref_ptr<vsg::ShaderSet> shaderSet, const std::string& name)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open " + path.toStdString());
        QByteArray bytes = file.readAll();

        const qsizetype headerSize = 84, triangleSize = 50;
        if (bytes.size() < headerSize) throw std::runtime_error("Truncated STL " + path.toStdString());
        uint32_t numTriangles;
        std::memcpy(&numTriangles, bytes.constData() + 80, sizeof(numTriangles));
        if (bytes.size() < headerSize + qsizetype(numTriangles) * triangleSize) throw std::runtime_error("Truncated STL " + path.toStdString());

        auto vertices = vsg::vec3Array::create(numTriangles * 3);
        auto normals = vsg::vec3Array::create(numTriangles * 3);
        for (uint32_t i = 0; i < numTriangles; ++i)
        {
            // Skip the stored facet normal; recompute it from the winding.
            const char* triangle = bytes.constData() + headerSize + i * triangleSize + 12;
            float coordinates[9];
            std::memcpy(coordinates, triangle, sizeof(coordinates));
            const vsg::vec3 v[3] = {{coordinates[0], coordinates[1], coordinates[2]},
                                    {coordinates[3], coordinates[4], coordinates[5]},
                                    {coordinates[6], coordinates[7], coordinates[8]}};
            vsg::vec3 normal = vsg::cross(v[1] - v[0], v[2] - v[0]);
            float length = vsg::length(normal);
            if (length > 0.0f) normal /= length;
            for (int j = 0; j < 3; ++j)
            {
                vertices->set(i * 3 + j, v[j]);
                normals->set(i * 3 + j, normal);
            }
        }

        auto config = vsg::GraphicsPipelineConfigurator::create(shaderSet);
        config->assignDescriptor("material", vsg::PhongMaterialValue::create());

        vsg::DataList arrays;
        config->assignArray(arrays, "vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, vertices);
        config->assignArray(arrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, normals);
        config->assignArray(arrays, "vsg_TexCoord0", VK_VERTEX_INPUT_RATE_INSTANCE, vsg::vec2Array::create(1));
        config->assignArray(arrays, "vsg_Color", VK_VERTEX_INPUT_RATE_INSTANCE, vsg::vec4Array::create({color}));

        // The STL windings aren't consistent, so light and draw both sides.
        config->shaderHints->defines.insert("VSG_TWO_SIDED_LIGHTING");
        struct DisableCulling : public vsg::Visitor
        {
            void apply(vsg::Object& object) override { object.traverse(*this); }
            void apply(vsg::RasterizationState& rs) override { rs.cullMode = VK_CULL_MODE_NONE; }
        } disableCulling;
        config->accept(disableCulling);
        config->init();

        auto stateGroup = vsg::StateGroup::create();
        config->copyTo(stateGroup);

        auto draw = vsg::VertexDraw::create();
        draw->assignArrays(arrays);
        draw->vertexCount = numTriangles * 3;
        draw->instanceCount = 1;
        // A non-empty name makes the part clickable (see GizmoIntersectionHandler).
        draw->setValue("name", name);
        stateGroup->addChild(draw);
        return stateGroup;
    }

    class GizmoIntersectionHandler : public vsg::Inherit<vsg::Visitor, GizmoIntersectionHandler>
    {
      public:
        GizmoIntersectionHandler(vsg::ref_ptr<vsg::Node> scene, vsg::ref_ptr<vsg::Camera> gizmoCamera, vsg::ref_ptr<vsg::Camera> sceneCamera, vsg::ref_ptr<MyTrackball> trackball, uint32_t button = 1, std::function<void()> onClick = nullptr)
            : m_Scene(scene), m_GizmoCamera(gizmoCamera), m_SceneCamera(sceneCamera), m_Trackball(trackball), m_Button(button), m_OnClick(std::move(onClick))
        {
        }

        void apply(vsg::ButtonPressEvent& buttonPressEvent) override
        {
            if (buttonPressEvent.handled || buttonPressEvent.button != m_Button) return;

            intersection(buttonPressEvent);
            buttonPressEvent.handled = m_Intersected;
            m_PressPos = vsg::vec2(buttonPressEvent.x, buttonPressEvent.y);
            m_ButtonPressed = true;
        }

        void apply(vsg::MoveEvent& moveEvent) override
        {
            if (!m_ButtonPressed || moveEvent.handled) return;

            // Swallow drags that are over the cube.
            intersection(moveEvent);
            moveEvent.handled = m_Intersected;
        }

        void apply(vsg::ButtonReleaseEvent& buttonReleaseEvent) override
        {
            if (buttonReleaseEvent.handled || buttonReleaseEvent.button != m_Button) return;

            m_ButtonPressed = false;
            buttonReleaseEvent.handled = m_Intersected;
            // Only a click (not a drag) turns the view.
            float dragLength = vsg::length(vsg::vec2(buttonReleaseEvent.x, buttonReleaseEvent.y) - m_PressPos);
            if (m_Intersected && dragLength < 5)
            {
                if (m_OnClick) m_OnClick();
                else rotateToFace();
            }
            m_Intersected = false;
        }

      private:
        vsg::ref_ptr<vsg::Node> m_Scene;
        vsg::ref_ptr<vsg::Camera> m_GizmoCamera;
        vsg::ref_ptr<vsg::Camera> m_SceneCamera;
        vsg::ref_ptr<MyTrackball> m_Trackball;
        uint32_t m_Button;
        std::function<void()> m_OnClick; // when set, replaces rotateToFace()
        vsg::vec2 m_PressPos = {0, 0};
        std::string m_RotationFace;
        bool m_Intersected = false;
        bool m_ButtonPressed = false;

        // Sets m_RotationFace/m_Intersected from the frontmost named cube part
        // under the pointer. Leaves them unchanged when nothing is hit.
        void intersection(vsg::PointerEvent& pointerEvent)
        {
            auto intersector = vsg::LineSegmentIntersector::create(*m_GizmoCamera, pointerEvent.x, pointerEvent.y);
            m_Scene->accept(*intersector);

            std::sort(intersector->intersections.begin(), intersector->intersections.end(), [](auto& lhs, auto& rhs) { return lhs->ratio < rhs->ratio; });

            for (auto& hit : intersector->intersections)
                for (const vsg::Node* node : hit->nodePath)
                {
                    std::string name;
                    if (node->is_compatible(typeid(vsg::VertexDraw)) && node->getValue("name", name) && !name.empty())
                    {
                        m_RotationFace = name;
                        m_Intersected = true;
                        return;
                    }
                }
        }

        void rotateToFace()
        {
            const auto& lookAtMatrices = getLookAtMatrices();
            auto itr = lookAtMatrices.find(m_RotationFace);
            auto sceneLookAt = m_SceneCamera->viewMatrix.cast<vsg::LookAt>();
            if (itr == lookAtMatrices.end() || !sceneLookAt) return;

            // Look from the clicked direction at the current center, keeping
            // the current eye distance.
            double distance = vsg::length(sceneLookAt->eye - sceneLookAt->center);
            const auto& direction = itr->second;
            auto lookAt = vsg::LookAt::create(sceneLookAt->center + vsg::normalize(direction->eye - direction->center) * distance, sceneLookAt->center, direction->up);

            // The gizmo camera follows the scene camera automatically.
            m_Trackball->setViewpoint(lookAt, 0.5);
        }
    };
} // namespace

CCubeGizmo::CCubeGizmo(vsg::ref_ptr<vsg::Options> options, vsg::ref_ptr<vsg::Camera> sceneCamera, vsg::ref_ptr<MyTrackball> trackball)
{
    auto shaderSet = vsg::createPhongShaderSet(options);

    auto scene = vsg::Group::create();
    const QDir dir(":/cube-gizmo");
    const QStringList files = dir.entryList({"*.stl"}, QDir::Files);
    if (files.isEmpty()) throw std::runtime_error("No orientation cube meshes found in :/cube-gizmo");
    for (const QString& file : files)
    {
        std::string base = QFileInfo(file).completeBaseName().toStdString();

        // face_XYZ.stl are the clickable parts; the rest (labels) are
        // decoration only.
        std::string name;
        if (base.starts_with("face_")) name = base.substr(5);

        // Same styling as XjetStudio: white faces, pink cube body, red labels.
        vsg::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
        if (base.find("ZZZ") != std::string::npos) color = vsg::vec4(1.0f, 0.5f, 0.5f, 1.0f);
        if (base.find("label") != std::string::npos) color = vsg::vec4(1.0f, 0.0f, 0.0f, 1.0f);

        scene->addChild(loadStlNode(dir.filePath(file), color, shaderSet, name));
    }

    auto gizmoCamera = vsg::Camera::create(GizmoProjection::create(sceneCamera->viewportState), RotationTrackingMatrix::create(sceneCamera->viewMatrix), sceneCamera->viewportState);

    m_View = vsg::View::create(gizmoCamera);
    m_View->addChild(ClearDepth::create(sceneCamera->viewportState));
    m_View->addChild(vsg::createHeadlight());
    m_View->addChild(scene);

    m_IntersectionHandler = GizmoIntersectionHandler::create(scene, gizmoCamera, sceneCamera, trackball);
    m_ContextMenuHandler = GizmoIntersectionHandler::create(scene, gizmoCamera, sceneCamera, trackball, 3, [this]() {
        if (m_ContextMenuCallback) m_ContextMenuCallback();
    });
}
