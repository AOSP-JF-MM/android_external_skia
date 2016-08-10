/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCanvas.h"
#include "SkData.h"
#include "SkImageFilter.h"
#include "SkLiteDL.h"
#include "SkPicture.h"
#include "SkMutex.h"
#include "SkRSXform.h"
#include "SkSpinlock.h"
#include "SkTextBlob.h"

// A stand-in for an optional SkRect which was not set, e.g. bounds for a saveLayer().
static const SkRect kUnset = { SK_ScalarInfinity, 0,0,0};
static const SkRect* maybe_unset(const SkRect& r) {
    return r.left() == SK_ScalarInfinity ? nullptr : &r;
}

// copy_v(dst, src,n, src,n, ...) copies an arbitrary number of typed srcs into dst.
static void copy_v(void* dst) {}

template <typename S, typename... Rest>
static void copy_v(void* dst, const S* src, int n, Rest&&... rest) {
    SkASSERTF(((uintptr_t)dst & (alignof(S)-1)) == 0,
              "Expected %p to be aligned for at least %zu bytes.", dst, alignof(S));
    sk_careful_memcpy(dst, src, n*sizeof(S));
    copy_v(SkTAddOffset<void>(dst, n*sizeof(S)), std::forward<Rest>(rest)...);
}

// Helper for getting back at arrays which have been copy_v'd together after an Op.
template <typename D, typename T>
static D* pod(T* op, size_t offset = 0) {
    return SkTAddOffset<D>(op+1, offset);
}

// Convert images and image-based shaders to textures.
static void optimize_for(GrContext* ctx, SkPaint* paint, sk_sp<const SkImage>* image = nullptr) {
    SkMatrix matrix;
    SkShader::TileMode xy[2];
    if (auto shader = paint->getShader())
    if (auto image  = shader->isAImage(&matrix, xy)) {  // TODO: compose shaders, etc.
        paint->setShader(image->makeTextureImage(ctx)->makeShader(xy[0], xy[1], &matrix));
    }

    if (image) {
        *image = (*image)->makeTextureImage(ctx);
    }
}

// Pre-cache lazy non-threadsafe fields on SkPath and/or SkMatrix.
static void make_threadsafe(SkPath* path, SkMatrix* matrix) {
    if (path)   { path->updateBoundsCache(); }
    if (matrix) { (void)matrix->getType(); }
}

namespace {
#define TYPES(M)                                                                \
    M(Save) M(Restore) M(SaveLayer)                                             \
    M(Concat) M(SetMatrix) M(TranslateZ)                                        \
    M(ClipPath) M(ClipRect) M(ClipRRect) M(ClipRegion)                          \
    M(DrawPaint) M(DrawPath) M(DrawRect) M(DrawOval) M(DrawRRect) M(DrawDRRect) \
    M(DrawAnnotation) M(DrawDrawable) M(DrawPicture) M(DrawShadowedPicture)     \
    M(DrawImage) M(DrawImageNine) M(DrawImageRect) M(DrawImageLattice)          \
    M(DrawText) M(DrawPosText) M(DrawPosTextH)                                  \
    M(DrawTextOnPath) M(DrawTextRSXform) M(DrawTextBlob)                        \
    M(DrawPatch) M(DrawPoints) M(DrawVertices) M(DrawAtlas)

#define M(T) T,
    enum class Type : uint8_t { TYPES(M) };
#undef M

    struct Op {
        // These are never called, only used to distinguish Ops that implement
        // them from those that don't by return type: the real methods return void.
        int optimizeFor(GrContext*) { sk_throw(); return 0;}
        int makeThreadsafe()        { sk_throw(); return 0;}

        uint32_t type :  8;
        uint32_t skip : 24;
    };
    static_assert(sizeof(Op) == 4, "");

    struct Save final : Op {
        static const auto kType = Type::Save;
        void draw(SkCanvas* c) { c->save(); }
    };
    struct Restore final : Op {
        static const auto kType = Type::Restore;
        void draw(SkCanvas* c) { c->restore(); }
    };
    struct SaveLayer final : Op {
        static const auto kType = Type::SaveLayer;
        SaveLayer(const SkRect* bounds, const SkPaint* paint,
                  const SkImageFilter* backdrop, SkCanvas::SaveLayerFlags flags) {
            if (bounds) { this->bounds = *bounds; }
            if (paint)  { this->paint  = *paint;  }
            this->backdrop = sk_ref_sp(backdrop);
            this->flags = flags;
        }
        SkRect                     bounds = kUnset;
        SkPaint                    paint;
        sk_sp<const SkImageFilter> backdrop;
        SkCanvas::SaveLayerFlags   flags;
        void draw(SkCanvas* c) {
            c->saveLayer({ maybe_unset(bounds), &paint, backdrop.get(), flags });
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };

