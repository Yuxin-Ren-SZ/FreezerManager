// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Theme.h"

#include <QApplication>
#include <cmath>

namespace fmgr::qt::theme {

// ── Helpers ─────────────────────────────────────────────────────────
QColor Theme::hexToColor(std::string_view hex) {
    if (hex.starts_with('#')) hex = hex.substr(1);
    if (hex.size() != 6) return {};
    size_t pos = 0;
    auto r = uint8_t(std::stoul(std::string(hex.substr(0, 2)), &pos, 16));
    auto g = uint8_t(std::stoul(std::string(hex.substr(2, 2)), &pos, 16));
    auto b = uint8_t(std::stoul(std::string(hex.substr(4, 2)), &pos, 16));
    return QColor(r, g, b);
}

QColor Theme::colorMix(const QColor& a, const QColor& b, double ratio) {
    // "color-mix(in srgb, a ratio%, b)" — ratio is fraction of a
    double r = a.redF() * ratio + b.redF() * (1.0 - ratio);
    double g = a.greenF() * ratio + b.greenF() * (1.0 - ratio);
    double bl = a.blueF() * ratio + b.blueF() * (1.0 - ratio);
    return QColor::fromRgbF(std::clamp(r, 0.0, 1.0),
                            std::clamp(g, 0.0, 1.0),
                            std::clamp(bl, 0.0, 1.0));
}

Theme& Theme::instance() {
    static Theme th;
    return th;
}

Theme::Theme() = default;

void Theme::setMode(Mode m) {
    mode_ = m;
    // re-apply global stylesheet when mode changes
    if (qApp) qApp->setStyleSheet(globalStyleSheet());
}
void Theme::setDensity(Density d) { density_ = d; }
void Theme::setAccent(const QString& hex) {
    accentHex_ = hex;
    if (qApp) qApp->setStyleSheet(globalStyleSheet());
}

// ── Color tokens ────────────────────────────────────────────────────
QColor Theme::bg() const {
    return mode_ == Mode::Light ? QColor(0xEE, 0xF1, 0xF4)
                                : QColor(0x0E, 0x12, 0x16);
}
QColor Theme::surface() const {
    return mode_ == Mode::Light ? QColor(0xFF, 0xFF, 0xFF)
                                : QColor(0x16, 0x1B, 0x22);
}
QColor Theme::surface2() const {
    return mode_ == Mode::Light ? QColor(0xF6, 0xF8, 0xFA)
                                : QColor(0x1C, 0x23, 0x2C);
}
QColor Theme::surface3() const {
    return mode_ == Mode::Light ? QColor(0xEE, 0xF1, 0xF4)
                                : QColor(0x23, 0x2C, 0x37);
}
QColor Theme::border() const {
    return mode_ == Mode::Light ? QColor(0xE0, 0xE5, 0xEA)
                                : QColor(0x2A, 0x33, 0x3D);
}
QColor Theme::borderStrong() const {
    return mode_ == Mode::Light ? QColor(0xC8, 0xD0, 0xD8)
                                : QColor(0x3A, 0x46, 0x54);
}
QColor Theme::text() const {
    return mode_ == Mode::Light ? QColor(0x1B, 0x25, 0x30)
                                : QColor(0xE6, 0xED, 0xF3);
}
QColor Theme::text2() const {
    return mode_ == Mode::Light ? QColor(0x56, 0x63, 0x6F)
                                : QColor(0x9A, 0xA7, 0xB4);
}
QColor Theme::text3() const {
    return mode_ == Mode::Light ? QColor(0x84, 0x92, 0xA0)
                                : QColor(0x6B, 0x78, 0x86);
}
QColor Theme::accent() const { return hexToColor(accentHex_.toStdString()); }
QColor Theme::accentPress() const {
    return colorMix(accent(), QColor(0, 0, 0), 0.80);
}
QColor Theme::accentSoft() const {
    double ratio = mode_ == Mode::Light ? 0.12 : 0.24;
    return colorMix(accent(), bg(), ratio);
}
QColor Theme::accentSofter() const {
    double ratio = mode_ == Mode::Light ? 0.07 : 0.14;
    return colorMix(accent(), bg(), ratio);
}
QColor Theme::accentRing() const {
    return colorMix(accent(), QColor(0, 0, 0, 0), 0.35);
}
QColor Theme::onAccent() const { return QColor(0xFF, 0xFF, 0xFF); }

// Status colors
QColor Theme::stActive() const { return QColor(0x1F, 0x9D, 0x57); }
QColor Theme::stActiveBg() const {
    double ratio = mode_ == Mode::Light ? 0.14 : 0.26;
    return colorMix(stActive(), surface(), ratio);
}
QColor Theme::stChecked() const { return QColor(0xC5, 0x84, 0x1A); }
QColor Theme::stCheckedBg() const {
    double ratio = mode_ == Mode::Light ? 0.16 : 0.28;
    return colorMix(stChecked(), surface(), ratio);
}
QColor Theme::stDepleted() const { return QColor(0x84, 0x92, 0xA0); }
QColor Theme::stDepletedBg() const {
    double ratio = mode_ == Mode::Light ? 0.16 : 0.24;
    return colorMix(stDepleted(), surface(), ratio);
}
QColor Theme::stDestroyed() const { return QColor(0xC6, 0x3A, 0x3A); }
QColor Theme::stDestroyedBg() const {
    double ratio = mode_ == Mode::Light ? 0.13 : 0.26;
    return colorMix(stDestroyed(), surface(), ratio);
}

// ── Metric tokens ───────────────────────────────────────────────────
int Theme::rowPadY() const { return density_ == Density::Compact ? 5 : 9; }
int Theme::rowPadX() const { return density_ == Density::Compact ? 10 : 14; }
int Theme::controlH() const { return density_ == Density::Compact ? 28 : 34; }
int Theme::radius() const { return 7; }
int Theme::radiusSm() const { return 5; }
int Theme::radiusLg() const { return 11; }
int Theme::sidebarW() const { return 232; }
int Theme::fontSize() const { return density_ == Density::Compact ? 12 : 14; }

// ── Fonts ───────────────────────────────────────────────────────────
QString Theme::fontSans() const {
    return QStringLiteral("\"IBM Plex Sans\", \"Segoe UI\", \"Ubuntu\", "
                          "system-ui, -apple-system, sans-serif");
}
QString Theme::fontMono() const {
    return QStringLiteral("\"IBM Plex Mono\", ui-monospace, \"SF Mono\", "
                          "Menlo, \"Cascadia Code\", monospace");
}

// ── Full stylesheet ─────────────────────────────────────────────────
QString Theme::globalStyleSheet() const {
    auto bg = this->bg();
    auto s = surface();
    auto s2 = surface2();
    auto bd = border();
    auto bd2 = borderStrong();
    auto tx = text();
    auto tx2 = text2();
    auto tx3 = text3();
    auto ac = accent();
    auto acRing = accentRing();
    auto onAc = onAccent();
    auto stD = stDestroyed();
    auto stDBg = stDestroyedBg();

    auto col = [](const QColor& c) {
        return QString("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
    };

    auto colA = [](const QColor& c, int alpha) {
        return QString("rgba(%1,%2,%3,%4)")
            .arg(c.red()).arg(c.green()).arg(c.blue())
            .arg(alpha / 255.0, 0, 'f', 2);
    };

    return QStringLiteral(R"(
/* FreezerManager Desktop — generated from Theme tokens */
QMainWindow,QWidget {
    background: %1;
    color: %2;
    font-family: "IBM Plex Sans","Segoe UI","Ubuntu",system-ui,sans-serif;
    font-size: %3px;
}
QMenuBar {
    background: %4;
    color: %5;
    border-bottom: 1px solid %6;
    padding: 2px 8px;
}
QMenuBar::item:selected {
    background: %7;
    border-radius: 5px;
}
QMenu {
    background: %4;
    border: 1px solid %6;
    border-radius: 7px;
    padding: 4px;
}
QMenu::item {
    padding: 6px 32px 6px 16px;
    border-radius: 5px;
}
QMenu::item:selected {
    background: %8;
    color: %9;
}
QMenu::separator {
    height: 1px;
    background: %6;
    margin: 4px 8px;
}
QStatusBar {
    background: %4;
    color: %10;
    border-top: 1px solid %6;
    padding: 2px 12px;
    font-size: 12px;
}
QSplitter::handle {
    background: %6;
    width: 1px;
}
QTableView {
    background: %4;
    alternate-background-color: %7;
    color: %5;
    gridline-color: %11;
    border: 1px solid %6;
    border-radius: 11px;
    selection-background-color: %8;
    selection-color: %9;
}
QTableView::item {
    padding: %12px %13px;
    border-bottom: 1px solid %11;
}
QTableView::item:hover {
    background: %14;
}
QHeaderView::section {
    background: %7;
    color: %10;
    border: none;
    border-bottom: 1px solid %6;
    padding: 8px %13px;
    font-weight: 600;
    font-size: 12px;
}
QScrollBar:vertical {
    background: %1;
    width: 10px;
    border-radius: 5px;
}
QScrollBar::handle:vertical {
    background: %15;
    border-radius: 5px;
    min-height: 30px;
    border: 3px solid transparent;
    background-clip: padding-box;
}
QScrollBar::handle:vertical:hover {
    background: %10;
    background-clip: padding-box;
    border: 3px solid transparent;
}
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar:horizontal {
    background: %1;
    height: 10px;
    border-radius: 5px;
}
QScrollBar::handle:horizontal {
    background: %15;
    border-radius: 5px;
    min-width: 30px;
    border: 3px solid transparent;
    background-clip: padding-box;
}
QLineEdit {
    background: %7;
    color: %5;
    border: 1px solid %15;
    border-radius: 7px;
    padding: %16px %13px;
    selection-background-color: %8;
}
QLineEdit:focus {
    border-color: %8;
    background: %4;
}
QComboBox {
    background: %7;
    color: %5;
    border: 1px solid %15;
    border-radius: 7px;
    padding: 6px 12px;
    min-width: 120px;
}
QComboBox:hover {
    border-color: %8;
}
QComboBox QAbstractItemView {
    background: %4;
    border: 1px solid %6;
    border-radius: 7px;
    selection-background-color: %8;
}
QTextEdit {
    background: %4;
    color: %5;
    border: 1px solid %6;
    border-radius: 11px;
    padding: 12px;
}
QGraphicsView {
    background: %1;
    border: 1px solid %6;
    border-radius: 11px;
}
QScrollArea {
    background: transparent;
    border: none;
}
QPushButton {
    background: %4;
    color: %5;
    border: 1px solid %15;
    border-radius: 7px;
    padding: 7px 14px;
    font-weight: 550;
}
QPushButton:hover {
    background: %7;
    border-color: %10;
}
QPushButton:pressed {
    background: %11;
}
QPushButton:disabled {
    opacity: 0.45;
}
QToolTip {
    background: #11181f;
    color: #fff;
    border-radius: 6px;
    padding: 6px 9px;
    font-size: 12px;
}
QLabel[role="heading"] {
    font-size: 21px;
    font-weight: 600;
    letter-spacing: -0.015em;
}
)")
        .arg(col(bg), col(tx))
        .arg(fontSize())
        .arg(col(s), col(tx), col(bd))
        .arg(col(s2))
        .arg(col(ac), col(onAc))
        .arg(col(tx3))
        .arg(colA(bd, 80))
        .arg(rowPadY())
        .arg(rowPadX())
        .arg(col(accentSofter()))
        .arg(col(bd2))
        .arg(controlH());
}

QString Theme::sidebarStyleSheet() const {
    auto ac = accent();
    auto acSoft = accentSoft();
    auto s2 = surface2();
    auto tx = text();
    auto tx2 = text2();
    auto bd = border();
    auto bd2 = borderStrong();
    auto col = [](const QColor& c) {
        return QString("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
    };
    return QStringLiteral(R"(
QWidget#sidebar {
    background: %1;
    border-right: 1px solid %2;
}
QWidget#sidebar QPushButton {
    color: %3;
    background: transparent;
    border: none;
    border-radius: 7px;
    padding: 8px 10px;
    margin: 1px 10px;
    font-size: 13.5px;
    font-weight: 500;
    text-align: left;
    border-left: 3px solid transparent;
}
QWidget#sidebar QPushButton:hover {
    background: %4;
    color: %5;
}
QWidget#sidebar QPushButton:checked {
    background: %6;
    color: %7;
    border-left: 3px solid %7;
    font-weight: 600;
}
QWidget#sidebar QLabel[role="nav-group"] {
    font-size: 10.5px;
    text-transform: uppercase;
    letter-spacing: 0.07em;
    color: %8;
    font-weight: 600;
    padding: 14px 10px 5px;
    background: transparent;
    margin: 0px 0px;
}
)")
        .arg(col(surface()), col(bd), col(tx2), col(s2), col(tx),
             col(acSoft), col(ac), col(text3()));
}

QString Theme::cardStyleSheet() const {
    auto s = surface();
    auto bd = border();
    auto col = [](const QColor& c) {
        return QString("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
    };
    return QStringLiteral(
        "QFrame#card{background:%1;border:1px solid %2;border-radius:11px;}")
        .arg(col(s), col(bd));
}

}  // namespace fmgr::qt::theme
