#include "AppIcon.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QString>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

void appendU16LE(QByteArray& out, uint16_t v) {
    out.append(static_cast<char>(v & 0xFFu));
    out.append(static_cast<char>((v >> 8) & 0xFFu));
}

void appendU32LE(QByteArray& out, uint32_t v) {
    out.append(static_cast<char>(v & 0xFFu));
    out.append(static_cast<char>((v >> 8) & 0xFFu));
    out.append(static_cast<char>((v >> 16) & 0xFFu));
    out.append(static_cast<char>((v >> 24) & 0xFFu));
}

void appendI32LE(QByteArray& out, int32_t v) {
    appendU32LE(out, static_cast<uint32_t>(v));
}

void appendU32BE(QByteArray& out, uint32_t v) {
    out.append(static_cast<char>((v >> 24) & 0xFFu));
    out.append(static_cast<char>((v >> 16) & 0xFFu));
    out.append(static_cast<char>((v >> 8) & 0xFFu));
    out.append(static_cast<char>(v & 0xFFu));
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "Failed to open for writing: %s\n", qPrintable(path));
        return false;
    }
    if (f.write(data) != data.size()) {
        std::fprintf(stderr, "Failed to write all bytes: %s\n", qPrintable(path));
        return false;
    }
    return true;
}

QByteArray encodePng(const QImage& img) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

// 32-bit BGRA DIB (прямой альфа-канал + 1-bit AND-маска) для Windows ICO <= 64px
QByteArray encodeIcoDib32(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width();
    const int h = img.height();
    const int andStride = ((w + 31) / 32) * 4;
    const uint32_t imageSize = static_cast<uint32_t>(w * h * 4 + andStride * h);

    QByteArray out;
    out.reserve(40 + static_cast<int>(imageSize));

    // BITMAPINFOHEADER (40 bytes)
    appendU32LE(out, 40u);
    appendI32LE(out, w);
    appendI32LE(out, h * 2); // XOR + AND mask
    appendU16LE(out, 1u);    // planes
    appendU16LE(out, 32u);   // bpp
    appendU32LE(out, 0u);    // BI_RGB
    appendU32LE(out, imageSize);
    appendI32LE(out, 0);
    appendI32LE(out, 0);
    appendU32LE(out, 0u);
    appendU32LE(out, 0u);

    // Пиксели XOR (снизу вверх, BGRA)
    for (int y = h - 1; y >= 0; --y) {
        const auto* scan = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = scan[x];
            out.append(static_cast<char>(qBlue(px)));
            out.append(static_cast<char>(qGreen(px)));
            out.append(static_cast<char>(qRed(px)));
            out.append(static_cast<char>(qAlpha(px)));
        }
    }

    // 1-битная AND-маска (снизу вверх, выравнивание строки до 4 байт)
    QByteArray maskRow(andStride, '\0');
    for (int y = h - 1; y >= 0; --y) {
        maskRow.fill('\0');
        const auto* scan = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (qAlpha(scan[x]) < 128) {
                maskRow[x / 8] = static_cast<char>(
                    static_cast<uint8_t>(maskRow[x / 8]) | (0x80u >> (x & 7)));
            }
        }
        out.append(maskRow);
    }

    return out;
}

bool writeWindowsIco(const QString& path) {
    struct Entry {
        int size;
        QByteArray payload;
    };

    static constexpr int kIcoSizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    std::vector<Entry> entries;
    entries.reserve(std::size(kIcoSizes));

    for (int sz : kIcoSizes) {
        const QImage img = AppIcon::renderImage(sz);
        QByteArray payload = (sz >= 256) ? encodePng(img) : encodeIcoDib32(img);
        if (payload.isEmpty()) {
            return false;
        }
        entries.push_back({sz, std::move(payload)});
    }

    QByteArray ico;
    appendU16LE(ico, 0u); // reserved
    appendU16LE(ico, 1u); // type = ICON
    appendU16LE(ico, static_cast<uint16_t>(entries.size()));

    uint32_t offset = 6u + static_cast<uint32_t>(entries.size()) * 16u;
    for (const auto& e : entries) {
        ico.append(static_cast<char>(e.size >= 256 ? 0 : e.size));
        ico.append(static_cast<char>(e.size >= 256 ? 0 : e.size));
        ico.append('\0'); // colorCount
        ico.append('\0'); // reserved
        appendU16LE(ico, 1u);
        appendU16LE(ico, 32u);
        appendU32LE(ico, static_cast<uint32_t>(e.payload.size()));
        appendU32LE(ico, offset);
        offset += static_cast<uint32_t>(e.payload.size());
    }

    for (const auto& e : entries) {
        ico.append(e.payload);
    }

    return writeFile(path, ico);
}

