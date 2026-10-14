#include "SceneSettingsDialog.h"

#include "Viewport3D.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace
{
  const QColor DefaultModelColor(0xed, 0xed, 0xed); // linear 0.85, as the models were always drawn
  constexpr double DefaultMetallic = 0.0;
  constexpr double DefaultRoughness = 0.5;
  constexpr int SliderSteps = 1000;

  void setSwatch(QPushButton* button, const QColor& color)
  {
    button->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #808080; }").arg(color.name()));
    button->setToolTip(color.name());
  }

  QColor storedColor(const QSettings& settings, const char* key, const QColor& fallback)
  {
    const QColor color = settings.value(key, fallback).value<QColor>();
    return color.isValid() ? color : fallback;
  }
} // namespace

vsg::vec4 linearColor(const QColor& color)
{
  auto toLinear = [](qreal c) { return c <= 0.04045 ? float(c / 12.92) : float(std::pow((c + 0.055) / 1.055, 2.4)); };
  return vsg::vec4(toLinear(color.redF()), toLinear(color.greenF()), toLinear(color.blueF()), 1.0f);
}

SceneSettingsDialog::SceneSettingsDialog(Viewport3D* viewport, QWidget* parent) : QDialog(parent), m_Viewport(viewport)
{
  setWindowTitle("Scene Settings");

  const QSettings settings;
  m_Viewport->setBackground(storedColor(settings, "View/BackgroundColor", Viewport3D::defaultBackground()));
  m_Viewport->setBackgroundBottom(storedColor(settings, "View/BackgroundBottomColor", Viewport3D::defaultBackgroundBottom()));
  m_Viewport->setGradientBackground(settings.value("View/GradientBackground", false).toBool());
  m_ModelColor = storedColor(settings, "View/ModelColor", DefaultModelColor);
  m_OverrideMaterial = settings.value("Material/Override", false).toBool();
  m_Metallic = std::clamp(settings.value("Material/Metallic", DefaultMetallic).toDouble(), 0.0, 1.0);
  m_Roughness = std::clamp(settings.value("Material/Roughness", DefaultRoughness).toDouble(), 0.0, 1.0);
  applyMaterial();

  auto colorButton = [this]() {
    auto* button = new QPushButton(this);
    button->setFixedSize(56, 22);
    return button;
  };

  auto* background = new QGroupBox("Background", this);
  auto* backgroundForm = new QFormLayout(background);
  m_TopButton = colorButton();
  m_Gradient = new QCheckBox("Gradient", this);
  m_BottomButton = colorButton();
  backgroundForm->addRow("Color (top):", m_TopButton);
  backgroundForm->addRow(m_Gradient);
  backgroundForm->addRow("Bottom:", m_BottomButton);

  auto* models = new QGroupBox("STL and 3MF models", this);
  auto* modelsForm = new QFormLayout(models);
  m_ModelButton = colorButton();
  modelsForm->addRow("Color:", m_ModelButton);

  auto* materials = new QGroupBox("Material", this);
  auto* materialsForm = new QFormLayout(materials);
  m_Override = new QCheckBox("Use PBR shader", this);
  m_Override->setToolTip("Draw STL and 3MF models with the physically based shader, and replace the metallic and roughness of glTF materials");
  materialsForm->addRow(m_Override);
  // A slider with a spin box beside it, for a value from 0 to 1.
  auto valueRow = [this](QSlider*& slider, QDoubleSpinBox*& spin) {
    slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(0, SliderSteps);
    spin = new QDoubleSpinBox(this);
    spin->setRange(0.0, 1.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    auto* row = new QHBoxLayout;
    row->addWidget(slider, 1);
    row->addWidget(spin);
    return row;
  };
  materialsForm->addRow("Metallic:", valueRow(m_MetallicSlider, m_MetallicSpin));
  materialsForm->addRow("Roughness:", valueRow(m_RoughnessSlider, m_RoughnessSpin));

  auto* reset = new QPushButton("Reset to Defaults", this);
  auto* close = new QPushButton("Close", this);
  close->setDefault(true);
  auto* buttons = new QHBoxLayout;
  buttons->addWidget(reset);
  buttons->addStretch(1);
  buttons->addWidget(close);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(background);
  layout->addWidget(models);
  layout->addWidget(materials);
  layout->addLayout(buttons);
  setMinimumWidth(340);

  connect(m_TopButton, &QPushButton::clicked, this, [this]() {
    const QColor initial = m_Viewport->background();
    if (pickColor("Background Color", initial, [this](const QColor& c) { m_Viewport->setBackground(c); })) updateWidgets();
  });
  connect(m_BottomButton, &QPushButton::clicked, this, [this]() {
    if (pickColor("Gradient Bottom Color", m_Viewport->backgroundBottom(), [this](const QColor& c) { m_Viewport->setBackgroundBottom(c); })) updateWidgets();
  });
  connect(m_Gradient, &QCheckBox::toggled, this, [this](bool on) {
    m_Viewport->setGradientBackground(on);
    updateWidgets();
  });
  connect(m_ModelButton, &QPushButton::clicked, this, [this]() {
    auto apply = [this](const QColor& c) {
      m_ModelColor = c;
      emit modelColorChanged(c);
    };
    pickColor("Model Color (STL and 3MF)", m_ModelColor, apply);
    updateWidgets();
  });
  connect(m_Override, &QCheckBox::toggled, this, [this](bool on) {
    m_OverrideMaterial = on;
    applyMaterial();
    updateWidgets();
    emit usePbrChanged(on);
  });
  connect(m_MetallicSlider, &QSlider::valueChanged, this, [this](int v) {
    m_Metallic = double(v) / SliderSteps;
    applyMaterial();
    updateWidgets();
  });
  connect(m_RoughnessSlider, &QSlider::valueChanged, this, [this](int v) {
    m_Roughness = double(v) / SliderSteps;
    applyMaterial();
    updateWidgets();
  });
  connect(m_MetallicSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
    m_Metallic = v;
    applyMaterial();
    updateWidgets();
  });
  connect(m_RoughnessSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
    m_Roughness = v;
    applyMaterial();
    updateWidgets();
  });
  connect(reset, &QPushButton::clicked, this, &SceneSettingsDialog::resetToDefaults);
  connect(close, &QPushButton::clicked, this, &QDialog::hide);
  updateWidgets();
}