    struct Concat final : Op {
        static const auto kType = Type::Concat;
        Concat(const SkMatrix& matrix) : matrix(matrix) {}
        SkMatrix matrix;
        void draw(SkCanvas* c) { c->concat(matrix); }
        void makeThreadsafe() { make_threadsafe(nullptr, &matrix); }
    };
    struct SetMatrix final : Op {
        static const auto kType = Type::SetMatrix;
        SetMatrix(const SkMatrix& matrix) : matrix(matrix) {}
        SkMatrix matrix;
        void draw(SkCanvas* c) { c->setMatrix(matrix); }
        void makeThreadsafe() { make_threadsafe(nullptr, &matrix); }
    };
    struct TranslateZ final : Op {
        static const auto kType = Type::TranslateZ;
        TranslateZ(SkScalar dz) : dz(dz) {}
        SkScalar dz;
        void draw(SkCanvas* c) {
        #ifdef SK_EXPERIMENTAL_SHADOWING
            c->translateZ(dz);
        #endif
        }
    };

    struct ClipPath final : Op {
        static const auto kType = Type::ClipPath;
        ClipPath(const SkPath& path, SkRegion::Op op, bool aa) : path(path), op(op), aa(aa) {}
        SkPath       path;
        SkRegion::Op op;
        bool         aa;
        void draw(SkCanvas* c) { c->clipPath(path, op, aa); }
        void makeThreadsafe() { make_threadsafe(&path, nullptr); }
    };
    struct ClipRect final : Op {
        static const auto kType = Type::ClipRect;
        ClipRect(const SkRect& rect, SkRegion::Op op, bool aa) : rect(rect), op(op), aa(aa) {}
        SkRect       rect;
        SkRegion::Op op;
        bool         aa;
        void draw(SkCanvas* c) { c->clipRect(rect, op, aa); }
    };
    struct ClipRRect final : Op {
        static const auto kType = Type::ClipRRect;
        ClipRRect(const SkRRect& rrect, SkRegion::Op op, bool aa) : rrect(rrect), op(op), aa(aa) {}
        SkRRect      rrect;
        SkRegion::Op op;
        bool         aa;
        void draw(SkCanvas* c) { c->clipRRect(rrect, op, aa); }
    };
    struct ClipRegion final : Op {
        static const auto kType = Type::ClipRegion;
        ClipRegion(const SkRegion& region, SkRegion::Op op) : region(region), op(op) {}
        SkRegion     region;
        SkRegion::Op op;
        void draw(SkCanvas* c) { c->clipRegion(region, op); }
    };

