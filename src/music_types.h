#ifndef MUSIC_TYPES_H
#define MUSIC_TYPES_H

#include <QJsonObject>
#include <QList>
#include <QString>

// Persisted values: pending = 0, completed (including an empty artist) = 1, failed = 2.
enum class ArtistScanState
{
    Pending = 0,
    Completed = 1,
    Failed = 2
};

struct Song
{
    qint64 id = 0;
    QString title;
    QString artist;
    QString path;
    ArtistScanState artistScanState = ArtistScanState::Pending;
};

struct MusicGroup
{
    qint64 id = 0;
    QString name;
};

struct LibrarySongRecord
{
    qint64 id = 0;
    QString title;
    QString path;
    QString contentSha256;
};

struct LibraryPathUpdate
{
    qint64 songId = 0;
    QString originalPath;
    QString title;
    QString path;
    QString contentSha256;
};

struct LibraryHashUpdate
{
    qint64 songId = 0;
    QString originalPath;
    QString contentSha256;
};

struct LibraryRefreshResult
{
    QList<LibraryPathUpdate> pathUpdates;
    QList<LibraryHashUpdate> hashUpdates;
    QString error;
};

struct MusicImportResult
{
    int added = 0;
    int duplicates = 0;
    int failed = 0;
    QString error;
};

#endif
