#pragma once

#ifdef SPEEDYNOTE_HAS_PADDLE_OCR

// ============================================================================
// PaddleOcrEngine - Linux raster OCR backend (PaddleOCR via ONNX Runtime)
// ============================================================================
// Part of OCR Phase 4B. Subclass of RasterOcrEngine (Phase 4A): the shared base
// owns the stroke buffer, line grouping, line-signature cache, transform
// inversion, and Latin-word / CJK-glyph WordSegment assembly. This class
// implements only the one platform-specific bridge -- recognizeImage() -- by
// running a PP-OCRv5 mobile *recognition* model (detection skipped; the grouper
// already segments lines/cells, QA Q5.2) through ONNX Runtime (CPU EP only,
// QA Q11.3), CTC-decoding the logits to text plus approximate per-character X
// boxes (good X, full-height Y, QA Q11.2).
//
// The recognition models are the RapidAI/RapidOCR pre-converted ONNX exports,
// which embed their character dictionary in the ONNX metadata (key
// "character"), so no separate dict files are needed (see fetch-ocr-models.sh).
//
// ONNX Runtime types are kept out of this header via a PIMPL so the rest of the
// build need not see <onnxruntime_cxx_api.h>.
// ============================================================================

#include "RasterOcrEngine.h"

#include <QString>
#include <QStringList>

#include <map>
#include <memory>

class PaddleOcrEngine : public RasterOcrEngine {
public:
    PaddleOcrEngine();
    ~PaddleOcrEngine() override;

    PaddleOcrEngine(const PaddleOcrEngine&) = delete;
    PaddleOcrEngine& operator=(const PaddleOcrEngine&) = delete;

    QString engineId() const override { return QStringLiteral("paddle_ocr"); }
    bool isAvailable() const override;
    QStringList availableLanguages() const override;

protected:
    ImageRecognition recognizeImage(const QImage& strip,
                                    const QString& languageTag) override;

    /// PP-OCRv5 mobile recognition models expect ~48 px input height.
    int targetStripHeightPx() const override { return 48; }

private:
    struct Model;  ///< Ort::Session + decoded char table + IO names (defined in .cpp)
    struct Impl;   ///< shared Ort::Env (defined in .cpp)

    /// Resolve + lazily load (and cache) the recognition model for a language
    /// tag. Returns nullptr if the model file cannot be found or loaded.
    Model* modelForLanguage(const QString& languageTag);

    /// Absolute path to the bundled models directory, or empty if none found.
    static QString modelsDir();
    /// Map a BCP-47-ish language tag to a bundled model file name.
    static QString modelFileForLanguage(const QString& languageTag);

    std::unique_ptr<Impl> m_impl;                        ///< shared Ort::Env
    std::map<QString, std::unique_ptr<Model>> m_models;  ///< key = model file name
                                                         ///< (std::map: move-only values OK)
};

#endif // SPEEDYNOTE_HAS_PADDLE_OCR
