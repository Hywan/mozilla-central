/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ipc/AutoOpenSurface.h"
#include "mozilla/layers/PLayers.h"
#include "TiledLayerBuffer.h"
#include "TextureOGL.h"

/* This must occur *after* layers/PLayers.h to avoid typedefs conflicts. */
#include "mozilla/Util.h"

#include "mozilla/layers/ShadowLayers.h"

#include "ThebesLayerBuffer.h"
#include "ThebesLayerOGL.h"
#include "ContentHost.h"
#include "gfxUtils.h"
#include "gfxTeeSurface.h"

#include "base/message_loop.h"

namespace mozilla {
namespace layers {

using gl::GLContext;
using gl::TextureImage;

static const int ALLOW_REPEAT = ThebesLayerBuffer::ALLOW_REPEAT;

GLenum
WrapMode(GLContext *aGl, PRUint32 aFlags)
{
  if ((aFlags & ALLOW_REPEAT) &&
      (aGl->IsExtensionSupported(GLContext::ARB_texture_non_power_of_two) ||
       aGl->IsExtensionSupported(GLContext::OES_texture_npot))) {
    return LOCAL_GL_REPEAT;
  }
  return LOCAL_GL_CLAMP_TO_EDGE;
}

// BindAndDrawQuadWithTextureRect can work with either GL_REPEAT (preferred)
// or GL_CLAMP_TO_EDGE textures. If ALLOW_REPEAT is set in aFlags, we
// select based on whether REPEAT is valid for non-power-of-two textures --
// if we have NPOT support we use it, otherwise we stick with CLAMP_TO_EDGE and
// decompose.
// If ALLOW_REPEAT is not set, we always use GL_CLAMP_TO_EDGE.
static already_AddRefed<TextureImage>
CreateClampOrRepeatTextureImage(GLContext *aGl,
                                const nsIntSize& aSize,
                                TextureImage::ContentType aContentType,
                                PRUint32 aFlags)
{

  return aGl->CreateTextureImage(aSize, aContentType, WrapMode(aGl, aFlags));
}

static void
SetAntialiasingFlags(Layer* aLayer, gfxContext* aTarget)
{
  nsRefPtr<gfxASurface> surface = aTarget->CurrentSurface();
  if (surface->GetContentType() != gfxASurface::CONTENT_COLOR_ALPHA) {
    // Destination doesn't have alpha channel; no need to set any special flags
    return;
  }

  surface->SetSubpixelAntialiasingEnabled(
      !(aLayer->GetContentFlags() & Layer::CONTENT_COMPONENT_ALPHA));
}


class ThebesLayerBufferOGL : public CompositingThebesLayerBuffer
{
public:
  enum { PAINT_WILL_RESAMPLE = ThebesLayerBuffer::PAINT_WILL_RESAMPLE };

protected:
  ThebesLayerBufferOGL(ThebesLayer* aLayer, LayerOGL* aOGLLayer, Compositor* aCompositor)
    : CompositingThebesLayerBuffer(aCompositor)
    , mLayer(aLayer)
    , mOGLLayer(aOGLLayer)
  {}

  //TODO[nrc] get rid of the GL stuff
  GLContext* gl() const { return mOGLLayer->gl(); }

  ThebesLayer* mLayer;
  LayerOGL* mOGLLayer;
};

// This implementation is the fast-path for when our TextureImage is
// permanently backed with a server-side ASurface.  We can simply
// reuse the ThebesLayerBuffer logic in its entirety and profit.
class SurfaceBufferOGL : public ThebesLayerBufferOGL, private ThebesLayerBuffer
{
public:
  typedef CompositingThebesLayerBuffer::ContentType ContentType;
  typedef CompositingThebesLayerBuffer::PaintState PaintState;

  SurfaceBufferOGL(ThebesLayerOGL* aLayer, Compositor* aCompositor)
    : ThebesLayerBufferOGL(aLayer, aLayer, aCompositor)
    , ThebesLayerBuffer(SizedToVisibleBounds)
  {
    
  }
  virtual ~SurfaceBufferOGL() {}

