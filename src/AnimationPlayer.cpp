#include "AnimationPlayer.h"

#include <algorithm>
#include <cmath>

AnimationPlayer::AnimationPlayer(QObject* parent) : QObject(parent)
{
  m_Timer.setTimerType(Qt::PreciseTimer);
  m_Timer.setInterval(16);
  connect(&m_Timer, &QTimer::timeout, this, &AnimationPlayer::tick);
}

void AnimationPlayer::setAnimations(const vsg::Animations& animations)
{
  const bool couldPlay = hasAnimations();
  m_Timer.stop();
  m_Animations = animations;
  m_Clip = 0;
  updateDuration();
  m_Position = 0.0;
  m_State = State::Stopped;
  applyPosition();
  emit stateChanged(m_State);
  emit positionChanged(m_Position);
  emit clipChanged(m_Clip);
  if (couldPlay != hasAnimations()) emit availabilityChanged(hasAnimations());
}

QStringList AnimationPlayer::clipNames() const
{
  QStringList names;
  for (size_t i = 0; i < m_Animations.size(); ++i)
  {
    const std::string& name = m_Animations[i]->name;
    names << (name.empty() ? tr("Animation %1").arg(i + 1) : QString::fromStdString(name));
  }
  if (names.size() > 1) names << tr("All together");
  return names;
}

vsg::Animations AnimationPlayer::selected() const
{
  if (m_Clip >= 0 && size_t(m_Clip) < m_Animations.size()) return {m_Animations[m_Clip]};
  return m_Animations; // all
}

void AnimationPlayer::updateDuration()
{
  m_Duration = 0.0;
  for (auto& animation : selected()) m_Duration = std::max(m_Duration, animation->maxTime());
}

void AnimationPlayer::setClip(int index)
{
  if (!hasAnimations() || index == m_Clip || index < 0 || index > int(m_Animations.size())) return;
  m_Clip = index;
  updateDuration();
  m_Position = 0.0;
  if (isPlaying()) m_Clock.start();
  applyPosition();
  emit positionChanged(m_Position);
  emit clipChanged(m_Clip);
}

void AnimationPlayer::play()
{
  if (!hasAnimations() || m_State == State::Playing) return;
  if (m_Position >= m_Duration) m_Position = 0.0;
  m_Clock.start();
  m_Timer.start();
  setState(State::Playing);
  applyPosition();
  emit positionChanged(m_Position);
}

void AnimationPlayer::pause()
{
  if (m_State != State::Playing) return;
  m_Timer.stop();
  setState(State::Paused);
}

void AnimationPlayer::togglePlay()
{
  if (isPlaying())
    pause();
  else
    play();
}

void AnimationPlayer::stop()
{
  if (!hasAnimations()) return;
  m_Timer.stop();
  m_Position = 0.0;
  setState(State::Stopped);
  applyPosition();
  emit positionChanged(m_Position);
}

void AnimationPlayer::seek(double seconds)
{
  if (!hasAnimations()) return;
  m_Position = std::clamp(seconds, 0.0, m_Duration);
  if (m_State == State::Stopped) setState(State::Paused);
  if (isPlaying()) m_Clock.start(); // the time since the last tick is spent
  applyPosition();
  emit positionChanged(m_Position);
}

void AnimationPlayer::seekBy(double seconds)
{
  // At the end of a loop, going on wraps around, as playing does.
  double target = m_Position + seconds;
  if (m_Loop && m_Duration > 0.0 && std::abs(seconds) >= 1e-9)
  {
    if (target > m_Duration && m_Position >= m_Duration) target -= m_Duration;
    else if (target < 0.0 && m_Position <= 0.0) target += m_Duration;
  }
  seek(target);
}

void AnimationPlayer::setSpeed(double speed)
{
  speed = std::clamp(speed, 0.01, 100.0);
  if (speed == m_Speed) return;
  m_Speed = speed;
  emit speedChanged(m_Speed);
}

void AnimationPlayer::setLoop(bool loop)
{
  if (loop == m_Loop) return;
  m_Loop = loop;
  emit loopChanged(m_Loop);
}

void AnimationPlayer::tick()
{
  const double elapsed = m_Clock.restart() * 0.001;
  m_Position += elapsed * m_Speed;
  if (m_Position >= m_Duration)
  {
    if (m_Loop && m_Duration > 0.0)
      m_Position = std::fmod(m_Position, m_Duration);
    else
    {
      // Plays to the end and waits there; play() starts over.
      m_Position = m_Duration;
      m_Timer.stop();
      setState(State::Paused);
    }
  }
  applyPosition();
  emit positionChanged(m_Position);
}

void AnimationPlayer::applyPosition()
{
  for (auto& animation : selected())
  {
    animation->time = m_Position;
    for (auto& sampler : animation->samplers) sampler->update(m_Position);
  }
  emit posesChanged();
}

void AnimationPlayer::setState(State state)
{
  if (state == m_State) return;
  m_State = state;
  emit stateChanged(m_State);
}
