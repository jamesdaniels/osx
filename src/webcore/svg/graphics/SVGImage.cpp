/*
 * Copyright (C) 2006 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008-2009, 2015-2016 Apple Inc. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "SVGImage.h"

#include "Chrome.h"
#include "CommonVM.h"
#include "DOMWindow.h"
#include "DocumentLoader.h"
#include "EditorClient.h"
#include "ElementIterator.h"
#include "FrameLoader.h"
#include "FrameView.h"
#include "ImageBuffer.h"
#include "ImageObserver.h"
#include "IntRect.h"
#include "JSDOMWindowBase.h"
#include "LibWebRTCProvider.h"
#include "MainFrame.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "RenderSVGRoot.h"
#include "RenderStyle.h"
#include "SVGDocument.h"
#include "SVGFEImageElement.h"
#include "SVGForeignObjectElement.h"
#include "SVGImageClients.h"
#include "SVGImageElement.h"
#include "SVGSVGElement.h"
#include "Settings.h"
#include "SocketProvider.h"
#include "TextStream.h"
#include <runtime/JSCInlines.h>
#include <runtime/JSLock.h>

#if USE(DIRECT2D)
#include "COMPtr.h"
#include <d2d1.h>
#endif

namespace WebCore {

SVGImage::SVGImage(ImageObserver& observer, const URL& url)
    : Image(&observer)
    , m_url(url)
{
}

SVGImage::~SVGImage()
{
    if (m_page) {
        // Store m_page in a local variable, clearing m_page, so that SVGImageChromeClient knows we're destructed.
        std::unique_ptr<Page> currentPage = WTFMove(m_page);
        currentPage->mainFrame().loader().frameDetached(); // Break both the loader and view references to the frame
    }

    // Verify that page teardown destroyed the Chrome
    ASSERT(!m_chromeClient || !m_chromeClient->image());
}

inline SVGSVGElement* SVGImage::rootElement() const
{
    if (!m_page)
        return nullptr;
    return SVGDocument::rootElement(*m_page->mainFrame().document());
}

bool SVGImage::hasSingleSecurityOrigin() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return true;

    // FIXME: Once foreignObject elements within SVG images are updated to not leak cross-origin data
    // (e.g., visited links, spellcheck) we can remove the SVGForeignObjectElement check here and
    // research if we can remove the Image::hasSingleSecurityOrigin mechanism entirely.
    for (auto& element : descendantsOfType<SVGElement>(*rootElement)) {
        if (is<SVGForeignObjectElement>(element))
            return false;
        if (is<SVGImageElement>(element)) {
            if (!downcast<SVGImageElement>(element).hasSingleSecurityOrigin())
                return false;
        } else if (is<SVGFEImageElement>(element)) {
            if (!downcast<SVGFEImageElement>(element).hasSingleSecurityOrigin())
                return false;
        }
    }

    // Because SVG image rendering disallows external resources and links,
    // these images effectively are restricted to a single security origin.
    return true;
}

void SVGImage::setContainerSize(const FloatSize& size)
{
    if (!usesContainerSize())
        return;

    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return;
    auto* renderer = downcast<RenderSVGRoot>(rootElement->renderer());
    if (!renderer)
        return;

    FrameView* view = frameView();
    view->resize(this->containerSize());

    renderer->setContainerSize(IntSize(size));
}

IntSize SVGImage::containerSize() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return IntSize();

    auto* renderer = downcast<RenderSVGRoot>(rootElement->renderer());
    if (!renderer)
        return IntSize();

    // If a container size is available it has precedence.
    IntSize containerSize = renderer->containerSize();
    if (!containerSize.isEmpty())
        return containerSize;

    // Assure that a container size is always given for a non-identity zoom level.
    ASSERT(renderer->style().effectiveZoom() == 1);

    FloatSize currentSize;
    if (rootElement->hasIntrinsicWidth() && rootElement->hasIntrinsicHeight())
        currentSize = rootElement->currentViewportSize();
    else
        currentSize = rootElement->currentViewBoxRect().size();

    if (!currentSize.isEmpty())
        return IntSize(static_cast<int>(ceilf(currentSize.width())), static_cast<int>(ceilf(currentSize.height())));

    // As last resort, use CSS default intrinsic size.
    return IntSize(300, 150);
}

ImageDrawResult SVGImage::drawForContainer(GraphicsContext& context, const FloatSize containerSize, float zoom, const FloatRect& dstRect,
    const FloatRect& srcRect, CompositeOperator compositeOp, BlendMode blendMode)
{
    if (!m_page)
        return ImageDrawResult::DidNothing;

    ImageObserver* observer = imageObserver();
    ASSERT(observer);

    // Temporarily reset image observer, we don't want to receive any changeInRect() calls due to this relayout.
    setImageObserver(nullptr);

    IntSize roundedContainerSize = roundedIntSize(containerSize);
    setContainerSize(roundedContainerSize);

    FloatRect scaledSrc = srcRect;
    scaledSrc.scale(1 / zoom);

    // Compensate for the container size rounding by adjusting the source rect.
    FloatSize adjustedSrcSize = scaledSrc.size();
    adjustedSrcSize.scale(roundedContainerSize.width() / containerSize.width(), roundedContainerSize.height() / containerSize.height());
    scaledSrc.setSize(adjustedSrcSize);

    ImageDrawResult result = draw(context, dstRect, scaledSrc, compositeOp, blendMode, DecodingMode::Synchronous, ImageOrientationDescription());

    setImageObserver(observer);
    return result;
}

#if USE(CAIRO)
// Passes ownership of the native image to the caller so NativeImagePtr needs
// to be a smart pointer type.
NativeImagePtr SVGImage::nativeImageForCurrentFrame(const GraphicsContext*)
{
    if (!m_page)
        return nullptr;

    // Cairo does not use the accelerated drawing flag, so it's OK to make an unconditionally unaccelerated buffer.
    std::unique_ptr<ImageBuffer> buffer = ImageBuffer::create(size(), Unaccelerated);
    if (!buffer) // failed to allocate image
        return nullptr;

    draw(buffer->context(), rect(), rect(), CompositeSourceOver, BlendModeNormal, DecodingMode::Synchronous, ImageOrientationDescription());

    // FIXME: WK(Bug 113657): We should use DontCopyBackingStore here.
    return buffer->copyImage(CopyBackingStore)->nativeImageForCurrentFrame();
}
#endif

#if USE(DIRECT2D)
NativeImagePtr SVGImage::nativeImage(const GraphicsContext* targetContext)
{
    ASSERT(targetContext);
    if (!m_page || !targetContext)
        return nullptr;

    auto platformContext = targetContext->platformContext();
    ASSERT(platformContext);

    // Draw the SVG into a bitmap.
    COMPtr<ID2D1BitmapRenderTarget> nativeImageTarget;
    HRESULT hr = platformContext->CreateCompatibleRenderTarget(IntSize(rect().size()), &nativeImageTarget);
    ASSERT(SUCCEEDED(hr));

    GraphicsContext localContext(nativeImageTarget.get());

    draw(localContext, rect(), rect(), CompositeSourceOver, BlendModeNormal, DecodingMode::Synchronous, ImageOrientationDescription());

    COMPtr<ID2D1Bitmap> nativeImage;
    hr = nativeImageTarget->GetBitmap(&nativeImage);
    ASSERT(SUCCEEDED(hr));

    return nativeImage;
}
#endif

void SVGImage::drawPatternForContainer(GraphicsContext& context, const FloatSize& containerSize, float zoom, const FloatRect& srcRect,
    const AffineTransform& patternTransform, const FloatPoint& phase, const FloatSize& spacing, CompositeOperator compositeOp, const FloatRect& dstRect, BlendMode blendMode)
{
    FloatRect zoomedContainerRect = FloatRect(FloatPoint(), containerSize);
    zoomedContainerRect.scale(zoom);

    // The ImageBuffer size needs to be scaled to match the final resolution.
    AffineTransform transform = context.getCTM();
    FloatSize imageBufferScale = FloatSize(transform.xScale(), transform.yScale());
    ASSERT(imageBufferScale.width());
    ASSERT(imageBufferScale.height());

    FloatRect imageBufferSize = zoomedContainerRect;
    imageBufferSize.scale(imageBufferScale.width(), imageBufferScale.height());

    std::unique_ptr<ImageBuffer> buffer = ImageBuffer::createCompatibleBuffer(expandedIntSize(imageBufferSize.size()), 1, ColorSpaceSRGB, context);
    if (!buffer) // Failed to allocate buffer.
        return;
    drawForContainer(buffer->context(), containerSize, zoom, imageBufferSize, zoomedContainerRect, CompositeSourceOver, BlendModeNormal);
    if (context.drawLuminanceMask())
        buffer->convertToLuminanceMask();

    RefPtr<Image> image = ImageBuffer::sinkIntoImage(WTFMove(buffer), Unscaled);
    if (!image)
        return;

    // Adjust the source rect and transform due to the image buffer's scaling.
    FloatRect scaledSrcRect = srcRect;
    scaledSrcRect.scale(imageBufferScale.width(), imageBufferScale.height());
    AffineTransform unscaledPatternTransform(patternTransform);
    unscaledPatternTransform.scale(1 / imageBufferScale.width(), 1 / imageBufferScale.height());

    context.setDrawLuminanceMask(false);
    image->drawPattern(context, dstRect, scaledSrcRect, unscaledPatternTransform, phase, spacing, compositeOp, blendMode);
}

ImageDrawResult SVGImage::draw(GraphicsContext& context, const FloatRect& dstRect, const FloatRect& srcRect, CompositeOperator compositeOp, BlendMode blendMode, DecodingMode, ImageOrientationDescription)
{
    if (!m_page)
        return ImageDrawResult::DidNothing;

    FrameView* view = frameView();
    ASSERT(view);

    GraphicsContextStateSaver stateSaver(context);
    context.setCompositeOperation(compositeOp, blendMode);
    context.clip(enclosingIntRect(dstRect));

    float alpha = context.alpha();
    bool compositingRequiresTransparencyLayer = compositeOp != CompositeSourceOver || blendMode != BlendModeNormal || alpha < 1;
    if (compositingRequiresTransparencyLayer) {
        context.beginTransparencyLayer(alpha);
        context.setCompositeOperation(CompositeSourceOver, BlendModeNormal);
    }

    FloatSize scale(dstRect.width() / srcRect.width(), dstRect.height() / srcRect.height());
    
    // We can only draw the entire frame, clipped to the rect we want. So compute where the top left
    // of the image would be if we were drawing without clipping, and translate accordingly.
    FloatSize topLeftOffset(srcRect.location().x() * scale.width(), srcRect.location().y() * scale.height());
    FloatPoint destOffset = dstRect.location() - topLeftOffset;

    context.translate(destOffset.x(), destOffset.y());
    context.scale(scale);

    view->resize(containerSize());

    if (!m_url.isEmpty())
        view->scrollToFragment(m_url);
    
    if (view->needsLayout())
        view->layout();

    view->paint(context, intersection(context.clipBounds(), enclosingIntRect(srcRect)));

    if (compositingRequiresTransparencyLayer)
        context.endTransparencyLayer();

    stateSaver.restore();

    if (imageObserver())
        imageObserver()->didDraw(*this);

    return ImageDrawResult::DidDraw;
}

RenderBox* SVGImage::embeddedContentBox() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return nullptr;
    return downcast<RenderBox>(rootElement->renderer());
}

FrameView* SVGImage::frameView() const
{
    if (!m_page)
        return nullptr;
    return m_page->mainFrame().view();
}

bool SVGImage::hasRelativeWidth() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return false;
    return rootElement->intrinsicWidth().isPercentOrCalculated();
}

bool SVGImage::hasRelativeHeight() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return false;
    return rootElement->intrinsicHeight().isPercentOrCalculated();
}

void SVGImage::computeIntrinsicDimensions(Length& intrinsicWidth, Length& intrinsicHeight, FloatSize& intrinsicRatio)
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return;

    intrinsicWidth = rootElement->intrinsicWidth();
    intrinsicHeight = rootElement->intrinsicHeight();
    if (rootElement->preserveAspectRatio().align() == SVGPreserveAspectRatioValue::SVG_PRESERVEASPECTRATIO_NONE)
        return;

    intrinsicRatio = rootElement->viewBox().size();
    if (intrinsicRatio.isEmpty() && intrinsicWidth.isFixed() && intrinsicHeight.isFixed())
        intrinsicRatio = FloatSize(floatValueForLength(intrinsicWidth, 0), floatValueForLength(intrinsicHeight, 0));
}

void SVGImage::startAnimation()
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement || !rootElement->animationsPaused())
        return;
    rootElement->unpauseAnimations();
    rootElement->setCurrentTime(0);
}

void SVGImage::stopAnimation()
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return;
    rootElement->pauseAnimations();
}

void SVGImage::resetAnimation()
{
    stopAnimation();
}

bool SVGImage::isAnimating() const
{
    SVGSVGElement* rootElement = this->rootElement();
    if (!rootElement)
        return false;
    return rootElement->hasActiveAnimation();
}

void SVGImage::reportApproximateMemoryCost() const
{
    Document* document = m_page->mainFrame().document();
    size_t decodedImageMemoryCost = 0;

    for (Node* node = document; node; node = NodeTraversal::next(*node))
        decodedImageMemoryCost += node->approximateMemoryCost();

    JSC::VM& vm = commonVM();
    JSC::JSLockHolder lock(vm);
    // FIXME: Adopt reportExtraMemoryVisited, and switch to reportExtraMemoryAllocated.
    // https://bugs.webkit.org/show_bug.cgi?id=142595
    vm.heap.deprecatedReportExtraMemory(decodedImageMemoryCost + data()->size());
}

EncodedDataStatus SVGImage::dataChanged(bool allDataReceived)
{
    // Don't do anything; it is an empty image.
    if (!data()->size())
        return EncodedDataStatus::Complete;

    if (allDataReceived) {
        PageConfiguration pageConfiguration(
            createEmptyEditorClient(),
            SocketProvider::create(),
            makeUniqueRef<LibWebRTCProvider>()
        );
        fillWithEmptyClients(pageConfiguration);
        m_chromeClient = std::make_unique<SVGImageChromeClient>(this);
        pageConfiguration.chromeClient = m_chromeClient.get();

        // FIXME: If this SVG ends up loading itself, we might leak the world.
        // The Cache code does not know about CachedImages holding Frames and
        // won't know to break the cycle.
        // This will become an issue when SVGImage will be able to load other
        // SVGImage objects, but we're safe now, because SVGImage can only be
        // loaded by a top-level document.
        m_page = std::make_unique<Page>(WTFMove(pageConfiguration));
        m_page->settings().setMediaEnabled(false);
        m_page->settings().setScriptEnabled(false);
        m_page->settings().setPluginsEnabled(false);
        m_page->settings().setAcceleratedCompositingEnabled(false);

        Frame& frame = m_page->mainFrame();
        frame.setView(FrameView::create(frame));
        frame.init();
        FrameLoader& loader = frame.loader();
        loader.forceSandboxFlags(SandboxAll);

        frame.view()->setCanHaveScrollbars(false); // SVG Images will always synthesize a viewBox, if it's not available, and thus never see scrollbars.
        frame.view()->setTransparent(true); // SVG Images are transparent.

        ASSERT(loader.activeDocumentLoader()); // DocumentLoader should have been created by frame->init().
        loader.activeDocumentLoader()->writer().setMIMEType("image/svg+xml");
        loader.activeDocumentLoader()->writer().begin(URL()); // create the empty document
        loader.activeDocumentLoader()->writer().addData(data()->data(), data()->size());
        loader.activeDocumentLoader()->writer().end();

        frame.document()->updateLayoutIgnorePendingStylesheets();

        // Set the intrinsic size before a container size is available.
        m_intrinsicSize = containerSize();
        reportApproximateMemoryCost();
    }

    return m_page ? EncodedDataStatus::Complete : EncodedDataStatus::Unknown;
}

String SVGImage::filenameExtension() const
{
    return ASCIILiteral("svg");
}

bool isInSVGImage(const Element* element)
{
    ASSERT(element);

    Page* page = element->document().page();
    if (!page)
        return false;

    return page->chrome().client().isSVGImageChromeClient();
}

void SVGImage::dump(TextStream& ts) const
{
    Image::dump(ts);
    ts.dumpProperty("url", m_url.string());
}


}