  // CompositingThebesLayerBuffer interface
  virtual PaintState BeginPaint(ContentType aContentType, 
                                PRUint32 aFlags)
  {
    // Let ThebesLayerBuffer do all the hard work for us! :D
    return ThebesLayerBuffer::BeginPaint(mLayer,
                                         aContentType, 
                                         aFlags);
  }

  // ThebesLayerBuffer interface
  virtual already_AddRefed<gfxASurface>
  CreateBuffer(ContentType aType, const nsIntSize& aSize, PRUint32 aFlags)
  {
    NS_ASSERTION(gfxASurface::CONTENT_ALPHA != aType,"ThebesBuffer has color");

    nsRefPtr<TextureImage> texImage = CreateClampOrRepeatTextureImage(gl(), aSize, aType, aFlags);
    if (!texImage) {
      mTextureHost = nullptr;
      return nullptr;
    }

    mTextureHost = new TextureImageHost(gl(), texImage);
    return texImage->GetBackingSurface();
  }

protected:
  virtual nsIntPoint GetOriginOffset() {
    return BufferRect().TopLeft() - BufferRotation();
  }
};


// This implementation is (currently) the slow-path for when we can't
// implement pixel retaining using thebes.  This implementation and
// the above could be unified by abstracting buffer-copy operations
// and implementing them here using GL hacketry.
class BasicBufferOGL : public ThebesLayerBufferOGL
{
public:
  BasicBufferOGL(ThebesLayerOGL* aLayer, Compositor* aCompositor)
    : ThebesLayerBufferOGL(aLayer, aLayer, aCompositor)
    , mBufferRect(0,0,0,0)
    , mBufferRotation(0,0)
  {}
  virtual ~BasicBufferOGL() {}

  virtual PaintState BeginPaint(ContentType aContentType,
                                PRUint32 aFlags);

protected:
  enum XSide {
    LEFT, RIGHT
  };
  enum YSide {
    TOP, BOTTOM
  };
  nsIntRect GetQuadrantRectangle(XSide aXSide, YSide aYSide);

