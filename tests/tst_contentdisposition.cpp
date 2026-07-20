#include <QtTest>
#include "ContentDisposition.h"

class TstContentDisposition : public QObject {
    Q_OBJECT
private slots:
    void plain_filename() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"Audiobook.m4a\""),
                 QString("Audiobook.m4a"));
    }
    void plain_filename_unquoted() {
        QCOMPARE(parseContentDisposition("attachment; filename=song.mp3"),
                 QString("song.mp3"));
    }
    void extended_utf8_decodes_accents() {
        QCOMPARE(parseContentDisposition("attachment; filename*=UTF-8''Cora%C3%A7%C3%A3o.m4a"),
                 QString("Coração.m4a"));
    }
    void extended_wins_over_plain() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"x.m4a\"; filename*=UTF-8''y.m4a"),
                 QString("y.m4a"));
    }
    void strips_path_traversal_to_basename() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"../../etc/passwd\""),
                 QString("passwd"));
    }
    void strips_backslash_path() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"..\\\\..\\\\secret.txt\""),
                 QString("secret.txt"));
    }
    void inline_without_filename_is_empty() {
        QCOMPARE(parseContentDisposition("inline"), QString());
    }
    void empty_and_malformed_are_empty() {
        QCOMPARE(parseContentDisposition(""), QString());
        QCOMPARE(parseContentDisposition("garbage;;;="), QString());
    }
    // Fix 2: extended value must be sanitized BEFORE the non-empty check that
    // decides between filename* and filename, so a malicious/degenerate
    // extended value (here: "../" -> sanitizes to empty) falls back to the
    // valid plain filename instead of discarding it.
    void extended_sanitizing_to_empty_falls_back_to_plain() {
        QCOMPARE(parseContentDisposition(
                     "attachment; filename=\"real.zip\"; filename*=UTF-8''%2e%2e%2f"),
                 QString("real.zip"));
    }
    // Fix 3: splitParams must not toggle quote state on an escaped quote
    // (\"), otherwise the ';' inside the quoted value would be treated as a
    // param separator and truncate/garble the filename. Expected result
    // derives from: quotes stripped -> a\";b.txt -> \" unescaped to " ->
    // a";b.txt -> sanitizeFileName drops the invalid '"' char -> a;b.txt.
    void escaped_quote_inside_value_does_not_split_params() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"a\\\";b.txt\""),
                 QString("a;b.txt"));
    }
};

QTEST_MAIN(TstContentDisposition)
#include "tst_contentdisposition.moc"
