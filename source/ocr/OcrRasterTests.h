#pragma once

// ============================================================================
// OcrRasterTests - Unit tests for the Phase 4A raster OCR pipeline
// ============================================================================
// Header-only, runnable on any desktop OS before a real backend exists. Uses a
// StubRasterOcrEngine whose recognizeImage() returns a fixed string with
// evenly-spaced synthetic per-character boxes spanning the strip, so the whole
// pipeline (grouping, rasterization, line-signature cache, transform inverse,
// segment assembly, serialization) is exercised deterministically.
//
// Run with:  speedynote --test-ocr-raster   (debug, non-mobile builds only)
// ============================================================================

#include "engines/RasterOcrEngine.h"
#include "OcrStrokeRasterizer.h"
#include "OcrTextBlock.h"
#include "../strokes/VectorStroke.h"

#include <QDebug>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <cmath>

namespace OcrRasterTests {

// ----------------------------------------------------------------------------
// Stub backend: deterministic recognizer, counts calls for cache testing.
// ----------------------------------------------------------------------------
class StubRasterOcrEngine : public RasterOcrEngine {
public:
    QString text = QStringLiteral("ab"); ///< text returned per strip
    bool emitCharBoxes = true;           ///< when false, return text only
    int recognizeCalls = 0;              ///< number of recognizeImage() calls