  virtual nsIntPoint GetOriginOffset() {
    return mBufferRect.TopLeft() - mBufferRotation;
  }

private:
  nsIntRect mBufferRect;
  nsIntPoint mBufferRotation;
};

static void
WrapRotationAxis(PRInt32* aRotationPoint, PRInt32 aSize)
{
  if (*aRotationPoint < 0) {
    *aRotationPoint += aSize;
  } else if (*aRotationPoint >= aSize) {
    *aRotationPoint -= aSize;
  }
}

nsIntRect
BasicBufferOGL::GetQuadrantRectangle(XSide aXSide, YSide aYSide)
{
  // quadrantTranslation is the amount we translate the top-left
  // of the quadrant by to get coordinates relative to the layer
  nsIntPoint quadrantTranslation = -mBufferRotation;
  quadrantTranslation.x += aXSide == LEFT ? mBufferRect.width : 0;
  quadrantTranslation.y += aYSide == TOP ? mBufferRect.height : 0;
  return mBufferRect + quadrantTranslation;
}

static void
FillSurface(gfxASurface* aSurface, const nsIntRegion& aRegion,
            const nsIntPoint& aOffset, const gfxRGBA& aColor)
{
  nsRefPtr<gfxContext> ctx = new gfxContext(aSurface);
  ctx->Translate(-gfxPoint(aOffset.x, aOffset.y));
  gfxUtils::ClipToRegion(ctx, aRegion);
  ctx->SetColor(aColor);
  ctx->Paint();
}

BasicBufferOGL::PaintState
BasicBufferOGL::BeginPaint(ContentType aContentType,
                           PRUint32 aFlags)
{
  PaintState result;
  // We need to disable rotation if we're going to be resampled when
  // drawing, because we might sample across the rotation boundary.
  bool canHaveRotation =  !(aFlags & PAINT_WILL_RESAMPLE);

  nsIntRegion validRegion = mLayer->GetValidRegion();

  Layer::SurfaceMode mode;
  ContentType contentType;
  nsIntRegion neededRegion;
  bool canReuseBuffer;
  nsIntRect destBufferRect;

  nsRefPtr<TextureImage> texImage = mTextureHost
                                    ? static_cast<TextureImageHost*>(mTextureHost.get())->GetTextureImage()
                                    : nullptr;
  nsRefPtr<TextureImage> texImageOnWhite = mTextureHostOnWhite 
                                           ? static_cast<TextureImageHost*>(mTextureHostOnWhite.get())->GetTextureImage()
                                           : nullptr;

  while (true) {
    mode = mLayer->GetSurfaceMode();
    contentType = aContentType;
    neededRegion = mLayer->GetVisibleRegion();
    // If we're going to resample, we need a buffer that's in clamp mode.
    canReuseBuffer = neededRegion.GetBounds().Size() <= mBufferRect.Size() &&
      texImage &&
      (!(aFlags & PAINT_WILL_RESAMPLE) ||
       texImage->GetWrapMode() == LOCAL_GL_CLAMP_TO_EDGE);

    if (canReuseBuffer) {
      if (mBufferRect.Contains(neededRegion.GetBounds())) {
        // We don't need to adjust mBufferRect.
        destBufferRect = mBufferRect;
      } else {
        // The buffer's big enough but doesn't contain everything that's
        // going to be visible. We'll move it.
        destBufferRect = nsIntRect(neededRegion.GetBounds().TopLeft(), mBufferRect.Size());
      }
    } else {
      destBufferRect = neededRegion.GetBounds();
    }

    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
#ifdef MOZ_GFX_OPTIMIZE_MOBILE
      mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
#else
      if (!mLayer->GetParent() || !mLayer->GetParent()->SupportsComponentAlphaChildren()) {
        mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
      } else {
        contentType = gfxASurface::CONTENT_COLOR;
      }
 #endif
    }
 
    if ((aFlags & PAINT_WILL_RESAMPLE) &&
        (!neededRegion.GetBounds().IsEqualInterior(destBufferRect) ||
         neededRegion.GetNumRects() > 1)) {
      // The area we add to neededRegion might not be painted opaquely
      if (mode == Layer::SURFACE_OPAQUE) {
        contentType = gfxASurface::CONTENT_COLOR_ALPHA;
        mode = Layer::SURFACE_SINGLE_CHANNEL_ALPHA;
      }
      // For component alpha layers, we leave contentType as CONTENT_COLOR.

      // We need to validate the entire buffer, to make sure that only valid
      // pixels are sampled
      neededRegion = destBufferRect;
    }

    if (texImage &&
        (texImage->GetContentType() != contentType ||
         (mode == Layer::SURFACE_COMPONENT_ALPHA) != (texImageOnWhite != nullptr))) {
      // We're effectively clearing the valid region, so we need to draw
      // the entire needed region now.
      result.mRegionToInvalidate = mLayer->GetValidRegion();
      validRegion.SetEmpty();
      texImage = nullptr;
      texImageOnWhite = nullptr;
      mBufferRect.SetRect(0, 0, 0, 0);
      mBufferRotation.MoveTo(0, 0);
      // Restart decision process with the cleared buffer. We can only go
      // around the loop one more iteration, since texImage is null now.
      continue;
    }

    break;
  }

  result.mRegionToDraw.Sub(neededRegion, validRegion);
  if (result.mRegionToDraw.IsEmpty())
    return result;

  if (destBufferRect.width > gl()->GetMaxTextureImageSize() ||
      destBufferRect.height > gl()->GetMaxTextureImageSize()) {
    return result;
  }

  nsIntRect drawBounds = result.mRegionToDraw.GetBounds();
  nsRefPtr<TextureImage> destBuffer;
  nsRefPtr<TextureImage> destBufferOnWhite;

