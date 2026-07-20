#include "CredentialsDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

CredentialsDialog::CredentialsDialog(const QString& host, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Authentication required"));

    auto* form = new QFormLayout(this);
    form->addRow(new QLabel(tr("The server %1 requires a login.").arg(host), this));

    m_user = new QLineEdit(this);
    m_user->setObjectName("userEdit");
    form->addRow(tr("User:"), m_user);

    m_pass = new QLineEdit(this);
    m_pass->setObjectName("passEdit");
    m_pass->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), m_pass);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(box);
}

QString CredentialsDialog::user() const { return m_user->text(); }
QString CredentialsDialog::pass() const { return m_pass->text(); }
