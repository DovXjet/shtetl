#pragma once

#include "CompactMesh.h"

#include <QWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The hierarchy of a 3MF or STEP file (objects and components, assemblies and
// parts) as a tree, each node with a state: off, transparent or opaque (after
// the parts panel of the machine view of MetalJet). Click the state of a node
// to go to the next one; a group has the state of all that is in it, or
// "mixed" if they differ, and the click puts all of them in the same state.
// Below the tree, the parts can be given colors of their own.
class PartsPanel : public QWidget
{
    Q_OBJECT

  public:
    explicit PartsPanel(QWidget* parent = nullptr);

    // Shows the hierarchy of a new model, all opaque; empty for one without.
    void setParts(const std::vector<PartNode>& parts);
    bool isEmpty() const { return m_Parts.empty(); }
    // The state of the random colors, without a signal (for the settings).
    void setRandomColors(bool enabled, unsigned seed);

  signals:
    // One state per node of the hierarchy (only those of the leaves matter).
    void statesChanged(const std::vector<PartState>& states);
    void randomColorsChanged(bool enabled, unsigned seed);
    // What a shift-click did, for a status bar.
    void statusMessage(const QString& text);

  private:
    void collectLeaves(int node, std::vector<int>& leaves) const; // -1: the whole model
    // The state of all the leaves of a node, or none if they differ.
    std::optional<PartState> stateOf(int node) const;
    // The state of all the leaves that are not in the node, or none if they differ.
    std::optional<PartState> othersState(int node) const;
    void setNode(int node, PartState state);
    void setOthers(int node, PartState state);
    void setAll(PartState state);
    void changed();
    void refreshItems();
    void onItemClicked(QTreeWidgetItem* item, int column);
    void showContextMenu(const QPoint& pos);

    QTreeWidget* m_Tree = nullptr;
    QLabel* m_Empty = nullptr;
    QWidget* m_Contents = nullptr;
    QCheckBox* m_Random = nullptr;
    unsigned m_Seed = 0;
    std::vector<PartNode> m_Parts;
    std::vector<PartState> m_States; // per node; those of the leaves are what counts
    std::vector<std::vector<int>> m_Children;
    std::vector<QTreeWidgetItem*> m_Items; // per node
    QTreeWidgetItem* m_Root = nullptr;
};
