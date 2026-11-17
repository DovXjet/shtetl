#include "Preferences.h"

#include "SceneSettingsPage.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
  // The bold heading at the top of a page.
  QLabel* pageTitle(const QString& text, QWidget* parent)
  {
    auto* title = new QLabel(text, parent);
    QFont font = title->font();
    font.setPointSize(font.pointSize() + 4);
    font.setBold(true);
    title->setFont(font);
    return title;
  }
} // namespace

Preferences::Preferences(QSettings* settings, SceneSettingsPage* scenePage, QWidget* parent) : QDialog(parent), m_Settings(settings), m_ScenePage(scenePage)
{
  createUi();
  loadSettings();
}

void Preferences::createUi()
{
  auto* mainLayout = new QHBoxLayout(this);
  setWindowTitle(tr("shtetl Preferences"));

  m_Tree = new QTreeWidget(this);
  m_Tree->setHeaderHidden(true);
  m_Tree->setMinimumWidth(160);

  auto* sceneItem = new QTreeWidgetItem(m_Tree);
  sceneItem->setText(0, tr("Scene"));
  auto* occlusionItem = new QTreeWidgetItem(m_Tree);
  occlusionItem->setText(0, tr("Ambient occlusion"));
  auto* importItem = new QTreeWidgetItem(m_Tree);
  importItem->setText(0, tr("Import"));
  auto* stepItem = new QTreeWidgetItem(importItem);
  stepItem->setText(0, tr("STEP / IGES"));
  m_Tree->expandAll();

  // The page each item shows is its index in the stack; a category with no page of
  // its own (Import) shows the first of its children.
  m_Stack = new QStackedWidget(this);
  addStepPage();
  stepItem->setData(0, Qt::UserRole, 0);
  importItem->setData(0, Qt::UserRole, 0);
  addOcclusionPage();
  occlusionItem->setData(0, Qt::UserRole, OcclusionPage);
  // The scene settings are taller than the window: scrolled.
  auto* sceneScroll = new QScrollArea(this);
  sceneScroll->setWidget(m_ScenePage);
  sceneScroll->setWidgetResizable(true);
  sceneScroll->setFrameShape(QFrame::NoFrame);
  m_Stack->addWidget(sceneScroll); // ScenePage
  sceneItem->setData(0, Qt::UserRole, ScenePage);
  m_PageItems = {stepItem, occlusionItem, sceneItem};

  auto* okButton = new QPushButton(tr("OK"), this);
  auto* cancelButton = new QPushButton(tr("Close"), this);
  cancelButton->setToolTip(tr("The scene settings and the ambient occlusion are already applied; unapplied import values are dropped"));
  auto* applyButton = new QPushButton(tr("Apply"), this);
  okButton->setDefault(true);
  auto* buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch();
  buttonLayout->addWidget(okButton);
  buttonLayout->addWidget(cancelButton);
  buttonLayout->addWidget(applyButton);

  auto* rightLayout = new QVBoxLayout();
  rightLayout->addWidget(m_Stack);
  rightLayout->addLayout(buttonLayout);

  mainLayout->addWidget(m_Tree);
  mainLayout->addLayout(rightLayout, 1);

  connect(m_Tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { onCategoryChanged(item); });
  connect(okButton, &QPushButton::clicked, this, &Preferences::onOk);
  connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
  connect(applyButton, &QPushButton::clicked, this, &Preferences::onApply);

  m_Tree->setCurrentItem(sceneItem);
  resize(620, 560);
}

void Preferences::addStepPage()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->addWidget(pageTitle(tr("STEP / IGES"), page));

  auto* note = new QLabel(tr("How finely STEP and IGES models are turned into triangles when they are opened. "
                             "Smaller values give smoother curves, but take longer and use more memory."), page);
  note->setWordWrap(true);
  layout->addWidget(note);

  m_StepLinearDeflection = new QDoubleSpinBox(page);
  m_StepLinearDeflection->setRange(0.0001, 100.0);
  m_StepLinearDeflection->setDecimals(4);
  m_StepLinearDeflection->setSingleStep(0.005);
  m_StepLinearDeflection->setSuffix(tr(" mm"));
  m_StepLinearDeflection->setToolTip(tr("The largest distance between the triangles and the true surface."));

  m_StepAngularDeflection = new QDoubleSpinBox(page);
  m_StepAngularDeflection->setRange(0.01, 3.14);
  m_StepAngularDeflection->setDecimals(3);
  m_StepAngularDeflection->setSingleStep(0.05);
  m_StepAngularDeflection->setSuffix(tr(" rad"));
  m_StepAngularDeflection->setToolTip(tr("The largest angle between the normals of neighbouring triangles."));

  auto* grid = new QGridLayout();
  grid->addWidget(new QLabel(tr("Import linear max deflection:"), page), 0, 0);
  grid->addWidget(m_StepLinearDeflection, 0, 1);
  grid->addWidget(new QLabel(tr("Import angular max deflection:"), page), 1, 0);
  grid->addWidget(m_StepAngularDeflection, 1, 1);
  grid->setColumnStretch(2, 1);
  layout->addLayout(grid);

  auto* reopen = new QLabel(tr("Applies to models opened afterwards (File > Reload to open the current one again). "
                               "Thumbnails already made are kept until they are refreshed."), page);
  reopen->setWordWrap(true);
  layout->addWidget(reopen);
  layout->addStretch();

  m_Stack->addWidget(page);
}

