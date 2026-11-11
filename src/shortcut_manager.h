#ifndef SHORTCUT_MANAGER_H
#define SHORTCUT_MANAGER_H

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class ShortcutManager final : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    enum class Status
    {
        Active,
        Occupied,
        Unset,
        Disabled,
        Failed
    };

    struct Entry
    {
        QString id;
        QString title;
        QKeySequence sequence;
        QKeySequence defaultSequence;
        bool enabled = true;
        Status status = Status::Unset;
        bool registered = false;
        QString failureHint;
    };

    using SaveConfiguration = std::function<bool(const QString &)>;

    explicit ShortcutManager(QObject *parent = nullptr);
    ~ShortcutManager() override;
    const QList<Entry> &entries() const;
    void initialize(const QString &configuration, SaveConfiguration save);
    bool update(int index, const QKeySequence &sequence, bool enabled);
    void setAllEnabled(bool enabled);
    void restoreDefaults();
    void retry(int index);
    void setRecording(bool recording);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void changed();
    void triggered(const QString &action);

private:
    bool commit(const QList<Entry> &entries);
    void release();
    void registerShortcuts(int onlyIndex = -1);

    QList<Entry> m_entries;
    SaveConfiguration m_save;
    bool m_initialized = false;
    bool m_recording = false;
    quint64 m_generation = 0;
};

#endif // SHORTCUT_MANAGER_H