  PRUint32 bufferFlags = canHaveRotation ? ALLOW_REPEAT : 0;
  if (canReuseBuffer) {
    nsIntRect keepArea;
    if (keepArea.IntersectRect(destBufferRect, mBufferRect)) {
      // Set mBufferRotation so that the pixels currently in mBuffer
      // will still be rendered in the right place when mBufferRect
      // changes to destBufferRect.
      nsIntPoint newRotation = mBufferRotation +
        (destBufferRect.TopLeft() - mBufferRect.TopLeft());
      WrapRotationAxis(&newRotation.x, mBufferRect.width);
      WrapRotationAxis(&newRotation.y, mBufferRect.height);
      NS_ASSERTION(nsIntRect(nsIntPoint(0,0), mBufferRect.Size()).Contains(newRotation),
                   "newRotation out of bounds");
      PRInt32 xBoundary = destBufferRect.XMost() - newRotation.x;
      PRInt32 yBoundary = destBufferRect.YMost() - newRotation.y;
      if ((drawBounds.x < xBoundary && xBoundary < drawBounds.XMost()) ||
          (drawBounds.y < yBoundary && yBoundary < drawBounds.YMost()) ||
          (newRotation != nsIntPoint(0,0) && !canHaveRotation)) {
        // The stuff we need to redraw will wrap around an edge of the
        // buffer, so we will need to do a self-copy
        // If mBufferRotation == nsIntPoint(0,0) we could do a real
        // self-copy but we're not going to do that in GL yet.
        // We can't do a real self-copy because the buffer is rotated.
        // So allocate a new buffer for the destination.
        destBufferRect = neededRegion.GetBounds();
        destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        if (!destBuffer)
          return result;
        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite =
            CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
          if (!destBufferOnWhite)
            return result;
        }
      } else {
        mBufferRect = destBufferRect;
        mBufferRotation = newRotation;
      }
    } else {
      // No pixels are going to be kept. The whole visible region
      // will be redrawn, so we don't need to copy anything, so we don't
      // set destBuffer.
      mBufferRect = destBufferRect;
      mBufferRotation = nsIntPoint(0,0);
    }
  } else {
    // The buffer's not big enough, so allocate a new one
    destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
    if (!destBuffer)
      return result;

    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
      destBufferOnWhite = 
        CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
      if (!destBufferOnWhite)
        return result;
    }
  }
  NS_ASSERTION(!(aFlags & PAINT_WILL_RESAMPLE) || destBufferRect == neededRegion.GetBounds(),
               "If we're resampling, we need to validate the entire buffer");

  if (!destBuffer && !texImage) {
    return result;
  }

  if (destBuffer) {
    if (texImage && (mode != Layer::SURFACE_COMPONENT_ALPHA || texImageOnWhite)) {
      // BlitTextureImage depends on the FBO texture target being
      // TEXTURE_2D.  This isn't the case on some older X1600-era Radeons.
      if (mOGLLayer->OGLManager()->FBOTextureTarget() == LOCAL_GL_TEXTURE_2D) {
        nsIntRect overlap;
        overlap.IntersectRect(mBufferRect, destBufferRect);

        nsIntRect srcRect(overlap), dstRect(overlap);
        srcRect.MoveBy(- mBufferRect.TopLeft() + mBufferRotation);
        dstRect.MoveBy(- destBufferRect.TopLeft());
        
        if (mBufferRotation != nsIntPoint(0, 0)) {
          // If mBuffer is rotated, then BlitTextureImage will only be copying the bottom-right section
          // of the buffer. We need to invalidate the remaining sections so that they get redrawn too.
          // Alternatively we could teach BlitTextureImage to rearrange the rotated segments onto
          // the new buffer.
          
          // When the rotated buffer is reorganised, the bottom-right section will be drawn in the top left.
          // Find the point where this content ends.
          nsIntPoint rotationPoint(mBufferRect.x + mBufferRect.width - mBufferRotation.x, 
                                   mBufferRect.y + mBufferRect.height - mBufferRotation.y);

          // The buffer looks like:
          //  ______
          // |1  |2 |  Where the center point is offset by mBufferRotation from the top-left corner.
          // |___|__|
          // |3  |4 |
          // |___|__|
          //
          // This is drawn to the screen as:
          //  ______
          // |4  |3 |  Where the center point is { width - mBufferRotation.x, height - mBufferRotation.y } from
          // |___|__|  from the top left corner - rotationPoint. Since only quadrant 4 will actually be copied, 
          // |2  |1 |  we need to invalidate the others.
          // |___|__|
          //
          // Quadrants 2 and 1
          nsIntRect bottom(mBufferRect.x, rotationPoint.y, mBufferRect.width, mBufferRotation.y);
          // Quadrant 3
          nsIntRect topright(rotationPoint.x, mBufferRect.y, mBufferRotation.x, rotationPoint.y - mBufferRect.y);

          if (!bottom.IsEmpty()) {
            nsIntRegion temp;
            temp.And(destBufferRect, bottom);
            result.mRegionToDraw.Or(result.mRegionToDraw, temp);
          }
          if (!topright.IsEmpty()) {
            nsIntRegion temp;
            temp.And(destBufferRect, topright);
            result.mRegionToDraw.Or(result.mRegionToDraw, temp);
          }
        }

        destBuffer->Resize(destBufferRect.Size());

        gl()->BlitTextureImage(texImage, srcRect,
                               destBuffer, dstRect);
        destBuffer->MarkValid();

        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite->Resize(destBufferRect.Size());
          gl()->BlitTextureImage(texImageOnWhite, srcRect,
                                 destBufferOnWhite, dstRect);
          destBufferOnWhite->MarkValid();
        }
      } else {
        // can't blit, just draw everything
        destBuffer = CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
          destBufferOnWhite = 
            CreateClampOrRepeatTextureImage(gl(), destBufferRect.Size(), contentType, bufferFlags);
        }
      }
    }

    texImage = destBuffer.forget();
    if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
      texImageOnWhite = destBufferOnWhite.forget();
    }
    mBufferRect = destBufferRect;
    mBufferRotation = nsIntPoint(0,0);
  }
  NS_ASSERTION(canHaveRotation || mBufferRotation == nsIntPoint(0,0),
               "Rotation disabled, but we have nonzero rotation?");

  if (mTextureHost) {
    static_cast<TextureImageHost*>(mTextureHost.get())->SetTextureImage(texImage);
  } else {
    mTextureHost = new TextureImageHost(gl(), texImage);
  }
  if (mTextureHostOnWhite) {
    static_cast<TextureImageHost*>(mTextureHostOnWhite.get())->SetTextureImage(texImageOnWhite);
  } else {
    mTextureHostOnWhite = new TextureImageHost(gl(), texImage);
  }

  nsIntRegion invalidate;
  invalidate.Sub(mLayer->GetValidRegion(), destBufferRect);
  result.mRegionToInvalidate.Or(result.mRegionToInvalidate, invalidate);

  // Figure out which quadrant to draw in
  PRInt32 xBoundary = mBufferRect.XMost() - mBufferRotation.x;
  PRInt32 yBoundary = mBufferRect.YMost() - mBufferRotation.y;
  XSide sideX = drawBounds.XMost() <= xBoundary ? RIGHT : LEFT;
  YSide sideY = drawBounds.YMost() <= yBoundary ? BOTTOM : TOP;
  nsIntRect quadrantRect = GetQuadrantRectangle(sideX, sideY);
  NS_ASSERTION(quadrantRect.Contains(drawBounds), "Messed up quadrants");

  nsIntPoint offset = -nsIntPoint(quadrantRect.x, quadrantRect.y);

  // Make the region to draw relative to the buffer, before
  // passing to BeginUpdate.
  result.mRegionToDraw.MoveBy(offset);
  // BeginUpdate is allowed to modify the given region,
  // if it wants more to be repainted than we request.
  if (mode == Layer::SURFACE_COMPONENT_ALPHA) {
    nsIntRegion drawRegionCopy = result.mRegionToDraw;
    gfxASurface *onBlack = texImage->BeginUpdate(drawRegionCopy);
    gfxASurface *onWhite = texImageOnWhite->BeginUpdate(result.mRegionToDraw);
    NS_ASSERTION(result.mRegionToDraw == drawRegionCopy,
                 "BeginUpdate should always modify the draw region in the same way!");
    FillSurface(onBlack, result.mRegionToDraw, nsIntPoint(0,0), gfxRGBA(0.0, 0.0, 0.0, 1.0));
    FillSurface(onWhite, result.mRegionToDraw, nsIntPoint(0,0), gfxRGBA(1.0, 1.0, 1.0, 1.0));
    gfxASurface* surfaces[2] = { onBlack, onWhite };
    nsRefPtr<gfxTeeSurface> surf = new gfxTeeSurface(surfaces, ArrayLength(surfaces));

    // XXX If the device offset is set on the individual surfaces instead of on
    // the tee surface, we render in the wrong place. Why?
    gfxPoint deviceOffset = onBlack->GetDeviceOffset();
    onBlack->SetDeviceOffset(gfxPoint(0, 0));
    onWhite->SetDeviceOffset(gfxPoint(0, 0));
    surf->SetDeviceOffset(deviceOffset);

    // Using this surface as a source will likely go horribly wrong, since
    // only the onBlack surface will really be used, so alpha information will
    // be incorrect.
    surf->SetAllowUseAsSource(false);
    result.mContext = new gfxContext(surf);
  } else {
    result.mContext = new gfxContext(texImage->BeginUpdate(result.mRegionToDraw));
    if (texImage->GetContentType() == gfxASurface::CONTENT_COLOR_ALPHA) {
      gfxUtils::ClipToRegion(result.mContext, result.mRegionToDraw);
      result.mContext->SetOperator(gfxContext::OPERATOR_CLEAR);
      result.mContext->Paint();
      result.mContext->SetOperator(gfxContext::OPERATOR_OVER);
    }
  }
  if (!result.mContext) {
    NS_WARNING("unable to get context for update");
    return result;
  }
  result.mContext->Translate(-gfxPoint(quadrantRect.x, quadrantRect.y));
  // Move rgnToPaint back into position so that the thebes callback
  // gets the right coordintes.
  result.mRegionToDraw.MoveBy(-offset);

  // If we do partial updates, we have to clip drawing to the regionToDraw.
  // If we don't clip, background images will be fillrect'd to the region correctly,
  // while text or lines will paint outside of the regionToDraw. This becomes apparent
  // with concave regions. Right now the scrollbars invalidate a narrow strip of the awesomebar
  // although they never cover it. This leads to two draw rects, the narow strip and the actually
  // newly exposed area. It would be wise to fix this glitch in any way to have simpler
  // clip and draw regions.
  gfxUtils::ClipToRegion(result.mContext, result.mRegionToDraw);

  return result;
}