void Preferences::addOcclusionPage()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->addWidget(pageTitle(tr("Ambient occlusion"), page));

  auto* note = new QLabel(tr("Darkens the creases of the model (View > Ambient Occlusion switches it on). "
                             "Ctrl+Shift+A toggles it, Shift+F11 steps through the algorithms."), page);
  note->setWordWrap(true);
  layout->addWidget(note);

  m_OcclusionAlgorithm = new QComboBox(page);
  m_OcclusionAlgorithm->addItem(tr("SSAO (original)"));
  m_OcclusionAlgorithm->addItem(tr("CORTAO"));
  m_OcclusionAlgorithm->addItem(tr("Hybrid (CORTAO and SSAO)"));
  m_OcclusionAlgorithm->setItemData(0, tr("The hemisphere SSAO of XjetStudio"), Qt::ToolTipRole);
  m_OcclusionAlgorithm->setItemData(1, tr("Circular occluders, ray traced in compute shaders: no occlusion for walls seen edge-on"), Qt::ToolTipRole);
  m_OcclusionAlgorithm->setItemData(2, tr("The darker of the two at each pixel: CORTAO's creases, and the SSAO's steps in depth"), Qt::ToolTipRole);

  m_OcclusionSlider = new QSlider(Qt::Horizontal, page);
  m_OcclusionSlider->setRange(0, 100);
  m_OcclusionSpin = new QDoubleSpinBox(page);
  m_OcclusionSpin->setRange(0.0, 1.0);
  m_OcclusionSpin->setSingleStep(0.05);
  m_OcclusionSpin->setDecimals(2);
  m_OcclusionSlider->setToolTip(tr("How much the occlusion darkens: 0 is not at all, 1 is all of it"));
  connect(m_OcclusionAlgorithm, &QComboBox::currentIndexChanged, this, [this]() { onOcclusionEdited(); });
  // (The two follow each other.)
  connect(m_OcclusionSlider, &QSlider::valueChanged, this, [this](int v) {
    const QSignalBlocker block(m_OcclusionSpin);
    m_OcclusionSpin->setValue(v / 100.0);
    onOcclusionEdited();
  });
  connect(m_OcclusionSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
    const QSignalBlocker block(m_OcclusionSlider);
    m_OcclusionSlider->setValue(qRound(v * 100.0));
    onOcclusionEdited();
  });

  auto* strengthRow = new QHBoxLayout();
  strengthRow->addWidget(m_OcclusionSlider, 1);
  strengthRow->addWidget(m_OcclusionSpin);
  auto* grid = new QGridLayout();
  grid->addWidget(new QLabel(tr("Algorithm:"), page), 0, 0);
  grid->addWidget(m_OcclusionAlgorithm, 0, 1);
  grid->addWidget(new QLabel(tr("Strength:"), page), 1, 0);
  grid->addLayout(strengthRow, 1, 1);
  grid->setColumnStretch(1, 1);
  layout->addLayout(grid);
  layout->addStretch();

  m_Stack->addWidget(page);
}

// The keys of the STEP page are XjetStudio's, so the two programs agree.
void Preferences::loadSettings()
{
  m_StepLinearDeflection->setValue(m_Settings->value("STEPLinearDeflection", 0.01).toDouble());
  m_StepAngularDeflection->setValue(m_Settings->value("STEPAngularDeflection", 0.5).toDouble());
  refreshOcclusion();
}

void Preferences::refreshOcclusion()
{
  // (What is loaded is not an edit.)
  const QSignalBlocker blockAlgorithm(m_OcclusionAlgorithm), blockSlider(m_OcclusionSlider), blockSpin(m_OcclusionSpin);
  m_OcclusionAlgorithm->setCurrentIndex(std::clamp(m_Settings->value(OcclusionAlgorithmKey, 0).toInt(), 0, 2));
  const double strength = std::clamp(m_Settings->value(OcclusionStrengthKey, 0.85).toDouble(), 0.0, 1.0);
  m_OcclusionSpin->setValue(strength);
  m_OcclusionSlider->setValue(qRound(strength * 100.0));
}

void Preferences::onOcclusionEdited()
{
  m_Settings->setValue(OcclusionAlgorithmKey, m_OcclusionAlgorithm->currentIndex());
  m_Settings->setValue(OcclusionStrengthKey, m_OcclusionSpin->value());
  emit occlusionChanged();
}

void Preferences::saveSettings()
{
  m_Settings->setValue("STEPLinearDeflection", m_StepLinearDeflection->value());
  m_Settings->setValue("STEPAngularDeflection", m_StepAngularDeflection->value());
  m_Settings->setValue(OcclusionAlgorithmKey, m_OcclusionAlgorithm->currentIndex());
  m_Settings->setValue(OcclusionStrengthKey, m_OcclusionSpin->value());
}

void Preferences::showEvent(QShowEvent* event)
{
  loadSettings();
  QDialog::showEvent(event);
}

void Preferences::onCategoryChanged(QTreeWidgetItem* item)
{
  if (!item) return;
  const QVariant page = item->data(0, Qt::UserRole);
  if (page.isValid() && page.toInt() >= 0 && page.toInt() < m_Stack->count()) m_Stack->setCurrentIndex(page.toInt());
}

void Preferences::onApply()
{
  saveSettings();
  emit apply();
}

void Preferences::onOk()
{
  saveSettings();
  accept();
  emit apply();
}
