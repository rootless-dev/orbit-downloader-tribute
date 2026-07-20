#include "FileType.h"
#include <QFileInfo>
#include <QSet>

namespace {
    const QSet<QString> kMovie    = {"mp4","mkv","avi","mov","wmv","flv","webm","m4v","mpg","mpeg"};
    const QSet<QString> kSoftware = {"dmg","pkg","exe","msi","deb","rpm","appimage","app",
                                     "zip","7z","rar","tar","gz","iso"};
    const QSet<QString> kMusic    = {"mp3","flac","aac","wav","ogg","m4a","wma","opus"};
}

namespace FileType {
Category categorize(const QString& fileName) {
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (kMovie.contains(ext))    return Category::Movie;
    if (kSoftware.contains(ext)) return Category::Software;
    if (kMusic.contains(ext))    return Category::Music;
    return Category::Others;
}
QString displayName(Category c) {
    switch (c) {
        case Category::Movie:    return "Movie";
        case Category::Software: return "Software";
        case Category::Music:    return "Music";
        case Category::Others:   return "Others";
    }
    return "Others";
}
}