ThebesLayerOGL::ThebesLayerOGL(LayerManagerOGL *aManager)
  : ThebesLayer(aManager, nullptr)
  , LayerOGL(aManager)
  , mBuffer(nullptr)
{
  mImplData = static_cast<LayerOGL*>(this);
}

ThebesLayerOGL::~ThebesLayerOGL()
{
  Destroy();
}

void
ThebesLayerOGL::Destroy()
{
  if (!mDestroyed) {
    mBuffer = nullptr;
    mDestroyed = true;
  }
}

bool
ThebesLayerOGL::CreateSurface()
{
  NS_ASSERTION(!mBuffer, "buffer already created?");

  if (mVisibleRegion.IsEmpty()) {
    return false;
  }

  if (gl()->TextureImageSupportsGetBackingSurface()) {
    // use the ThebesLayerBuffer fast-path
    mBuffer = new SurfaceBufferOGL(this, mOGLManager->GetCompositor());
  } else {
    mBuffer = new BasicBufferOGL(this, mOGLManager->GetCompositor());
  }
  return true;
}

void
ThebesLayerOGL::SetVisibleRegion(const nsIntRegion &aRegion)
{
  if (aRegion.IsEqual(mVisibleRegion))
    return;
  ThebesLayer::SetVisibleRegion(aRegion);
}

