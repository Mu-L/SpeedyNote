#include "PaddleOcrEngine.h"

#ifdef SPEEDYNOTE_HAS_PADDLE_OCR

// ============================================================================
// PaddleOcrEngine (Linux) - PP-OCRv5 recognition via ONNX Runtime (CPU EP).
// ============================================================================
// Implements the single RasterOcrEngine bridge, recognizeImage():
//   normalized strip -> resize/normalize/CHW tensor -> Ort::Session::Run ->
//   greedy CTC decode -> text + approximate per-character X boxes.
//
// The character dictionary is read from the ONNX model metadata (key
// "character"; RapidOCR convention), then the PaddleOCR CTC label table is
// built as: index 0 = "blank" (ignored), 1..N = dict chars, N+1 = " ".
// Greedy decode collapses consecutive duplicates and drops blanks (matching
// PaddleOCR / RapidOCR CTCLabelDecode).
// ============================================================================

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStringList>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// PIMPL: a single shared Ort::Env for the engine.
// ----------------------------------------------------------------------------
struct PaddleOcrEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "speedynote-paddle-ocr"};
};

// ----------------------------------------------------------------------------
// One loaded recognition model: session + IO names + decoded CTC char table.
// ----------------------------------------------------------------------------
struct PaddleOcrEngine::Model {
    Ort::Session session{nullptr};
    std::string inputName;
    std::string outputName;
    int recHeight = 48;            ///< fixed model input height (px)
    QVector<QString> charTable;    ///< index 0 = blank; size == num classes (ideally)

    explicit Model(Ort::Session&& s) : session(std::move(s)) {}
};

namespace {

constexpr int kMinStripWidth = 16;
constexpr int kMaxStripWidth = 4096;

// Build the PaddleOCR CTC label table from a model's "character" metadata.
// Returns {} when the metadata is absent. Layout: ["blank"] + dict + [" "].
QVector<QString> buildCharTable(Ort::Session& session)
{
    QVector<QString> table;
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::ModelMetadata md = session.GetModelMetadata();
    Ort::AllocatedStringPtr value =
        md.LookupCustomMetadataMapAllocated("character", allocator);
    if (!value)
        return table; // no embedded dict

    QString dict = QString::fromUtf8(value.get());
    // Match Python str.splitlines() semantics (which RapidOCR/PaddleOCR rely on
    // when building the label table): a terminating newline must NOT yield a
    // trailing empty entry. These dicts are stored newline-terminated, so a
    // naive QString::split('\n') would add one spurious "" element -- shifting
    // every class after it by one, which silently steals the CTC *space* token
    // (the appended " " below) and makes Latin words run together. Strip one
    // trailing line terminator ("\n" or "\r\n") first.
    if (dict.endsWith(QLatin1Char('\n')))
        dict.chop(1);
    if (dict.endsWith(QLatin1Char('\r')))
        dict.chop(1);
    const QStringList lines = dict.split(QLatin1Char('\n'));

    table.reserve(lines.size() + 2);
    table.append(QString());            // index 0 = blank (never emitted)
    for (const QString& raw : lines) {
        QString ch = raw;
        if (ch.endsWith(QLatin1Char('\r')))
            ch.chop(1);
        table.append(ch);
    }
    table.append(QStringLiteral(" "));  // PaddleOCR appends the space token
    return table;
}

} // namespace

PaddleOcrEngine::PaddleOcrEngine()
    : m_impl(std::make_unique<Impl>())
{
}

PaddleOcrEngine::~PaddleOcrEngine() = default;

QString PaddleOcrEngine::modelsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    candidates << appDir + QStringLiteral("/ocr-models");
    candidates << appDir + QStringLiteral("/../share/speedynote/ocr-models");
    candidates << QStringLiteral("/usr/share/speedynote/ocr-models");
    candidates << QStringLiteral("/usr/local/share/speedynote/ocr-models");
    // Dev tree: linux/ocr-models relative to the build output.
    candidates << appDir + QStringLiteral("/../linux/ocr-models");
    candidates << appDir + QStringLiteral("/../../linux/ocr-models");

    for (const QString& dir : candidates) {
        if (QFileInfo::exists(dir + QStringLiteral("/latin_rec.onnx")))
            return QDir(dir).absolutePath();
    }
    return QString();
}

