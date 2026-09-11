#include "audio_file_editor.h"
#include "music_file_utils.h"
#include "music_library_repository.h"

#include <QStringList>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>

#include <audioproperties.h>
#include <fileref.h>
#include <tag.h>
#include <tfile.h>
#include <tpropertymap.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
TagLib::FileRef openTags(const QString &path, bool readAudioProperties = false)
{
#ifdef Q_OS_WIN
    return TagLib::FileRef(path.toStdWString().c_str(), readAudioProperties,
                           TagLib::AudioProperties::Fast);
#else
    return TagLib::FileRef(QFile::encodeName(path).constData(), readAudioProperties,
                           TagLib::AudioProperties::Fast);
#endif
}

QJsonObject propertiesJson(const TagLib::PropertyMap &properties)
{
    QJsonObject result;
    for (const auto &property : properties) {
        QJsonArray values;
        for (const auto &value : property.second) {
            values.append(QString::fromStdString(value.to8Bit(true)));
        }
        result.insert(QString::fromStdString(property.first.to8Bit(true)), values);
    }
    return result;
}

bool isLyricsKey(const TagLib::String &key)
{
    return key == "LYRICS" || key.startsWith("LYRICS:") || key == "UNSYNCEDLYRICS" ||
           key == "SYNCEDLYRICS" || key == "SYNCHRONIZEDLYRICS";
}

bool copyAndHash(const QString &sourcePath, const QString &targetPath, QString &hash)
{
    QFile source(sourcePath);
    QFile target(targetPath);
    if (!source.open(QIODevice::ReadOnly) ||
        !target.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if ((chunk.isEmpty() && source.error() != QFileDevice::NoError) ||
            target.write(chunk) != chunk.size()) {
            return false;
        }
        digest.addData(chunk);
    }
    if (!target.flush()) {
        return false;
    }
    hash = QString::fromLatin1(digest.result().toHex());
    return true;
}
} // namespace

void readAudioMetadata(QPromise<AudioMetadataSnapshot> &promise, const QString &audioPath)
{
    AudioMetadataSnapshot snapshot;
    snapshot.path = audioPath;
    const auto canceled = [&promise] {
        return promise.isCanceled();
    };
    snapshot.contentHash = fileSha256(audioPath, canceled);
    if (canceled()) {
        return;
    }
    if (snapshot.contentHash.isEmpty()) {
        snapshot.error = QStringLiteral("无法读取歌曲文件，请检查文件是否存在及读取权限。");
        promise.addResult(snapshot);
        return;
    }
    {
        auto file = openTags(audioPath, true);
        if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
            snapshot.error = QStringLiteral("无法读取该音频文件的元数据。");
            promise.addResult(snapshot);
            return;
        }
        snapshot.properties = propertiesJson(file.file()->properties());
        if (const auto *tag = file.tag()) {
            const auto addFallback = [&snapshot](const QString &key, const TagLib::String &value) {
                snapshot.fallbackProperties.insert(key, QString::fromStdString(value.to8Bit(true)));
            };
            addFallback(QStringLiteral("TITLE"), tag->title());
            addFallback(QStringLiteral("ARTIST"), tag->artist());
            addFallback(QStringLiteral("ALBUM"), tag->album());
            addFallback(QStringLiteral("GENRE"), tag->genre());
            addFallback(QStringLiteral("COMMENT"), tag->comment());
            if (tag->year() > 0) {
                snapshot.fallbackProperties.insert(QStringLiteral("DATE"),
                                                   QString::number(tag->year()));
            }
            if (tag->track() > 0) {
                snapshot.fallbackProperties.insert(QStringLiteral("TRACKNUMBER"),
                                                   QString::number(tag->track()));
            }
        }
        if (const auto *properties = file.audioProperties()) {
            snapshot.durationMs = properties->lengthInMilliseconds();
            snapshot.bitrateKbps = properties->bitrate();
            snapshot.sampleRateHz = properties->sampleRate();
            snapshot.channels = properties->channels();
        }
        snapshot.sizeBytes = QFileInfo(audioPath).size();
    }
    // 标签和哈希必须来自同一版文件；外部程序在读取期间改写时拒绝提交旧快照。
    const QString finalHash = fileSha256(audioPath, canceled);
    if (canceled()) {
        return;
    }
    if (finalHash != snapshot.contentHash) {
        snapshot.error = QStringLiteral("歌曲在读取期间已发生变化，请重新打开后重试。");
    }
    promise.addResult(snapshot);
}