void
ThebesLayerOGL::InvalidateRegion(const nsIntRegion &aRegion)
{
  mValidRegion.Sub(mValidRegion, aRegion);
}

void
ThebesLayerOGL::RenderLayer(const nsIntPoint& aOffset,
                            const nsIntRect& aClipRect,
                            Surface* aPreviousSurface)
{
  if (!mBuffer && !CreateSurface()) {
    return;
  }
  NS_ABORT_IF_FALSE(mBuffer, "should have a buffer here");

  mOGLManager->MakeCurrent();
  gl()->fActiveTexture(LOCAL_GL_TEXTURE0);

  TextureImage::ContentType contentType =
    CanUseOpaqueSurface() ? gfxASurface::CONTENT_COLOR :
                            gfxASurface::CONTENT_COLOR_ALPHA;

  uint32_t flags = 0;
#ifndef MOZ_GFX_OPTIMIZE_MOBILE
  gfxMatrix transform2d;
  bool paintWillResample = !GetEffectiveTransform().Is2D(&transform2d) ||
                           transform2d.HasNonIntegerTranslation();
  if (paintWillResample) {
    flags |= ThebesLayerBufferOGL::PAINT_WILL_RESAMPLE;
  }
  mBuffer->SetPaintWillResample(paintWillResample);
#endif

  Buffer::PaintState state = mBuffer->BeginPaint(contentType, flags);
  mValidRegion.Sub(mValidRegion, state.mRegionToInvalidate);

  if (state.mContext) {
    state.mRegionToInvalidate.And(state.mRegionToInvalidate, mVisibleRegion);

    LayerManager::DrawThebesLayerCallback callback =
      mOGLManager->GetThebesLayerCallback();
    if (!callback) {
      NS_ERROR("GL should never need to update ThebesLayers in an empty transaction");
    } else {
      void* callbackData = mOGLManager->GetThebesLayerCallbackData();
      SetAntialiasingFlags(this, state.mContext);
      callback(this, state.mContext, state.mRegionToDraw,
               state.mRegionToInvalidate, callbackData);
      // Everything that's visible has been validated. Do this instead of just
      // OR-ing with aRegionToDraw, since that can lead to a very complex region
      // here (OR doesn't automatically simplify to the simplest possible
      // representation of a region.)
      nsIntRegion tmp;
      tmp.Or(mVisibleRegion, state.mRegionToDraw);
      mValidRegion.Or(mValidRegion, tmp);
    }
  }

  // Drawing thebes layers can change the current context, reset it.
  gl()->MakeCurrent();

  // TODO: We won't be supporting non-shadow-thebes layers, so commenting the next
  // line is the simplest build fix. ORLY?
  //gl()->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, aPreviousFrameBuffer);

  gfx::Matrix4x4 transform;
  mOGLManager->ToMatrix4x4(GetEffectiveTransform(), transform);
  gfx::Rect clipRect(aClipRect.x, aClipRect.y, aClipRect.width, aClipRect.height);

#ifdef MOZ_DUMP_PAINTING
  //TODO[nrc]
  /*if (gfxUtils::sDumpPainting) {
    nsRefPtr<gfxImageSurface> surf = 
      gl()->GetTexImage(mBuffer->GetTextureImage()->GetTextureID(), false,
                        mBuffer->GetTextureImage()->GetShaderProgramType());
    
    WriteSnapshotToDumpFile(this, surf);
  }*/
#endif

  EffectChain effectChain;
  effectChain.mEffects[EFFECT_MASK] = mManager->MakeMaskEffect(mMaskLayer);

  mBuffer->Composite(effectChain,
                     GetEffectiveOpacity(),
                     transform,
                     gfx::Point(aOffset.x, aOffset.y),
                     gfx::FILTER_LINEAR,
                     clipRect,
                     &GetEffectiveVisibleRegion());
}

