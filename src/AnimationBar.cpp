#include "AnimationBar.h"

#include "QtAwesome.h"

#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace
{
  constexpr int SliderSteps = 10000;
  constexpr double SeekStep = 1.0;       // Left / Right, the rewind and forward buttons
  constexpr double FineSeekStep = 0.1;   // with Shift
  constexpr double CoarseSeekStep = 5.0; // with Ctrl
  const double Speeds[] = {0.1, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0};
  constexpr int NormalSpeedIndex = 4;

  // A slider whose groove jumps to where it is clicked, as a media player's.
  class ScrubSlider : public QSlider
  {
    public:
      using QSlider::QSlider;

    protected:
      void mousePressEvent(QMouseEvent* event) override
      {
        if (event->button() == Qt::LeftButton)
        {
          QStyleOptionSlider option;
          initStyleOption(&option);
          const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
          const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
          if (!handle.contains(event->position().toPoint()))
          {
            const int span = groove.width() - handle.width();
            const int pos = event->position().toPoint().x() - groove.x() - handle.width() / 2;
            setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), std::clamp(pos, 0, span), span, option.upsideDown));
            // Carry on as a drag of the handle, which is now under the pointer.
            emit sliderMoved(value());
          }
        }
        QSlider::mousePressEvent(event);
      }
  };
} // namespace

AnimationBar::AnimationBar(AnimationPlayer* player, QWidget* parent) : QWidget(parent), m_Player(player)
{
  m_Awesome = new fa::QtAwesome(this);
  m_Awesome->initFontAwesome();
  auto icon = [this](int glyph) { return m_Awesome->icon(fa::fa_solid, glyph); };
  m_PlayIcon = icon(fa::fa_play);
  m_PauseIcon = icon(fa::fa_pause);

  // An action for everything with a key, so the buttons, the menu and the keys share one state.
  auto makeAction = [this](const QString& text, const QIcon& actionIcon, const QList<QKeySequence>& keys, auto&& slot) {
    auto* action = new QAction(actionIcon, text, this);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, slot);
    addAction(action); // a shortcut works for the actions of a visible widget
    m_Actions.append(action);
    return action;
  };
  auto seekAction = [this, &makeAction](const QString& text, const QKeySequence& key, double delta) {
    return makeAction(text, QIcon(), {key}, [this, delta]() { m_Player->seekBy(delta); });
  };

  auto* rewind = makeAction(tr("Rewind"), icon(fa::fa_backward), {QKeySequence("Left")}, [this]() { m_Player->seekBy(-SeekStep); });
  m_PlayAction = makeAction(tr("Play"), m_PlayIcon, {QKeySequence("Space")}, [this]() { m_Player->togglePlay(); });
  auto* forward = makeAction(tr("Forward"), icon(fa::fa_forward), {QKeySequence("Right")}, [this]() { m_Player->seekBy(SeekStep); });
  m_StopAction = makeAction(tr("Stop"), icon(fa::fa_stop), {QKeySequence("S")}, [this]() { m_Player->stop(); });
  rewind->setToolTip(tr("Rewind 1 s (Left; Shift: 0.1 s, Ctrl: 5 s)"));
  forward->setToolTip(tr("Forward 1 s (Right; Shift: 0.1 s, Ctrl: 5 s)"));
  m_StopAction->setToolTip(tr("Stop (S)"));
  seekAction(tr("Rewind 0.1 s"), QKeySequence("Shift+Left"), -FineSeekStep);
  seekAction(tr("Forward 0.1 s"), QKeySequence("Shift+Right"), FineSeekStep);
  seekAction(tr("Rewind 5 s"), QKeySequence("Ctrl+Left"), -CoarseSeekStep);
  seekAction(tr("Forward 5 s"), QKeySequence("Ctrl+Right"), CoarseSeekStep);

  auto stepSpeed = [this](int delta) {
    int index = NormalSpeedIndex;
    for (int i = 0; i < int(std::size(Speeds)); ++i)
      if (std::abs(Speeds[i] - m_Player->speed()) < 1e-6) index = i;
    m_Player->setSpeed(Speeds[std::clamp(index + delta, 0, int(std::size(Speeds)) - 1)]);
  };
  makeAction(tr("Slower"), QIcon(), {QKeySequence("[")}, [stepSpeed]() { stepSpeed(-1); });
  makeAction(tr("Faster"), QIcon(), {QKeySequence("]")}, [stepSpeed]() { stepSpeed(+1); });
  makeAction(tr("Normal Speed"), QIcon(), {QKeySequence("=")}, [this]() { m_Player->setSpeed(1.0); });

  m_LoopAction = makeAction(tr("Loop"), icon(fa::fa_repeat), {QKeySequence("L")}, [this]() { m_Player->setLoop(!m_Player->loop()); });
  m_LoopAction->setCheckable(true);
  m_LoopAction->setToolTip(tr("Loop (L)"));

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(6, 3, 6, 3);
  layout->setSpacing(2);
  // Keys belong to the actions, and a button that has the focus would take the arrows and the space.
  auto addButton = [this, layout](QAction* action) {
    auto* button = new QToolButton(this);
    button->setDefaultAction(action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(button);
    return button;
  };
  addButton(rewind);
  addButton(m_PlayAction);
  addButton(forward);
  addButton(m_StopAction);

  m_Slider = new ScrubSlider(Qt::Horizontal, this);
  m_Slider->setRange(0, SliderSteps);
  m_Slider->setFocusPolicy(Qt::NoFocus);
  layout->addSpacing(6);
  layout->addWidget(m_Slider, 1);
  connect(m_Slider, &QSlider::sliderMoved, this, [this](int value) { m_Player->seek(m_Player->duration() * value / SliderSteps); });

  // Which animation of the file, when it has several (as the clips of a character).
  m_ClipCombo = new QComboBox(this);
  m_ClipCombo->setFocusPolicy(Qt::NoFocus);
  m_ClipCombo->setToolTip(tr("Animation"));
  m_ClipCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_ClipCombo->setMinimumContentsLength(18);
  m_ClipCombo->setMaximumWidth(280);
  layout->addWidget(m_ClipCombo);
  connect(m_ClipCombo, &QComboBox::activated, this, [this](int index) { m_Player->setClip(index); });

  m_TimeLabel = new QLabel(this);
  m_TimeLabel->setMinimumWidth(m_TimeLabel->fontMetrics().horizontalAdvance("00:00.0 / 00:00.0") + 4);
  m_TimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  layout->addWidget(m_TimeLabel);

  m_SpeedCombo = new QComboBox(this);
  for (double speed : Speeds) m_SpeedCombo->addItem(QString("%1x").arg(speed), speed);
  m_SpeedCombo->setFocusPolicy(Qt::NoFocus);
  m_SpeedCombo->setToolTip(tr("Speed ([ slower, ] faster, = normal)"));
  layout->addWidget(m_SpeedCombo);
  connect(m_SpeedCombo, &QComboBox::activated, this, [this](int index) { m_Player->setSpeed(m_SpeedCombo->itemData(index).toDouble()); });

  addButton(m_LoopAction);

  connect(m_Player, &AnimationPlayer::stateChanged, this, &AnimationBar::updateState);
  connect(m_Player, &AnimationPlayer::positionChanged, this, &AnimationBar::updatePosition);
  connect(m_Player, &AnimationPlayer::speedChanged, this, &AnimationBar::updateSpeed);
  connect(m_Player, &AnimationPlayer::loopChanged, m_LoopAction, &QAction::setChecked);
  connect(m_Player, &AnimationPlayer::clipChanged, this, &AnimationBar::updateClips);
  connect(m_Player, &AnimationPlayer::availabilityChanged, this, [this](bool available) {
    setVisible(available);
    for (auto* action : m_Actions) action->setEnabled(available);
  });

  m_LoopAction->setChecked(m_Player->loop());
  updateState();
  updatePosition();
  updateSpeed();
  updateClips();
  setVisible(m_Player->hasAnimations());
  for (auto* action : m_Actions) action->setEnabled(m_Player->hasAnimations());
}

