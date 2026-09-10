#ifndef MUSIC_FILE_UTILS_H
#define MUSIC_FILE_UTILS_H

#include <QString>

#include <functional>

QString normalizedPath(const QString &path);
QString fileSha256(const QString &path, const std::function<bool()> &canceled = {});

#endif
