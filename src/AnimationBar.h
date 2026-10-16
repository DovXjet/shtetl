#pragma once

#include "AnimationPlayer.h"

#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QSlider;
namespace fa
{
class QtAwesome;
}

// The transport bar under the 3D view, as VLC's: rewind, play/pause, forward,
// stop, a scrubber with the time, the speed and looping. It shows while the
// model has animations. Its actions hold the keyboard shortcuts.
class AnimationBar : public QWidget
{
    Q_OBJECT

  public:
    AnimationBar(AnimationPlayer* player, QWidget* parent = nullptr);

    // Play/pause, stop, rewind, forward, the speeds and loop; for a menu.
    // Their shortcuts work while the bar is shown.
    const QList<QAction*>& playbackActions() const { return m_Actions; }

  private:
    void updateState();
    void updatePosition();
    void updateSpeed();
    void updateClips(); // the list of animations, and the one selected
    static QString formatTime(double seconds);

    AnimationPlayer* m_Player;
    fa::QtAwesome* m_Awesome;
    QAction* m_PlayAction;
    QAction* m_StopAction;
    QAction* m_LoopAction;
    QSlider* m_Slider;
    QLabel* m_TimeLabel;
    QComboBox* m_ClipCombo;
    QComboBox* m_SpeedCombo;
    QList<QAction*> m_Actions;
    QIcon m_PlayIcon, m_PauseIcon;
};
