// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QApplication>
#include <QCache>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleHints>
#include <QTextLayout>
#include <QtMath>

#include "yuzu/game/library/library_theme.h"
#include "yuzu/game/library/library_types.h"

namespace Library {
namespace {

Theme g_theme;
bool g_theme_initialized = false;
QColor g_accent_override; ///< Invalid: use the theme's own accent

QColor Rgba(int r, int g, int b, qreal a) {
    QColor color(r, g, b);
    color.setAlphaF(static_cast<float>(a));
    return color;
}

qreal Luminance(const QColor& color) {
    const auto channel = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}

/// Makes sure the accent color is vivid and readable on the given background brightness.
QColor NormalizeAccent(QColor accent, bool dark) {
    const QColor fallback = dark ? QColor(0x60, 0xCD, 0xFF) : QColor(0x00, 0x5F, 0xB8);
    if (!accent.isValid()) {
        return fallback;
    }

    float h, s, l, a;
    accent.getHslF(&h, &s, &l, &a);
    if (s < 0.30f || h < 0) {
        return fallback;
    }
    if (dark) {
        l = std::clamp(l, 0.56f, 0.70f);
    } else {
        l = std::clamp(l, 0.32f, 0.46f);
    }
    return QColor::fromHslF(h, std::max(s, 0.55f), l);
}

/// Separable box blur operating on all channels of a premultiplied ARGB32 image.
void BoxBlur(QImage& image, int radius) {
    if (radius < 1 || image.isNull()) {
        return;
    }
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = image.width();
    const int h = image.height();
    const int window = radius * 2 + 1;
    std::vector<QRgb> line(static_cast<size_t>(std::max(w, h)));

    const auto blur_line = [&](QRgb* data, int length, int stride) {
        for (int i = 0; i < length; ++i) {
            line[i] = data[i * stride];
        }
        int sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
        const auto at = [&](int i) { return line[std::clamp(i, 0, length - 1)]; };
        for (int i = -radius; i <= radius; ++i) {
            const QRgb p = at(i);
            sum_a += qAlpha(p);
            sum_r += qRed(p);
            sum_g += qGreen(p);
            sum_b += qBlue(p);
        }
        for (int i = 0; i < length; ++i) {
            data[i * stride] =
                qRgba(sum_r / window, sum_g / window, sum_b / window, sum_a / window);
            const QRgb out = at(i - radius);
            const QRgb in = at(i + radius + 1);
            sum_a += qAlpha(in) - qAlpha(out);
            sum_r += qRed(in) - qRed(out);
            sum_g += qGreen(in) - qGreen(out);
            sum_b += qBlue(in) - qBlue(out);
        }
    };

    for (int y = 0; y < h; ++y) {
        blur_line(reinterpret_cast<QRgb*>(image.scanLine(y)), w, 1);
    }
    auto* bits = reinterpret_cast<QRgb*>(image.bits());
    const int stride = static_cast<int>(image.bytesPerLine() / sizeof(QRgb));
    for (int x = 0; x < w; ++x) {
        blur_line(bits + x, h, stride);
    }
}

QString FontFamily(bool display) {
    static const QString text_family = [] {
        const QStringList families = QFontDatabase::families();
        for (const auto& candidate :
             {QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
              QStringLiteral("Inter"), QStringLiteral("SF Pro Text"), QStringLiteral("Cantarell"),
              QStringLiteral("Noto Sans")}) {
            if (families.contains(candidate)) {
                return candidate;
            }
        }
        return QApplication::font().family();
    }();
    static const QString display_family = [] {
        const QStringList families = QFontDatabase::families();
        for (const auto& candidate :
             {QStringLiteral("Segoe UI Variable Display"), QStringLiteral("SF Pro Display")}) {
            if (families.contains(candidate)) {
                return candidate;
            }
        }
        return text_family;
    }();
    return display ? display_family : text_family;
}

QCache<QString, QImage>& SourceCache() {
    static QCache<QString, QImage> cache(48 * 1024); // KiB
    return cache;
}

QCache<QString, QPixmap>& IconCache() {
    static QCache<QString, QPixmap> cache(96 * 1024); // KiB
    return cache;
}

QCache<QString, QPixmap>& BackdropCache() {
    static QCache<QString, QPixmap> cache(24 * 1024); // KiB
    return cache;
}

QHash<QString, QPixmap>& ShadowCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

int CostKiB(const QImage& image) {
    return std::max(1, static_cast<int>(image.sizeInBytes() / 1024));
}

int CostKiB(const QPixmap& pixmap) {
    return std::max(1, pixmap.width() * pixmap.height() * 4 / 1024);
}

QColor HueFromText(const QString& text, qreal offset, qreal saturation, qreal lightness) {
    const auto hash = qHash(text);
    const qreal hue = std::fmod((hash % 360) / 360.0 + offset + 1.0, 1.0);
    return QColor::fromHslF(static_cast<float>(hue), static_cast<float>(saturation),
                            static_cast<float>(lightness));
}

Theme MakeTheme(const QString& theme_name, const QColor& accent_override) {
    // The application palette is replaced by the Eden UI, so remember the system's own colors
    // from the first call.
    static const QPalette system_palette = QApplication::palette();
    bool native_dark = system_palette.color(QPalette::Window).lightness() < 128;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) {
        native_dark = scheme == Qt::ColorScheme::Dark;
    }
#endif