QString PaddleOcrEngine::modelFileForLanguage(const QString& languageTag)
{
    const QString tag = languageTag.toLower();
    if (tag.startsWith(QStringLiteral("zh")) || tag.startsWith(QStringLiteral("ja")))
        return QStringLiteral("ch_rec.onnx");   // v5 ch model is multilingual (zh/en/ja/cht)
    if (tag.startsWith(QStringLiteral("ko")))
        return QStringLiteral("korean_rec.onnx");
    return QStringLiteral("latin_rec.onnx");    // default (en-US + Latin scripts)
}

bool PaddleOcrEngine::isAvailable() const
{
    // The vendored ONNX Runtime is link-time guaranteed when this TU is built;
    // availability hinges on the mandatory default (latin) model existing.
    return !modelsDir().isEmpty();
}

QStringList PaddleOcrEngine::availableLanguages() const
{
    const QString dir = modelsDir();
    if (dir.isEmpty())
        return {};

    QStringList langs;
    if (QFileInfo::exists(dir + QStringLiteral("/latin_rec.onnx")))
        langs << QStringLiteral("en-US");
    if (QFileInfo::exists(dir + QStringLiteral("/ch_rec.onnx")))
        langs << QStringLiteral("zh-CN") << QStringLiteral("ja-JP");
    if (QFileInfo::exists(dir + QStringLiteral("/korean_rec.onnx")))
        langs << QStringLiteral("ko-KR");
    return langs;
}