bool writeMacIcns(const QString& path) {
    struct ChunkSpec {
        char tag[4];
        int size;
    };
    static constexpr ChunkSpec kSpecs[] = {
        {{'i','c','p','4'}, 16},
        {{'i','c','p','5'}, 32},
        {{'i','c','p','6'}, 64},
        {{'i','c','0','7'}, 128},
        {{'i','c','0','8'}, 256},
        {{'i','c','0','9'}, 512},
        {{'i','c','1','0'}, 1024},
        {{'i','c','1','1'}, 32},   // 16@2x
        {{'i','c','1','2'}, 64},   // 32@2x
        {{'i','c','1','3'}, 256},  // 128@2x
        {{'i','c','1','4'}, 512},  // 256@2x
    };

    QByteArray body;
    for (const auto& spec : kSpecs) {
        const QByteArray png = encodePng(AppIcon::renderImage(spec.size));
        if (png.isEmpty()) {
            return false;
        }
        body.append(spec.tag, 4);
        appendU32BE(body, static_cast<uint32_t>(8 + png.size()));
        body.append(png);
    }

    QByteArray icns;
    icns.reserve(8 + body.size());
    icns.append("icns", 4);
    appendU32BE(icns, static_cast<uint32_t>(8 + body.size()));
    icns.append(body);

    return writeFile(path, icns);
}

// Классический несжатый 24-bit BI_RGB BMP (40-байтный BITMAPINFOHEADER) для 100% совместимости с NSIS
bool writeBmp24(const QString& path, const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_RGB888);
    const int w = img.width();
    const int h = img.height();
    const int rowStride = ((w * 3 + 3) / 4) * 4;
    const uint32_t pixelBytes = static_cast<uint32_t>(rowStride * h);
    const uint32_t fileSize = 14u + 40u + pixelBytes;

    QByteArray bmp;
    bmp.reserve(static_cast<int>(fileSize));

    // BITMAPFILEHEADER (14 bytes)
    bmp.append('B');
    bmp.append('M');
    appendU32LE(bmp, fileSize);
    appendU16LE(bmp, 0u);
    appendU16LE(bmp, 0u);
    appendU32LE(bmp, 54u);

    // BITMAPINFOHEADER (40 bytes)
    appendU32LE(bmp, 40u);
    appendI32LE(bmp, w);
    appendI32LE(bmp, h);
    appendU16LE(bmp, 1u);
    appendU16LE(bmp, 24u);
    appendU32LE(bmp, 0u); // BI_RGB
    appendU32LE(bmp, pixelBytes);
    appendI32LE(bmp, 2835); // 72 DPI
    appendI32LE(bmp, 2835);
    appendU32LE(bmp, 0u);
    appendU32LE(bmp, 0u);

    const int padBytes = rowStride - w * 3;
    for (int y = h - 1; y >= 0; --y) {
        const uchar* scan = img.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            bmp.append(static_cast<char>(scan[x * 3 + 2])); // B
            bmp.append(static_cast<char>(scan[x * 3 + 1])); // G
            bmp.append(static_cast<char>(scan[x * 3 + 0])); // R
        }
        for (int p = 0; p < padBytes; ++p) {
            bmp.append('\0');
        }
    }

    return writeFile(path, bmp);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <output_directory>\n", argv[0]);
        return 1;
    }

    const QString outDirPath = QString::fromLocal8Bit(argv[1]);
    QDir outDir(outDirPath);
    if (!outDir.exists() && !outDir.mkpath(".")) {
        std::fprintf(stderr, "Failed to create output directory: %s\n", qPrintable(outDirPath));
        return 1;
    }

    // 1. Векторный SVG
    if (!writeFile(outDir.filePath("deepdive.svg"), AppIcon::svgDocument().toUtf8())) {
        return 2;
    }

    // 2. Растровые PNG (для Linux hicolor и общего использования)
    static constexpr int kPngSizes[] = {16, 32, 48, 64, 128, 256, 512};
    for (int sz : kPngSizes) {
        const QString pngPath = outDir.filePath(QString("deepdive_%1.png").arg(sz));
        if (!writeFile(pngPath, encodePng(AppIcon::renderImage(sz)))) {
            return 3;
        }
    }

    // 3. Windows ICO (exe, ярлыки, NSIS)
    if (!writeWindowsIco(outDir.filePath("deepdive.ico"))) {
        return 4;
    }

    // 4. macOS ICNS (.app бандл и .dmg)
    if (!writeMacIcns(outDir.filePath("deepdive.icns"))) {
        return 5;
    }

    // 5. Баннеры NSIS (Welcome/Finish 164x314 и Header 150x57)
    if (!writeBmp24(outDir.filePath("nsis_welcome.bmp"), AppIcon::renderNsisWelcomeBanner())) {
        return 6;
    }
    if (!writeBmp24(outDir.filePath("nsis_header.bmp"), AppIcon::renderNsisHeaderBanner())) {
        return 7;
    }

    return 0;
}
