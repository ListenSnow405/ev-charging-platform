#include "loading_status.h"

#include <QStyle>
#include <QTimer>
#include <QVariant>

LoadingStatus::LoadingStatus(const QString &text, QWidget *parent)
    : QLabel(parent), m_animationTimer(new QTimer(this)), m_message(text)
{
    setObjectName(QStringLiteral("Status"));
    setWordWrap(true);
    setTextFormat(Qt::PlainText);
    m_animationTimer->setInterval(500);
    connect(m_animationTimer, &QTimer::timeout, this, [this] {
        m_dots = m_dots % 3 + 1;
        render();
    });
    render();
}

void LoadingStatus::setText(const QString &text)
{
    setMessage(text, Tone::Neutral);
}

void LoadingStatus::setMessage(const QString &text, Tone tone)
{
    m_message = text;
    m_tone = tone;
    render();
}

void LoadingStatus::setLoading(bool loading)
{
    if (loading == isLoading()) return;
    if (loading) {
        m_dots = 1;
        m_animationTimer->start();
    } else {
        m_animationTimer->stop();
    }
    render();
}

bool LoadingStatus::isLoading() const
{
    return m_animationTimer->isActive();
}

void LoadingStatus::render()
{
    const char *tone = "neutral";
    switch (m_tone) {
    case Tone::Success: tone = "success"; break;
    case Tone::Warning: tone = "warning"; break;
    case Tone::Error: tone = "error"; break;
    case Tone::Loading: tone = "loading"; break;
    case Tone::Neutral: break;
    }
    if (isLoading() && m_tone != Tone::Error && m_tone != Tone::Warning)
        tone = "loading";
    const QString toneName = QString::fromLatin1(tone);
    if (property("tone").toString() != toneName) {
        setProperty("tone", toneName);
        style()->unpolish(this);
        style()->polish(this);
    }
    QString display = m_message;
    if (isLoading()) {
        if (m_tone == Tone::Loading && display.endsWith(QChar(0x2026))) {
            display.chop(1);
            display += QString(m_dots, QLatin1Char('.'));
        } else {
            display += QStringLiteral("　加载中") + QString(m_dots, QLatin1Char('.'));
        }
    }
    QLabel::setText(display);
}