    QColor system_accent = system_palette.color(QPalette::Highlight);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    system_accent = system_palette.color(QPalette::Accent);
#endif

    Theme t;
    const bool amoled = theme_name == QStringLiteral("amoled");
    if (amoled) {
        // Pure black surfaces for OLED screens; cards and controls stay just visible.
        t.dark = true;
        t.canvas = QColor(0x00, 0x00, 0x00);
        t.panel = QColor(0x00, 0x00, 0x00);
        t.card = QColor(0x10, 0x10, 0x12);
        t.card_hover = QColor(0x1A, 0x1A, 0x1D);
        t.control = QColor(0x14, 0x14, 0x17);
        t.text = QColor(0xF2, 0xF2, 0xF3);
        t.text_dim = QColor(0xA7, 0xA8, 0xAD);
        t.text_faint = QColor(0x6E, 0x70, 0x76);
        t.accent = QColor(0x3D, 0xAE, 0xE9);
    } else if (theme_name.contains(QStringLiteral("midnight"))) {
        t.dark = true;
        t.canvas = QColor(0x12, 0x1A, 0x22);
        t.panel = QColor(0x17, 0x21, 0x2B);
        t.card = QColor(0x1F, 0x2B, 0x37);
        t.card_hover = QColor(0x27, 0x36, 0x45);
        t.control = QColor(0x22, 0x30, 0x3D);
        t.text = QColor(0xE6, 0xE8, 0xEB);
        t.text_dim = QColor(0xA5, 0xAD, 0xB7);
        t.text_faint = QColor(0x70, 0x7C, 0x89);
        t.accent = QColor(0x4F, 0xA3, 0xF7);
    } else if (theme_name == QStringLiteral("qdarkstyle") ||
               theme_name == QStringLiteral("colorful_dark")) {
        t.dark = true;
        t.canvas = QColor(0x26, 0x2A, 0x2E);
        t.panel = QColor(0x2C, 0x30, 0x35);
        t.card = QColor(0x34, 0x3A, 0x40);
        t.card_hover = QColor(0x3D, 0x44, 0x4B);
        t.control = QColor(0x38, 0x3E, 0x45);
        t.text = QColor(0xEF, 0xF0, 0xF1);
        t.text_dim = QColor(0xB1, 0xB5, 0xBA);
        t.text_faint = QColor(0x80, 0x85, 0x8B);
        t.accent = QColor(0x3D, 0xAE, 0xE9);
    } else if (native_dark || theme_name == QStringLiteral("default_dark")) {
        t.dark = true;
        t.canvas = QColor(0x19, 0x1A, 0x1D);
        t.panel = QColor(0x20, 0x21, 0x25);
        t.card = QColor(0x27, 0x29, 0x2E);
        t.card_hover = QColor(0x30, 0x32, 0x38);
        t.control = QColor(0x2B, 0x2D, 0x33);
        t.text = QColor(0xF3, 0xF4, 0xF6);
        t.text_dim = QColor(0xB4, 0xB8, 0xC0);
        t.text_faint = QColor(0x7D, 0x82, 0x8C);
        t.accent = NormalizeAccent(system_accent, true);
    } else {
        t.dark = false;
        t.canvas = QColor(0xF1, 0xF2, 0xF4);
        t.panel = QColor(0xFA, 0xFA, 0xFB);
        t.card = QColor(0xFF, 0xFF, 0xFF);
        t.card_hover = QColor(0xF7, 0xF8, 0xFA);
        t.control = QColor(0xFF, 0xFF, 0xFF);
        t.text = QColor(0x1B, 0x1C, 0x1F);
        t.text_dim = QColor(0x55, 0x5A, 0x63);
        t.text_faint = QColor(0x8A, 0x8F, 0x98);
        t.accent = NormalizeAccent(system_accent, false);
    }

    if (accent_override.isValid()) {
        t.accent = accent_override;
    }

    t.elevated = t.dark ? Mix(t.panel, t.text, amoled ? 0.07 : 0.055) : QColor(0xFF, 0xFF, 0xFF);
    t.danger = t.dark ? QColor(0xFF, 0x6B, 0x6B) : QColor(0xC4, 0x2B, 0x1C);

    if (t.dark) {
        // Black surfaces only separate through their outlines, so draw them a little stronger.
        t.stroke = Rgba(255, 255, 255, amoled ? 0.10 : 0.07);
        t.separator = Rgba(255, 255, 255, amoled ? 0.085 : 0.06);
        t.shadow = Rgba(0, 0, 0, 0.60);
        t.star = QColor(0xFF, 0xC8, 0x3D);
        t.accent_hover = t.accent.lighter(110);
        t.accent_pressed = t.accent.darker(115);
    } else {
        t.stroke = Rgba(0, 0, 0, 0.09);
        t.separator = Rgba(0, 0, 0, 0.07);
        t.shadow = Rgba(0, 0, 0, 0.22);
        t.star = QColor(0xE0, 0x9B, 0x00);
        t.accent_hover = t.accent.lighter(112);
        t.accent_pressed = t.accent.darker(112);
    }
    t.on_accent = Luminance(t.accent) > 0.45 ? QColor(0x0B, 0x0C, 0x0E) : QColor(Qt::white);
    return t;
}

} // Anonymous namespace

