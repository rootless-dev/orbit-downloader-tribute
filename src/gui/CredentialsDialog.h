#pragma once
#include <QDialog>
class QLineEdit;

// Pede user/senha para um host FTP. As credenciais vivem SÓ em memória
// (spec §3.6): nada aqui é persistido.
class CredentialsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CredentialsDialog(const QString& host, QWidget* parent = nullptr);
    QString user() const;
    QString pass() const;
private:
    QLineEdit* m_user;
    QLineEdit* m_pass;
};