PreparedAudioEdit prepareAudioEdit(const AudioEditRequest &request)
{
    PreparedAudioEdit prepared;
    prepared.request = request;
    const QFileInfo sourceInfo(request.originalPath);
    if (!sourceInfo.isFile()) {
        prepared.error = QStringLiteral("歌曲文件不存在，保存失败，请刷新音乐库后重试。");
        return prepared;
    }
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(request.originalPath.toStdWString().c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        prepared.error = QStringLiteral(
            "歌曲文件为只读，无法保存歌词或元数据。请在文件属性中取消“只读”后重试，原文件未修改。");
        return prepared;
    }
#endif
    if (!sourceInfo.isWritable()) {
        prepared.error = QStringLiteral("歌曲文件没有写入权限，保存失败，原文件未修改。");
        return prepared;
    }
    const bool rename = normalizedPath(request.originalPath) != normalizedPath(request.targetPath);
    if (rename && QFileInfo::exists(request.targetPath)) {
        prepared.error = QStringLiteral("目标文件已存在，原文件未修改。");
        return prepared;
    }
    prepared.directory = std::make_shared<QTemporaryDir>(
        sourceInfo.dir().filePath(QStringLiteral(".whq-edit-XXXXXX")));
    if (!prepared.directory->isValid()) {
        prepared.error = QStringLiteral("无法创建歌曲副本，请检查目录权限和磁盘空间。");
        return prepared;
    }
    prepared.stagedPath =
        prepared.directory->filePath(QStringLiteral("edited.") + sourceInfo.suffix());
    if (!copyAndHash(request.originalPath, prepared.stagedPath, prepared.originalHash)) {
        prepared.error = QStringLiteral("复制歌曲失败，原文件未修改。");
        return prepared;
    }
    if (!request.expectedHash.isEmpty() && prepared.originalHash != request.expectedHash) {
        prepared.error = QStringLiteral("歌曲在编辑期间已被修改，请重新打开后重试。");
        return prepared;
    }
    TagLib::PropertyMap wanted;
    TagLib::PropertyMap original;
    {
        auto file = openTags(prepared.stagedPath);
        if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
            prepared.error = QStringLiteral("无法读取歌曲标签，原文件未修改。");
            return prepared;
        }
        original = file.file()->properties();
        wanted = original;
        if (request.lyricsOnly) {
            if (request.lyrics.trimmed().isEmpty()) {
                prepared.error = QStringLiteral("歌词内容为空。");
                return prepared;
            }
            for (const auto &property : original) {
                if (!isLyricsKey(property.first)) {
                    continue;
                }
                for (const auto &value : property.second) {
                    if (!value.isEmpty() && !request.overwriteConfirmed) {
                        prepared.needsOverwrite = true;
                        return prepared;
                    }
                }
                wanted.erase(property.first);
            }
            wanted.replace("LYRICS",
                           TagLib::StringList(TagLib::String(request.lyrics.toUtf8().constData(),
                                                             TagLib::String::UTF8)));
        } else {
            wanted.clear();
            for (auto property = request.properties.begin(); property != request.properties.end();
                 ++property) {
                TagLib::StringList values;
                for (const QJsonValue &value : property.value().toArray()) {
                    values.append(TagLib::String(value.toString().toUtf8().constData(),
                                                 TagLib::String::UTF8));
                }
                wanted.replace(
                    TagLib::String(property.key().toUtf8().constData(), TagLib::String::UTF8),
                    values);
            }
        }
        if (!file.file()->setProperties(wanted).isEmpty() || !file.file()->save()) {
            prepared.error = QStringLiteral("此音频格式未能完整保存标签，原文件未修改。");
            return prepared;
        }
    }
    {
        auto file = openTags(prepared.stagedPath);
        if (file.isNull() || file.file() == nullptr || !file.file()->isValid()) {
            prepared.error = QStringLiteral("无法验证写入后的歌曲，原文件未修改。");
            return prepared;
        }
        const auto actual = file.file()->properties();
        // 验证每个要求保存及删除的字段，不能把部分写入当成保存成功。
        for (const auto &property : wanted) {
            const auto found = actual.find(property.first);
            if (found == actual.end() || found->second != property.second) {
                prepared.error = QStringLiteral("标签回读验证不一致，原文件未修改。");
                return prepared;
            }
        }
        for (const auto &property : original) {
            if (!wanted.contains(property.first) && actual.contains(property.first)) {
                prepared.error = QStringLiteral("部分标签未能删除，原文件未修改。");
                return prepared;
            }
        }
        prepared.metadata = propertiesJson(actual);
    }
    if (!QFile::setPermissions(prepared.stagedPath, sourceInfo.permissions())) {
        prepared.error = QStringLiteral("无法保留原歌曲文件权限，原文件未修改。");
        return prepared;
    }
    prepared.updatedHash = fileSha256(prepared.stagedPath);
    if (prepared.updatedHash.isEmpty()) {
        prepared.error = QStringLiteral("无法校验保存后的歌曲，原文件未修改。");
    }
    return prepared;
}