SceneSettingsDialog::~SceneSettingsDialog()
{
  save();
}

bool SceneSettingsDialog::pickColor(const QString& title, const QColor& initial, const std::function<void(const QColor&)>& apply)
{
  QColorDialog dialog(initial, this);
  dialog.setWindowTitle(title);
  connect(&dialog, &QColorDialog::currentColorChanged, &dialog, apply);
  if (dialog.exec() == QDialog::Accepted)
  {
    apply(dialog.selectedColor());
    return true;
  }
  apply(initial);
  return false;
}

void SceneSettingsDialog::applyMaterial()
{
  m_Viewport->setMaterialOverride(m_OverrideMaterial, static_cast<float>(m_Metallic), static_cast<float>(m_Roughness));
}

// Brings the widgets in line with the state, without feeding back into it.
void SceneSettingsDialog::updateWidgets()
{
  const QSignalBlocker blockGradient(m_Gradient), blockOverride(m_Override), blockMetallicSlider(m_MetallicSlider),
      blockRoughnessSlider(m_RoughnessSlider), blockMetallicSpin(m_MetallicSpin), blockRoughnessSpin(m_RoughnessSpin);
  setSwatch(m_TopButton, m_Viewport->background());
  setSwatch(m_BottomButton, m_Viewport->backgroundBottom());
  setSwatch(m_ModelButton, m_ModelColor);
  m_Gradient->setChecked(m_Viewport->isGradientBackground());
  m_BottomButton->setEnabled(m_Viewport->isGradientBackground());
  m_Override->setChecked(m_OverrideMaterial);
  m_MetallicSlider->setValue(qRound(m_Metallic * SliderSteps));
  m_RoughnessSlider->setValue(qRound(m_Roughness * SliderSteps));
  m_MetallicSpin->setValue(m_Metallic);
  m_RoughnessSpin->setValue(m_Roughness);
  for (QWidget* w : {static_cast<QWidget*>(m_MetallicSlider), static_cast<QWidget*>(m_RoughnessSlider), static_cast<QWidget*>(m_MetallicSpin),
                     static_cast<QWidget*>(m_RoughnessSpin)})
    w->setEnabled(m_OverrideMaterial);
}

void SceneSettingsDialog::resetToDefaults()
{
  m_Viewport->setBackground(Viewport3D::defaultBackground());
  m_Viewport->setBackgroundBottom(Viewport3D::defaultBackgroundBottom());
  m_Viewport->setGradientBackground(false);
  m_ModelColor = DefaultModelColor;
  emit modelColorChanged(m_ModelColor);
  const bool wasPbr = m_OverrideMaterial;
  m_OverrideMaterial = false;
  m_Metallic = DefaultMetallic;
  m_Roughness = DefaultRoughness;
  applyMaterial();
  updateWidgets();
  if (wasPbr) emit usePbrChanged(false);
}

void SceneSettingsDialog::setUsePbr(bool on)
{
  m_Override->setChecked(on);
}

void SceneSettingsDialog::hideEvent(QHideEvent* event)
{
  QDialog::hideEvent(event);
  save();
}

void SceneSettingsDialog::save() const
{
  QSettings settings;
  settings.setValue("View/BackgroundColor", m_Viewport->background());
  settings.setValue("View/BackgroundBottomColor", m_Viewport->backgroundBottom());
  settings.setValue("View/GradientBackground", m_Viewport->isGradientBackground());
  settings.setValue("View/ModelColor", m_ModelColor);
  settings.setValue("Material/Override", m_OverrideMaterial);
  settings.setValue("Material/Metallic", m_Metallic);
  settings.setValue("Material/Roughness", m_Roughness);
}
