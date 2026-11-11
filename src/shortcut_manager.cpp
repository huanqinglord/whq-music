#include "shortcut_manager.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int kShortcutIdBase = 0x5700;

QKeySequence normalizedSequence(const QKeySequence &sequence)
{
    for (int index = 0; index < sequence.count(); ++index) {
        if (sequence[index].key() == Qt::Key_Backspace) {
            // Backspace is reserved for clearing, including combinations with modifiers.
            return QKeySequence();
        }
    }
    return sequence;
}

#ifdef Q_OS_WIN
UINT virtualKey(Qt::Key key)
{
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) ||
        (key >= Qt::Key_0 && key <= Qt::Key_9)) {
        return static_cast<UINT>(key);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        return VK_F1 + static_cast<UINT>(key - Qt::Key_F1);
    }
    switch (key) {
    case Qt::Key_Left:
        return VK_LEFT;
    case Qt::Key_Right:
        return VK_RIGHT;
    case Qt::Key_Up:
        return VK_UP;
    case Qt::Key_Down:
        return VK_DOWN;
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Home:
        return VK_HOME;
    case Qt::Key_End:
        return VK_END;
    case Qt::Key_PageUp:
        return VK_PRIOR;
    case Qt::Key_PageDown:
        return VK_NEXT;
    case Qt::Key_Insert:
        return VK_INSERT;
    case Qt::Key_Delete:
        return VK_DELETE;
    case Qt::Key_Backspace:
        return 0;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return VK_RETURN;
    case Qt::Key_Tab:
        return VK_TAB;
    case Qt::Key_Escape:
        return VK_ESCAPE;
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaTogglePlayPause:
        return VK_MEDIA_PLAY_PAUSE;
    case Qt::Key_MediaPrevious:
        return VK_MEDIA_PREV_TRACK;
    case Qt::Key_MediaNext:
        return VK_MEDIA_NEXT_TRACK;
    case Qt::Key_VolumeUp:
        return VK_VOLUME_UP;
    case Qt::Key_VolumeDown:
        return VK_VOLUME_DOWN;
    case Qt::Key_VolumeMute:
        return VK_VOLUME_MUTE;
    default:
        break;
    }
    // OEM punctuation depends on the active keyboard layout.
    if (key > 0 && key < 128) {
        const SHORT mapped = VkKeyScanW(static_cast<WCHAR>(key));
        if (mapped != -1) {
            return LOBYTE(mapped);
        }
    }
    return 0;
}
#endif

} // namespace

ShortcutManager::ShortcutManager(QObject *parent) : QObject(parent)
{
    const auto add = [this](const QString &id, const QString &title, const QString &keys) {
        const QKeySequence sequence(keys, QKeySequence::PortableText);
        m_entries.append({id, title, sequence, sequence});
    };
    add(QStringLiteral("playPause"), QStringLiteral("播放 / 暂停"),
        QStringLiteral("Ctrl+Alt+F5"));
    add(QStringLiteral("previous"), QStringLiteral("上一曲"), QStringLiteral("Ctrl+Alt+Left"));
    add(QStringLiteral("next"), QStringLiteral("下一曲"), QStringLiteral("Ctrl+Alt+Right"));
    add(QStringLiteral("volumeUp"), QStringLiteral("音量增加"), QStringLiteral("Ctrl+Alt+Up"));
    add(QStringLiteral("volumeDown"), QStringLiteral("音量减少"), QStringLiteral("Ctrl+Alt+Down"));
    add(QStringLiteral("mute"), QStringLiteral("静音 / 取消静音"), QStringLiteral("Ctrl+Alt+M"));
    add(QStringLiteral("playMode"), QStringLiteral("切换播放模式"), QStringLiteral("Ctrl+Alt+R"));
    qApp->installNativeEventFilter(this);
}

ShortcutManager::~ShortcutManager()
{
    release();
    qApp->removeNativeEventFilter(this);
}

const QList<ShortcutManager::Entry> &ShortcutManager::entries() const
{
    return m_entries;
}

