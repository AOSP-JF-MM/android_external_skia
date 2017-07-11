/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This file was autogenerated from GrDitherEffect.fp; do not modify.
 */
#ifndef GrDitherEffect_DEFINED
#define GrDitherEffect_DEFINED
#include "SkTypes.h"
#if SK_SUPPORT_GPU
#include "GrFragmentProcessor.h"
#include "GrCoordTransform.h"
#include "effects/GrProxyMove.h"
class GrDitherEffect : public GrFragmentProcessor {
public:
    static sk_sp<GrFragmentProcessor> Make() {
        return sk_sp<GrFragmentProcessor>(new GrDitherEffect());
    }
    const char* name() const override { return "DitherEffect"; }
private:
    GrDitherEffect()
    : INHERITED(kNone_OptimizationFlags) {
        this->initClassID<GrDitherEffect>();
    }
    GrGLSLFragmentProcessor* onCreateGLSLInstance() const override;
    void onGetGLSLProcessorKey(const GrShaderCaps&,GrProcessorKeyBuilder*) const override;
    bool onIsEqual(const GrFragmentProcessor&) const override;
    GR_DECLARE_FRAGMENT_PROCESSOR_TEST
    typedef GrFragmentProcessor INHERITED;
};
#endif
#endif