void SetThemeName(const QString& theme_name) {
    g_theme = MakeTheme(theme_name, g_accent_override);
    g_theme_initialized = true;
    ShadowCache().clear();
}

void SetAccentColor(const QColor& color) {
    g_accent_override = color.isValid() ? QColor(color.rgb()) : QColor{};
}

QColor AccentColor() {
    return g_accent_override;
}

QColor ThemeAccent(const QString& theme_name) {
    return MakeTheme(theme_name, QColor{}).accent;
}

const Theme& CurrentTheme() {
    if (!g_theme_initialized) {
        SetThemeName(QString{});
    }
    return g_theme;
}

QColor Mix(const QColor& a, const QColor& b, qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t),
                            static_cast<float>(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor WithAlpha(QColor color, qreal alpha) {
    color.setAlphaF(static_cast<float>(std::clamp(color.alphaF() * alpha, 0.0, 1.0)));
    return color;
}

QFont UiFont(qreal pixel_size, QFont::Weight weight) {
    QFont font(FontFamily(pixel_size >= 20.0));
    font.setPixelSize(std::max(1, qRound(pixel_size)));
    font.setWeight(weight);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

qreal EaseOutCubic(qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    const qreal inv = 1.0 - t;
    return 1.0 - inv * inv * inv;
}

qreal EaseInOutCubic(qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

qreal Approach(qreal current, qreal target, qreal dt_seconds, qreal speed) {
    const qreal value = target + (current - target) * std::exp(-speed * dt_seconds);
    return std::abs(value - target) < 0.002 ? target : value;
}

QPixmap ShadowPixmap(const QSize& size, qreal radius, int blur, const QColor& color) {
    const QString key = QStringLiteral("%1x%2:%3:%4:%5")
                            .arg(size.width())
                            .arg(size.height())
                            .arg(radius)
                            .arg(blur)
                            .arg(color.rgba(), 0, 16);
    auto& cache = ShadowCache();
    if (const auto it = cache.constFind(key); it != cache.constEnd()) {
        return *it;
    }
    if (cache.size() > 160) {
        cache.clear();
    }

    // Render at a reduced resolution; the blur hides the difference and it is much cheaper.
    constexpr qreal scale = 0.5;
    const int pad = std::max(1, qRound(blur * scale));
    const QSize inner(std::max(1, qRound(size.width() * scale)),
                      std::max(1, qRound(size.height() * scale)));
    QImage image(inner + QSize(pad * 2, pad * 2), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(pad, pad, inner.width(), inner.height()), radius * scale,
                          radius * scale);
    }
    const int pass_radius = std::max(1, pad / 2);
    BoxBlur(image, pass_radius);
    BoxBlur(image, pass_radius);
    BoxBlur(image, pass_radius);

    const QSize full = size + QSize(blur * 2, blur * 2);
    QPixmap pixmap =
        QPixmap::fromImage(image.scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    cache.insert(key, pixmap);
    return pixmap;
}

void DrawShadow(QPainter& painter, const QRectF& rect, qreal radius, int blur, const QColor& color,
                qreal opacity, const QPointF& offset) {
    if (opacity <= 0.01 || rect.isEmpty()) {
        return;
    }
    const QSize size = rect.size().toSize();
    const QPixmap shadow = ShadowPixmap(size, radius, blur, color);
    const qreal old_opacity = painter.opacity();
    painter.setOpacity(old_opacity * std::min(1.0, opacity));
    painter.drawPixmap(QRectF(rect.topLeft() + offset - QPointF(blur, blur),
                              QSizeF(size.width() + blur * 2, size.height() + blur * 2)),
                       shadow, QRectF(shadow.rect()));
    painter.setOpacity(old_opacity);
}

QPainterPath RoundedPath(const QRectF& rect, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

QPainterPath TopRoundedPath(const QRectF& rect, qreal radius) {
    QPainterPath path;
    const qreal r = std::min(radius, std::min(rect.width(), rect.height()) / 2.0);
    path.moveTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), rect.top() + r);
    path.arcTo(QRectF(rect.left(), rect.top(), r * 2, r * 2), 180, -90);
    path.lineTo(rect.right() - r, rect.top());
    path.arcTo(QRectF(rect.right() - r * 2, rect.top(), r * 2, r * 2), 90, -90);
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();
    return path;
}

QImage EntrySourceImage(const Entry& entry) {
    const QString key = entry.Key();
    auto& cache = SourceCache();
    if (QImage* cached = cache.object(key)) {
        return *cached;
    }

    QImage image;
    if (!entry.icon_data.isEmpty()) {
        image = QImage::fromData(entry.icon_data);
    }
    if (image.isNull() && !entry.icon.isNull()) {
        image = entry.icon.toImage();
    }
    if (!image.isNull()) {
        // Fully transparent placeholder icons count as "no icon".
        const QImage probe = image.scaled(8, 8).convertToFormat(QImage::Format_ARGB32);
        bool any_visible = false;
        for (int y = 0; y < probe.height() && !any_visible; ++y) {
            for (int x = 0; x < probe.width(); ++x) {
                if (qAlpha(probe.pixel(x, y)) > 8) {
                    any_visible = true;
                    break;
                }
            }
        }
        if (!any_visible) {
            image = QImage();
        }
    }
    if (!image.isNull()) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        cache.insert(key, new QImage(image), CostKiB(image));
    }
    return image;
}

QPixmap EntryIcon(const Entry& entry, int pixel_size, qreal device_pixel_ratio) {
    pixel_size = std::max(8, pixel_size);
    const int device_size = qRound(pixel_size * device_pixel_ratio);
    const QString key = QStringLiteral("%1|%2").arg(entry.Key()).arg(device_size);
    auto& cache = IconCache();
    if (QPixmap* cached = cache.object(key)) {
        return *cached;
    }

    QPixmap pixmap;
    const QImage source = EntrySourceImage(entry);
    if (!source.isNull()) {
        const QImage scaled = source.scaled(
            device_size, device_size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QRect crop((scaled.width() - device_size) / 2, (scaled.height() - device_size) / 2,
                         device_size, device_size);
        pixmap = QPixmap::fromImage(scaled.copy(crop));
    } else {
        pixmap = QPixmap(device_size, device_size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        PaintPlaceholderArt(painter, QRectF(0, 0, device_size, device_size), entry.title);
    }
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    cache.insert(key, new QPixmap(pixmap), CostKiB(pixmap));
    return pixmap;
}

QPixmap EntryBackdrop(const Entry& entry, const QSize& size, qreal device_pixel_ratio) {
    const QSize device_size = (QSizeF(size) * device_pixel_ratio).toSize();
    if (device_size.isEmpty()) {
        return {};
    }
    const QString key = QStringLiteral("%1|%2x%3")
                            .arg(entry.Key())
                            .arg(device_size.width())
                            .arg(device_size.height());
    auto& cache = BackdropCache();
    if (QPixmap* cached = cache.object(key)) {
        return *cached;
    }

    QImage small(40, 40, QImage::Format_ARGB32_Premultiplied);
    const QImage source = EntrySourceImage(entry);
    if (!source.isNull()) {
        small = source.scaled(40, 40, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else {
        QLinearGradient gradient(0, 0, 40, 40);
        gradient.setColorAt(0, HueFromText(entry.title, 0.0, 0.55, 0.45));
        gradient.setColorAt(1, HueFromText(entry.title, 0.12, 0.60, 0.35));
        QPainter painter(&small);
        painter.fillRect(small.rect(), gradient);
    }
    BoxBlur(small, 4);
    BoxBlur(small, 4);

    // Scale the blurred thumbnail to the target aspect ratio, keeping the centre of the art.
    const qreal aspect = static_cast<qreal>(device_size.width()) / device_size.height();
    QRect crop = small.rect();
    if (aspect > 1.0) {
        const int h = std::max(4, qRound(small.width() / aspect));
        crop = QRect(0, (small.height() - h) / 2, small.width(), h);
    }
    QPixmap pixmap = QPixmap::fromImage(
        small.copy(crop).scaled(device_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    cache.insert(key, new QPixmap(pixmap), CostKiB(pixmap));
    return pixmap;
}

void ClearImageCaches() {
    SourceCache().clear();
    IconCache().clear();
    BackdropCache().clear();
}

void PaintPlaceholderArt(QPainter& painter, const QRectF& rect, const QString& title) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
    gradient.setColorAt(0.0, HueFromText(title, 0.0, 0.50, 0.46));
    gradient.setColorAt(1.0, HueFromText(title, 0.10, 0.62, 0.30));
    painter.fillRect(rect, gradient);

    // Soft diagonal sheen
    QLinearGradient sheen(rect.topLeft(), QPointF(rect.left(), rect.bottom()));
    sheen.setColorAt(0.0, QColor(255, 255, 255, 40));
    sheen.setColorAt(0.6, QColor(255, 255, 255, 0));
    painter.fillRect(rect, sheen);

    QString initials;
    const QStringList words = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        if (initials.size() >= 2) {
            break;
        }
        const QChar c = word.at(0);
        if (c.isLetterOrNumber()) {
            initials.append(c.toUpper());
        }
    }
    if (!initials.isEmpty()) {
        QFont font = UiFont(rect.height() * 0.34, QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 230));
        painter.drawText(rect, Qt::AlignCenter, initials);
    }
    painter.restore();
}

void PaintIcon(QPainter& painter, Icon icon, const QRectF& rect, const QColor& color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = std::min(rect.width(), rect.height());
    const QRectF r(rect.center().x() - s / 2, rect.center().y() - s / 2, s, s);
    const qreal stroke = std::max(1.2, s / 11.0);
    QPen pen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const auto at = [&r](qreal x, qreal y) {
        return QPointF(r.left() + r.width() * x, r.top() + r.height() * y);
    };

    switch (icon) {
    case Icon::Play: {
        QPainterPath path;
        path.moveTo(at(0.30, 0.18));
        path.lineTo(at(0.82, 0.50));
        path.lineTo(at(0.30, 0.82));
        path.closeSubpath();
        painter.setBrush(color);
        painter.setPen(QPen(color, stroke * 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        break;
    }
    case Icon::Star:
    case Icon::StarOutline: {
        QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const qreal radius = (i % 2 == 0) ? 0.47 : 0.20;
            const qreal angle = -M_PI / 2 + i * M_PI / 5;
            star << at(0.5 + radius * std::cos(angle), 0.53 + radius * std::sin(angle));
        }
        if (icon == Icon::Star) {
            painter.setBrush(color);
        }
        painter.drawPolygon(star);
        break;
    }
    case Icon::Gear: {
        painter.setBrush(Qt::NoBrush);
        const QPointF c = at(0.5, 0.5);
        QPainterPath gear;
        constexpr int teeth = 8;
        for (int i = 0; i < teeth * 2; ++i) {
            const qreal a0 = i * M_PI / teeth - M_PI / (teeth * 2);
            const qreal a1 = a0 + M_PI / teeth;
            const qreal radius = (i % 2 == 0) ? s * 0.44 : s * 0.33;
            const QPointF p0(c.x() + radius * std::cos(a0), c.y() + radius * std::sin(a0));
            const QPointF p1(c.x() + radius * std::cos(a1), c.y() + radius * std::sin(a1));
            if (i == 0) {
                gear.moveTo(p0);
            } else {
                gear.lineTo(p0);
            }
            gear.lineTo(p1);
        }
        gear.closeSubpath();
        painter.drawPath(gear);
        painter.drawEllipse(c, s * 0.13, s * 0.13);
        break;
    }
    case Icon::More: {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int i = 0; i < 3; ++i) {
            painter.drawEllipse(at(0.22 + i * 0.28, 0.5), s * 0.075, s * 0.075);
        }
        break;
    }
    case Icon::Search: {
        painter.drawEllipse(QRectF(at(0.14, 0.14), at(0.68, 0.68)));
        painter.drawLine(at(0.62, 0.62), at(0.86, 0.86));
        break;
    }
    case Icon::Folder:
    case Icon::FolderAdd: {
        QPainterPath path;
        path.moveTo(at(0.10, 0.26));
        path.lineTo(at(0.40, 0.26));
        path.lineTo(at(0.48, 0.35));
        path.lineTo(at(0.90, 0.35));
        path.lineTo(at(0.90, 0.80));
        path.lineTo(at(0.10, 0.80));
        path.closeSubpath();
        painter.drawPath(path);
        if (icon == Icon::FolderAdd) {
            painter.drawLine(at(0.50, 0.46), at(0.50, 0.70));
            painter.drawLine(at(0.38, 0.58), at(0.62, 0.58));
        }
        break;
    }
    case Icon::List: {
        for (int i = 0; i < 3; ++i) {
            const qreal y = 0.28 + i * 0.22;
            painter.drawPoint(at(0.16, y));
            painter.drawLine(at(0.32, y), at(0.86, y));
        }
        break;
    }
    case Icon::Grid: {
        const qreal g = 0.30;
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                const QPointF tl = at(0.15 + i * (g + 0.10), 0.15 + j * (g + 0.10));
                painter.drawRoundedRect(QRectF(tl, QSizeF(s * g, s * g)), s * 0.06, s * 0.06);
            }
        }
        break;
    }
    case Icon::Sort: {
        painter.drawLine(at(0.32, 0.16), at(0.32, 0.84));
        painter.drawLine(at(0.16, 0.32), at(0.32, 0.16));
        painter.drawLine(at(0.48, 0.32), at(0.32, 0.16));
        painter.drawLine(at(0.68, 0.16), at(0.68, 0.84));
        painter.drawLine(at(0.52, 0.68), at(0.68, 0.84));
        painter.drawLine(at(0.84, 0.68), at(0.68, 0.84));
        break;
    }
    case Icon::Clock: {
        painter.drawEllipse(QRectF(at(0.12, 0.12), at(0.88, 0.88)));
        painter.drawLine(at(0.5, 0.5), at(0.5, 0.28));
        painter.drawLine(at(0.5, 0.5), at(0.66, 0.60));
        break;
    }
    case Icon::Controller: {
        QPainterPath body;
        body.addRoundedRect(QRectF(at(0.08, 0.28), at(0.92, 0.74)), s * 0.18, s * 0.18);
        painter.drawPath(body);
        painter.drawLine(at(0.24, 0.51), at(0.38, 0.51));
        painter.drawLine(at(0.31, 0.44), at(0.31, 0.58));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(0.66, 0.46), s * 0.045, s * 0.045);
        painter.drawEllipse(at(0.76, 0.56), s * 0.045, s * 0.045);
        break;
    }
    case Icon::Close: {
        painter.drawLine(at(0.25, 0.25), at(0.75, 0.75));
        painter.drawLine(at(0.75, 0.25), at(0.25, 0.75));
        break;
    }
    case Icon::ChevronDown: {
        painter.drawLine(at(0.25, 0.40), at(0.50, 0.64));
        painter.drawLine(at(0.75, 0.40), at(0.50, 0.64));
        break;
    }
    case Icon::ChevronLeft: {
        painter.drawLine(at(0.60, 0.25), at(0.36, 0.50));
        painter.drawLine(at(0.60, 0.75), at(0.36, 0.50));
        break;
    }
    case Icon::ChevronRight: {
        painter.drawLine(at(0.40, 0.25), at(0.64, 0.50));
        painter.drawLine(at(0.40, 0.75), at(0.64, 0.50));
        break;
    }
    case Icon::Check: {
        QPainterPath path;
        path.moveTo(at(0.20, 0.52));
        path.lineTo(at(0.42, 0.73));
        path.lineTo(at(0.80, 0.30));
        painter.drawPath(path);
        break;
    }
    case Icon::Trash: {
        painter.drawLine(at(0.16, 0.26), at(0.84, 0.26));
        painter.drawLine(at(0.40, 0.26), at(0.42, 0.14));
        painter.drawLine(at(0.42, 0.14), at(0.58, 0.14));
        painter.drawLine(at(0.58, 0.14), at(0.60, 0.26));
        QPainterPath body;
        body.moveTo(at(0.24, 0.26));
        body.lineTo(at(0.30, 0.86));
        body.lineTo(at(0.70, 0.86));
        body.lineTo(at(0.76, 0.26));
        painter.drawPath(body);
        painter.drawLine(at(0.43, 0.42), at(0.44, 0.70));
        painter.drawLine(at(0.57, 0.42), at(0.56, 0.70));
        break;
    }
    case Icon::Copy: {
        painter.drawRoundedRect(QRectF(at(0.32, 0.30), at(0.84, 0.86)), s * 0.08, s * 0.08);
        QPainterPath back;
        back.moveTo(at(0.22, 0.68));
        back.lineTo(at(0.16, 0.68));
        back.lineTo(at(0.16, 0.14));
        back.lineTo(at(0.66, 0.14));
        back.lineTo(at(0.66, 0.22));
        painter.drawPath(back);
        break;
    }
    case Icon::Link: {
        painter.save();
        painter.translate(r.center());
        painter.rotate(-45);
        painter.translate(-r.center());
        painter.drawRoundedRect(QRectF(at(0.12, 0.38), at(0.52, 0.62)), s * 0.12, s * 0.12);
        painter.drawRoundedRect(QRectF(at(0.48, 0.38), at(0.88, 0.62)), s * 0.12, s * 0.12);
        painter.restore();
        break;
    }
    case Icon::Shield: {
        QPainterPath path;
        path.moveTo(at(0.50, 0.12));
        path.lineTo(at(0.82, 0.24));
        path.cubicTo(at(0.82, 0.58), at(0.68, 0.78), at(0.50, 0.88));
        path.cubicTo(at(0.32, 0.78), at(0.18, 0.58), at(0.18, 0.24));
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(at(0.36, 0.50), at(0.46, 0.60));
        painter.drawLine(at(0.46, 0.60), at(0.64, 0.40));
        break;
    }
    case Icon::Globe: {
        painter.drawEllipse(QRectF(at(0.14, 0.14), at(0.86, 0.86)));
        painter.drawEllipse(QRectF(at(0.34, 0.14), at(0.66, 0.86)));
        painter.drawLine(at(0.14, 0.50), at(0.86, 0.50));
        break;
    }
    case Icon::Download: {
        painter.drawLine(at(0.50, 0.14), at(0.50, 0.62));
        painter.drawLine(at(0.32, 0.46), at(0.50, 0.64));
        painter.drawLine(at(0.68, 0.46), at(0.50, 0.64));
        painter.drawLine(at(0.18, 0.66), at(0.18, 0.84));
        painter.drawLine(at(0.18, 0.84), at(0.82, 0.84));
        painter.drawLine(at(0.82, 0.84), at(0.82, 0.66));
        break;
    }
    case Icon::Shortcut: {
        painter.drawRoundedRect(QRectF(at(0.14, 0.24), at(0.76, 0.86)), s * 0.08, s * 0.08);
        painter.drawLine(at(0.46, 0.54), at(0.86, 0.14));
        painter.drawLine(at(0.60, 0.14), at(0.86, 0.14));
        painter.drawLine(at(0.86, 0.14), at(0.86, 0.40));
        break;
    }
    case Icon::Info: {
        painter.drawEllipse(QRectF(at(0.12, 0.12), at(0.88, 0.88)));
        painter.drawLine(at(0.50, 0.46), at(0.50, 0.70));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(0.50, 0.32), s * 0.05, s * 0.05);
        break;
    }
    case Icon::Power: {
        painter.drawArc(QRectF(at(0.16, 0.20), at(0.84, 0.88)), 120 * 16, -240 * 16);
        painter.drawLine(at(0.50, 0.10), at(0.50, 0.48));
        break;
    }
    case Icon::Wrench: {
        painter.drawLine(at(0.22, 0.78), at(0.56, 0.44));
        painter.drawArc(QRectF(at(0.48, 0.14), at(0.86, 0.52)), 200 * 16, -270 * 16);
        break;
    }
    case Icon::People: {
        painter.drawEllipse(QRectF(at(0.22, 0.16), at(0.50, 0.44)));
        painter.drawArc(QRectF(at(0.10, 0.52), at(0.62, 1.04)), 0, 180 * 16);
        painter.drawEllipse(QRectF(at(0.56, 0.22), at(0.80, 0.46)));
        painter.drawArc(QRectF(at(0.50, 0.56), at(0.92, 0.98)), 0, 180 * 16);
        break;
    }
    case Icon::Eye: {
        QPainterPath path;
        path.moveTo(at(0.08, 0.50));
        path.quadTo(at(0.50, 0.10), at(0.92, 0.50));
        path.quadTo(at(0.50, 0.90), at(0.08, 0.50));
        painter.drawPath(path);
        painter.drawEllipse(at(0.50, 0.50), s * 0.12, s * 0.12);
        break;
    }
    case Icon::Menu: {
        for (int i = 0; i < 3; ++i) {
            painter.drawLine(at(0.18, 0.28 + i * 0.22), at(0.82, 0.28 + i * 0.22));
        }
        break;
    }
    case Icon::Speaker: {
        QPainterPath body;
        body.moveTo(at(0.12, 0.38));
        body.lineTo(at(0.30, 0.38));
        body.lineTo(at(0.52, 0.18));
        body.lineTo(at(0.52, 0.82));
        body.lineTo(at(0.30, 0.62));
        body.lineTo(at(0.12, 0.62));
        body.closeSubpath();
        painter.drawPath(body);
        painter.drawArc(QRectF(at(0.52, 0.34), at(0.72, 0.66)), -60 * 16, 120 * 16);
        painter.drawArc(QRectF(at(0.52, 0.20), at(0.88, 0.80)), -60 * 16, 120 * 16);
        break;
    }
    case Icon::Image: {
        painter.drawRoundedRect(QRectF(at(0.12, 0.18), at(0.88, 0.82)), s * 0.08, s * 0.08);
        painter.drawEllipse(at(0.34, 0.38), s * 0.07, s * 0.07);
        QPainterPath hills;
        hills.moveTo(at(0.14, 0.76));
        hills.lineTo(at(0.40, 0.52));
        hills.lineTo(at(0.56, 0.66));
        hills.lineTo(at(0.68, 0.56));
        hills.lineTo(at(0.86, 0.74));
        painter.drawPath(hills);
        break;
    }
    case Icon::Refresh: {
        const QRectF circle(at(0.16, 0.16), at(0.84, 0.84));
        painter.drawArc(circle, 70 * 16, 290 * 16);
        // Arrow head at the start of the arc (top, pointing clockwise)
        const qreal a = 70.0 * M_PI / 180.0;
        const QPointF tip(circle.center().x() + circle.width() / 2 * std::cos(a),
                          circle.center().y() - circle.height() / 2 * std::sin(a));
        painter.drawLine(tip, tip + QPointF(-s * 0.20, -s * 0.06));
        painter.drawLine(tip, tip + QPointF(-s * 0.04, s * 0.19));
        break;
    }
    case Icon::Chip: {
        painter.drawRoundedRect(QRectF(at(0.24, 0.24), at(0.76, 0.76)), s * 0.07, s * 0.07);
        painter.drawRect(QRectF(at(0.40, 0.40), at(0.60, 0.60)));
        for (int i = 0; i < 3; ++i) {
            const qreal p = 0.36 + i * 0.14;
            painter.drawLine(at(p, 0.10), at(p, 0.24));
            painter.drawLine(at(p, 0.76), at(p, 0.90));
            painter.drawLine(at(0.10, p), at(0.24, p));
            painter.drawLine(at(0.76, p), at(0.90, p));
        }
        break;
    }
    case Icon::Display: {
        painter.drawRoundedRect(QRectF(at(0.10, 0.18), at(0.90, 0.70)), s * 0.07, s * 0.07);
        painter.drawLine(at(0.50, 0.70), at(0.50, 0.84));
        painter.drawLine(at(0.32, 0.84), at(0.68, 0.84));
        break;
    }
    case Icon::Handheld: {
        painter.drawRoundedRect(QRectF(at(0.06, 0.28), at(0.94, 0.72)), s * 0.12, s * 0.12);
        painter.drawLine(at(0.24, 0.28), at(0.24, 0.72));
        painter.drawLine(at(0.76, 0.28), at(0.76, 0.72));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(0.15, 0.42), s * 0.04, s * 0.04);
        painter.drawEllipse(at(0.85, 0.58), s * 0.04, s * 0.04);
        break;
    }
    case Icon::Lock: {
        painter.drawRoundedRect(QRectF(at(0.20, 0.44), at(0.80, 0.88)), s * 0.08, s * 0.08);
        QPainterPath shackle;
        shackle.moveTo(at(0.32, 0.44));
        shackle.lineTo(at(0.32, 0.32));
        shackle.arcTo(QRectF(at(0.32, 0.12), at(0.68, 0.48)), 180, -180);
        shackle.lineTo(at(0.68, 0.44));
        painter.drawPath(shackle);
        break;
    }
    case Icon::User: {
        painter.drawEllipse(QRectF(at(0.32, 0.12), at(0.68, 0.48)));
        QPainterPath body;
        body.moveTo(at(0.14, 0.90));
        body.cubicTo(at(0.14, 0.62), at(0.34, 0.56), at(0.50, 0.56));
        body.cubicTo(at(0.66, 0.56), at(0.86, 0.62), at(0.86, 0.90));
        painter.drawPath(body);
        break;
    }
    case Icon::Gauge: {
        painter.drawArc(QRectF(at(0.10, 0.20), at(0.90, 1.00)), 0, 180 * 16);
        painter.drawLine(at(0.50, 0.60), at(0.70, 0.36));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(0.50, 0.60), s * 0.06, s * 0.06);
        break;
    }
    case Icon::Wifi: {
        for (int i = 0; i < 3; ++i) {
            const qreal radius = 0.14 + i * 0.15;
            painter.drawArc(
                QRectF(at(0.50 - radius, 0.80 - radius), at(0.50 + radius, 0.80 + radius)), 45 * 16,
                90 * 16);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(at(0.50, 0.80), s * 0.06, s * 0.06);
        break;
    }
    case Icon::Filter: {
        QPainterPath funnel;
        funnel.moveTo(at(0.14, 0.20));
        funnel.lineTo(at(0.86, 0.20));
        funnel.lineTo(at(0.58, 0.54));
        funnel.lineTo(at(0.58, 0.82));
        funnel.lineTo(at(0.42, 0.74));
        funnel.lineTo(at(0.42, 0.54));
        funnel.closeSubpath();
        painter.drawPath(funnel);
        break;
    }
    case Icon::Keyboard: {
        painter.drawRoundedRect(QRectF(at(0.06, 0.24), at(0.94, 0.76)), s * 0.08, s * 0.08);
        painter.drawLine(at(0.30, 0.62), at(0.70, 0.62));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int i = 0; i < 5; ++i) {
            painter.drawEllipse(at(0.22 + i * 0.14, 0.40), s * 0.035, s * 0.035);
        }
        break;
    }
    case Icon::FrameGen: {
        // A frame with a ghosted copy behind it and motion streaks: frames in between frames
        painter.save();
        painter.setOpacity(painter.opacity() * 0.45);
        painter.drawRoundedRect(QRectF(at(0.08, 0.20), at(0.58, 0.64)), s * 0.07, s * 0.07);
        painter.restore();
        painter.drawRoundedRect(QRectF(at(0.36, 0.36), at(0.92, 0.82)), s * 0.07, s * 0.07);
        painter.drawLine(at(0.08, 0.76), at(0.24, 0.76));
        painter.drawLine(at(0.14, 0.86), at(0.30, 0.86));
        break;
    }
    case Icon::Send: {
        // Paper plane
        QPainterPath plane;
        plane.moveTo(at(0.10, 0.46));
        plane.lineTo(at(0.90, 0.12));
        plane.lineTo(at(0.62, 0.90));
        plane.lineTo(at(0.46, 0.56));
        plane.closeSubpath();
        painter.drawPath(plane);
        painter.drawLine(at(0.46, 0.56), at(0.90, 0.12));
        break;
    }
    case Icon::Chat: {
        // Speech bubble with a tail and three dots
        QPainterPath bubble;
        bubble.addRoundedRect(QRectF(at(0.10, 0.14), at(0.90, 0.70)), s * 0.14, s * 0.14);
        painter.drawPath(bubble);
        painter.drawLine(at(0.30, 0.70), at(0.24, 0.88));
        painter.drawLine(at(0.24, 0.88), at(0.48, 0.70));
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            painter.drawEllipse(at(0.32 + i * 0.18, 0.42), s * 0.045, s * 0.045);
        }
        break;
    }
    case Icon::Exit: {
        // Door frame with an arrow leaving it
        QPainterPath frame;
        frame.moveTo(at(0.50, 0.14));
        frame.lineTo(at(0.18, 0.14));
        frame.lineTo(at(0.18, 0.86));
        frame.lineTo(at(0.50, 0.86));
        painter.drawPath(frame);
        painter.drawLine(at(0.40, 0.50), at(0.88, 0.50));
        painter.drawLine(at(0.72, 0.34), at(0.88, 0.50));
        painter.drawLine(at(0.72, 0.66), at(0.88, 0.50));
        break;
    }
    case Icon::Sliders: {
        const std::array<qreal, 3> knobs{0.34, 0.66, 0.46};
        for (int i = 0; i < 3; ++i) {
            const qreal y = 0.24 + i * 0.26;
            painter.drawLine(at(0.12, y), at(0.88, y));
            painter.setBrush(color);
            painter.drawEllipse(at(knobs[i], y), s * 0.075, s * 0.075);
            painter.setBrush(Qt::NoBrush);
        }
        break;
    }
    }
    painter.restore();
}

