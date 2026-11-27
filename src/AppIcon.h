#pragma once

#include <QBrush>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QRadialGradient>
#include <QRectF>
#include <QString>
#include <algorithm>
#include <cmath>

namespace AppIcon {

// Рисует векторный значок Deep Dive в квадрат [0..size, 0..size]
// Использует только геометрию/градиенты QPainter (без QFont/QPixmap — безопасно без QGuiApplication).
inline void paint(QPainter& p, qreal size) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qreal s = size / 100.0;
    p.scale(s, s);

    // 1. Внешний контур — современный скруглённый квадрат (squircle-пропорция)
    QPainterPath badgePath;
    badgePath.addRoundedRect(QRectF(4.0, 4.0, 92.0, 92.0), 22.0, 22.0);

    // Глубоководный фон
    QLinearGradient bgGrad(50.0, 4.0, 50.0, 96.0);
    bgGrad.setColorAt(0.00, QColor(11, 88, 118));
    bgGrad.setColorAt(0.45, QColor(5, 42, 64));
    bgGrad.setColorAt(1.00, QColor(2, 16, 27));
    p.fillPath(badgePath, bgGrad);

    p.save();
    p.setClipPath(badgePath);

    // 2. Окно Снеллиуса (свечение солнечного света сверху)
    QRadialGradient snellGlow(50.0, 12.0, 50.0);
    snellGlow.setColorAt(0.00, QColor(156, 253, 245, 165));
    snellGlow.setColorAt(0.35, QColor(42, 205, 222, 82));
    snellGlow.setColorAt(0.75, QColor(12, 112, 145, 22));
    snellGlow.setColorAt(1.00, QColor(4, 30, 48, 0));
    p.fillRect(QRectF(0.0, 0.0, 100.0, 100.0), snellGlow);

    // 3. Объёмные лучи света (God Rays)
    auto drawRay = [&](qreal topX1, qreal topX2, qreal botX1, qreal botX2, int alpha) {
        QPainterPath ray;
        ray.moveTo(topX1, 4.0);
        ray.lineTo(topX2, 4.0);
        ray.lineTo(botX2, 74.0);
        ray.lineTo(botX1, 74.0);
        ray.closeSubpath();

        QLinearGradient rg(50.0, 4.0, 50.0, 74.0);
        rg.setColorAt(0.0, QColor(135, 250, 242, alpha));
        rg.setColorAt(1.0, QColor(60, 210, 225, 0));
        p.fillPath(ray, rg);
    };
    drawRay(38.0, 47.0, 14.0, 34.0, 42);
    drawRay(47.0, 55.0, 40.0, 60.0, 50);
    drawRay(54.0, 63.0, 66.0, 86.0, 38);

