#include "shortcut_key_edit.h"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QVariant>

ShortcutKeyEdit::ShortcutKeyEdit(const QKeySequence &sequence, QWidget *parent)
    : QLineEdit(parent), m_sequence(sequence)
{
    setObjectName(QStringLiteral("shortcutKeyEditor"));
    setReadOnly(true);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setFocusPolicy(Qt::ClickFocus);
    setContextMenuPolicy(Qt::NoContextMenu);
    setToolTip(QStringLiteral("双击修改快捷键，Backspace 清空"));
    updateAppearance();
}

void ShortcutKeyEdit::setKeySequence(const QKeySequence &sequence)
{
    m_sequence = sequence;
    setText(sequence.toString(QKeySequence::NativeText));
}

void ShortcutKeyEdit::updateAppearance()
{
    setProperty("recording", m_recording);
    // Enable the native blinking caret only while recording; keys remain handled below.
    setReadOnly(!m_recording);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setCursor(m_recording ? Qt::IBeamCursor : Qt::PointingHandCursor);
    setPlaceholderText(m_recording ? QStringLiteral("请按快捷键") : QStringLiteral("双击设置"));
    setText(m_sequence.toString(QKeySequence::NativeText));
    deselect();
    setCursorPosition(text().size());
    style()->unpolish(this);
    style()->polish(this);
    update();
}

bool ShortcutKeyEdit::event(QEvent *event)
{
    if (m_recording && event->type() == QEvent::ShortcutOverride) {
        // Recording owns the keystroke, including shortcuts normally handled by a parent widget.
        event->accept();
        return true;
    }
    return QLineEdit::event(event);
}

void ShortcutKeyEdit::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        if (!m_recording) {
            m_recording = true;
            m_pendingKey = 0;
            m_pendingSequence = QKeySequence();
            emit recordingChanged(true);
            updateAppearance();
        }
        event->accept();
        return;
    }
    QLineEdit::mouseDoubleClickEvent(event);
}

void ShortcutKeyEdit::keyPressEvent(QKeyEvent *event)
{
    event->accept();
    if (!m_recording || event->isAutoRepeat()) {
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Backspace) {
        // Backspace is always a clear command, even while modifier keys are held.
        m_pendingKey = 0;
        m_pendingSequence = QKeySequence();
        setText(QString());
        emit sequenceEdited(QKeySequence());
        return;
    }
    if (key == Qt::Key_Escape) {
        finishRecording();
        return;
    }
    if (key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Shift ||
        key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_unknown || key == 0) {
        return;
    }
    if (m_pendingKey == 0) {
        m_pendingKey = key;
        m_pendingSequence = QKeySequence(event->keyCombination());
        setText(m_pendingSequence.toString(QKeySequence::NativeText));
    }
}

void ShortcutKeyEdit::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();
    if (m_recording && !event->isAutoRepeat() && event->key() == m_pendingKey) {
        // Commit one combination after its main key is released; no multi-step sequences.
        const QKeySequence sequence = m_pendingSequence;
        m_pendingKey = 0;
        m_pendingSequence = QKeySequence();
        emit sequenceEdited(sequence);
        finishRecording();
    }
}

void ShortcutKeyEdit::finishRecording()
{
    if (!m_recording) {
        return;
    }
    m_recording = false;
    m_pendingKey = 0;
    m_pendingSequence = QKeySequence();
    updateAppearance();
    emit recordingChanged(false);
}

void ShortcutKeyEdit::focusOutEvent(QFocusEvent *event)
{
    finishRecording();
    QLineEdit::focusOutEvent(event);
}
