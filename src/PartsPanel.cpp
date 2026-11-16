#include "PartsPanel.h"

#include <QApplication>
#include <algorithm>
#include <QCheckBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace
{
  // The state is a narrow column on the left; the tree (arrows and indent) is in the name column.
  constexpr int StateColumn = 0;
  constexpr int NameColumn = 1;
  constexpr int NodeRole = Qt::UserRole; // the index of the node, on its item; -1 for the root

  QString stateIcon(const std::optional<PartState>& state)
  {
    if (!state) return QStringLiteral("◑");
    switch (*state)
    {
      case PartState::Off: return QStringLiteral("○");
      case PartState::Transparent: return QStringLiteral("◐");
      case PartState::Opaque: return QStringLiteral("●");
    }
    return {};
  }

  QString stateToolTip(const std::optional<PartState>& state, bool isRoot)
  {
    QString text;
    if (!state) text = QObject::tr("Mixed: the parts inside are not all the same. Click to make them opaque");
    else
      switch (*state)
      {
        case PartState::Off: text = QObject::tr("Off. Click to make opaque"); break;
        case PartState::Transparent: text = QObject::tr("Transparent. Click to turn off"); break;
        case PartState::Opaque: text = QObject::tr("Opaque. Click to make transparent"); break;
      }
    if (!isRoot) text += QObject::tr("\nShift-click: only this part, then the rest transparent, then the rest off\nRight-click: more");
    return text;
  }

  // Opaque, then transparent, then off, and round again; from mixed it goes to
  // opaque, so the first click on a mixed group makes them all the same.
  PartState nextState(const std::optional<PartState>& state)
  {
    if (!state) return PartState::Opaque;
    switch (*state)
    {
      case PartState::Opaque: return PartState::Transparent;
      case PartState::Transparent: return PartState::Off;
      case PartState::Off: return PartState::Opaque;
    }
    return PartState::Opaque;
  }
} // namespace

PartsPanel::PartsPanel(QWidget* parent) : QWidget(parent)
{
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(2, 2, 2, 2);

  m_Contents = new QWidget(this);
  auto* contents = new QVBoxLayout(m_Contents);
  contents->setContentsMargins(0, 0, 0, 0);

  m_Tree = new QTreeWidget(m_Contents);
  m_Tree->setColumnCount(2);
  m_Tree->setHeaderLabels({QString(), tr("Part")});
  m_Tree->setTreePosition(NameColumn);
  m_Tree->header()->setStretchLastSection(true);
  m_Tree->header()->setSectionResizeMode(StateColumn, QHeaderView::ResizeToContents);
  m_Tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
  m_Tree->setSelectionMode(QAbstractItemView::NoSelection);
  m_Tree->setUniformRowHeights(true);
  m_Tree->setContextMenuPolicy(Qt::CustomContextMenu);
  contents->addWidget(m_Tree, 1);

  // The colors: one for each part, and another set at a click.
  auto* colors = new QHBoxLayout;
  m_Random = new QCheckBox(tr("Random colors"), m_Contents);
  m_Random->setToolTip(tr("Gives every part a color of its own, instead of the colors of the file"));
  auto* another = new QPushButton(tr("New colors"), m_Contents);
  another->setToolTip(tr("Another set of random colors"));
  colors->addWidget(m_Random);
  colors->addStretch(1);
  colors->addWidget(another);
  contents->addLayout(colors);
  layout->addWidget(m_Contents, 1);

  m_Empty = new QLabel(tr("This model has no parts to choose from."), this);
  m_Empty->setWordWrap(true);
  m_Empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  layout->addWidget(m_Empty, 1);

  connect(m_Tree, &QTreeWidget::itemClicked, this, &PartsPanel::onItemClicked);
  connect(m_Tree, &QTreeWidget::customContextMenuRequested, this, &PartsPanel::showContextMenu);
  connect(m_Random, &QCheckBox::toggled, this, [this](bool on) { emit randomColorsChanged(on, m_Seed); });
  connect(another, &QPushButton::clicked, this, [this]() {
    ++m_Seed;
    if (m_Random->isChecked()) emit randomColorsChanged(true, m_Seed);
    else m_Random->setChecked(true); // which emits
  });

  setParts({});
}

void PartsPanel::setRandomColors(bool enabled, unsigned seed)
{
  m_Seed = seed;
  const QSignalBlocker blocker(m_Random);
  m_Random->setChecked(enabled);
}

void PartsPanel::setParts(const std::vector<PartNode>& parts)
{
  m_Tree->clear();
  m_Parts = parts;
  m_States.assign(parts.size(), PartState::Opaque);
  m_Children.assign(parts.size(), {});
  m_Items.assign(parts.size(), nullptr);
  m_Root = nullptr;

  if (!parts.empty())
  {
    // The root is the whole model; its state is the one of all the parts.
    m_Root = new QTreeWidgetItem(m_Tree);
    m_Root->setText(NameColumn, tr("Model"));
    m_Root->setData(StateColumn, NodeRole, -1);
    m_Root->setTextAlignment(StateColumn, Qt::AlignCenter);
    for (size_t i = 0; i < parts.size(); ++i)
    {
      const PartNode& node = parts[i];
      if (node.parent >= 0) m_Children[size_t(node.parent)].push_back(int(i));
      auto* item = new QTreeWidgetItem(node.parent >= 0 ? m_Items[size_t(node.parent)] : m_Root);
      item->setText(NameColumn, node.name);
      item->setData(StateColumn, NodeRole, int(i));
      item->setTextAlignment(StateColumn, Qt::AlignCenter);
      if (node.leaf) item->setToolTip(NameColumn, tr("%1 triangles").arg(node.endTriangle - node.firstTriangle));
      m_Items[i] = item;
    }
    refreshItems();
    // A big assembly opens only down to its first level.
    m_Tree->expandToDepth(parts.size() <= 200 ? 64 : 1);
  }

  m_Contents->setVisible(!parts.empty());
  m_Empty->setVisible(parts.empty());
}

