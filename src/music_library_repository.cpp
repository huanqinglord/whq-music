#include "music_library_repository.h"

#include <QStringList>
#include <QDebug>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

MusicLibraryRepository::MusicLibraryRepository(const QSqlDatabase &database) : m_database(database)
{
}

bool MusicLibraryRepository::transaction(const std::function<bool()> &operation, QString &error)
{
    error.clear();
    if (!m_database.transaction()) {
        error = QStringLiteral("无法开始数据库操作，请稍后重试。");
        return false;
    }
    if (operation() && m_database.commit()) {
        return true;
    }
    error = QStringLiteral("数据库操作未能完成，本次更改未保存。");
    if (!m_database.rollback()) {
        error = QStringLiteral("数据库操作失败且无法确认回退结果，请重新加载后核对数据。");
        qWarning() << "Music database rollback failed" << m_database.lastError().type();
    }
    return false;
}

bool MusicLibraryRepository::initialize(QString &error)
{
    QSqlQuery pragma(m_database);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) {
        error = QStringLiteral("无法初始化音乐数据库。");
        return false;
    }
    return transaction(
        [this] {
            QSqlQuery query(m_database);
            const QStringList statements = {
                QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS songs(id INTEGER PRIMARY KEY AUTOINCREMENT,title "
                    "TEXT NOT NULL,artist TEXT NOT NULL DEFAULT '',path TEXT NOT NULL UNIQUE)"),
                QStringLiteral("CREATE TABLE IF NOT EXISTS groups(id INTEGER PRIMARY KEY "
                               "AUTOINCREMENT,name TEXT NOT NULL UNIQUE)"),
                QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS song_groups(song_id INTEGER NOT NULL,group_id "
                    "INTEGER NOT NULL,PRIMARY KEY(song_id,group_id),FOREIGN KEY(song_id) "
                    "REFERENCES songs(id) ON DELETE CASCADE,FOREIGN KEY(group_id) REFERENCES "
                    "groups(id) ON DELETE CASCADE)"),
                QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT)")};
            for (const QString &statement : statements) {
                if (!query.exec(statement)) {
                    return false;
                }
            }
            if (!query.exec(QStringLiteral("PRAGMA table_info(songs)"))) {
                return false;
            }
            QSet<QString> columns;
            while (query.next()) {
                columns.insert(query.value(1).toString());
            }
            query.finish();
            const QList<QPair<QString, QString>> additions = {
                {QStringLiteral("artist_scan_state"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
                {QStringLiteral("artist"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
                {QStringLiteral("content_sha256"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
                {QStringLiteral("metadata_json"), QStringLiteral("TEXT NOT NULL DEFAULT '{}'")}};
            for (const auto &column : additions) {
                if (!columns.contains(column.first) &&
                    !query.exec(QStringLiteral("ALTER TABLE songs ADD COLUMN %1 %2")
                                    .arg(column.first, column.second))) {
                    return false;
                }
            }
            return true;
        },
        error);
}

bool MusicLibraryRepository::loadSongs(QList<Song> &songs, QString &error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,artist,path,artist_scan_state FROM songs ORDER BY title,id"))) {
        error = QStringLiteral("无法读取音乐库，请稍后重试。");
        return false;
    }
    songs.clear();
    while (query.next()) {
        const QString path = query.value(2).toString();
        songs.append({query.value(0).toLongLong(), QFileInfo(path).completeBaseName(),
                      query.value(1).toString(), path,
                      static_cast<ArtistScanState>(query.value(3).toInt())});
    }
    return true;
}

bool MusicLibraryRepository::loadGroups(QList<MusicGroup> &groups, QString &error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id,name FROM groups ORDER BY id"))) {
        error = QStringLiteral("无法读取分组，请稍后重试。");
        return false;
    }
    groups.clear();
    while (query.next()) {
        groups.append({query.value(0).toLongLong(), query.value(1).toString()});
    }
    return true;
}

bool MusicLibraryRepository::groupSongs(qint64 groupId, QSet<qint64> &ids, QString &error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT song_id FROM song_groups WHERE group_id=?"));
    query.addBindValue(groupId);
    if (!query.exec()) {
        error = QStringLiteral("无法读取分组歌曲。");
        return false;
    }
    ids.clear();
    while (query.next()) {
        ids.insert(query.value(0).toLongLong());
    }
    return true;
}

bool MusicLibraryRepository::songGroups(qint64 songId, QSet<qint64> &ids, QString &error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT group_id FROM song_groups WHERE song_id=?"));
    query.addBindValue(songId);
    if (!query.exec()) {
        error = QStringLiteral("无法读取歌曲所属分组。");
        return false;
    }
    ids.clear();
    while (query.next()) {
        ids.insert(query.value(0).toLongLong());
    }
    return true;
}

bool MusicLibraryRepository::saveGroup(const QString &name, qint64 groupId, QString &error)
{
    QSqlQuery query(m_database);
    query.prepare(groupId > 0 ? QStringLiteral("UPDATE groups SET name=? WHERE id=?")
                              : QStringLiteral("INSERT INTO groups(name) VALUES(?)"));
    query.addBindValue(name);
    if (groupId > 0) {
        query.addBindValue(groupId);
    }
    if (!query.exec() || query.numRowsAffected() != 1) {
        error = QStringLiteral("无法保存分组，请检查名称是否重复或分组是否已被删除。");
        return false;
    }
    return true;
}

bool MusicLibraryRepository::deleteGroup(qint64 groupId, QString &error)
{
    return transaction(
        [this, groupId] {
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("DELETE FROM groups WHERE id=?"));
            query.addBindValue(groupId);
            return query.exec() && query.numRowsAffected() == 1;
        },
        error);
}

bool MusicLibraryRepository::deleteSong(qint64 songId, QString &error)
{
    return transaction(
        [this, songId] {
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("DELETE FROM songs WHERE id=?"));
            query.addBindValue(songId);
            return query.exec() && query.numRowsAffected() == 1;
        },
        error);
}

bool MusicLibraryRepository::assignGroups(qint64 songId, const QList<qint64> &groups,
                                          QString &error)
{
    return transaction(
        [this, songId, groups] {
            QSqlQuery query(m_database);
            query.prepare(QStringLiteral("SELECT id FROM songs WHERE id=?"));
            query.addBindValue(songId);
            if (!query.exec() || !query.next()) {
                return false;
            }
            query.finish();
            query.prepare(QStringLiteral("DELETE FROM song_groups WHERE song_id=?"));
            query.addBindValue(songId);
            if (!query.exec()) {
                return false;
            }
            for (qint64 groupId : groups) {
                query.prepare(
                    QStringLiteral("INSERT INTO song_groups(song_id,group_id) VALUES(?,?)"));
                query.addBindValue(songId);
                query.addBindValue(groupId);
                if (!query.exec()) {
                    return false;
                }
            }
            return true;
        },
        error);
}

bool MusicLibraryRepository::saveArtist(qint64 songId, const QString &artist, bool onlyEmpty,
                                        QString &error)
{
    QSqlQuery query(m_database);
    query.prepare(onlyEmpty ? QStringLiteral("UPDATE songs SET artist=? WHERE id=? AND artist=''")
                            : QStringLiteral("UPDATE songs SET artist=? WHERE id=?"));
    query.addBindValue(artist);
    query.addBindValue(songId);
    if (!query.exec()) {
        error = QStringLiteral("无法保存歌曲歌手信息。");
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool MusicLibraryRepository::saveArtistScan(qint64 songId, const QString &artist,
                                           ArtistScanState state, QString &error)
{
    QSqlQuery query(m_database);
    // Preserve artist information that playback may have populated during the scan.
    query.prepare(QStringLiteral(
        "UPDATE songs SET artist=CASE WHEN artist='' THEN ? ELSE artist END,"
        "artist_scan_state=? WHERE id=?"));
    query.addBindValue(artist);
    query.addBindValue(static_cast<int>(state));
    query.addBindValue(songId);
    if (!query.exec()) {
        error = QStringLiteral("无法保存歌曲扫描结果。");
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool MusicLibraryRepository::applyRefresh(const LibraryRefreshResult &result, QString &error)
{
    return transaction(
        [this, &result] {
            QSqlQuery query(m_database);
            for (const auto &update : result.hashUpdates) {
                query.prepare(QStringLiteral("UPDATE songs SET content_sha256=? WHERE id=? AND "
                                             "path=? AND content_sha256=''"));
                query.addBindValue(update.contentSha256);
                query.addBindValue(update.songId);
                query.addBindValue(update.originalPath);
                if (!query.exec()) {
                    return false;
                }
            }
            for (const auto &update : result.pathUpdates) {
                query.prepare(QStringLiteral(
                    "UPDATE songs SET title=?,path=? WHERE id=? AND path=? AND content_sha256=?"));
                query.addBindValue(update.title);
                query.addBindValue(update.path);
                query.addBindValue(update.songId);
                query.addBindValue(update.originalPath);
                query.addBindValue(update.contentSha256);
                if (!query.exec()) {
                    return false;
                }
            }
            return true;
        },
        error);
}

QString MusicLibraryRepository::setting(const QString &key, const QString &fallback) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key=?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : fallback;
}

bool MusicLibraryRepository::saveSettings(const QList<QPair<QString, QString>> &values,
                                          QString &error)
{
    return transaction(
        [this, &values] {
            QSqlQuery query(m_database);
            for (const auto &value : values) {
                query.prepare(
                    QStringLiteral("INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)"));
                query.addBindValue(value.first);
                query.addBindValue(value.second);
                if (!query.exec()) {
                    return false;
                }
            }
            return true;
        },
        error);
}