    struct DrawPaint final : Op {
        static const auto kType = Type::DrawPaint;
        DrawPaint(const SkPaint& paint) : paint(paint) {}
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawPaint(paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawPath final : Op {
        static const auto kType = Type::DrawPath;
        DrawPath(const SkPath& path, const SkPaint& paint) : path(path), paint(paint) {}
        SkPath  path;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawPath(path, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
        void makeThreadsafe() { make_threadsafe(&path, nullptr); }
    };
    struct DrawRect final : Op {
        static const auto kType = Type::DrawRect;
        DrawRect(const SkRect& rect, const SkPaint& paint) : rect(rect), paint(paint) {}
        SkRect  rect;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawRect(rect, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawOval final : Op {
        static const auto kType = Type::DrawOval;
        DrawOval(const SkRect& oval, const SkPaint& paint) : oval(oval), paint(paint) {}
        SkRect  oval;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawOval(oval, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawRRect final : Op {
        static const auto kType = Type::DrawRRect;
        DrawRRect(const SkRRect& rrect, const SkPaint& paint) : rrect(rrect), paint(paint) {}
        SkRRect rrect;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawRRect(rrect, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawDRRect final : Op {
        static const auto kType = Type::DrawDRRect;
        DrawDRRect(const SkRRect& outer, const SkRRect& inner, const SkPaint& paint)
            : outer(outer), inner(inner), paint(paint) {}
        SkRRect outer, inner;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawDRRect(outer, inner, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };

    struct DrawAnnotation final : Op {
        static const auto kType = Type::DrawAnnotation;
        DrawAnnotation(const SkRect& rect, SkData* value) : rect(rect), value(sk_ref_sp(value)) {}
        SkRect        rect;
        sk_sp<SkData> value;
        void draw(SkCanvas* c) { c->drawAnnotation(rect, pod<char>(this), value.get()); }
    };
    struct DrawDrawable final : Op {
        static const auto kType = Type::DrawDrawable;
        DrawDrawable(SkDrawable* drawable, const SkMatrix* matrix) : drawable(sk_ref_sp(drawable)) {
            if (matrix) { this->matrix = *matrix; }
        }
        sk_sp<SkDrawable>      drawable;
        sk_sp<const SkPicture> snapped;
        SkMatrix               matrix = SkMatrix::I();
        void draw(SkCanvas* c) {
            snapped ? c->drawPicture(snapped.get(), &matrix, nullptr)
                    : c->drawDrawable(drawable.get(), &matrix);
        }
        void makeThreadsafe() {
            snapped.reset(drawable->newPictureSnapshot());
            make_threadsafe(nullptr, &matrix);
        }
    };
    struct DrawPicture final : Op {
        static const auto kType = Type::DrawPicture;
        DrawPicture(const SkPicture* picture, const SkMatrix* matrix, const SkPaint* paint)
            : picture(sk_ref_sp(picture)) {
            if (matrix) { this->matrix = *matrix; }
            if (paint)  { this->paint  = *paint; has_paint = true; }
        }
        sk_sp<const SkPicture> picture;
        SkMatrix               matrix = SkMatrix::I();
        SkPaint                paint;
        bool                   has_paint = false;  // TODO: why is a default paint not the same?
        void draw(SkCanvas* c) {
            c->drawPicture(picture.get(), &matrix, has_paint ? &paint : nullptr);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
        void makeThreadsafe() { make_threadsafe(nullptr, &matrix); }
    };
    struct DrawShadowedPicture final : Op {
        static const auto kType = Type::DrawShadowedPicture;
        DrawShadowedPicture(const SkPicture* picture, const SkMatrix* matrix, const SkPaint* paint)
            : picture(sk_ref_sp(picture)) {
            if (matrix) { this->matrix = *matrix; }
            if (paint)  { this->paint  = *paint;  }
        }
        sk_sp<const SkPicture> picture;
        SkMatrix               matrix = SkMatrix::I();
        SkPaint                paint;
        void draw(SkCanvas* c) {
        #ifdef SK_EXPERIMENTAL_SHADOWING
            c->drawShadowedPicture(picture.get(), &matrix, &paint);
        #endif
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
        void makeThreadsafe() { make_threadsafe(nullptr, &matrix); }
    };

    struct DrawImage final : Op {
        static const auto kType = Type::DrawImage;
        DrawImage(sk_sp<const SkImage>&& image, SkScalar x, SkScalar y, const SkPaint* paint)
            : image(std::move(image)), x(x), y(y) {
            if (paint) { this->paint = *paint; }
        }
        sk_sp<const SkImage> image;
        SkScalar x,y;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawImage(image.get(), x,y, &paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint, &image); }
    };
    struct DrawImageNine final : Op {
        static const auto kType = Type::DrawImageNine;
        DrawImageNine(sk_sp<const SkImage>&& image,
                      const SkIRect& center, const SkRect& dst, const SkPaint* paint)
            : image(std::move(image)), center(center), dst(dst) {
            if (paint) { this->paint = *paint; }
        }
        sk_sp<const SkImage> image;
        SkIRect center;
        SkRect  dst;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawImageNine(image.get(), center, dst, &paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint, &image); }
    };
    struct DrawImageRect final : Op {
        static const auto kType = Type::DrawImageRect;
        DrawImageRect(sk_sp<const SkImage>&& image, const SkRect* src, const SkRect& dst,
                      const SkPaint* paint, SkCanvas::SrcRectConstraint constraint)
            : image(std::move(image)), dst(dst), constraint(constraint) {
            this->src = src ? *src : SkRect::MakeIWH(image->width(), image->height());
            if (paint) { this->paint = *paint; }
        }
        sk_sp<const SkImage> image;
        SkRect src, dst;
        SkPaint paint;
        SkCanvas::SrcRectConstraint constraint;
        void draw(SkCanvas* c) {
            c->drawImageRect(image.get(), src, dst, &paint, constraint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint, &image); }
    };
    struct DrawImageLattice final : Op {
        static const auto kType = Type::DrawImageLattice;
        DrawImageLattice(sk_sp<const SkImage>&& image, int xs, int ys,
                         const SkRect& dst, const SkPaint* paint)
            : image(std::move(image)), xs(xs), ys(ys), dst(dst) {
            if (paint) { this->paint = *paint; }
        }
        sk_sp<const SkImage> image;
        int                  xs, ys;
        SkRect               dst;
        SkPaint              paint;
        void draw(SkCanvas* c) {
            auto xdivs = pod<int>(this, 0),
                 ydivs = pod<int>(this, xs*sizeof(int));
            c->drawImageLattice(image.get(), {xdivs, xs, ydivs, ys}, dst, &paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint, &image); }
    };

    struct DrawText final : Op {
        static const auto kType = Type::DrawText;
        DrawText(size_t bytes, SkScalar x, SkScalar y, const SkPaint& paint)
            : bytes(bytes), x(x), y(y), paint(paint) {}
        size_t bytes;
        SkScalar x,y;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawText(pod<void>(this), bytes, x,y, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawPosText final : Op {
        static const auto kType = Type::DrawPosText;
        DrawPosText(size_t bytes, const SkPaint& paint, int n)
            : bytes(bytes), paint(paint), n(n) {}
        size_t bytes;
        SkPaint paint;
        int n;
        void draw(SkCanvas* c) {
            auto points = pod<SkPoint>(this);
            auto text   = pod<void>(this, n*sizeof(SkPoint));
            c->drawPosText(text, bytes, points, paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawPosTextH final : Op {
        static const auto kType = Type::DrawPosTextH;
        DrawPosTextH(size_t bytes, SkScalar y, const SkPaint& paint, int n)
            : bytes(bytes), y(y), paint(paint), n(n) {}
        size_t   bytes;
        SkScalar y;
        SkPaint  paint;
        int n;
        void draw(SkCanvas* c) {
            auto xs   = pod<SkScalar>(this);
            auto text = pod<void>(this, n*sizeof(SkScalar));
            c->drawPosTextH(text, bytes, xs, y, paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawTextOnPath final : Op {
        static const auto kType = Type::DrawTextOnPath;
        DrawTextOnPath(size_t bytes, const SkPath& path,
                       const SkMatrix* matrix, const SkPaint& paint)
            : bytes(bytes), path(path), paint(paint) {
            if (matrix) { this->matrix = *matrix; }
        }
        size_t   bytes;
        SkPath   path;
        SkMatrix matrix = SkMatrix::I();
        SkPaint  paint;
        void draw(SkCanvas* c) {
            c->drawTextOnPath(pod<void>(this), bytes, path, &matrix, paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
        void makeThreadsafe() { make_threadsafe(&path, &matrix); }
    };
    struct DrawTextRSXform final : Op {
        static const auto kType = Type::DrawTextRSXform;
        DrawTextRSXform(size_t bytes, const SkRect* cull, const SkPaint& paint)
            : bytes(bytes), paint(paint) {
            if (cull) { this->cull = *cull; }
        }
        size_t  bytes;
        SkRect  cull = kUnset;
        SkPaint paint;
        void draw(SkCanvas* c) {
            c->drawTextRSXform(pod<void>(this), bytes, pod<SkRSXform>(this, bytes),
                               maybe_unset(cull), paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawTextBlob final : Op {
        static const auto kType = Type::DrawTextBlob;
        DrawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y, const SkPaint& paint)
            : blob(sk_ref_sp(blob)), x(x), y(y), paint(paint) {}
        sk_sp<const SkTextBlob> blob;
        SkScalar x,y;
        SkPaint paint;
        void draw(SkCanvas* c) { c->drawTextBlob(blob.get(), x,y, paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };

    struct DrawPatch final : Op {
        static const auto kType = Type::DrawPatch;
        DrawPatch(const SkPoint cubics[12], const SkColor colors[4], const SkPoint texs[4],
                  SkXfermode* xfermode, const SkPaint& paint)
            : xfermode(sk_ref_sp(xfermode)), paint(paint) {
            copy_v(this->cubics, cubics, 12);
            if (colors) { copy_v(this->colors, colors, 4); has_colors = true; }
            if (texs  ) { copy_v(this->texs  , texs  , 4); has_texs   = true; }
        }
        SkPoint           cubics[12];
        SkColor           colors[4];
        SkPoint           texs[4];
        sk_sp<SkXfermode> xfermode;
        SkPaint           paint;
        bool              has_colors = false;
        bool              has_texs   = false;
        void draw(SkCanvas* c) {
            c->drawPatch(cubics, has_colors ? colors : nullptr, has_texs ? texs : nullptr,
                         xfermode.get(), paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawPoints final : Op {
        static const auto kType = Type::DrawPoints;
        DrawPoints(SkCanvas::PointMode mode, size_t count, const SkPaint& paint)
            : mode(mode), count(count), paint(paint) {}
        SkCanvas::PointMode mode;
        size_t              count;
        SkPaint             paint;
        void draw(SkCanvas* c) { c->drawPoints(mode, count, pod<SkPoint>(this), paint); }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawVertices final : Op {
        static const auto kType = Type::DrawVertices;
        DrawVertices(SkCanvas::VertexMode mode, int count, SkXfermode* xfermode, int nindices,
                     const SkPaint& paint, bool has_texs, bool has_colors, bool has_indices)
            : mode(mode), count(count), xfermode(sk_ref_sp(xfermode)), nindices(nindices)
            , paint(paint), has_texs(has_texs), has_colors(has_colors), has_indices(has_indices) {}
        SkCanvas::VertexMode mode;
        int                  count;
        sk_sp<SkXfermode>    xfermode;
        int                  nindices;
        SkPaint              paint;
        bool                 has_texs;
        bool                 has_colors;
        bool                 has_indices;
        void draw(SkCanvas* c) {
            SkPoint* vertices = pod<SkPoint>(this, 0);
            size_t offset = count*sizeof(SkPoint);

            SkPoint* texs = nullptr;
            if (has_texs) {
                texs = pod<SkPoint>(this, offset);
                offset += count*sizeof(SkPoint);
            }

            SkColor* colors = nullptr;
            if (has_colors) {
                colors = pod<SkColor>(this, offset);
                offset += count*sizeof(SkColor);
            }

            uint16_t* indices = nullptr;
            if (has_indices) {
                indices = pod<uint16_t>(this, offset);
            }
            c->drawVertices(mode, count, vertices, texs, colors, xfermode.get(),
                            indices, nindices, paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint); }
    };
    struct DrawAtlas final : Op {
        static const auto kType = Type::DrawAtlas;
        DrawAtlas(const SkImage* atlas, int count, SkXfermode::Mode xfermode,
                  const SkRect* cull, const SkPaint* paint, bool has_colors)
            : atlas(sk_ref_sp(atlas)), count(count), xfermode(xfermode), has_colors(has_colors) {
            if (cull)  { this->cull  = *cull; }
            if (paint) { this->paint = *paint; }
        }
        sk_sp<const SkImage> atlas;
        int                  count;
        SkXfermode::Mode     xfermode;
        SkRect               cull = kUnset;
        SkPaint              paint;
        bool                 has_colors;
        void draw(SkCanvas* c) {
            auto xforms = pod<SkRSXform>(this, 0);
            auto   texs = pod<SkRect>(this, count*sizeof(SkRSXform));
            auto colors = has_colors
                ? pod<SkColor>(this, count*(sizeof(SkRSXform) + sizeof(SkRect)))
                : nullptr;
            c->drawAtlas(atlas.get(), xforms, texs, colors, count, xfermode,
                         maybe_unset(cull), &paint);
        }
        void optimizeFor(GrContext* ctx) { optimize_for(ctx, &paint, &atlas); }
    };
}

template <typename T, typename... Args>
void* SkLiteDL::push(size_t pod, Args&&... args) {
    size_t skip = SkAlignPtr(sizeof(T) + pod);
    SkASSERT(skip < (1<<24));
    if (fUsed + skip > fReserved) {
        fReserved = (fUsed + skip + 4096) & ~4095;  // Next greater multiple of 4096.
        fBytes.realloc(fReserved);
    }
    SkASSERT(fUsed + skip <= fReserved);
    auto op = (T*)(fBytes.get() + fUsed);
    fUsed += skip;
    new (op) T{ std::forward<Args>(args)... };
    op->type = (uint32_t)T::kType;
    op->skip = skip;
    return op+1;
}

template <typename... Args>
inline void SkLiteDL::map(void (*const fns[])(void*, Args...), Args... args) {
    auto end = fBytes.get() + fUsed;
    for (uint8_t* ptr = fBytes.get(); ptr < end; ) {
        auto op = (Op*)ptr;
        auto type = op->type;
        auto skip = op->skip;
        if (auto fn = fns[type]) {  // We replace no-op functions with nullptrs
            fn(op, args...);        // to avoid the overhead of a pointless call.
        }
        ptr += skip;
    }
}

void SkLiteDL::   save() { this->push   <Save>(0); }
void SkLiteDL::restore() { this->push<Restore>(0); }
void SkLiteDL::saveLayer(const SkRect* bounds, const SkPaint* paint,
                         const SkImageFilter* backdrop, SkCanvas::SaveLayerFlags flags) {
    this->push<SaveLayer>(0, bounds, paint, backdrop, flags);
}

void SkLiteDL::   concat(const SkMatrix& matrix) { this->push   <Concat>(0, matrix); }
void SkLiteDL::setMatrix(const SkMatrix& matrix) { this->push<SetMatrix>(0, matrix); }
void SkLiteDL::translateZ(SkScalar dz) { this->push<TranslateZ>(0, dz); }

void SkLiteDL::clipPath(const SkPath& path, SkRegion::Op op, bool aa) {
    this->push<ClipPath>(0, path, op, aa);
}
void SkLiteDL::clipRect(const SkRect& rect, SkRegion::Op op, bool aa) {
    this->push<ClipRect>(0, rect, op, aa);
}
void SkLiteDL::clipRRect(const SkRRect& rrect, SkRegion::Op op, bool aa) {
    this->push<ClipRRect>(0, rrect, op, aa);
}
void SkLiteDL::clipRegion(const SkRegion& region, SkRegion::Op op) {
    this->push<ClipRegion>(0, region, op);
}

void SkLiteDL::drawPaint(const SkPaint& paint) {
    this->push<DrawPaint>(0, paint);
}
void SkLiteDL::drawPath(const SkPath& path, const SkPaint& paint) {
    this->push<DrawPath>(0, path, paint);
}
void SkLiteDL::drawRect(const SkRect& rect, const SkPaint& paint) {
    this->push<DrawRect>(0, rect, paint);
}
void SkLiteDL::drawOval(const SkRect& oval, const SkPaint& paint) {
    this->push<DrawOval>(0, oval, paint);
}
void SkLiteDL::drawRRect(const SkRRect& rrect, const SkPaint& paint) {
    this->push<DrawRRect>(0, rrect, paint);
}
void SkLiteDL::drawDRRect(const SkRRect& outer, const SkRRect& inner, const SkPaint& paint) {
    this->push<DrawDRRect>(0, outer, inner, paint);
}

void SkLiteDL::drawAnnotation(const SkRect& rect, const char* key, SkData* value) {
    size_t bytes = strlen(key)+1;
    void* pod = this->push<DrawAnnotation>(bytes, rect, value);
    copy_v(pod, key,bytes);
}
void SkLiteDL::drawDrawable(SkDrawable* drawable, const SkMatrix* matrix) {
    this->push<DrawDrawable>(0, drawable, matrix);
}
void SkLiteDL::drawPicture(const SkPicture* picture,
                           const SkMatrix* matrix, const SkPaint* paint) {
    this->push<DrawPicture>(0, picture, matrix, paint);
}
void SkLiteDL::drawShadowedPicture(const SkPicture* picture,
                                   const SkMatrix* matrix, const SkPaint* paint) {
    this->push<DrawShadowedPicture>(0, picture, matrix, paint);
}

void SkLiteDL::drawBitmap(const SkBitmap& bm, SkScalar x, SkScalar y, const SkPaint* paint) {
    this->push<DrawImage>(0, SkImage::MakeFromBitmap(bm), x,y, paint);
}
void SkLiteDL::drawBitmapNine(const SkBitmap& bm, const SkIRect& center,
                              const SkRect& dst, const SkPaint* paint) {
    this->push<DrawImageNine>(0, SkImage::MakeFromBitmap(bm), center, dst, paint);
}
void SkLiteDL::drawBitmapRect(const SkBitmap& bm, const SkRect* src, const SkRect& dst,
                              const SkPaint* paint, SkCanvas::SrcRectConstraint constraint) {
    this->push<DrawImageRect>(0, SkImage::MakeFromBitmap(bm), src, dst, paint, constraint);
}

void SkLiteDL::drawImage(const SkImage* image, SkScalar x, SkScalar y, const SkPaint* paint) {
    this->push<DrawImage>(0, sk_ref_sp(image), x,y, paint);
}
void SkLiteDL::drawImageNine(const SkImage* image, const SkIRect& center,
                             const SkRect& dst, const SkPaint* paint) {
    this->push<DrawImageNine>(0, sk_ref_sp(image), center, dst, paint);
}
void SkLiteDL::drawImageRect(const SkImage* image, const SkRect* src, const SkRect& dst,
                             const SkPaint* paint, SkCanvas::SrcRectConstraint constraint) {
    this->push<DrawImageRect>(0, sk_ref_sp(image), src, dst, paint, constraint);
}
void SkLiteDL::drawImageLattice(const SkImage* image, const SkCanvas::Lattice& lattice,
                                const SkRect& dst, const SkPaint* paint) {
    int xs = lattice.fXCount, ys = lattice.fYCount;
    size_t bytes = (xs + ys) * sizeof(int);
    void* pod = this->push<DrawImageLattice>(bytes, sk_ref_sp(image), xs, ys, dst, paint);
    copy_v(pod, lattice.fXDivs, xs,
                lattice.fYDivs, ys);
}

void SkLiteDL::drawText(const void* text, size_t bytes,
                        SkScalar x, SkScalar y, const SkPaint& paint) {
    void* pod = this->push<DrawText>(bytes, bytes, x, y, paint);
    copy_v(pod, (const char*)text,bytes);
}
void SkLiteDL::drawPosText(const void* text, size_t bytes,
                           const SkPoint pos[], const SkPaint& paint) {
    int n = paint.countText(text, bytes);
    void* pod = this->push<DrawPosText>(n*sizeof(SkPoint)+bytes, bytes, paint, n);
    copy_v(pod, pos,n, (const char*)text,bytes);
}
void SkLiteDL::drawPosTextH(const void* text, size_t bytes,
                           const SkScalar xs[], SkScalar y, const SkPaint& paint) {
    int n = paint.countText(text, bytes);
    void* pod = this->push<DrawPosTextH>(n*sizeof(SkScalar)+bytes, bytes, y, paint, n);
    copy_v(pod, xs,n, (const char*)text,bytes);
}
void SkLiteDL::drawTextOnPath(const void* text, size_t bytes,
                              const SkPath& path, const SkMatrix* matrix, const SkPaint& paint) {
    void* pod = this->push<DrawTextOnPath>(bytes, bytes, path, matrix, paint);
    copy_v(pod, (const char*)text,bytes);
}
void SkLiteDL::drawTextRSXform(const void* text, size_t bytes,
                               const SkRSXform xforms[], const SkRect* cull, const SkPaint& paint) {
    int n = paint.countText(text, bytes);
    void* pod = this->push<DrawTextRSXform>(bytes+n*sizeof(SkRSXform), bytes, cull, paint);
    copy_v(pod, (const char*)text,bytes, xforms,n);
}
void SkLiteDL::drawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y, const SkPaint& paint) {
    this->push<DrawTextBlob>(0, blob, x,y, paint);
}

void SkLiteDL::drawPatch(const SkPoint points[12], const SkColor colors[4], const SkPoint texs[4],
                         SkXfermode* xfermode, const SkPaint& paint) {
    this->push<DrawPatch>(0, points, colors, texs, xfermode, paint);
}
void SkLiteDL::drawPoints(SkCanvas::PointMode mode, size_t count, const SkPoint points[],
                          const SkPaint& paint) {
    void* pod = this->push<DrawPoints>(count*sizeof(SkPoint), mode, count, paint);
    copy_v(pod, points,count);
}
void SkLiteDL::drawVertices(SkCanvas::VertexMode mode, int count, const SkPoint vertices[],
                            const SkPoint texs[], const SkColor colors[], SkXfermode* xfermode,
                            const uint16_t indices[], int nindices, const SkPaint& paint) {
    size_t bytes = count * sizeof(SkPoint);
    if (texs  )  { bytes += count    * sizeof(SkPoint); }
    if (colors)  { bytes += count    * sizeof(SkColor); }
    if (indices) { bytes += nindices * sizeof(uint16_t); }
    void* pod = this->push<DrawVertices>(bytes, mode, count, xfermode, nindices, paint,
                                         texs != nullptr, colors != nullptr, indices != nullptr);
    copy_v(pod, vertices, count,
                    texs, texs    ? count    : 0,
                  colors, colors  ? count    : 0,
                 indices, indices ? nindices : 0);
}
void SkLiteDL::drawAtlas(const SkImage* atlas, const SkRSXform xforms[], const SkRect texs[],
                         const SkColor colors[], int count, SkXfermode::Mode xfermode,
                         const SkRect* cull, const SkPaint* paint) {
    size_t bytes = count*(sizeof(SkRSXform) + sizeof(SkRect));
    if (colors) {
        bytes += count*sizeof(SkColor);
    }
    void* pod = this->push<DrawAtlas>(bytes,
                                      atlas, count, xfermode, cull, paint, colors != nullptr);
    copy_v(pod, xforms, count,
                  texs, count,
                colors, colors ? count : 0);
}

typedef void(* skcanvas_fn)(void*,  SkCanvas*);
typedef void(*grcontext_fn)(void*, GrContext*);
typedef void(*     void_fn)(void*);

// All ops implement draw().
#define M(T) [](void* op, SkCanvas* c) { ((T*)op)->draw(c); },
static const skcanvas_fn draw_fns[] = { TYPES(M) };
#undef M

// Ops that implement optimizeFor() or makeThreadsafe() return void from those functions;
// the (throwing) defaults return int.
#define M(T) std::is_void<decltype(((T*)nullptr)->optimizeFor(nullptr))>::value \
    ? [](void* op, GrContext* ctx) { ((T*)op)->optimizeFor(ctx); } : (grcontext_fn)nullptr,
static const grcontext_fn optimize_for_fns[] = { TYPES(M) };
#undef M

#define M(T) std::is_void<decltype(((T*)nullptr)->makeThreadsafe())>::value \
    ? [](void* op) { ((T*)op)->makeThreadsafe(); } : (void_fn)nullptr,
static const void_fn make_threadsafe_fns[] = { TYPES(M) };
#undef M

// Most state ops (matrix, clip, save, restore) have a trivial destructor.
#define M(T) !std::is_trivially_destructible<T>::value \
    ? [](void* op) { ((T*)op)->~T(); } : (void_fn)nullptr,
static const void_fn dtor_fns[] = { TYPES(M) };
#undef M

void SkLiteDL::onDraw        (SkCanvas* canvas) { this->map(draw_fns, canvas); }
void SkLiteDL::optimizeFor   (GrContext* ctx)   { this->map(optimize_for_fns, ctx); }
void SkLiteDL::makeThreadsafe()                 { this->map(make_threadsafe_fns); }

SkRect SkLiteDL::onGetBounds() {
    return fBounds;
}

#if !defined(SK_LITEDL_USES)
    #define  SK_LITEDL_USES 16
#endif

SkLiteDL:: SkLiteDL() : fUsed(0), fReserved(0), fUsesRemaining(SK_LITEDL_USES) {}
SkLiteDL::~SkLiteDL() {}

// If you're tempted to make this lock free, please don't forget about ABA.
static SkSpinlock gFreeStackLock;
static SkLiteDL*  gFreeStack = nullptr;

sk_sp<SkLiteDL> SkLiteDL::New(SkRect bounds) {
    sk_sp<SkLiteDL> dl;
    {
        SkAutoMutexAcquire lock(gFreeStackLock);
        if (gFreeStack) {
            dl.reset(gFreeStack);  // Adopts the ref the stack's been holding.
            gFreeStack = gFreeStack->fNext;
        }
    }

    if (!dl) {
        dl.reset(new SkLiteDL);
    }

    dl->fBounds = bounds;
    return dl;
}

void SkLiteDL::internal_dispose() const {
    // Whether we delete this or leave it on the free stack,
    // we want its refcnt at 1.
    this->internal_dispose_restore_refcnt_to_1();

    auto self = const_cast<SkLiteDL*>(this);
    self->map(dtor_fns);

    if (--self->fUsesRemaining > 0) {
        self->fUsed = 0;

        SkAutoMutexAcquire lock(gFreeStackLock);
        self->fNext = gFreeStack;
        gFreeStack = self;
        return;
    }

    delete this;
}

void SkLiteDL::PurgeFreelist() {
    SkAutoMutexAcquire lock(gFreeStackLock);
    while (gFreeStack) {
        SkLiteDL* top = gFreeStack;
        gFreeStack = gFreeStack->fNext;
        delete top;   // Calling unref() here would just put it back on the list!
    }
}
