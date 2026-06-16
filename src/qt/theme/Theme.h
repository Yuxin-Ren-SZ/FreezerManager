// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_QT_THEME_THEME_H
#define FMGR_QT_THEME_THEME_H

#include <QColor>
#include <QFont>
#include <QString>
#include <array>
#include <string_view>

namespace fmgr::qt::theme {

/// Design-token system mirroring the CSS custom properties from the
/// FreezerManager Desktop design prototype.
///
/// Two modes are supported: Light (clinical lab feel, default) and Dark.
/// All values are read-only; switching modes regenerates stylesheets.

enum class Mode { Light, Dark };
enum class Density { Comfortable, Compact };

// ── Accent presets from the design toolbar ──────────────────────────
inline constexpr std::array<std::string_view, 4> kAccentPresets{
    "#2563c9",  // blue (default)
    "#0f8a8a",  // teal
    "#5b54d6",  // purple
    "#1f7a44",  // green
};

// ── Theme accessor ──────────────────────────────────────────────────
class Theme {
public:
    static Theme& instance();

    void setMode(Mode m);
    void setDensity(Density d);
    void setAccent(const QString& hex);

    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] Density density() const { return density_; }
    [[nodiscard]] const QString& accentHex() const { return accentHex_; }

    // ── Color tokens ────────────────────────────────────────────────
    [[nodiscard]] QColor bg() const;
    [[nodiscard]] QColor surface() const;
    [[nodiscard]] QColor surface2() const;
    [[nodiscard]] QColor surface3() const;
    [[nodiscard]] QColor border() const;
    [[nodiscard]] QColor borderStrong() const;
    [[nodiscard]] QColor text() const;
    [[nodiscard]] QColor text2() const;
    [[nodiscard]] QColor text3() const;
    [[nodiscard]] QColor accent() const;
    [[nodiscard]] QColor accentPress() const;
    [[nodiscard]] QColor accentSoft() const;
    [[nodiscard]] QColor accentSofter() const;
    [[nodiscard]] QColor accentRing() const;
    [[nodiscard]] QColor onAccent() const;

    // Status colors
    [[nodiscard]] QColor stActive() const;
    [[nodiscard]] QColor stActiveBg() const;
    [[nodiscard]] QColor stChecked() const;
    [[nodiscard]] QColor stCheckedBg() const;
    [[nodiscard]] QColor stDepleted() const;
    [[nodiscard]] QColor stDepletedBg() const;
    [[nodiscard]] QColor stDestroyed() const;
    [[nodiscard]] QColor stDestroyedBg() const;

    // ── Metric tokens ───────────────────────────────────────────────
    [[nodiscard]] int rowPadY() const;
    [[nodiscard]] int rowPadX() const;
    [[nodiscard]] int controlH() const;
    [[nodiscard]] int radius() const;
    [[nodiscard]] int radiusSm() const;
    [[nodiscard]] int radiusLg() const;
    [[nodiscard]] int sidebarW() const;
    [[nodiscard]] int fontSize() const;

    // ── Font families ───────────────────────────────────────────────
    [[nodiscard]] QString fontSans() const;
    [[nodiscard]] QString fontMono() const;

    // ── Full Qt stylesheet ──────────────────────────────────────────
    [[nodiscard]] QString globalStyleSheet() const;
    [[nodiscard]] QString sidebarStyleSheet() const;
    [[nodiscard]] QString cardStyleSheet() const;

    // ── Color utility ───────────────────────────────────────────────
    static QColor colorMix(const QColor& a, const QColor& b, double ratio);
    static QColor hexToColor(std::string_view hex);

private:
    Theme();
    Mode mode_ = Mode::Light;
    Density density_ = Density::Comfortable;
    QString accentHex_{"#2563c9"};
};

// Convenience free functions
inline const Theme& t() { return Theme::instance(); }

}  // namespace fmgr::qt::theme

#endif
