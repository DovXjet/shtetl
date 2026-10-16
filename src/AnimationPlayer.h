#pragma once

#include <vsg/animation/Animation.h>

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

// Plays the animations (glTF, FBX, ... through vsgXchange) of the model shown
// as one timeline, like a media player: play, pause, stop, seek, speed, loop.
// It sets the poses itself, from its own clock, rather than through VSG's
// AnimationManager, so that the position can be scrubbed.
class AnimationPlayer : public QObject
{
    Q_OBJECT

  public:
    enum class State
    {
        Stopped, // at the start
        Playing,
        Paused,
    };

    explicit AnimationPlayer(QObject* parent = nullptr);

    // Replaces the animations (none: there is nothing to play); stopped, at the
    // start, with the first one selected.
    void setAnimations(const vsg::Animations& animations);
    // The animations ("clips") of a file, by name; with several, one more that
    // plays them all together. One is played at a time.
    QStringList clipNames() const;
    int clip() const { return m_Clip; }
    bool hasAnimations() const { return !m_Animations.empty(); }
    // Seconds: the end of the longest animation.
    double duration() const { return m_Duration; }
    double position() const { return m_Position; }
    State state() const { return m_State; }
    bool isPlaying() const { return m_State == State::Playing; }
    double speed() const { return m_Speed; }
    bool loop() const { return m_Loop; }

  public slots:
    void play(); // from the start, when it is at the end
    void pause();
    void togglePlay();
    void stop(); // back to the start
    void seek(double seconds); // clamped; a stopped animation is paused where it was moved to
    void seekBy(double seconds);
    void setClip(int index); // from the start, keeping the play state
    void setSpeed(double speed);
    void setLoop(bool loop);

  signals:
    // Whether there is anything to play, when that changes.
    void availabilityChanged(bool available);
    void stateChanged(AnimationPlayer::State state);
    void positionChanged(double seconds);
    void speedChanged(double speed);
    void loopChanged(bool loop);
    void clipChanged(int index);
    // The poses changed: a frame should be drawn.
    void posesChanged();

  private:
    void tick();
    void applyPosition();
    // The animations of the selected clip.
    vsg::Animations selected() const;
    void updateDuration();
    void setState(State state);

    vsg::Animations m_Animations;
    int m_Clip = 0;
    double m_Duration = 0.0;
    double m_Position = 0.0;
    double m_Speed = 1.0;
    bool m_Loop = true;
    State m_State = State::Stopped;
    QTimer m_Timer;
    QElapsedTimer m_Clock;
};
