#pragma once

#include <QLabel>

class QTimer;

// 仅呈现页面已有的在途状态，不持有请求、不参与响应匹配或超时判定。
class LoadingStatus : public QLabel
{
public:
    enum class Tone { Neutral, Success, Warning, Error, Loading };

    explicit LoadingStatus(const QString &text, QWidget *parent = nullptr);
    void setText(const QString &text);
    void setMessage(const QString &text, Tone tone);
    void setLoading(bool loading);
    bool isLoading() const;

private:
    void render();

    QTimer *m_animationTimer = nullptr;
    QString m_message;
    Tone m_tone = Tone::Neutral;
    int m_dots = 1;
};