void ShortcutManager::initialize(const QString &configuration, SaveConfiguration save)
{
    m_save = std::move(save);
    const QJsonObject root = QJsonDocument::fromJson(configuration.toUtf8()).object();
    bool configurationCleaned = false;
    for (Entry &entry : m_entries) {
        if (!root.value(entry.id).isObject()) {
            continue;
        }
        const QJsonObject value = root.value(entry.id).toObject();
        if (value.value(QStringLiteral("keys")).isString()) {
            const QKeySequence savedSequence(value.value(QStringLiteral("keys")).toString(),
                                              QKeySequence::PortableText);
            entry.sequence = normalizedSequence(savedSequence);
            configurationCleaned = configurationCleaned || entry.sequence != savedSequence;
        }
        entry.enabled = value.value(QStringLiteral("enabled")).toBool(true);
    }
    m_initialized = true;
    if (configurationCleaned && commit(m_entries)) {
        return;
    }
    // Keep the in-memory configuration safe even if saving the cleaned configuration fails.
    registerShortcuts();
}

bool ShortcutManager::commit(const QList<Entry> &entries)
{
    if (!m_initialized) {
        return false;
    }
    QJsonObject root;
    for (const Entry &entry : entries) {
        root.insert(entry.id,
                    QJsonObject{
                        {QStringLiteral("keys"), entry.sequence.toString(QKeySequence::PortableText)},
                        {QStringLiteral("enabled"), entry.enabled}});
    }
    const QString configuration =
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    // Persist first: a failed save must leave both active bindings and editors unchanged.
    if (!m_save || !m_save(configuration)) {
        emit changed();
        return false;
    }
    ++m_generation;
    // Preserve unchanged registrations so editing one action cannot surrender another's priority.
    QList<Entry> updated = entries;
    for (int index = 0; index < m_entries.size(); ++index) {
        const Entry &previous = m_entries.at(index);
        Entry &next = updated[index];
        const bool unchanged = previous.sequence == next.sequence && previous.enabled == next.enabled;
        next.registered = previous.registered && unchanged;
        if (previous.registered && !unchanged) {
#ifdef Q_OS_WIN
            UnregisterHotKey(nullptr, kShortcutIdBase + index);
#endif
        }
    }
    m_entries = updated;
    registerShortcuts();
    return true;
}

bool ShortcutManager::update(int index, const QKeySequence &sequence, bool enabled)
{
    if (index < 0 || index >= m_entries.size()) {
        return false;
    }
    const QKeySequence normalized = normalizedSequence(sequence);
    if (m_entries.at(index).sequence == normalized && m_entries.at(index).enabled == enabled) {
        return true;
    }
    QList<Entry> updated = m_entries;
    updated[index].sequence = normalized;
    updated[index].enabled = enabled;
    return commit(updated);
}

void ShortcutManager::setAllEnabled(bool enabled)
{
    QList<Entry> updated = m_entries;
    for (Entry &entry : updated) {
        entry.enabled = enabled;
    }
    commit(updated);
}

void ShortcutManager::restoreDefaults()
{
    QList<Entry> updated = m_entries;
    for (Entry &entry : updated) {
        entry.sequence = entry.defaultSequence;
        entry.enabled = true;
    }
    commit(updated);
}

void ShortcutManager::release()
{
    ++m_generation;
    for (int index = 0; index < m_entries.size(); ++index) {
        Entry &entry = m_entries[index];
        if (entry.registered) {
#ifdef Q_OS_WIN
            UnregisterHotKey(nullptr, kShortcutIdBase + index);
#endif
            entry.registered = false;
        }
    }
}

