#ifndef AUDIO_FILE_EDITOR_H
#define AUDIO_FILE_EDITOR_H

#include <QJsonObject>
#include <QPromise>
#include <QString>

#include <memory>

class QTemporaryDir;

struct AudioMetadataSnapshot
{
    QString path;
    QString contentHash;
    QJsonObject properties;
    QJsonObject fallbackProperties;
    qint64 sizeBytes = 0;
    qint64 durationMs = 0;
    int bitrateKbps = 0;
    int sampleRateHz = 0;
    int channels = 0;
    QString error;
};

struct AudioEditRequest
{
    QString originalPath;
    QString targetPath;
    QString databasePath;
    QString expectedHash;
    QJsonObject properties;
    QString lyrics;
    bool lyricsOnly = false;
    bool overwriteConfirmed = false;
};

struct PreparedAudioEdit
{
    AudioEditRequest request;
    std::shared_ptr<QTemporaryDir> directory;
    QString stagedPath;
    QString originalHash;
    QString updatedHash;
    QJsonObject metadata;
    QString error;
    bool needsOverwrite = false;
};

struct AudioEditResult
{
    bool success = false;
    QString actualPath;
    QString message;
    QString warning;
};

void readAudioMetadata(QPromise<AudioMetadataSnapshot> &promise, const QString &audioPath);
PreparedAudioEdit prepareAudioEdit(const AudioEditRequest &request);
AudioEditResult commitAudioEdit(const PreparedAudioEdit &prepared);

#endif