PaddleOcrEngine::Model* PaddleOcrEngine::modelForLanguage(const QString& languageTag)
{
    const QString dir = modelsDir();
    if (dir.isEmpty())
        return nullptr;

    QString file = modelFileForLanguage(languageTag);
    if (!QFileInfo::exists(dir + QLatin1Char('/') + file))
        file = QStringLiteral("latin_rec.onnx"); // fall back to the default
    const QString path = dir + QLatin1Char('/') + file;
    if (!QFileInfo::exists(path))
        return nullptr;

    auto it = m_models.find(file);
    if (it != m_models.end())
        return it->second.get();

    try {
        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        opts.SetIntraOpNumThreads(1); // tiny single-line model; avoid oversubscription

        const std::string pathStd = path.toStdString();
        Ort::Session session(m_impl->env, pathStd.c_str(), opts);

        auto model = std::make_unique<Model>(std::move(session));

        Ort::AllocatorWithDefaultOptions allocator;
        Ort::AllocatedStringPtr inName  = model->session.GetInputNameAllocated(0, allocator);
        Ort::AllocatedStringPtr outName = model->session.GetOutputNameAllocated(0, allocator);
        model->inputName  = inName.get();
        model->outputName = outName.get();

        // Fixed input height if the model declares one (NCHW: dims[2]).
        const std::vector<int64_t> inShape =
            model->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (inShape.size() == 4 && inShape[2] > 0)
            model->recHeight = static_cast<int>(inShape[2]);

        model->charTable = buildCharTable(model->session);
        if (model->charTable.isEmpty())
            return nullptr; // can't decode without a dictionary

        Model* raw = model.get();
        m_models[file] = std::move(model);
        return raw;
    } catch (const Ort::Exception&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

RasterOcrEngine::ImageRecognition
PaddleOcrEngine::recognizeImage(const QImage& strip, const QString& languageTag)
{
    ImageRecognition out;
    if (strip.isNull())
        return out;

    Model* model = modelForLanguage(languageTag);
    if (!model)
        return out;

    // --- 1. Preprocess: Grayscale8 strip -> resized RGB CHW float tensor. ---
    const QImage gray = strip.convertToFormat(QImage::Format_Grayscale8);
    const int stripW = gray.width();
    const int stripH = gray.height();
    if (stripW <= 0 || stripH <= 0)
        return out;

    const int H = model->recHeight;
    int W = static_cast<int>(std::lround(static_cast<double>(H) * stripW / stripH));
    W = std::clamp(W, kMinStripWidth, kMaxStripWidth);

    const QImage resized =
        gray.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_Grayscale8);

    // CHW, 3 channels (grayscale replicated), PP-OCR normalize (x/255-0.5)/0.5.
    std::vector<float> input(static_cast<size_t>(3) * H * W);
    for (int y = 0; y < H; ++y) {
        const uchar* row = resized.constScanLine(y);
        for (int x = 0; x < W; ++x) {
            const float v = (static_cast<float>(row[x]) / 255.0f - 0.5f) / 0.5f;
            const size_t idx = static_cast<size_t>(y) * W + x;
            input[idx] = v;                                  // R
            input[static_cast<size_t>(H) * W + idx] = v;     // G
            input[static_cast<size_t>(2) * H * W + idx] = v; // B
        }
    }

    // --- 2. Run inference. --------------------------------------------------
    std::vector<float> logits;
    int T = 0, C = 0;
    try {
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> shape{1, 3, H, W};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inNames[]  = {model->inputName.c_str()};
        const char* outNames[] = {model->outputName.c_str()};
        auto outputs = model->session.Run(Ort::RunOptions{nullptr},
                                          inNames, &inputTensor, 1, outNames, 1);
        if (outputs.empty())
            return out;

        const std::vector<int64_t> oShape =
            outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (oShape.size() != 3)
            return out; // expect [1, T, C]
        T = static_cast<int>(oShape[1]);
        C = static_cast<int>(oShape[2]);
        if (T <= 0 || C <= 0)
            return out;

        const float* data = outputs[0].GetTensorData<float>();
        logits.assign(data, data + static_cast<size_t>(T) * C);
    } catch (const Ort::Exception&) {
        return out;
    } catch (...) {
        return out;
    }

    // --- 3. Greedy CTC decode + per-char column. ----------------------------
    // Keep a token iff (it differs from the previous raw token) AND (not blank,
    // index 0) -- identical to PaddleOCR/RapidOCR CTCLabelDecode.
    struct Emit { QString ch; int col; };
    std::vector<Emit> emits;
    emits.reserve(T);

    int prevRaw = -1;
    for (int t = 0; t < T; ++t) {
        const float* p = logits.data() + static_cast<size_t>(t) * C;
        int best = 0;
        float bestVal = p[0];
        for (int c = 1; c < C; ++c) {
            if (p[c] > bestVal) { bestVal = p[c]; best = c; }
        }
        if (best != prevRaw && best != 0) {
            const QString ch = (best < model->charTable.size())
                                   ? model->charTable[best]
                                   : QString();
            if (!ch.isEmpty())
                emits.push_back({ch, t});
        }
        prevRaw = best;
    }

    if (emits.empty())
        return out;

    // --- 4. Text + approximate per-char boxes in received-strip pixels. -----
    // CTC gives a meaningful column (good X); Y is weak so each box spans the
    // full strip height (QA Q11.2). Box edges are midpoints between adjacent
    // character centers, mapped from feature columns to strip width.
    QString text;
    QVector<QRectF> boxes;
    bool charBoxesValid = true;

    const int n = static_cast<int>(emits.size());
    std::vector<double> centers(n);
    for (int i = 0; i < n; ++i)
        centers[i] = (emits[i].col + 0.5) / static_cast<double>(T) * stripW;

    for (int i = 0; i < n; ++i) {
        const double left  = (i == 0)      ? 0.0    : (centers[i - 1] + centers[i]) / 2.0;
        const double right = (i == n - 1)  ? stripW : (centers[i] + centers[i + 1]) / 2.0;
        const QRectF box(left, 0.0, std::max(0.0, right - left), stripH);

        const QString& ch = emits[i].ch;
        text += ch;
        for (int k = 0; k < ch.length(); ++k)
            boxes.append(box);
        if (ch.length() != 1)
            charBoxesValid = false; // multi-codepoint token -> degrade to word rects
    }

    out.text = text;
    if (charBoxesValid && boxes.size() == text.length())
        out.charBoxesImage = boxes; // else base falls back to the word rect
    return out;
}

#endif // SPEEDYNOTE_HAS_PADDLE_OCR