void ShortcutManager::registerShortcuts(int onlyIndex)
{
    if (!m_initialized) {
        return;
    }
    if (m_recording) {
        // Clearing must immediately show Unset without re-enabling hotkeys during recording.
        for (Entry &entry : m_entries) {
            if (entry.sequence.isEmpty()) {
                entry.status = Status::Unset;
                entry.failureHint.clear();
            } else if (!entry.enabled) {
                entry.status = Status::Disabled;
                entry.failureHint.clear();
            }
        }
        emit changed();
        return;
    }
    for (int index = 0; index < m_entries.size(); ++index) {
        if (onlyIndex >= 0 && index != onlyIndex) {
            continue;
        }
        Entry &entry = m_entries[index];
        entry.failureHint.clear();
        if (entry.sequence.isEmpty()) {
            entry.status = Status::Unset;
        } else if (!entry.enabled) {
            entry.status = Status::Disabled;
        } else if (entry.registered) {
            entry.status = Status::Active;
        } else {
            entry.status = Status::Failed;
            entry.failureHint = QStringLiteral(
                "该快捷键注册失败，点击重试；若仍失败，请更换组合键。");
            bool duplicate = false;
            for (int other = 0; other < m_entries.size(); ++other) {
                if (other != index && m_entries.at(other).registered &&
                    m_entries.at(other).sequence == entry.sequence) {
                    duplicate = true;
                    entry.failureHint =
                        QStringLiteral("该快捷键与“%1”操作重复，请更换快捷键，"
                                       "或禁用使用相同按键的另一项操作后重试。")
                            .arg(m_entries.at(other).title);
                    break;
                }
            }
            if (duplicate) {
                entry.status = Status::Occupied;
                continue;
            }
#ifdef Q_OS_WIN
            if (entry.sequence.count() != 1) {
                continue;
            }
            const QKeyCombination combination = entry.sequence[0];
            const auto modifiers = combination.keyboardModifiers();
            const UINT key = virtualKey(combination.key());
            if (key == 0 || modifiers.testFlag(Qt::MetaModifier) ||
                modifiers.testFlag(Qt::KeypadModifier)) {
                continue;
            }
            UINT nativeModifiers = MOD_NOREPEAT;
            if (modifiers.testFlag(Qt::ControlModifier)) {
                nativeModifiers |= MOD_CONTROL;
            }
            if (modifiers.testFlag(Qt::AltModifier)) {
                nativeModifiers |= MOD_ALT;
            }
            if (modifiers.testFlag(Qt::ShiftModifier)) {
                nativeModifiers |= MOD_SHIFT;
            }
            if (RegisterHotKey(nullptr, kShortcutIdBase + index, nativeModifiers, key)) {
                entry.registered = true;
                entry.status = Status::Active;
            } else if (GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED) {
                entry.status = Status::Occupied;
                entry.failureHint = QStringLiteral(
                    "该快捷键已被系统或其他程序占用，释放后点击重试，"
                    "也可以双击输入框更换快捷键。");
            }
#endif
        }
    }
    emit changed();
}

void ShortcutManager::retry(int index)
{
    if (m_recording || index < 0 || index >= m_entries.size()) {
        return;
    }
    const Entry &entry = m_entries.at(index);
    if (entry.status != Status::Occupied && entry.status != Status::Failed) {
        return;
    }
    // Retry only this row, without releasing or retrying any other registration.
    registerShortcuts(index);
}

void ShortcutManager::setRecording(bool recording)
{
    if (m_recording == recording) {
        return;
    }
    m_recording = recording;
    if (m_recording) {
        release();
    } else {
        registerShortcuts();
    }
}

bool ShortcutManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(eventType);
#ifdef Q_OS_WIN
    const auto *nativeMessage = static_cast<MSG *>(message);
    if (nativeMessage != nullptr && nativeMessage->message == WM_HOTKEY) {
        const int index = static_cast<int>(nativeMessage->wParam) - kShortcutIdBase;
        if (index >= 0 && index < m_entries.size()) {
            if (m_entries.at(index).registered && !m_recording) {
                const QString action = m_entries.at(index).id;
                const quint64 generation = m_generation;
                // Ignore commands queued before recording or a configuration change.
                QTimer::singleShot(0, this, [this, index, action, generation] {
                    if (!m_recording && generation == m_generation &&
                        m_entries.at(index).registered) {
                        emit triggered(action);
                    }
                });
            }
            // Thread-level Windows messages can arrive with no result storage supplied by Qt.
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}
