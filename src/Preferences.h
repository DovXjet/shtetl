#pragma once

#include <QDialog>

#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSlider;
class QSettings;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class SceneSettingsPage;

// Edit > Preferences: a tree of categories on the left, their pages on the
// right and OK / Close / Apply below - after XjetStudio's dialog, with the
// import settings it keeps under the same keys. Not modal: the scene settings
// and the ambient occlusion apply (and are saved) at once, as they are changed,
// so that their effect can be watched; the import settings wait for OK or Apply.
// The pages are of the same objects as the window shows.
class Preferences : public QDialog
{
    Q_OBJECT

  public:
    enum Page
    {
        ImportPage = 0,
        OcclusionPage = 1,
        ScenePage = 2
    };

    // The scene page is made by the caller (it needs the viewport) and taken over.
    Preferences(QSettings* settings, SceneSettingsPage* scenePage, QWidget* parent = nullptr);

    // The occlusion widgets from the settings, e.g. after Shift+F11 changed them.
    void refreshOcclusion();

  // The keys of the ambient occlusion settings (the algorithm as 0 SSAO, 1 CORTAO, 2 hybrid;
  // the strength from 0 to 1), for the main window, which applies them to the view.
  static constexpr const char* OcclusionAlgorithmKey = "View/OcclusionAlgorithm";
  static constexpr const char* OcclusionStrengthKey = "View/OcclusionStrength";

  signals:
    // The settings were saved (OK or Apply).
    void apply();
    // The algorithm or the strength of the ambient occlusion was changed, and saved.
    void occlusionChanged();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void createUi();
    void addStepPage();
    void addOcclusionPage();
    void onOcclusionEdited();
    void loadSettings();
    void saveSettings();
    void onCategoryChanged(QTreeWidgetItem* item);
    void onApply();
    void onOk();

    QSettings* m_Settings;
    SceneSettingsPage* m_ScenePage;
    std::vector<QTreeWidgetItem*> m_PageItems; // by Page
    QTreeWidget* m_Tree = nullptr;
    QStackedWidget* m_Stack = nullptr;
    QDoubleSpinBox* m_StepLinearDeflection = nullptr;
    QDoubleSpinBox* m_StepAngularDeflection = nullptr;
    QComboBox* m_OcclusionAlgorithm = nullptr;
    QSlider* m_OcclusionSlider = nullptr;
    QDoubleSpinBox* m_OcclusionSpin = nullptr;
};
