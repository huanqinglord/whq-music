#ifndef METADATA_SCANNER_H
#define METADATA_SCANNER_H

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>

class QMediaPlayer;
class QTimer;

class MetadataScanner final : public QObject
{
    Q_OBJECT
public:
    explicit MetadataScanner(QObject *parent = nullptr);
    void enqueue(qint64 songId, const QString &path);
    void suspend();
    void resume();

signals:
    void scanFinished(qint64 songId, const QString &artist, bool succeeded);

private:
    void startNext();
    void finishCurrent(bool readArtist);
    QList<QPair<qint64, QString>> m_queue;
    QPair<qint64, QString> m_current;
    QSet<qint64> m_pendingIds;
    QPointer<QMediaPlayer> m_player;
    QTimer *m_timeout;
    bool m_suspended = false;
};

#endif
