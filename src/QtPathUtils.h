#pragma once
// QString <-> std::filesystem::path without going through the ANSI code page
// (see PathUtils.h).

#include "PathUtils.h"

#include <QString>

namespace shtetl
{

inline QString pathToQString(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<qsizetype>(u8.size()));
}

inline std::filesystem::path qStringToPath(const QString& s)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(s.toStdWString());
#else
    return utf8ToPath(s.toUtf8().toStdString());
#endif
}

} // namespace shtetl
