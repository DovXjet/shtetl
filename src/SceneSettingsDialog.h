#pragma once

#include <vsg/maths/vec4.h>

#include <QColor>
#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class Viewport3D;

// sRGB to the linear values the shaders take.
vsg::vec4 linearColor(const QColor& color);

// The settings of the 3D scene in one place: the background (solid or a
// gradient), the color of STL and 3MF models, and the metallic / roughness
// that override the materials of glTF models. Not modal: every change shows in
// the view at once. Restored from, and saved to, QSettings.
class SceneSettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    SceneSettingsDialog(Viewport3D* viewport, QWidget* parent);
    ~SceneSettingsDialog() override;

    // The color STL and 3MF models are loaded with.
    QColor modelColor() const { return m_ModelColor; }

  signals:
    void modelColorChanged(const QColor& color);

  private:
    void hideEvent(QHideEvent* event) override;
    void save() const;
    void resetToDefaults();
    void updateWidgets(); // from the state
    void applyMaterial();
    // Asks for a color, reporting the choice at once through `apply`;
    // cancelling restores `initial`. Returns whether a color was accepted.
    bool pickColor(const QString& title, const QColor& initial, const std::function<void(const QColor&)>& apply);

    Viewport3D* m_Viewport;
    QColor m_ModelColor;
    bool m_OverrideMaterial = false;
    double m_Metallic = 0.0, m_Roughness = 0.5;

    QPushButton* m_TopButton = nullptr;
    QPushButton* m_BottomButton = nullptr;
    QPushButton* m_ModelButton = nullptr;
    QCheckBox* m_Gradient = nullptr;
    QCheckBox* m_Override = nullptr;
    QSlider* m_MetallicSlider = nullptr;
    QSlider* m_RoughnessSlider = nullptr;
    QDoubleSpinBox* m_MetallicSpin = nullptr;
    QDoubleSpinBox* m_RoughnessSpin = nullptr;
};
