#include "music_library_service.h"
#include "music_file_utils.h"

#include <QStringList>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

void collectMusicImport(QPromise<MusicImportResult> &promise, const QString &databasePath,
                        const QStringList &selectedFiles, const QString &folder, qint64 groupId)
{
    QStringList files = selectedFiles;
    promise.setProgressRange(0, 0);
    if (!folder.isEmpty()) {
        const QStringList filters = {QStringLiteral("*.mp3"),  QStringLiteral("*.wav"),
                                     QStringLiteral("*.flac"), QStringLiteral("*.m4a"),
                                     QStringLiteral("*.aac"),  QStringLiteral("*.ogg")};
        QDirIterator iterator(folder, filters, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (promise.isCanceled()) {
                return;
            }
            files.append(iterator.next());
            if (files.size() % 100 == 0) {
                promise.setProgressValueAndText(
                    0, QStringLiteral("正在扫描，已发现 %1 个音频文件").arg(files.size()));
            }
        }
    }
    MusicImportResult result;
    promise.setProgressRange(0, qMax(1, files.size()));
    const QString connectionName =
        QStringLiteral("music-import-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        // SQLite 连接仅在创建它的工作线程使用，并在移除连接前销毁所有查询。
        QSqlDatabase database =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
        if (!database.open()) {
            result.error = QStringLiteral("无法打开音乐数据库，未导入任何歌曲。");
        } else {
            QSqlQuery setup(database);
            if (!setup.exec(QStringLiteral("PRAGMA foreign_keys=ON")) || !database.transaction()) {
                result.error = QStringLiteral("无法开始导入，请稍后重试。");
            } else {
                for (int index = 0; index < files.size(); ++index) {
                    if (promise.isCanceled()) {
                        break;
                    }
                    const QFileInfo info(files.at(index));
                    const QString path = info.canonicalFilePath();
                    QFile readableFile(path);
                    bool saved =
                        !path.isEmpty() && info.isFile() && readableFile.open(QIODevice::ReadOnly);
                    readableFile.close();
                    bool added = false;
                    QSqlQuery query(database);
                    const bool savepoint =
                        saved && query.exec(QStringLiteral("SAVEPOINT import_song"));
                    saved = savepoint;
                    if (saved) {
                        query.prepare(
                            QStringLiteral("INSERT OR IGNORE INTO songs(title,path) VALUES(?,?)"));
                        query.addBindValue(info.completeBaseName());
                        query.addBindValue(path);
                        saved = query.exec();
                        added = saved && query.numRowsAffected() > 0;
                    }
                    if (saved && groupId > 0) {
                        query.prepare(
                            QStringLiteral("INSERT OR IGNORE INTO song_groups(song_id,group_id) "
                                           "SELECT id,? FROM songs WHERE path=?"));
                        query.addBindValue(groupId);
                        query.addBindValue(path);
                        saved = query.exec();
                    }
                    if (!saved && savepoint) {
                        if (!query.exec(QStringLiteral("ROLLBACK TO import_song"))) {
                            result.error = QStringLiteral("导入回退失败，本次导入已取消。");
                            break;
                        }
                    }
                    if (savepoint && !query.exec(QStringLiteral("RELEASE import_song"))) {
                        result.error = QStringLiteral("无法完成歌曲保存，本次导入已取消。");
                        break;
                    }
                    if (!saved) {
                        ++result.failed;
                    } else if (added) {
                        ++result.added;
                    } else {
                        ++result.duplicates;
                    }
                    promise.setProgressValueAndText(
                        index + 1,
                        QStringLiteral("正在导入：%1 / %2").arg(index + 1).arg(files.size()));
                }
                if (promise.isCanceled() || !result.error.isEmpty()) {
                    if (!database.rollback()) {
                        result.error = QStringLiteral("导入回退失败，请重新加载并核对音乐库。");
                    }
                    result.added = 0;
                    result.duplicates = 0;
                } else if (!database.commit()) {
                    result.error =
                        database.rollback()
                            ? QStringLiteral("无法提交导入结果，本次导入未保存，请重试。")
                            : QStringLiteral("导入回退失败，请重新加载并核对音乐库。");
                    result.added = 0;
                    result.duplicates = 0;
                }
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (!result.error.isEmpty()) {
        result.failed = files.size();
    }
    promise.addResult(result);
}

void collectLibraryRefresh(QPromise<LibraryRefreshResult> &promise,
                           const QList<LibrarySongRecord> &songs)
{
    LibraryRefreshResult result;
    QSet<QString> paths;
    QSet<QString> missingDirectories;
    QHash<QString, QList<LibrarySongRecord>> missingByHash;
    promise.setProgressRange(0, static_cast<int>(songs.size()) + 1);
    int completed = 0;
    for (const LibrarySongRecord &song : songs) {
        if (promise.isCanceled()) {
            return;
        }
        paths.insert(normalizedPath(song.path));
        const QFileInfo file(song.path);
        if (!file.exists() && !song.contentSha256.isEmpty()) {
            missingDirectories.insert(file.absolutePath());
            missingByHash[song.contentSha256].append(song);
        } else if (file.isFile() && song.contentSha256.isEmpty()) {
            const QString hash = fileSha256(song.path, [&promise] {
                return promise.isCanceled();
            });
            if (hash.isEmpty() && !promise.isCanceled()) {
                result.error = QStringLiteral("部分歌曲无法读取，未能完成文件校验。");
            } else if (!hash.isEmpty()) {
                result.hashUpdates.append({song.id, song.path, hash});
            }
        }
        promise.setProgressValue(++completed);
    }
    // 只为缺失记录寻找同目录的改名文件，未登记的新歌曲不自动入库。
    QHash<QString, QStringList> matches;
    const QStringList filters = {QStringLiteral("*.mp3"),  QStringLiteral("*.wav"),
                                 QStringLiteral("*.flac"), QStringLiteral("*.m4a"),
                                 QStringLiteral("*.aac"),  QStringLiteral("*.ogg")};
    QStringList candidates;
    for (const QString &directory : missingDirectories) {
        QDirIterator iterator(directory, filters, QDir::Files | QDir::Readable);
        while (iterator.hasNext()) {
            if (promise.isCanceled()) {
                return;
            }
            iterator.next();
            const QString path = iterator.fileInfo().canonicalFilePath();
            if (!path.isEmpty() && !paths.contains(normalizedPath(path))) {
                candidates.append(path);
            }
        }
    }
    promise.setProgressRange(0, static_cast<int>(songs.size() + candidates.size()) + 1);
    for (const QString &path : candidates) {
        if (promise.isCanceled()) {
            return;
        }
        const QString hash = fileSha256(path, [&promise] {
            return promise.isCanceled();
        });
        if (missingByHash.contains(hash)) {
            matches[hash].append(path);
        }
        promise.setProgressValue(++completed);
    }
    for (auto match = matches.cbegin(); match != matches.cend(); ++match) {
        const QList<LibrarySongRecord> originals = missingByHash.value(match.key());
        if (originals.size() == 1 && match.value().size() == 1) {
            const auto &original = originals.first();
            const QString path = match.value().first();
            result.pathUpdates.append({original.id, original.path,
                                       QFileInfo(path).completeBaseName(), path, match.key()});
        }
    }
    promise.setProgressValue(++completed);
    promise.addResult(result);
}

QHash<QString, qint64> collectFileSizes(const QStringList &paths)
{
    QHash<QString, qint64> sizes;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        sizes.insert(normalizedPath(path), info.isFile() ? info.size() : -1);
    }
    return sizes;
}
