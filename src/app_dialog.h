#ifndef APP_DIALOG_H
#define APP_DIALOG_H

#include <QDialog>
#include <QPixmap>
#include <QPointer>

class QVBoxLayout;

class AppDialog : public QDialog
{
public:
    AppDialog(const QString &title, QWidget *backgroundSurface,
              const QPixmap &backdrop, QWidget *parent);

    QVBoxLayout *bodyLayout() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QPointer<QWidget> m_backgroundSurface;
    QPixmap m_backdrop;
    QVBoxLayout *m_bodyLayout = nullptr;
    QWidget *m_titleBar = nullptr;
    bool m_positioned = false;
};

#endif // APP_DIALOG_H