AudioEditResult commitAudioEdit(const PreparedAudioEdit &prepared)
{
    AudioEditResult result;
    result.actualPath = prepared.request.originalPath;
    if (!prepared.error.isEmpty() || prepared.needsOverwrite || prepared.directory == nullptr) {
        result.message = prepared.error;
        return result;
    }
    const auto &request = prepared.request;
    if (fileSha256(request.originalPath) != prepared.originalHash) {
        result.message = QStringLiteral("歌曲已被其他程序修改，本次保存已停止。");
        return result;
    }
    const QFileInfo originalInfo(request.originalPath);
    const QFileInfo targetInfo(request.targetPath);
    const bool rename = normalizedPath(request.originalPath) != normalizedPath(request.targetPath);
    const QString oldLyrics =
        originalInfo.dir().filePath(originalInfo.completeBaseName() + QStringLiteral(".lrc"));
    const QString newLyrics =
        targetInfo.dir().filePath(targetInfo.completeBaseName() + QStringLiteral(".lrc"));
    const bool moveLyrics = rename && QFileInfo::exists(oldLyrics);
    if ((rename && QFileInfo::exists(request.targetPath)) ||
        (moveLyrics && QFileInfo::exists(newLyrics))) {
        result.message = QStringLiteral("目标音频或歌词文件已存在，本次保存已停止。");
        return result;
    }
    const QString backupPath =
        prepared.directory->filePath(QStringLiteral("original.") + originalInfo.suffix());
    bool originalMoved = false;
    bool editedMoved = false;
    bool lyricsMoved = false;
    // 从开始替换到数据库提交前保留原始文件；崩溃或回退失败时备份不能自动删除。
    prepared.directory->setAutoRemove(false);
    const auto rollbackFiles = [&] {
        bool restored = true;
        if (lyricsMoved && !QFile::rename(newLyrics, oldLyrics)) {
            restored = false;
        }
        if (editedMoved && !QFile::remove(request.targetPath)) {
            restored = false;
        }
        if (originalMoved && !QFile::rename(backupPath, request.originalPath)) {
            restored = false;
        }
        if (restored) {
            prepared.directory->setAutoRemove(true);
            result.message += QStringLiteral(" 原文件已恢复。");
        } else {
            result.message += QStringLiteral(" 未能完整回退，请保留并检查备份目录：%1")
                                  .arg(prepared.directory->path());
            if (!QFileInfo::exists(request.originalPath)) {
                result.actualPath =
                    QFileInfo::exists(request.targetPath) ? request.targetPath : QString();
            }
        }
    };
    originalMoved = QFile::rename(request.originalPath, backupPath);
    if (!originalMoved) {
        prepared.directory->setAutoRemove(true);
        result.message = QStringLiteral("音频文件被占用或不可修改，原文件未修改。");
        return result;
    }
    editedMoved = QFile::rename(prepared.stagedPath, request.targetPath);
    if (!editedMoved) {
        result.message = QStringLiteral("无法替换音频文件。");
        rollbackFiles();
        return result;
    }
    if (moveLyrics) {
        lyricsMoved = QFile::rename(oldLyrics, newLyrics);
        if (!lyricsMoved) {
            result.message = QStringLiteral("无法重命名配套歌词文件。");
            rollbackFiles();
            return result;
        }
    }
    bool saved = false;
    const QString connectionName =
        QStringLiteral("audio-edit-%1").arg(QUuid::createUuid().toString());
    {
        QSqlDatabase database =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(request.databasePath);
        database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=1000"));
        if (database.open()) {
            MusicLibraryRepository repository(database);
            QString error;
            saved = repository.transaction(
                [&] {
                    QSqlQuery query(database);
                    query.prepare(QStringLiteral(
                        "UPDATE songs SET title=?,artist=?,path=?,content_sha256=?,metadata_json=? "
                        "WHERE path=?"));
                    query.addBindValue(targetInfo.completeBaseName());
                    const QJsonArray artists =
                        prepared.metadata.value(QStringLiteral("ARTIST")).toArray();
                    QStringList names;
                    for (const QJsonValue &artist : artists) {
                        names.append(artist.toString());
                    }
                    // 无 ARTIST 标签的歌曲也能保存歌词，不能向非空字段写入 SQL NULL。
                    const QString artist = names.join(QStringLiteral(" / "));
                    query.addBindValue(artist.isNull() ? QStringLiteral("") : artist);
                    query.addBindValue(request.targetPath);
                    query.addBindValue(prepared.updatedHash);
                    query.addBindValue(QString::fromUtf8(
                        QJsonDocument(prepared.metadata).toJson(QJsonDocument::Compact)));
                    query.addBindValue(request.originalPath);
                    if (!query.exec()) {
                        qWarning() << "Failed to persist audio edit, lyrics only:"
                                   << request.lyricsOnly
                                   << "database error:" << query.lastError().databaseText()
                                   << "code:" << query.lastError().nativeErrorCode();
                        return false;
                    }
                    if (query.numRowsAffected() != 1) {
                        qWarning() << "Audio edit record mismatch, lyrics only:"
                                   << request.lyricsOnly
                                   << "affected rows:" << query.numRowsAffected();
                        return false;
                    }
                    return true;
                },
                error);
            if (!saved) {
                result.message = error;
            }
        } else {
            result.message = QStringLiteral("无法更新音乐数据库。");
            qWarning() << "Failed to open audio edit database:"
                       << database.lastError().databaseText()
                       << "code:" << database.lastError().nativeErrorCode();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    if (!saved) {
        rollbackFiles();
        return result;
    }
    result.success = true;
    result.actualPath = request.targetPath;
    result.message = request.lyricsOnly ? QStringLiteral("歌词已保存到歌曲。")
                                        : QStringLiteral("歌曲元数据已保存。");
    if (QFile::remove(backupPath)) {
        prepared.directory->setAutoRemove(true);
    } else {
        qWarning() << "Audio edit backup retained after successful save";
        result.warning = QStringLiteral("旧文件备份未能清理，保留于：%1").arg(backupPath);
        result.message += QLatin1Char('\n') + result.warning;
    }
    return result;
}