    QString engineId() const override { return QStringLiteral("stub_raster"); }
    bool isAvailable() const override { return true; }
    QStringList availableLanguages() const override { return {QStringLiteral("en-US")}; }

protected:
    ImageRecognition recognizeImage(const QImage& strip, const QString& /*lang*/) override
    {
        ++recognizeCalls;
        ImageRecognition rec;
        rec.text = text;
        if (emitCharBoxes && !text.isEmpty()) {
            const int n = text.length();
            const qreal w = static_cast<qreal>(strip.width()) / n;
            for (int i = 0; i < n; ++i)
                rec.charBoxesImage.append(QRectF(i * w, 0, w, strip.height()));
        }
        return rec;
    }
};

inline bool nearlyEqual(qreal a, qreal b, qreal eps = 0.5)
{
    return std::abs(a - b) <= eps;
}

inline VectorStroke makeLineStroke(const QString& id, qreal x0, qreal y0,
                                   qreal x1, qreal y1)
{
    VectorStroke s;
    s.id = id;
    s.baseThickness = 3.0;
    s.points.append({QPointF(x0, y0), 0.5});
    s.points.append({QPointF((x0 + x1) / 2.0, (y0 + y1) / 2.0), 0.5});
    s.points.append({QPointF(x1, y1), 0.5});
    s.updateBoundingBox();
    return s;
}

// ----------------------------------------------------------------------------
// Test: line grouping yields one result per visually separated line.
// ----------------------------------------------------------------------------
inline bool testGrouping()
{
    qDebug() << "=== Test: Grouping ===";
    StubRasterOcrEngine engine;
    engine.text = QStringLiteral("x");

    engine.addStrokes({
        makeLineStroke(QStringLiteral("l1"), 10, 10, 110, 12),
        makeLineStroke(QStringLiteral("l2"), 10, 200, 110, 202),
    });

    const auto results = engine.analyze();
    const bool ok = results.size() == 2;
    qDebug() << (ok ? "PASS" : "FAIL") << "- expected 2 line results, got" << results.size();
    return ok;
}

// ----------------------------------------------------------------------------
// Test: rasterizer normalization (golden-ish strip properties).
// ----------------------------------------------------------------------------
inline bool testRenderNormalization()
{
    qDebug() << "=== Test: Render Normalization ===";
    QVector<VectorStroke> strokes{makeLineStroke(QStringLiteral("s"), 0, 0, 100, 20)};
    const int target = 48;
    const RasterStrip strip = rasterizeStrokes(strokes, {0}, target);

    bool ok = !strip.image.isNull();
    ok = ok && strip.image.format() == QImage::Format_Grayscale8;

    const int padding = strip.transform.padding;
    ok = ok && strip.image.height() == target + 2 * padding;

    // Some ink must have been drawn (a pixel darker than mid-gray).
    bool foundInk = false;
    for (int y = 0; y < strip.image.height() && !foundInk; ++y)
        for (int x = 0; x < strip.image.width(); ++x)
            if (qGray(strip.image.pixel(x, y)) < 128) { foundInk = true; break; }
    ok = ok && foundInk;

    qDebug() << (ok ? "PASS" : "FAIL") << "- size" << strip.image.size()
             << "format ok / ink found:" << foundInk;
    return ok;
}

// ----------------------------------------------------------------------------
// Test: image->canvas transform round-trips a known point and rect.
// ----------------------------------------------------------------------------
inline bool testTransformRoundTrip()
{
    qDebug() << "=== Test: Transform Round-Trip ===";
    QVector<VectorStroke> strokes{makeLineStroke(QStringLiteral("s"), 30, 40, 230, 80)};
    const RasterStrip strip = rasterizeStrokes(strokes, {0}, 48);
    const RasterTransform& xf = strip.transform;

    // Forward-map a canvas point, then invert it.
    const QPointF canvasPt(130, 60);
    const QPointF imgPt((canvasPt.x() - xf.originPage.x()) * xf.scale + xf.padding,
                        (canvasPt.y() - xf.originPage.y()) * xf.scale + xf.padding);
    const QPointF back = xf.imageToCanvas(imgPt);

    bool ok = nearlyEqual(back.x(), canvasPt.x()) && nearlyEqual(back.y(), canvasPt.y());

    const QRectF imgRect(xf.padding, xf.padding, 50.0 * xf.scale, 20.0 * xf.scale);
    const QRectF canvasRect = xf.imageToCanvas(imgRect);
    ok = ok && nearlyEqual(canvasRect.x(), xf.originPage.x())
            && nearlyEqual(canvasRect.width(), 50.0)
            && nearlyEqual(canvasRect.height(), 20.0);

    qDebug() << (ok ? "PASS" : "FAIL") << "- point back" << back << "rect" << canvasRect;
    return ok;
}

// ----------------------------------------------------------------------------
// Test: line-signature cache hit on no-op, miss on edit, evict on empty.
// ----------------------------------------------------------------------------
inline bool testCacheHitEvict()
{
    qDebug() << "=== Test: Cache Hit / Evict ===";
    StubRasterOcrEngine engine;
    engine.text = QStringLiteral("hi");

    engine.addStrokes({makeLineStroke(QStringLiteral("a"), 10, 10, 110, 12)});
    engine.analyze();
    bool ok = engine.recognizeCalls == 1;

    engine.analyze(); // no change -> cache hit
    ok = ok && engine.recognizeCalls == 1;

    // Add a second stroke to the same line -> group signature changes.
    engine.addStrokes({makeLineStroke(QStringLiteral("b"), 120, 10, 220, 12)});
    engine.analyze();
    ok = ok && engine.recognizeCalls == 2;

    engine.analyze(); // no change -> cache hit again
    ok = ok && engine.recognizeCalls == 2;

    // Remove everything -> empty result, cache cleared.
    engine.removeStrokes({QStringLiteral("a"), QStringLiteral("b")});
    const auto empty = engine.analyze();
    ok = ok && empty.isEmpty();

    // Re-add the original single stroke -> must recompute (cache was cleared).
    engine.addStrokes({makeLineStroke(QStringLiteral("a"), 10, 10, 110, 12)});
    engine.analyze();
    ok = ok && engine.recognizeCalls == 3;

    qDebug() << (ok ? "PASS" : "FAIL") << "- recognizeCalls" << engine.recognizeCalls;
    return ok;
}

// ----------------------------------------------------------------------------
// Test: Latin words split on space; CJK emits one segment per glyph.
// ----------------------------------------------------------------------------
inline bool testSegmentAssembly()
{
    qDebug() << "=== Test: Segment Assembly ===";
    bool ok = true;

    // Latin: "hi there" -> 2 words with char boxes.
    {
        StubRasterOcrEngine engine;
        engine.text = QStringLiteral("hi there");
        engine.addStrokes({makeLineStroke(QStringLiteral("a"), 10, 10, 200, 12)});
        const auto results = engine.analyze();
        ok = ok && results.size() == 1;
        if (ok) {
            const auto& segs = results[0].wordSegments;
            ok = ok && segs.size() == 2;
            if (ok) {
                ok = ok && segs[0].text == QStringLiteral("hi")
                        && segs[0].charBoundingBoxes.size() == 2;
                ok = ok && segs[1].text == QStringLiteral("there")
                        && segs[1].charBoundingBoxes.size() == 5;
            }
        }
        qDebug() << "  Latin words:" << (ok ? "ok" : "FAIL");
    }

    // CJK: "你好" -> 2 single-glyph segments.
    {
        StubRasterOcrEngine engine;
        engine.text = QString::fromUtf8("\xE4\xBD\xA0\xE5\xA5\xBD"); // 你好
        engine.addStrokes({makeLineStroke(QStringLiteral("a"), 10, 10, 120, 12)});
        const auto results = engine.analyze();
        bool cjkOk = results.size() == 1;
        if (cjkOk) {
            const auto& segs = results[0].wordSegments;
            cjkOk = segs.size() == 2
                 && segs[0].text.length() == 1 && segs[0].charBoundingBoxes.size() == 1
                 && segs[1].text.length() == 1 && segs[1].charBoundingBoxes.size() == 1;
        }
        qDebug() << "  CJK glyphs:" << (cjkOk ? "ok" : "FAIL");
        ok = ok && cjkOk;
    }

    // Fallback: no char boxes -> single line-level segment.
    {
        StubRasterOcrEngine engine;
        engine.text = QStringLiteral("fallback");
        engine.emitCharBoxes = false;
        engine.addStrokes({makeLineStroke(QStringLiteral("a"), 10, 10, 120, 12)});
        const auto results = engine.analyze();
        bool fbOk = results.size() == 1
                 && results[0].wordSegments.size() == 1
                 && results[0].wordSegments[0].charBoundingBoxes.isEmpty()
                 && results[0].wordSegments[0].text == QStringLiteral("fallback");
        qDebug() << "  Fallback segment:" << (fbOk ? "ok" : "FAIL");
        ok = ok && fbOk;
    }

    qDebug() << (ok ? "PASS" : "FAIL") << "- segment assembly";
    return ok;
}

// ----------------------------------------------------------------------------
// Test: charBoundingBoxes survive OcrTextBlock JSON round-trip.
// ----------------------------------------------------------------------------
inline bool testCharBoxJsonRoundTrip()
{
    qDebug() << "=== Test: charBoundingBoxes JSON Round-Trip ===";
    OcrTextBlock block = OcrTextBlock::create();
    block.text = QStringLiteral("hi");
    block.boundingRect = QRectF(0, 0, 50, 20);
    block.engineId = QStringLiteral("stub_raster");

    OcrTextBlock::WordSegment seg;
    seg.text = QStringLiteral("hi");
    seg.boundingRect = QRectF(0, 0, 50, 20);
    seg.charBoundingBoxes = {QRectF(0, 0, 25, 20), QRectF(25, 0, 25, 20)};
    block.wordSegments.append(seg);

    // A second segment with no char boxes (must stay empty after round-trip).
    OcrTextBlock::WordSegment seg2;
    seg2.text = QStringLiteral("yo");
    seg2.boundingRect = QRectF(60, 0, 30, 20);
    block.wordSegments.append(seg2);

    const OcrTextBlock restored = OcrTextBlock::fromJson(block.toJson());

    bool ok = restored.wordSegments.size() == 2;
    if (ok) {
        const auto& r0 = restored.wordSegments[0];
        ok = ok && r0.charBoundingBoxes.size() == 2
                && nearlyEqual(r0.charBoundingBoxes[0].width(), 25.0, 0.01)
                && nearlyEqual(r0.charBoundingBoxes[1].x(), 25.0, 0.01);
        ok = ok && restored.wordSegments[1].charBoundingBoxes.isEmpty();
    }

    qDebug() << (ok ? "PASS" : "FAIL") << "- char-box serialization";
    return ok;
}

inline bool runAllTests()
{
    qDebug() << "\n========================================";
    qDebug() << "Running OCR Raster Pipeline Tests (Phase 4A)";
    qDebug() << "========================================\n";

    bool allPass = true;
    allPass &= testGrouping();
    allPass &= testRenderNormalization();
    allPass &= testTransformRoundTrip();
    allPass &= testCacheHitEvict();
    allPass &= testSegmentAssembly();
    allPass &= testCharBoxJsonRoundTrip();

    qDebug() << "\n========================================";
    qDebug() << (allPass ? "ALL TESTS PASSED!" : "SOME TESTS FAILED!");
    qDebug() << "========================================\n";
    return allPass;
}

} // namespace OcrRasterTests
