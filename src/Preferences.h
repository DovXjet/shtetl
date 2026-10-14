#pragma once

#include <QDialog>

class QDoubleSpinBox;
class QPushButton;
class QSettings;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

// File > Preferences: a tree of categories on the left, their pages on the
// right and OK / Cancel / Apply below - after XjetStudio's dialog, with the
// settings it keeps under the same keys.
class Preferences : public QDialog
{
    Q_OBJECT

  public:
    explicit Preferences(QSettings* settings, QWidget* parent = nullptr);

  signals:
    // The settings were saved (OK or Apply).
    void apply();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void createUi();
    void addStepPage();
    void loadSettings();
    void saveSettings();
    void onCategoryChanged(QTreeWidgetItem* item);
    void onApply();
    void onOk();

    QSettings* m_Settings;
    QTreeWidget* m_Tree = nullptr;
    QStackedWidget* m_Stack = nullptr;
    QDoubleSpinBox* m_StepLinearDeflection = nullptr;
    QDoubleSpinBox* m_StepAngularDeflection = nullptr;
};
