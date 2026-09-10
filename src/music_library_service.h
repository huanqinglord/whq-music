#ifndef MUSIC_LIBRARY_SERVICE_H
#define MUSIC_LIBRARY_SERVICE_H

#include "music_types.h"

#include <QHash>
#include <QPromise>

void collectMusicImport(QPromise<MusicImportResult> &promise, const QString &databasePath,
                        const QStringList &selectedFiles, const QString &folder, qint64 groupId);
void collectLibraryRefresh(QPromise<LibraryRefreshResult> &promise,
                           const QList<LibrarySongRecord> &songs);
QHash<QString, qint64> collectFileSizes(const QStringList &paths);

#endif