void AnimationBar::updateState()
{
  const bool playing = m_Player->isPlaying();
  m_PlayAction->setIcon(playing ? m_PauseIcon : m_PlayIcon);
  m_PlayAction->setText(playing ? tr("Pause") : tr("Play"));
  m_PlayAction->setToolTip(playing ? tr("Pause (Space)") : tr("Play (Space)"));
  m_StopAction->setEnabled(m_Player->hasAnimations() && m_Player->state() != AnimationPlayer::State::Stopped);
}

void AnimationBar::updatePosition()
{
  const double duration = m_Player->duration();
  // Not while the handle is being dragged: the pointer decides where it is.
  if (!m_Slider->isSliderDown())
  {
    const QSignalBlocker blocker(m_Slider);
    m_Slider->setValue(duration > 0.0 ? int(std::lround(m_Player->position() / duration * SliderSteps)) : 0);
  }
  m_TimeLabel->setText(formatTime(m_Player->position()) + " / " + formatTime(duration));
}

void AnimationBar::updateClips()
{
  const QStringList names = m_Player->clipNames();
  if (m_ClipCombo->count() != names.size() || [&] { for (int i = 0; i < names.size(); ++i) if (m_ClipCombo->itemText(i) != names[i]) return true; return false; }())
  {
    m_ClipCombo->clear();
    m_ClipCombo->addItems(names);
  }
  m_ClipCombo->setCurrentIndex(m_Player->clip());
  m_ClipCombo->setVisible(names.size() > 1);
}

void AnimationBar::updateSpeed()
{
  int index = -1;
  for (int i = 0; i < int(std::size(Speeds)); ++i)
    if (std::abs(Speeds[i] - m_Player->speed()) < 1e-6) index = i;
  m_SpeedCombo->setCurrentIndex(index);
}

QString AnimationBar::formatTime(double seconds)
{
  const int tenths = int(std::lround(seconds * 10.0));
  return QString("%1:%2.%3").arg(tenths / 600, 2, 10, QChar('0')).arg(tenths / 10 % 60, 2, 10, QChar('0')).arg(tenths % 10);
}
