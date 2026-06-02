#include "VisionOcrEngine.h"

#if defined(SPEEDYNOTE_HAS_VISION_OCR)

#import <Vision/Vision.h>
#import <CoreGraphics/CoreGraphics.h>

#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

// ---------------------------------------------------------------------------
// isAvailable
// ---------------------------------------------------------------------------
// macOS 12+ (the deployment target) always ships Vision text recognition; the
// runtime class check is a cheap safety net for unusual environments.
bool VisionOcrEngine::isAvailable() const
{
    return NSClassFromString(@"VNRecognizeTextRequest") != nil;
}

// ---------------------------------------------------------------------------
// availableLanguages
// ---------------------------------------------------------------------------
QStringList VisionOcrEngine::availableLanguages() const
{
    if (!m_cachedLanguages.isEmpty())
        return m_cachedLanguages;

    @autoreleasepool {
        VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
        req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        NSError *err = nil;
        NSArray<NSString *> *langs = [req supportedRecognitionLanguagesAndReturnError:&err];
        if (err) {
            NSLog(@"VisionOcrEngine: supportedRecognitionLanguages error: %@",
                  err.localizedDescription);
        }
        for (NSString *l in langs)
            m_cachedLanguages.append(QString::fromNSString(l));
    }
    return m_cachedLanguages;
}

// ---------------------------------------------------------------------------
// recognizeImage
// ---------------------------------------------------------------------------
RasterOcrEngine::ImageRecognition
VisionOcrEngine::recognizeImage(const QImage& strip, const QString& languageTag)
{
    ImageRecognition out;
    if (strip.isNull())
        return out;

    @autoreleasepool {
        // 1. Normalized strip (dark-on-white) -> CGImage. Expand to RGBA8888
        //    (Vision is happiest with a standard 32-bit RGB image). The
        //    convertToFormat copy is kept alive until performRequests: returns
        //    (synchronous), so the data provider never dangles.
        const QImage img = strip.convertToFormat(QImage::Format_RGBA8888);
        const int W = img.width();
        const int H = img.height();
        if (W <= 0 || H <= 0)
            return out;

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, img.bits(), static_cast<size_t>(img.sizeInBytes()), nullptr);
        CGImageRef cg = CGImageCreate(
            static_cast<size_t>(W), static_cast<size_t>(H), 8, 32,
            static_cast<size_t>(img.bytesPerLine()), cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault,
            provider, nullptr, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(cs);
        if (!cg)
            return out;

        // 2. Configure the request (QA Q6.1).
        VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
        req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        req.usesLanguageCorrection = YES;
        if (!languageTag.isEmpty())
            req.recognitionLanguages = @[ languageTag.toNSString() ];

        VNImageRequestHandler *handler =
            [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
        NSError *err = nil;
        const BOOL ok = [handler performRequests:@[ req ] error:&err];
        CGImageRelease(cg);

        if (!ok || err || req.results.count == 0) {
            if (err)
                NSLog(@"VisionOcrEngine: performRequests error: %@", err.localizedDescription);
            return out;
        }

        // 3. Walk observations left-to-right, accumulating one box per character
        //    (including a synthetic box for the space we insert between separate
        //    observations) so that charBoxesImage.size() == text.length() always
        //    holds. Vision boxes are normalized with a bottom-left origin, so we
        //    flip Y and denormalize to strip-image pixels (QA Q3.1/Q3.4).
        NSArray<VNRecognizedTextObservation *> *obs = [req.results
            sortedArrayUsingComparator:^NSComparisonResult(VNRecognizedTextObservation *a,
                                                           VNRecognizedTextObservation *b) {
                const CGFloat ax = a.boundingBox.origin.x;
                const CGFloat bx = b.boundingBox.origin.x;
                if (ax < bx) return NSOrderedAscending;
                if (ax > bx) return NSOrderedDescending;
                return NSOrderedSame;
            }];

        QString text;
        QVector<QRectF> boxes;

        for (VNRecognizedTextObservation *o in obs) {
            VNRecognizedText *cand = [[o topCandidates:1] firstObject];
            if (!cand)
                continue;
            NSString *s = cand.string;
            if (s.length == 0)
                continue;

            if (!text.isEmpty()) {
                text.append(QLatin1Char(' '));
                boxes.append(QRectF()); // null box keeps the size invariant
            }

            for (NSUInteger i = 0; i < s.length; ++i) {
                const NSRange range = NSMakeRange(i, 1);

                NSError *rangeErr = nil;
                VNRectangleObservation *r = [cand boundingBoxForRange:range error:&rangeErr];
                QRectF px;
                if (r) {
                    const CGRect b = r.boundingBox;
                    px = QRectF(b.origin.x * W,
                                (1.0 - b.origin.y - b.size.height) * H,
                                b.size.width * W,
                                b.size.height * H);
                }

                text.append(QString::fromNSString([s substringWithRange:range]));
                boxes.append(px);
            }
        }

        out.text = text;
        if (!text.isEmpty() && boxes.size() == text.length())
            out.charBoxesImage = boxes;
    }

    return out;
}

#endif // SPEEDYNOTE_HAS_VISION_OCR