QStringList WrapText(const QString& text, const QFont& font, qreal width, int max_lines) {
    QStringList lines;
    if (text.isEmpty() || width <= 4 || max_lines <= 0) {
        return lines;
    }

    const QFontMetricsF metrics(font);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        if (lines.size() == max_lines - 1) {
            const QString rest = text.mid(line.textStart()).simplified();
            lines << metrics.elidedText(rest, Qt::ElideRight, width);
            break;
        }
        lines << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines;
}

QString FormatPlayTime(quint64 seconds, bool compact) {
    if (seconds == 0) {
        return {};
    }
    const quint64 hours = seconds / 3600;
    const quint64 minutes = (seconds / 60) % 60;
    if (hours == 0) {
        if (minutes == 0) {
            return QCoreApplication::translate("Library", "< 1 min");
        }
        return QCoreApplication::translate("Library", "%1 min").arg(minutes);
    }
    if (compact || minutes == 0) {
        return QCoreApplication::translate("Library", "%1 h").arg(hours);
    }
    return QCoreApplication::translate("Library", "%1 h %2 min").arg(hours).arg(minutes);
}

QString GameCountText(int count) {
    return count == 1 ? QCoreApplication::translate("Library", "1 game")
                      : QCoreApplication::translate("Library", "%1 games").arg(count);
}

QString ResultCountText(int count) {
    return count == 1 ? QCoreApplication::translate("Library", "1 result")
                      : QCoreApplication::translate("Library", "%1 results").arg(count);
}

} // namespace Library
