#include "app_dialog.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

AppDialog::AppDialog(const QString &title, QWidget *backgroundSurface,
                     const QPixmap &backdrop, QWidget *parent)
    : QDialog(parent)
    , m_backgroundSurface(backgroundSurface)
    , m_backdrop(backdrop)
{
    setProperty("appDialog", true);
    setWindowTitle(title);
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setSizeGripEnabled(true);

    // 将公共样式限定在弹框自身，防止主窗口的背景取色覆盖控件对比度。
    QFile styleFile(QStringLiteral(":/styles/app_dialog.qss"));
    if (styleFile.open(QIODevice::ReadOnly)) {
        setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    m_bodyLayout = new QVBoxLayout(this);
    m_bodyLayout->setContentsMargins(22, 14, 22, 18);
    m_bodyLayout->setSpacing(12);
    m_titleBar = new QWidget(this);
    m_titleBar->setObjectName(QStringLiteral("appDialogTitleBar"));
    auto *titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(0, 0, 0, 8);
    auto *titleLabel = new QLabel(title, m_titleBar);
    titleLabel->setObjectName(QStringLiteral("appDialogTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    auto *closeButton = new QPushButton(m_titleBar);
    closeButton->setObjectName(QStringLiteral("appDialogCloseButton"));
    closeButton->setIcon(QIcon(QStringLiteral(":/icons/close.svg")));
    closeButton->setIconSize(QSize(20, 20));
    closeButton->setToolTip(QStringLiteral("关闭"));
    closeButton->setAccessibleName(QStringLiteral("关闭"));
    closeButton->setAutoDefault(false);
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();
    titleLayout->addWidget(closeButton);
    m_bodyLayout->addWidget(m_titleBar);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    qApp->installEventFilter(this);
}

QVBoxLayout *AppDialog::bodyLayout() const
{
    return m_bodyLayout;
}

bool AppDialog::eventFilter(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget == nullptr || widget->window() != this || !isVisible()
        || event->type() != QEvent::MouseButtonPress) {
        return QDialog::eventFilter(watched, event);
    }
    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() != Qt::LeftButton || windowHandle() == nullptr) {
        return QDialog::eventFilter(watched, event);
    }

    const QPoint globalPosition = mouseEvent->globalPosition().toPoint();
    const QPoint localPosition = mapFromGlobal(globalPosition);
    constexpr int resizeBorder = 7;
    Qt::Edges edges;
    if (localPosition.x() < resizeBorder) {
        edges |= Qt::LeftEdge;
    } else if (localPosition.x() >= width() - resizeBorder) {
        edges |= Qt::RightEdge;
    }
    if (localPosition.y() < resizeBorder) {
        edges |= Qt::TopEdge;
    } else if (localPosition.y() >= height() - resizeBorder) {
        edges |= Qt::BottomEdge;
    }
    if (edges != Qt::Edges() && !isMaximized()) {
        return windowHandle()->startSystemResize(edges);
    }

    // 标题栏按钮正常接收点击，其余标题区域用于拖动窗口。
    if (qobject_cast<QPushButton *>(widget) == nullptr
        && (widget == m_titleBar || m_titleBar->isAncestorOf(widget))) {
        return windowHandle()->startSystemMove();
    }
    return QDialog::eventFilter(watched, event);
}

void AppDialog::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QRect panel = rect().adjusted(1, 1, -1, -1);
    painter.setClipRect(panel);
    painter.fillRect(panel, QColor(18, 21, 31));
    // 模态窗口使用打开时的模糊背景，移动时只调整取图位置，不重复模糊。
    if (!m_backdrop.isNull() && m_backgroundSurface != nullptr) {
        const QPoint origin = mapFromGlobal(
            m_backgroundSurface->mapToGlobal(QPoint(0, 0)));
        painter.drawPixmap(origin, m_backdrop);
    }
    painter.fillRect(panel, QColor(12, 15, 24, 175));
    painter.setClipping(false);
    painter.setPen(QColor(255, 255, 255, 45));
    painter.drawRect(panel);
}

void AppDialog::moveEvent(QMoveEvent *event)
{
    QDialog::moveEvent(event);
    update();
}

void AppDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_positioned || parentWidget() == nullptr) {
        return;
    }
    m_positioned = true;
    const QRect parentArea(parentWidget()->mapToGlobal(QPoint(0, 0)),
                           parentWidget()->size());
    QPoint position = parentArea.center() - QPoint(width() / 2, height() / 2);
    if (screen() != nullptr) {
        const QRect available = screen()->availableGeometry();
        position.setX(qBound(available.left(), position.x(),
                             qMax(available.left(), available.right() - width() + 1)));
        position.setY(qBound(available.top(), position.y(),
                             qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(position);
}
