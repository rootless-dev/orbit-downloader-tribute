#pragma once
#include <QString>
namespace FileType {
    enum class Category { Movie, Software, Music, Others };
    Category categorize(const QString& fileName);
    QString  displayName(Category c);
}
