/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ColorLayerOGL.h"

namespace mozilla {
namespace layers {

static void
RenderColorLayer(ColorLayer* aLayer, LayerManagerOGL *aManager,
                 const nsIntPoint& aOffset, const nsIntRect& aClipRect)
{
  EffectChain effects;
  gfxRGBA color(aLayer->GetColor());
  RefPtr<EffectSolidColor> effectColor = new EffectSolidColor(gfx::Color(color.r, color.g, color.b, color.a));
  effects.mEffects[EFFECT_SOLID_COLOR] = effectColor;
  nsIntRect visibleRect = aLayer->GetEffectiveVisibleRegion().GetBounds();

  effects.mEffects[EFFECT_MASK] = aManager->MakeMaskEffect(aLayer->GetMaskLayer());

  gfx::Rect rect(visibleRect.x, visibleRect.y, visibleRect.width, visibleRect.height);
  float opacity = aLayer->GetEffectiveOpacity();
  gfx::Matrix4x4 transform;
  aManager->ToMatrix4x4(aLayer->GetEffectiveTransform(), transform);
  gfx::Rect clipRect(aClipRect.x, aClipRect.y, aClipRect.width, aClipRect.height);
  aManager->GetCompositor()->DrawQuad(rect, nullptr, nullptr, &clipRect, effects, opacity, transform,
                                      gfx::Point(aOffset.x, aOffset.y));
}

void
ColorLayerOGL::RenderLayer(const nsIntPoint& aOffset, const nsIntRect& aClipRect, Surface*)
{
  RenderColorLayer(this, mOGLManager, aOffset, aClipRect);
}

void
ShadowColorLayerOGL::RenderLayer(const nsIntPoint& aOffset, const nsIntRect& aClipRect, Surface*)
{
  RenderColorLayer(this, mOGLManager, aOffset, aClipRect);
}


} /* layers */
} /* mozilla */