Layer*
ThebesLayerOGL::GetLayer()
{
  return this;
}

bool
ThebesLayerOGL::IsEmpty()
{
  return !mBuffer;
}

void
ThebesLayerOGL::CleanupResources()
{
  mBuffer = nullptr;
}


ShadowThebesLayerOGL::ShadowThebesLayerOGL(LayerManagerOGL *aManager)
  : ShadowThebesLayer(aManager, nullptr)
  , LayerOGL(aManager)
  , mBuffer(nullptr)
{
#ifdef FORCE_BASICTILEDTHEBESLAYER
  NS_ABORT();
#endif
  mImplData = static_cast<LayerOGL*>(this);
}

ShadowThebesLayerOGL::~ShadowThebesLayerOGL()
{}

void
ShadowThebesLayerOGL::AddTextureHost(const TextureIdentifier& aTextureIdentifier, TextureHost* aTextureHost)
{
  EnsureBuffer(aTextureIdentifier.mBufferType);

  mBuffer->AddTextureHost(aTextureIdentifier, aTextureHost);
}


void
ShadowThebesLayerOGL::EnsureBuffer(BufferType aHostType)
{
  if (!mBuffer ||
      mBuffer->GetType() != aHostType) {
    RefPtr<BufferHost> bufferHost = mOGLManager->GetCompositor()->CreateBufferHost(aHostType);
    NS_ASSERTION(bufferHost->GetType() == BUFFER_THEBES ||
                 bufferHost->GetType() == BUFFER_DIRECT, "bad buffer type");
    mBuffer = static_cast<AContentHost*>(bufferHost.get());
  }
}

