#ifndef SHORTCUT_KEY_EDIT_H
#define SHORTCUT_KEY_EDIT_H

#include <QKeySequence>
#include <QLineEdit>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;

class ShortcutKeyEdit final : public QLineEdit
{
    Q_OBJECT

public:
    explicit ShortcutKeyEdit(const QKeySequence &sequence, QWidget *parent = nullptr);
    void setKeySequence(const QKeySequence &sequence);

signals:
    void recordingChanged(bool recording);
    void sequenceEdited(const QKeySequence &sequence);

protected:
    bool event(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void finishRecording();
    void updateAppearance();

    QKeySequence m_sequence;
    QKeySequence m_pendingSequence;
    int m_pendingKey = 0;
    bool m_recording = false;
};

#endif // SHORTCUT_KEY_EDIT_H
