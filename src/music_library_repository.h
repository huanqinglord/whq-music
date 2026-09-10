#ifndef MUSIC_LIBRARY_REPOSITORY_H
#define MUSIC_LIBRARY_REPOSITORY_H

#include "music_types.h"

#include <QSet>
#include <QSqlDatabase>

#include <functional>

class MusicLibraryRepository
{
public:
    explicit MusicLibraryRepository(const QSqlDatabase &database);
    bool initialize(QString &error);
    bool loadSongs(QList<Song> &songs, QString &error) const;
    bool loadGroups(QList<MusicGroup> &groups, QString &error) const;
    bool groupSongs(qint64 groupId, QSet<qint64> &ids, QString &error) const;
    bool songGroups(qint64 songId, QSet<qint64> &ids, QString &error) const;
    bool saveGroup(const QString &name, qint64 groupId, QString &error);
    bool deleteGroup(qint64 groupId, QString &error);
    bool deleteSong(qint64 songId, QString &error);
    bool assignGroups(qint64 songId, const QList<qint64> &groups, QString &error);
    bool saveArtist(qint64 songId, const QString &artist, bool onlyEmpty, QString &error);
    bool saveArtistScan(qint64 songId, const QString &artist, ArtistScanState state,
                        QString &error);
    bool applyRefresh(const LibraryRefreshResult &result, QString &error);
    bool saveSettings(const QList<QPair<QString, QString>> &values, QString &error);
    QString setting(const QString &key, const QString &fallback) const;
    bool transaction(const std::function<bool()> &operation, QString &error);

private:
    QSqlDatabase m_database;
};

#endif
