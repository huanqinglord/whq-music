#include "main_window.h"
#include "player_widgets.h"

#include <QApplication>
#include <QEasingCurve>
#include <QFileDialog>
#include <QFontComboBox>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSequentialAnimationGroup>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVariant>

using namespace PlayerWidgets;

QWidget *MainWindow::createSettingsPage()
{
    auto *settingsPage = new QWidget(this);
    settingsPage->setObjectName(QStringLiteral("settingsPage"));
    auto *settingsPageLayout = new QVBoxLayout(settingsPage);
    settingsPageLayout->setContentsMargins(48, 42, 48, 32);
    auto *settingsPanel = new QWidget(settingsPage);
    settingsPanel->setObjectName(QStringLiteral("settingsPanel"));
    settingsPanel->setMaximumWidth(820);
    settingsPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *settingsContentLayout = new QVBoxLayout(settingsPanel);
    settingsContentLayout->setContentsMargins(0, 0, 0, 0);
    settingsContentLayout->setSpacing(24);
    auto *settingsTitle = new QLabel(QStringLiteral("设置"), settingsPanel);
    settingsTitle->setObjectName(QStringLiteral("pageTitle"));
    auto *restoreSettingsButton = new QPushButton(QStringLiteral("恢复默认"), settingsPanel);
    restoreSettingsButton->setObjectName(QStringLiteral("ghostButton"));
    auto *settingsHeader = new QHBoxLayout;
    settingsHeader->addWidget(settingsTitle);
    settingsHeader->addStretch();
    settingsHeader->addWidget(restoreSettingsButton, 0, Qt::AlignVCenter);

    auto *fontColumns = new QHBoxLayout;
    fontColumns->setSpacing(28);
    auto *applicationFontColumn = new QWidget(settingsPanel);
    auto *applicationFontLayout = new QVBoxLayout(applicationFontColumn);
    applicationFontLayout->setContentsMargins(0, 0, 0, 0);
    applicationFontLayout->setSpacing(10);
    auto *applicationFontLabel = new QLabel(QStringLiteral("界面字体"), applicationFontColumn);
    applicationFontLabel->setObjectName(QStringLiteral("settingsFieldLabel"));
    m_applicationFontSelector = new QFontComboBox(settingsPanel);
    m_applicationFontSizeSelector = new QSpinBox(applicationFontColumn);
    m_applicationFontSizeSelector->setRange(10, 24);
    m_applicationFontSizeSelector->setValue(16);
    m_applicationFontSizeSelector->setSuffix(QStringLiteral(" px"));
    auto *applicationPreview =
        new QLabel(QStringLiteral("效果预览：Whq Music 音乐播放器"), applicationFontColumn);
    applicationPreview->setObjectName(QStringLiteral("settingsPreview"));
    applicationFontLayout->addWidget(applicationFontLabel);
    applicationFontLayout->addWidget(m_applicationFontSelector);
    applicationFontLayout->addWidget(m_applicationFontSizeSelector);
    applicationFontLayout->addWidget(applicationPreview);

    auto *lyricsFontColumn = new QWidget(settingsPanel);
    auto *lyricsFontLayout = new QVBoxLayout(lyricsFontColumn);
    lyricsFontLayout->setContentsMargins(0, 0, 0, 0);
    lyricsFontLayout->setSpacing(10);
    auto *lyricsFontLabel = new QLabel(QStringLiteral("歌词字体"), lyricsFontColumn);
    lyricsFontLabel->setObjectName(QStringLiteral("settingsFieldLabel"));
    m_lyricsFontSelector = new QFontComboBox(lyricsFontColumn);
    m_lyricsFontSizeSelector = new QSpinBox(lyricsFontColumn);
    m_lyricsFontSizeSelector->setRange(12, 36);
    m_lyricsFontSizeSelector->setValue(24);
    m_lyricsFontSizeSelector->setSuffix(QStringLiteral(" px"));
    auto *lyricsPreview = new QLabel(QStringLiteral("效果预览：让音乐陪伴此刻"), lyricsFontColumn);
    lyricsPreview->setObjectName(QStringLiteral("settingsPreview"));
    lyricsFontLayout->addWidget(lyricsFontLabel);
    lyricsFontLayout->addWidget(m_lyricsFontSelector);
    lyricsFontLayout->addWidget(m_lyricsFontSizeSelector);
    lyricsFontLayout->addWidget(lyricsPreview);
    fontColumns->addWidget(applicationFontColumn, 1);
    fontColumns->addWidget(lyricsFontColumn, 1);

    auto *backgroundSection = new QWidget(settingsPanel);
    auto *backgroundSectionLayout = new QVBoxLayout(backgroundSection);
    backgroundSectionLayout->setContentsMargins(0, 0, 0, 0);
    backgroundSectionLayout->setSpacing(10);
    auto *backgroundLabel = new QLabel(QStringLiteral("背景图片"), backgroundSection);
    backgroundLabel->setObjectName(QStringLiteral("settingsFieldLabel"));
    m_backgroundEditor = new QLineEdit(settingsPanel);
    m_backgroundEditor->setReadOnly(true);
    auto *browseBackgroundButton = new QPushButton(QStringLiteral("选择图片"), settingsPanel);
    auto *backgroundRow = new QWidget(backgroundSection);
    auto *backgroundRowLayout = new QHBoxLayout(backgroundRow);
    backgroundRowLayout->setContentsMargins(0, 0, 0, 0);
    backgroundRowLayout->setSpacing(10);
    backgroundRowLayout->addWidget(m_backgroundEditor, 1);
    backgroundRowLayout->addWidget(browseBackgroundButton);
    backgroundSectionLayout->addWidget(backgroundLabel);
    backgroundSectionLayout->addWidget(backgroundRow);

    auto *settingsActions = new QHBoxLayout;
    settingsActions->addStretch();
    auto *settingsSuccessMessage = new QLabel(QStringLiteral("应用成功"), settingsPanel);
    settingsSuccessMessage->setObjectName(QStringLiteral("settingsSuccessMessage"));
    settingsSuccessMessage->hide();
    auto *settingsSuccessEffect = new QGraphicsOpacityEffect(settingsSuccessMessage);
    settingsSuccessEffect->setOpacity(1.0);
    settingsSuccessMessage->setGraphicsEffect(settingsSuccessEffect);
    auto *settingsSuccessAnimation = new QSequentialAnimationGroup(settingsSuccessMessage);
    settingsSuccessAnimation->addPause(1000);
    auto *settingsSuccessFade = new QPropertyAnimation(settingsSuccessEffect, "opacity");
    settingsSuccessFade->setDuration(400);
    settingsSuccessFade->setStartValue(1.0);
    settingsSuccessFade->setEndValue(0.0);
    settingsSuccessFade->setEasingCurve(QEasingCurve::OutCubic);
    settingsSuccessAnimation->addAnimation(settingsSuccessFade);
    auto *applySettingsButton = new QPushButton(QStringLiteral("应用"), settingsPanel);
    applySettingsButton->setObjectName(QStringLiteral("primaryButton"));
    settingsActions->addWidget(settingsSuccessMessage, 0, Qt::AlignVCenter);
    settingsActions->addSpacing(10);
    settingsActions->addWidget(applySettingsButton);
    settingsContentLayout->addLayout(settingsHeader);
    settingsContentLayout->addLayout(fontColumns);
    settingsContentLayout->addWidget(backgroundSection);
    settingsContentLayout->addLayout(settingsActions);
    settingsPageLayout->addWidget(settingsPanel, 0, Qt::AlignHCenter | Qt::AlignTop);
    settingsPageLayout->addStretch(1);

    const auto updateSettingsPreview = [this, applicationPreview, lyricsPreview] {
        QFont applicationPreviewFont = applicationPreview->font();
        applicationPreviewFont.setFamily(m_applicationFontSelector->currentFont().family());
        applicationPreviewFont.setPixelSize(m_applicationFontSizeSelector->value());
        applicationPreview->setFont(applicationPreviewFont);
        // 预览字号独立于全局 QWidget 样式，避免应用界面字号时覆盖所选大小。
        applicationPreview->setStyleSheet(
            QStringLiteral("font-size: %1px;").arg(m_applicationFontSizeSelector->value()));

        QFont lyricsPreviewFont = lyricsPreview->font();
        lyricsPreviewFont.setFamily(m_lyricsFontSelector->currentFont().family());
        lyricsPreviewFont.setPixelSize(m_lyricsFontSizeSelector->value());
        lyricsPreview->setFont(lyricsPreviewFont);
        lyricsPreview->setStyleSheet(
            QStringLiteral("font-size: %1px;").arg(m_lyricsFontSizeSelector->value()));
    };
    updateSettingsPreview();
    connect(m_applicationFontSelector, &QFontComboBox::currentFontChanged, this,
            updateSettingsPreview);
    connect(m_lyricsFontSelector, &QFontComboBox::currentFontChanged, this, updateSettingsPreview);
    connect(m_applicationFontSizeSelector, &QSpinBox::valueChanged, this, updateSettingsPreview);
    connect(m_lyricsFontSizeSelector, &QSpinBox::valueChanged, this, updateSettingsPreview);
    connect(browseBackgroundButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择背景"), m_backgroundEditor->text(),
            QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
        if (!file.isEmpty()) {
            m_backgroundEditor->setText(file);
        }
    });
    connect(settingsSuccessAnimation, &QSequentialAnimationGroup::finished, settingsSuccessMessage,
            &QWidget::hide);
    const auto applySettings = [this, settingsSuccessMessage, settingsSuccessEffect,
                                settingsSuccessAnimation, updateSettingsPreview] {
        settingsSuccessAnimation->stop();
        settingsSuccessMessage->hide();
        const QString applicationFont = m_applicationFontSelector->currentFont().family();
        const QString lyricsFont = m_lyricsFontSelector->currentFont().family();
        const int applicationFontSize = m_applicationFontSizeSelector->value();
        const int lyricsFontSize = m_lyricsFontSizeSelector->value();
        const QString background = m_backgroundEditor->text();
        if (!saveSettings(
                {{QStringLiteral("applicationFont"), applicationFont},
                 {QStringLiteral("lyricsFont"), lyricsFont},
                 {QStringLiteral("applicationFontSize"), QString::number(applicationFontSize)},
                 {QStringLiteral("lyricsFontSize"), QString::number(lyricsFontSize)},
                 {QStringLiteral("background"), background}})) {
            return false;
        }
        const QFont currentApplicationFont = QApplication::font();
        const QFont currentLyricsFont = m_lyrics->font();
        const bool fontsChanged = currentApplicationFont.family() != applicationFont ||
                                  currentApplicationFont.pixelSize() != applicationFontSize ||
                                  currentLyricsFont.family() != lyricsFont ||
                                  currentLyricsFont.pixelSize() != lyricsFontSize;
        if (fontsChanged) {
            applyFonts(applicationFont, applicationFontSize, lyricsFont, lyricsFontSize);
        }
        if (background != m_backgroundPath) {
            applyBackground(background);
        }
        updateSettingsPreview();
        settingsSuccessAnimation->stop();
        settingsSuccessAnimation->setCurrentTime(0);
        settingsSuccessEffect->setOpacity(1.0);
        settingsSuccessMessage->show();
        settingsSuccessAnimation->start();
        return true;
    };
    connect(applySettingsButton, &QPushButton::clicked, this, applySettings);
    connect(restoreSettingsButton, &QPushButton::clicked, this,
            [this, updateSettingsPreview, applySettings] {
                m_applicationFontSelector->setCurrentFont(QFont(QStringLiteral("KaiTi")));
                m_lyricsFontSelector->setCurrentFont(QFont(QStringLiteral("KaiTi")));
                m_applicationFontSizeSelector->setValue(16);
                m_lyricsFontSizeSelector->setValue(24);
                m_backgroundEditor->setText(kDefaultBackgroundPath);
                updateSettingsPreview();
                // 与应用按钮共用保存逻辑，确保所有默认设置立即生效并持久化。
                if (!applySettings()) {
                    return;
                }
            });
    return settingsPage;
}