void PartsPanel::collectLeaves(int node, std::vector<int>& leaves) const
{
  if (node < 0)
  {
    for (size_t i = 0; i < m_Parts.size(); ++i)
      if (m_Parts[i].leaf) leaves.push_back(int(i));
    return;
  }
  if (m_Parts[size_t(node)].leaf) leaves.push_back(node);
  for (const int child : m_Children[size_t(node)]) collectLeaves(child, leaves);
}

std::optional<PartState> PartsPanel::stateOf(int node) const
{
  std::vector<int> leaves;
  collectLeaves(node, leaves);
  if (leaves.empty()) return PartState::Opaque;
  const PartState first = m_States[size_t(leaves.front())];
  for (const int leaf : leaves)
    if (m_States[size_t(leaf)] != first) return std::nullopt;
  return first;
}

std::optional<PartState> PartsPanel::othersState(int node) const
{
  std::vector<int> all, inside;
  collectLeaves(-1, all);
  collectLeaves(node, inside);
  std::optional<PartState> state;
  bool any = false;
  for (const int leaf : all)
  {
    if (std::find(inside.begin(), inside.end(), leaf) != inside.end()) continue;
    if (!any) state = m_States[size_t(leaf)];
    else if (state != m_States[size_t(leaf)]) return std::nullopt;
    any = true;
  }
  return any ? state : std::optional<PartState>(PartState::Opaque);
}

void PartsPanel::setNode(int node, PartState state)
{
  std::vector<int> leaves;
  collectLeaves(node, leaves);
  for (const int leaf : leaves) m_States[size_t(leaf)] = state;
}

void PartsPanel::setOthers(int node, PartState state)
{
  std::vector<int> all, inside;
  collectLeaves(-1, all);
  collectLeaves(node, inside);
  for (const int leaf : all)
    m_States[size_t(leaf)] = std::find(inside.begin(), inside.end(), leaf) != inside.end() ? PartState::Opaque : state;
}

void PartsPanel::setAll(PartState state)
{
  setNode(-1, state);
}

void PartsPanel::refreshItems()
{
  auto refresh = [this](QTreeWidgetItem* item, int node) {
    const auto state = stateOf(node);
    item->setText(StateColumn, stateIcon(state));
    item->setToolTip(StateColumn, stateToolTip(state, node < 0));
  };
  if (m_Root) refresh(m_Root, -1);
  for (size_t i = 0; i < m_Items.size(); ++i) refresh(m_Items[i], int(i));
}

void PartsPanel::changed()
{
  refreshItems();
  emit statesChanged(m_States);
}

void PartsPanel::onItemClicked(QTreeWidgetItem* item, int column)
{
  if (column != StateColumn) return;
  const int node = item->data(StateColumn, NodeRole).toInt();
  // Shift-click goes round the views of this part against the rest: the rest
  // transparent, then off, then all opaque. The root has no rest.
  if (node >= 0 && (QApplication::keyboardModifiers() & Qt::ShiftModifier))
  {
    const auto others = othersState(node);
    if (others == PartState::Opaque)
    {
      setOthers(node, PartState::Transparent);
      emit statusMessage(tr("%1: the rest is transparent").arg(item->text(NameColumn)));
    }
    else if (others == PartState::Transparent)
    {
      setOthers(node, PartState::Off);
      emit statusMessage(tr("%1: the rest is off").arg(item->text(NameColumn)));
    }
    else
    {
      setAll(PartState::Opaque);
      emit statusMessage(tr("All parts shown"));
    }
    changed();
    return;
  }
  setNode(node, nextState(stateOf(node)));
  changed();
}

void PartsPanel::showContextMenu(const QPoint& pos)
{
  QTreeWidgetItem* item = m_Tree->itemAt(pos);
  if (!item || m_Tree->columnAt(pos.x()) != StateColumn) return;
  const int node = item->data(StateColumn, NodeRole).toInt();
  if (node < 0) return; // the whole model has no rest

  QMenu menu(this);
  menu.addAction(tr("Isolate (hide the rest)"), [this, node] {
    setOthers(node, PartState::Off);
    changed();
  });
  menu.addAction(tr("Focus (make the rest transparent)"), [this, node] {
    setOthers(node, PartState::Transparent);
    changed();
  });
  menu.addAction(tr("Show all"), [this] {
    setAll(PartState::Opaque);
    changed();
  });
  menu.exec(m_Tree->viewport()->mapToGlobal(pos));
}