    // 4. Тонкое кольцо иллюминатора / глубиномера (технологичный акцент)
    p.setPen(QPen(QColor(110, 242, 234, 72), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(50.0, 48.0), 34.5, 34.5);

    // 4 риски компаса / глубиномера на кольце
    p.setPen(QPen(QColor(150, 252, 245, 110), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(50.0, 11.2), QPointF(50.0, 15.8));
    p.drawLine(QPointF(86.8, 48.0), QPointF(82.2, 48.0));
    p.drawLine(QPointF(13.2, 48.0), QPointF(17.8, 48.0));

    // 5. Главный узнаваемый силуэт: ныряющий хвост кита
    QPainterPath fluke;
    fluke.moveTo(44.5, 68.0);
    fluke.cubicTo(45.5, 57.0, 46.5, 49.0, 44.0, 44.5);
    fluke.cubicTo(37.5, 43.0, 26.0, 41.0, 18.5, 32.0);
    fluke.cubicTo(24.0, 26.5, 40.0, 27.5, 50.0, 36.5);
    fluke.cubicTo(60.0, 27.5, 76.0, 26.5, 81.5, 32.0);
    fluke.cubicTo(74.0, 41.0, 62.5, 43.0, 56.0, 44.5);
    fluke.cubicTo(53.5, 49.0, 54.5, 57.0, 55.5, 68.0);
    fluke.closeSubpath();

    // Мягкая тень/ореол за хвостом для контраста на малых размерах
    p.save();
    p.translate(0.0, 1.8);
    p.fillPath(fluke, QColor(2, 14, 24, 105));
    p.restore();

    QLinearGradient flukeGrad(50.0, 27.0, 50.0, 66.0);
    flukeGrad.setColorAt(0.00, QColor(232, 255, 255));
    flukeGrad.setColorAt(0.42, QColor(115, 240, 232));
    flukeGrad.setColorAt(1.00, QColor(28, 178, 202));
    p.fillPath(fluke, flukeGrad);

    // Тонкий световой блик по верхней кромке лопастей
    QPainterPath flukeRim;
    flukeRim.moveTo(20.0, 31.8);
    flukeRim.cubicTo(25.2, 27.3, 40.2, 28.4, 50.0, 36.9);
    flukeRim.cubicTo(59.8, 28.4, 74.8, 27.3, 80.0, 31.8);
    p.setPen(QPen(QColor(255, 255, 255, 195), 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(flukeRim);

    // 6. Слоёные подводные волны в нижней трети
    QPainterPath backWave;
    backWave.moveTo(2.0, 64.0);
    backWave.cubicTo(22.0, 55.0, 38.0, 67.0, 56.0, 61.5);
    backWave.cubicTo(72.0, 56.5, 84.0, 61.0, 98.0, 63.5);
    backWave.lineTo(98.0, 98.0);
    backWave.lineTo(2.0, 98.0);
    backWave.closeSubpath();

    QLinearGradient backWaveGrad(50.0, 56.0, 50.0, 96.0);
    backWaveGrad.setColorAt(0.0, QColor(22, 148, 176, 225));
    backWaveGrad.setColorAt(1.0, QColor(5, 48, 70, 245));
    p.fillPath(backWave, backWaveGrad);

    QPainterPath frontWave;
    frontWave.moveTo(2.0, 72.5);
    frontWave.cubicTo(24.0, 80.0, 46.0, 66.0, 68.0, 72.0);
    frontWave.cubicTo(81.0, 75.5, 90.0, 73.0, 98.0, 70.5);
    frontWave.lineTo(98.0, 98.0);
    frontWave.lineTo(2.0, 98.0);
    frontWave.closeSubpath();

    QLinearGradient frontWaveGrad(50.0, 68.0, 50.0, 96.0);
    frontWaveGrad.setColorAt(0.0, QColor(8, 72, 98, 248));
    frontWaveGrad.setColorAt(1.0, QColor(2, 20, 34, 255));
    p.fillPath(frontWave, frontWaveGrad);

    // Светящийся гребень передней волны
    QPainterPath waveCrest;
    waveCrest.moveTo(3.0, 72.5);
    waveCrest.cubicTo(24.0, 80.0, 46.0, 66.0, 68.0, 72.0);
    waveCrest.cubicTo(81.0, 75.5, 90.0, 73.0, 97.0, 70.5);
    p.setPen(QPen(QColor(115, 242, 232, 205), 2.1, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(waveCrest);

    // 7. Биолюминесцентные пузырьки
    auto drawBubble = [&](qreal cx, qreal cy, qreal r) {
        p.setPen(QPen(QColor(160, 252, 248, 190), 1.0));
        p.setBrush(QColor(90, 235, 232, 55));
        p.drawEllipse(QPointF(cx, cy), r, r);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(240, 255, 255, 220));
        p.drawEllipse(QPointF(cx - r * 0.32, cy - r * 0.32), r * 0.28, r * 0.28);
    };
    drawBubble(28.5, 51.0, 2.7);
    drawBubble(73.0, 48.0, 2.3);
    drawBubble(64.5, 22.5, 1.9);
    drawBubble(35.5, 21.0, 1.5);

    p.restore(); // снимаем clip

    // 8. Внешняя стеклянная окантовка (чтобы значок читался и на светлых, и на тёмных темах)
    QLinearGradient borderGrad(50.0, 4.0, 50.0, 96.0);
    borderGrad.setColorAt(0.0, QColor(145, 250, 242, 185));
    borderGrad.setColorAt(0.5, QColor(45, 185, 205, 125));
    borderGrad.setColorAt(1.0, QColor(18, 95, 122, 175));
    p.setPen(QPen(QBrush(borderGrad), 2.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(badgePath);

    p.restore();
}

// Рендер прозрачного RGBA-изображения произвольного разрешения
inline QImage renderImage(int pixelSize) {
    QImage img(pixelSize, pixelSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    paint(p, static_cast<qreal>(pixelSize));
    return img;
}

// Иконка приложения с набором чётких размеров для Qt (окно, панель задач, Alt+Tab)
inline QIcon createAppIcon() {
    QIcon icon;
    static constexpr int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    for (int sz : kSizes) {
        icon.addPixmap(QPixmap::fromImage(renderImage(sz)));
    }
    return icon;
}

// HiDPI-пиксмап для UI-виджетов (например, экрана загрузки)
inline QPixmap createPixmap(int logicalSize, qreal dpr = 1.0) {
    const qreal clampedDpr = std::max<qreal>(1.0, dpr);
    const int physSize = std::max(16, static_cast<int>(std::lround(logicalSize * clampedDpr)));
    QPixmap pm = QPixmap::fromImage(renderImage(physSize));
    pm.setDevicePixelRatio(clampedDpr);
    return pm;
}

// Боковой баннер NSIS Welcome / Finish (164 x 314, 24-bit RGB)
inline QImage renderNsisWelcomeBanner() {
    constexpr int W = 164;
    constexpr int H = 314;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(3, 18, 30));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient bg(0.0, 0.0, 0.0, H);
    bg.setColorAt(0.00, QColor(10, 74, 102));
    bg.setColorAt(0.45, QColor(4, 34, 54));
    bg.setColorAt(1.00, QColor(2, 13, 22));
    p.fillRect(QRectF(0, 0, W, H), bg);

    // Лучи света сверху
    QRadialGradient topGlow(W * 0.5, 14.0, 125.0);
    topGlow.setColorAt(0.0, QColor(125, 248, 240, 105));
    topGlow.setColorAt(0.6, QColor(26, 175, 198, 28));
    topGlow.setColorAt(1.0, QColor(4, 24, 38, 0));
    p.fillRect(QRectF(0, 0, W, H), topGlow);

    auto drawRay = [&](qreal x1, qreal x2, qreal bx1, qreal bx2, int a) {
        QPainterPath ray;
        ray.moveTo(x1, 0.0);
        ray.lineTo(x2, 0.0);
        ray.lineTo(bx2, 230.0);
        ray.lineTo(bx1, 230.0);
        ray.closeSubpath();
        QLinearGradient g(W * 0.5, 0.0, W * 0.5, 230.0);
        g.setColorAt(0.0, QColor(130, 248, 240, a));
        g.setColorAt(1.0, QColor(40, 190, 210, 0));
        p.fillPath(ray, g);
    };
    drawRay(42.0, 68.0, -10.0, 48.0, 32);
    drawRay(76.0, 108.0, 56.0, 128.0, 38);
    drawRay(114.0, 136.0, 120.0, 178.0, 26);

    // Декоративные дуги сонара на фоне
    p.setPen(QPen(QColor(105, 238, 232, 34), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(W * 0.5, 114.0), 68.0, 68.0);
    p.drawEllipse(QPointF(W * 0.5, 114.0), 84.0, 84.0);

    // Мягкий ореол и сам логотип Deep Dive по центру верхней половины
    QRadialGradient halo(W * 0.5, 114.0, 68.0);
    halo.setColorAt(0.0, QColor(85, 235, 232, 85));
    halo.setColorAt(1.0, QColor(85, 235, 232, 0));
    p.fillRect(QRectF(0, 40, W, 150), halo);

    p.save();
    p.translate(24.0, 56.0);
    paint(p, 116.0);
    p.restore();

    // Волновые ленты внизу панели
    QPainterPath w1;
    w1.moveTo(0.0, 246.0);
    w1.cubicTo(48.0, 230.0, 108.0, 262.0, W, 242.0);
    w1.lineTo(W, H);
    w1.lineTo(0.0, H);
    w1.closeSubpath();
    p.fillPath(w1, QColor(14, 112, 138, 125));

    QPainterPath w2;
    w2.moveTo(0.0, 270.0);
    w2.cubicTo(52.0, 286.0, 110.0, 256.0, W, 272.0);
    w2.lineTo(W, H);
    w2.lineTo(0.0, H);
    w2.closeSubpath();
    p.fillPath(w2, QColor(5, 48, 70, 210));

    QPainterPath crest;
    crest.moveTo(0.0, 270.0);
    crest.cubicTo(52.0, 286.0, 110.0, 256.0, W, 272.0);
    p.setPen(QPen(QColor(110, 242, 232, 140), 1.6));
    p.drawPath(crest);

    // Тонкая разделительная полоса справа
    p.setPen(QPen(QColor(65, 210, 222, 140), 1.0));
    p.drawLine(W - 1, 0, W - 1, H);

    p.end();
    return img.convertToFormat(QImage::Format_RGB888);
}

// Верхний баннер страниц NSIS Header (150 x 57, 24-bit RGB, плавный переход в белый фон заголовка)
inline QImage renderNsisHeaderBanner() {
    constexpr int W = 150;
    constexpr int H = 57;
    QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Мягкий аква-акцент слева, уходящий в чистый белый справа (без стыка с шапкой NSIS)
    QLinearGradient strip(0.0, 0.0, W, 0.0);
    strip.setColorAt(0.00, QColor(224, 248, 252));
    strip.setColorAt(0.55, QColor(242, 252, 254));
    strip.setColorAt(1.00, QColor(255, 255, 255));
    p.fillRect(QRectF(0, 0, W, H), strip);

    // Тонкая декоративная волна, затухающая вправо
    QPainterPath wave;
    wave.moveTo(0.0, 44.0);
    wave.cubicTo(42.0, 34.0, 88.0, 52.0, 146.0, 41.0);
    QLinearGradient waveGrad(0.0, 0.0, 146.0, 0.0);
    waveGrad.setColorAt(0.0, QColor(34, 185, 206, 150));
    waveGrad.setColorAt(0.6, QColor(95, 228, 224, 70));
    waveGrad.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setPen(QPen(QBrush(waveGrad), 2.0, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(wave);

    // Сам значок Deep Dive (слева в баннере)
    p.save();
    p.translate(10.0, 5.5);
    paint(p, 46.0);
    p.restore();

    // Три минималистичных аква-пузырька рядом со значком
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 175, 198, 120));
    p.drawEllipse(QPointF(68.0, 22.0), 3.4, 3.4);
    p.setBrush(QColor(68, 214, 220, 95));
    p.drawEllipse(QPointF(79.0, 31.0), 2.5, 2.5);
    p.setBrush(QColor(110, 232, 228, 70));
    p.drawEllipse(QPointF(89.0, 20.0), 1.9, 1.9);

    p.end();
    return img.convertToFormat(QImage::Format_RGB888);
}

// Эквивалентный векторный SVG-документ (для Linux hicolor/scalable и веба/документации)
inline QString svgDocument() {
    return QString::fromUtf8(R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="512" height="512">
  <defs>
    <clipPath id="badgeClip">
      <rect x="4" y="4" width="92" height="92" rx="22" ry="22"/>
    </clipPath>
    <linearGradient id="bgGrad" x1="50" y1="4" x2="50" y2="96" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#0B5876"/>
      <stop offset="45%" stop-color="#052A40"/>
      <stop offset="100%" stop-color="#02101B"/>
    </linearGradient>
    <radialGradient id="snellGlow" cx="50" cy="12" r="50" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#9CFDF5" stop-opacity="0.65"/>
      <stop offset="35%" stop-color="#2ACDDE" stop-opacity="0.32"/>
      <stop offset="75%" stop-color="#0C7091" stop-opacity="0.09"/>
      <stop offset="100%" stop-color="#041E30" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="rayGrad" x1="50" y1="4" x2="50" y2="74" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#87FAF2" stop-opacity="0.20"/>
      <stop offset="100%" stop-color="#3CD2E1" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="flukeGrad" x1="50" y1="27" x2="50" y2="66" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#E8FFFF"/>
      <stop offset="42%" stop-color="#73F0E8"/>
      <stop offset="100%" stop-color="#1CB2CA"/>
    </linearGradient>
    <linearGradient id="backWaveGrad" x1="50" y1="56" x2="50" y2="96" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#1694B0" stop-opacity="0.88"/>
      <stop offset="100%" stop-color="#053046" stop-opacity="0.96"/>
    </linearGradient>
    <linearGradient id="frontWaveGrad" x1="50" y1="68" x2="50" y2="96" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#084862" stop-opacity="0.97"/>
      <stop offset="100%" stop-color="#021422"/>
    </linearGradient>
    <linearGradient id="borderGrad" x1="50" y1="4" x2="50" y2="96" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#91FAF2" stop-opacity="0.72"/>
      <stop offset="50%" stop-color="#2DB9CD" stop-opacity="0.49"/>
      <stop offset="100%" stop-color="#125F7A" stop-opacity="0.69"/>
    </linearGradient>
  </defs>

  <rect x="4" y="4" width="92" height="92" rx="22" ry="22" fill="url(#bgGrad)"/>

  <g clip-path="url(#badgeClip)">
    <rect x="0" y="0" width="100" height="100" fill="url(#snellGlow)"/>

    <polygon points="38,4 47,4 34,74 14,74" fill="url(#rayGrad)"/>
    <polygon points="47,4 55,4 60,74 40,74" fill="url(#rayGrad)"/>
    <polygon points="54,4 63,4 86,74 66,74" fill="url(#rayGrad)"/>

    <circle cx="50" cy="48" r="34.5" fill="none" stroke="#6EF2EA" stroke-opacity="0.28" stroke-width="1.6"/>
    <g stroke="#96FCF5" stroke-opacity="0.43" stroke-width="1.6" stroke-linecap="round">
      <line x1="50" y1="11.2" x2="50" y2="15.8"/>
      <line x1="86.8" y1="48" x2="82.2" y2="48"/>
      <line x1="13.2" y1="48" x2="17.8" y2="48"/>
    </g>

    <path transform="translate(0 1.8)" fill="#020E18" fill-opacity="0.41"
          d="M44.5,68 C45.5,57 46.5,49 44,44.5 C37.5,43 26,41 18.5,32 C24,26.5 40,27.5 50,36.5 C60,27.5 76,26.5 81.5,32 C74,41 62.5,43 56,44.5 C53.5,49 54.5,57 55.5,68 Z"/>
    <path fill="url(#flukeGrad)"
          d="M44.5,68 C45.5,57 46.5,49 44,44.5 C37.5,43 26,41 18.5,32 C24,26.5 40,27.5 50,36.5 C60,27.5 76,26.5 81.5,32 C74,41 62.5,43 56,44.5 C53.5,49 54.5,57 55.5,68 Z"/>
    <path fill="none" stroke="#FFFFFF" stroke-opacity="0.76" stroke-width="1.35" stroke-linecap="round" stroke-linejoin="round"
          d="M20,31.8 C25.2,27.3 40.2,28.4 50,36.9 C59.8,28.4 74.8,27.3 80,31.8"/>

    <path fill="url(#backWaveGrad)"
          d="M2,64 C22,55 38,67 56,61.5 C72,56.5 84,61 98,63.5 L98,98 L2,98 Z"/>
    <path fill="url(#frontWaveGrad)"
          d="M2,72.5 C24,80 46,66 68,72 C81,75.5 90,73 98,70.5 L98,98 L2,98 Z"/>
    <path fill="none" stroke="#73F2E8" stroke-opacity="0.80" stroke-width="2.1" stroke-linecap="round"
          d="M3,72.5 C24,80 46,66 68,72 C81,75.5 90,73 97,70.5"/>

    <g>
      <circle cx="28.5" cy="51" r="2.7" fill="#5AEBE8" fill-opacity="0.22" stroke="#A0FCF8" stroke-opacity="0.75" stroke-width="1"/>
      <circle cx="27.6" cy="50.1" r="0.75" fill="#F0FFFF" fill-opacity="0.86"/>
      <circle cx="73" cy="48" r="2.3" fill="#5AEBE8" fill-opacity="0.22" stroke="#A0FCF8" stroke-opacity="0.75" stroke-width="1"/>
      <circle cx="72.3" cy="47.3" r="0.65" fill="#F0FFFF" fill-opacity="0.86"/>
      <circle cx="64.5" cy="22.5" r="1.9" fill="#5AEBE8" fill-opacity="0.22" stroke="#A0FCF8" stroke-opacity="0.75" stroke-width="1"/>
      <circle cx="63.9" cy="21.9" r="0.55" fill="#F0FFFF" fill-opacity="0.86"/>
      <circle cx="35.5" cy="21" r="1.5" fill="#5AEBE8" fill-opacity="0.22" stroke="#A0FCF8" stroke-opacity="0.75" stroke-width="1"/>
      <circle cx="35.0" cy="20.5" r="0.45" fill="#F0FFFF" fill-opacity="0.86"/>
    </g>
  </g>

  <rect x="4" y="4" width="92" height="92" rx="22" ry="22" fill="none" stroke="url(#borderGrad)" stroke-width="2.2"/>
</svg>
)svg");
}

} // namespace AppIcon