void
ShadowThebesLayerOGL::SwapTexture(const TextureIdentifier& aTextureIdentifier,
                                  const ThebesBuffer& aNewFront,
                                  const nsIntRegion& aUpdatedRegion,
                                  OptionalThebesBuffer* aNewBack,
                                  nsIntRegion* aNewBackValidRegion,
                                  OptionalThebesBuffer* aReadOnlyFront,
                                  nsIntRegion* aFrontUpdatedRegion)
{
  if (mDestroyed ||
      !mBuffer) {
    // Don't drop buffers on the floor.
    *aNewBack = aNewFront;
    *aNewBackValidRegion = aNewFront.rect();
    return;
  }
  
  mBuffer->UpdateThebes(aTextureIdentifier,
                        aNewFront,
                        aUpdatedRegion,
                        aNewBack,
                        mValidRegionForNextBackBuffer,
                        mValidRegion,
                        aReadOnlyFront,
                        aNewBackValidRegion,
                        aFrontUpdatedRegion);

  // Save the current valid region of our front buffer, because if
  // we're double buffering, it's going to be the valid region for the
  // next back buffer sent back to the renderer.
  //
  // NB: we rely here on the fact that mValidRegion is initialized to
  // empty, and that the first time Swap() is called we don't have a
  // valid front buffer that we're going to return to content.
  mValidRegionForNextBackBuffer = mValidRegion;
}

void
ShadowThebesLayerOGL::Disconnect()
{
  Destroy();
}

void
ShadowThebesLayerOGL::Destroy()
{
  if (!mDestroyed) {
    mBuffer = nullptr;
    mDestroyed = true;
  }
}

Layer*
ShadowThebesLayerOGL::GetLayer()
{
  return this;
}

bool
ShadowThebesLayerOGL::IsEmpty()
{
  return !mBuffer;
}

void
ShadowThebesLayerOGL::RenderLayer(const nsIntPoint& aOffset,
                                  const nsIntRect& aClipRect,
                                  Surface* aPreviousSurface)
{
  if (!mBuffer) {
    return;
  }

  gfx::Matrix4x4 transform;
  mOGLManager->ToMatrix4x4(GetEffectiveTransform(), transform);
  gfx::Rect clipRect(aClipRect.x, aClipRect.y, aClipRect.width, aClipRect.height);

#ifdef MOZ_DUMP_PAINTING
  if (gfxUtils::sDumpPainting) {
    nsRefPtr<gfxImageSurface> surf = mBuffer->Dump();
    WriteSnapshotToDumpFile(this, surf);
  }
#endif

  EffectChain effectChain;
  // TODO: Handle mask layers.
  RefPtr<Effect> effectMask;

  mBuffer->Composite(effectChain,
                     GetEffectiveOpacity(), 
                     transform,
                     gfx::Point(aOffset.x, aOffset.y),
                     gfx::FILTER_LINEAR,
                     clipRect,
                     &GetEffectiveVisibleRegion());
}
void

ShadowThebesLayerOGL::DestroyFrontBuffer()
{
  mBuffer = nullptr;
  mValidRegionForNextBackBuffer.SetEmpty();
}

void
ShadowThebesLayerOGL::CleanupResources()
{
  DestroyFrontBuffer();
}

void
ShadowThebesLayerOGL::SetAllocator(ISurfaceDeAllocator* aAllocator)
{
  mBuffer->SetDeAllocator(aAllocator);
}

} /* layers */
} /* mozilla */
