// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/damage_tracker.h"

#include "cc/layer_impl.h"
#include "cc/layer_tree_host_common.h"
#include "cc/math_util.h"
#include "cc/render_surface_impl.h"
#include <public/WebFilterOperations.h>

using WebKit::WebTransformationMatrix;

namespace cc {

scoped_ptr<DamageTracker> DamageTracker::create()
{
    return make_scoped_ptr(new DamageTracker());
}

DamageTracker::DamageTracker()
    : m_forceFullDamageNextUpdate(false),
      m_currentRectHistory(new RectMap),
      m_nextRectHistory(new RectMap)
{
}

DamageTracker::~DamageTracker()
{
}

static inline void expandRectWithFilters(gfx::RectF& rect, const WebKit::WebFilterOperations& filters)
{
    int top, right, bottom, left;
    filters.getOutsets(top, right, bottom, left);
    rect.Inset(-left, -top, -right, -bottom);
}

static inline void expandDamageRectInsideRectWithFilters(gfx::RectF& damageRect, const gfx::RectF& preFilterRect, const WebKit::WebFilterOperations& filters)
{
    gfx::RectF expandedDamageRect = damageRect;
    expandRectWithFilters(expandedDamageRect, filters);
    gfx::RectF filterRect = preFilterRect;
    expandRectWithFilters(filterRect, filters);

    expandedDamageRect.Intersect(filterRect);
    damageRect.Union(expandedDamageRect);
}

void DamageTracker::updateDamageTrackingState(const std::vector<LayerImpl*>& layerList, int targetSurfaceLayerID, bool targetSurfacePropertyChangedOnlyFromDescendant, const gfx::Rect& targetSurfaceContentRect, LayerImpl* targetSurfaceMaskLayer, const WebKit::WebFilterOperations& filters, SkImageFilter* filter)
{
    //
    // This function computes the "damage rect" of a target surface, and updates the state
    // that is used to correctly track damage across frames. The damage rect is the region
    // of the surface that may have changed and needs to be redrawn. This can be used to
    // scissor what is actually drawn, to save GPU computation and bandwidth.
    //
    // The surface's damage rect is computed as the union of all possible changes that
    // have happened to the surface since the last frame was drawn. This includes:
    //   - any changes for existing layers/surfaces that contribute to the target surface
    //   - layers/surfaces that existed in the previous frame, but no longer exist.
    //
    // The basic algorithm for computing the damage region is as follows:
    //
    //   1. compute damage caused by changes in active/new layers
    //       for each layer in the layerList:
    //           if the layer is actually a renderSurface:
    //               add the surface's damage to our target surface.
    //           else
    //               add the layer's damage to the target surface.
    //
    //   2. compute damage caused by the target surface's mask, if it exists.
    //
    //   3. compute damage caused by old layers/surfaces that no longer exist
    //       for each leftover layer:
    //           add the old layer/surface bounds to the target surface damage.
    //
    //   4. combine all partial damage rects to get the full damage rect.
    //
    // Additional important points:
    //
    // - This algorithm is implicitly recursive; it assumes that descendant surfaces have
    //   already computed their damage.
    //
    // - Changes to layers/surfaces indicate "damage" to the target surface; If a layer is
    //   not changed, it does NOT mean that the layer can skip drawing. All layers that
    //   overlap the damaged region still need to be drawn. For example, if a layer
    //   changed its opacity, then layers underneath must be re-drawn as well, even if
    //   they did not change.
    //
    // - If a layer/surface property changed, the old bounds and new bounds may overlap...
    //   i.e. some of the exposed region may not actually be exposing anything. But this
    //   does not artificially inflate the damage rect. If the layer changed, its entire
    //   old bounds would always need to be redrawn, regardless of how much it overlaps
    //   with the layer's new bounds, which also need to be entirely redrawn.
    //
    // - See comments in the rest of the code to see what exactly is considered a "change"
    //   in a layer/surface.
    //
    // - To correctly manage exposed rects, two RectMaps are maintained:
    //
    //      1. The "current" map contains all the layer bounds that contributed to the
    //         previous frame (even outside the previous damaged area). If a layer changes
    //         or does not exist anymore, those regions are then exposed and damage the
    //         target surface. As the algorithm progresses, entries are removed from the
    //         map until it has only leftover layers that no longer exist.
    //
    //      2. The "next" map starts out empty, and as the algorithm progresses, every
    //         layer/surface that contributes to the surface is added to the map.
    //
    //      3. After the damage rect is computed, the two maps are swapped, so that the
    //         damage tracker is ready for the next frame.
    //

    // These functions cannot be bypassed with early-exits, even if we know what the
    // damage will be for this frame, because we need to update the damage tracker state
    // to correctly track the next frame.
    gfx::RectF damageFromActiveLayers = trackDamageFromActiveLayers(layerList, targetSurfaceLayerID);
    gfx::RectF damageFromSurfaceMask = trackDamageFromSurfaceMask(targetSurfaceMaskLayer);
    gfx::RectF damageFromLeftoverRects = trackDamageFromLeftoverRects();

    gfx::RectF damageRectForThisUpdate;

    if (m_forceFullDamageNextUpdate || targetSurfacePropertyChangedOnlyFromDescendant) {
        damageRectForThisUpdate = targetSurfaceContentRect;
        m_forceFullDamageNextUpdate = false;
    } else {
        // FIXME: can we clamp this damage to the surface's content rect? (affects performance, but not correctness)
        damageRectForThisUpdate = damageFromActiveLayers;
        damageRectForThisUpdate.Union(damageFromSurfaceMask);
        damageRectForThisUpdate.Union(damageFromLeftoverRects);

        if (filters.hasFilterThatMovesPixels()) {
            expandRectWithFilters(damageRectForThisUpdate, filters);
        } else if (filter) {
            // TODO(senorblanco):  Once SkImageFilter reports its outsets, use
            // those here to limit damage.
            damageRectForThisUpdate = targetSurfaceContentRect;
        }
    }

    // Damage accumulates until we are notified that we actually did draw on that frame.
    m_currentDamageRect.Union(damageRectForThisUpdate);

    // The next history map becomes the current map for the next frame. Note this must
    // happen every frame to correctly track changes, even if damage accumulates over
    // multiple frames before actually being drawn.
    swap(m_currentRectHistory, m_nextRectHistory);
}

gfx::RectF DamageTracker::removeRectFromCurrentFrame(int layerID, bool& layerIsNew)
{
    RectMap::iterator iter = m_currentRectHistory->find(layerID);
    layerIsNew = iter == m_currentRectHistory->end();
    if (layerIsNew)
        return gfx::RectF();

    gfx::RectF ret = iter->second;
    m_currentRectHistory->erase(iter);
    return ret;
}

void DamageTracker::saveRectForNextFrame(int layerID, const gfx::RectF& targetSpaceRect)
{
    // This layer should not yet exist in next frame's history.
    DCHECK(layerID > 0);
    DCHECK(m_nextRectHistory->find(layerID) == m_nextRectHistory->end());
    (*m_nextRectHistory)[layerID] = targetSpaceRect;
}

gfx::RectF DamageTracker::trackDamageFromActiveLayers(const std::vector<LayerImpl*>& layerList, int targetSurfaceLayerID)
{
    gfx::RectF damageRect = gfx::RectF();

    for (unsigned layerIndex = 0; layerIndex < layerList.size(); ++layerIndex) {
        // Visit layers in back-to-front order.
        LayerImpl* layer = layerList[layerIndex];

        if (LayerTreeHostCommon::renderSurfaceContributesToTarget<LayerImpl>(layer, targetSurfaceLayerID))
            extendDamageForRenderSurface(layer, damageRect);
        else
            extendDamageForLayer(layer, damageRect);
    }

    return damageRect;
}

gfx::RectF DamageTracker::trackDamageFromSurfaceMask(LayerImpl* targetSurfaceMaskLayer)
{
    gfx::RectF damageRect = gfx::RectF();

    if (!targetSurfaceMaskLayer)
        return damageRect;

    // Currently, if there is any change to the mask, we choose to damage the entire
    // surface. This could potentially be optimized later, but it is not expected to be a
    // common case.
    if (targetSurfaceMaskLayer->layerPropertyChanged() || !targetSurfaceMaskLayer->updateRect().IsEmpty())
        damageRect = gfx::RectF(gfx::PointF(), targetSurfaceMaskLayer->bounds());

    return damageRect;
}

gfx::RectF DamageTracker::trackDamageFromLeftoverRects()
{
    // After computing damage for all active layers, any leftover items in the current
    // rect history correspond to layers/surfaces that no longer exist. So, these regions
    // are now exposed on the target surface.

    gfx::RectF damageRect = gfx::RectF();

    for (RectMap::iterator it = m_currentRectHistory->begin(); it != m_currentRectHistory->end(); ++it)
        damageRect.Union(it->second);

    m_currentRectHistory->clear();

    return damageRect;
}

static bool layerNeedsToRedrawOntoItsTargetSurface(LayerImpl* layer)
{
    // If the layer does NOT own a surface but has SurfacePropertyChanged,
    // this means that its target surface is affected and needs to be redrawn.
    // However, if the layer DOES own a surface, then the SurfacePropertyChanged 
    // flag should not be used here, because that flag represents whether the
    // layer's surface has changed.
    if (layer->renderSurface())
        return layer->layerPropertyChanged();
    return layer->layerPropertyChanged() || layer->layerSurfacePropertyChanged();
}

void DamageTracker::extendDamageForLayer(LayerImpl* layer, gfx::RectF& targetDamageRect)
{
    // There are two ways that a layer can damage a region of the target surface:
    //   1. Property change (e.g. opacity, position, transforms):
    //        - the entire region of the layer itself damages the surface.
    //        - the old layer region also damages the surface, because this region is now exposed.
    //        - note that in many cases the old and new layer rects may overlap, which is fine.
    //
    //   2. Repaint/update: If a region of the layer that was repainted/updated, that
    //      region damages the surface.
    //
    // Property changes take priority over update rects.
    //
    // This method is called when we want to consider how a layer contributes to its
    // targetRenderSurface, even if that layer owns the targetRenderSurface itself.
    // To consider how a layer's targetSurface contributes to the ancestorSurface,
    // extendDamageForRenderSurface() must be called instead.

    bool layerIsNew = false;
    gfx::RectF oldRectInTargetSpace = removeRectFromCurrentFrame(layer->id(), layerIsNew);

    gfx::RectF rectInTargetSpace = MathUtil::mapClippedRect(layer->drawTransform(), gfx::RectF(gfx::PointF(), layer->contentBounds()));
    saveRectForNextFrame(layer->id(), rectInTargetSpace);

    if (layerIsNew || layerNeedsToRedrawOntoItsTargetSurface(layer)) {
        // If a layer is new or has changed, then its entire layer rect affects the target surface.
        targetDamageRect.Union(rectInTargetSpace);

        // The layer's old region is now exposed on the target surface, too.
        // Note oldRectInTargetSpace is already in target space.
        targetDamageRect.Union(oldRectInTargetSpace);
    } else if (!layer->updateRect().IsEmpty()) {
        // If the layer properties haven't changed, then the the target surface is only
        // affected by the layer's update area, which could be empty.
        gfx::RectF updateContentRect = layer->layerRectToContentRect(layer->updateRect());
        gfx::RectF updateRectInTargetSpace = MathUtil::mapClippedRect(layer->drawTransform(), updateContentRect);
        targetDamageRect.Union(updateRectInTargetSpace);
    }
}

void DamageTracker::extendDamageForRenderSurface(LayerImpl* layer, gfx::RectF& targetDamageRect)
{
    // There are two ways a "descendant surface" can damage regions of the "target surface":
    //   1. Property change:
    //        - a surface's geometry can change because of
    //            - changes to descendants (i.e. the subtree) that affect the surface's content rect
    //            - changes to ancestor layers that propagate their property changes to their entire subtree.
    //        - just like layers, both the old surface rect and new surface rect will
    //          damage the target surface in this case.
    //
    //   2. Damage rect: This surface may have been damaged by its own layerList as well, and that damage
    //      should propagate to the target surface.
    //

    RenderSurfaceImpl* renderSurface = layer->renderSurface();

    bool surfaceIsNew = false;
    gfx::RectF oldSurfaceRect = removeRectFromCurrentFrame(layer->id(), surfaceIsNew);

    gfx::RectF surfaceRectInTargetSpace = renderSurface->drawableContentRect(); // already includes replica if it exists.
    saveRectForNextFrame(layer->id(), surfaceRectInTargetSpace);

    gfx::RectF damageRectInLocalSpace;
    if (surfaceIsNew || renderSurface->surfacePropertyChanged() || layer->layerSurfacePropertyChanged()) {
        // The entire surface contributes damage.
        damageRectInLocalSpace = renderSurface->contentRect();

        // The surface's old region is now exposed on the target surface, too.
        targetDamageRect.Union(oldSurfaceRect);
    } else {
        // Only the surface's damageRect will damage the target surface.
        damageRectInLocalSpace = renderSurface->damageTracker()->currentDamageRect();
    }

    // If there was damage, transform it to target space, and possibly contribute its reflection if needed.
    if (!damageRectInLocalSpace.IsEmpty()) {
        const WebTransformationMatrix& drawTransform = renderSurface->drawTransform();
        gfx::RectF damageRectInTargetSpace = MathUtil::mapClippedRect(drawTransform, damageRectInLocalSpace);
        targetDamageRect.Union(damageRectInTargetSpace);

        if (layer->replicaLayer()) {
            const WebTransformationMatrix& replicaDrawTransform = renderSurface->replicaDrawTransform();
            targetDamageRect.Union(MathUtil::mapClippedRect(replicaDrawTransform, damageRectInLocalSpace));
        }
    }

    // If there was damage on the replica's mask, then the target surface receives that damage as well.
    if (layer->replicaLayer() && layer->replicaLayer()->maskLayer()) {
        LayerImpl* replicaMaskLayer = layer->replicaLayer()->maskLayer();

        bool replicaIsNew = false;
        removeRectFromCurrentFrame(replicaMaskLayer->id(), replicaIsNew);

        const WebTransformationMatrix& replicaDrawTransform = renderSurface->replicaDrawTransform();
        gfx::RectF replicaMaskLayerRect = MathUtil::mapClippedRect(replicaDrawTransform, gfx::RectF(gfx::PointF(), replicaMaskLayer->bounds()));
        saveRectForNextFrame(replicaMaskLayer->id(), replicaMaskLayerRect);

        // In the current implementation, a change in the replica mask damages the entire replica region.
        if (replicaIsNew || replicaMaskLayer->layerPropertyChanged() || !replicaMaskLayer->updateRect().IsEmpty())
            targetDamageRect.Union(replicaMaskLayerRect);
    }

    // If the layer has a background filter, this may cause pixels in our surface to be expanded, so we will need to expand any damage
    // at or below this layer. We expand the damage from this layer too, as we need to readback those pixels from the surface with only
    // the contents of layers below this one in them. This means we need to redraw any pixels in the surface being used for the blur in
    // this layer this frame.
    if (layer->backgroundFilters().hasFilterThatMovesPixels())
        expandDamageRectInsideRectWithFilters(targetDamageRect, surfaceRectInTargetSpace, layer->backgroundFilters());
}

}  // namespace cc
