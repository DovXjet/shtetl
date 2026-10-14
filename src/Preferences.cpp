#include "Preferences.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

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

Preferences::Preferences(QSettings* settings, QWidget* parent) : QDialog(parent), m_Settings(settings)
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

  auto* okButton = new QPushButton(tr("OK"), this);
  auto* cancelButton = new QPushButton(tr("Cancel"), this);
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

  m_Tree->setCurrentItem(stepItem);
  resize(520, 300);
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

// The keys are XjetStudio's, so the two programs agree.
void Preferences::loadSettings()
{
  m_StepLinearDeflection->setValue(m_Settings->value("STEPLinearDeflection", 0.01).toDouble());
  m_StepAngularDeflection->setValue(m_Settings->value("STEPAngularDeflection", 0.5).toDouble());
}

void Preferences::saveSettings()
{
  m_Settings->setValue("STEPLinearDeflection", m_StepLinearDeflection->value());
  m_Settings->setValue("STEPAngularDeflection", m_StepAngularDeflection->value());
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
