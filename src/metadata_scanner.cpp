#include "metadata_scanner.h"

#include <QStringList>
#include <QDebug>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

MetadataScanner::MetadataScanner(QObject *parent) : QObject(parent), m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(10000);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        qWarning() << "Metadata scan timed out, song:" << m_current.first;
        finishCurrent(false);
    });
}

void MetadataScanner::enqueue(qint64 songId, const QString &path)
{
    if (songId <= 0 || path.isEmpty() || m_pendingIds.contains(songId)) {
        return;
    }
    m_pendingIds.insert(songId);
    m_queue.append({songId, path});
    startNext();
}

void MetadataScanner::suspend()
{
    m_suspended = true;
    m_timeout->stop();
    if (m_current.first > 0) {
        m_queue.prepend(m_current);
        m_current = {};
    }
    if (m_player != nullptr) {
        m_player->disconnect(this);
        delete m_player;
    }
}

void MetadataScanner::resume()
{
    m_suspended = false;
    startNext();
}

void MetadataScanner::startNext()
{
    if (m_suspended || m_player != nullptr || m_queue.isEmpty()) {
        return;
    }
    m_current = m_queue.takeFirst();
    // 每首使用独立对象，旧文件延迟到达的状态不能结束新文件的扫描。
    m_player = new QMediaPlayer(this);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::LoadedMedia) {
                    finishCurrent(true);
                } else if (status == QMediaPlayer::InvalidMedia) {
                    finishCurrent(false);
                }
            });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &) {
                if (error != QMediaPlayer::NoError) {
                    qWarning() << "Metadata scan failed, song:" << m_current.first << error;
                    finishCurrent(false);
                }
            });
    m_timeout->start();
    m_player->setSource(QUrl::fromLocalFile(m_current.second));
}

void MetadataScanner::finishCurrent(bool readArtist)
{
    if (m_player == nullptr) {
        return;
    }
    m_timeout->stop();
    const qint64 songId = m_current.first;
    const QMediaMetaData metadata = m_player->metaData();
    QString artist;
    if (readArtist) {
        artist = metadata.value(QMediaMetaData::ContributingArtist)
                     .toStringList()
                     .join(QStringLiteral(" / "));
        if (artist.isEmpty()) {
            artist = metadata.stringValue(QMediaMetaData::AlbumArtist);
        }
        if (artist.isEmpty()) {
            artist = metadata.stringValue(QMediaMetaData::LeadPerformer);
        }
    }
    m_player->disconnect(this);
    m_player->deleteLater();
    m_player = nullptr;
    m_current = {};
    m_pendingIds.remove(songId);
    // Empty metadata is also a completed scan and must survive application restarts.
    emit scanFinished(songId, artist.trimmed(), readArtist);
    QTimer::singleShot(0, this, &MetadataScanner::startNext);
}
