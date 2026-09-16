// Perspective-correct textured 3D quads.
//
// Shared by both backends on purpose. The projection and the subdivision are
// pure geometry that end in addVertex, which both Renderer.cpp (deko3d) and
// Renderer_sdl2.cpp already implement, so there is nothing backend-specific
// left to write twice. Keeping one copy is also the only way the deko3d and
// SDL2/simulator paths can be guaranteed to agree: a second implementation
// would be free to drift, and the simulator would stop predicting the console.
//
// Implements requirements C1, C2 and Q1-Q6 of the Flow requirements
// specification (integration/flow/FLOW_REQUIREMENTS.md in the integration
// workspace).

#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <algorithm>
#include <cmath>

namespace nxui {

namespace {
    // Floor for the depths that drive the perspective seam parameter. Separate
    // from kNearZ: this one only keeps a reciprocal finite.
    constexpr float kMinZ = 0.001f;

    // Vertical subdivision is chosen from how much the quad's depth varies.
    // A quad square-on to the camera has no projective error to correct, so it
    // stays a single row and costs nothing. (Q2)
    constexpr float kRowsPerUnitBend = 40.f;
    constexpr int   kMaxRows = 16;
    constexpr int   kMaxStrips = 32;
}

// (C1) Pinhole projection. Camera at the origin looking down +Z, Y up in view
// space, origin of the output at the top-left of the layout rectangle. Invalid
// input is rejected; callers must not receive a clamped giant trapezoid for a
// point that crossed the camera.
bool Renderer::project3D(const Vec3& p, Vec2& out) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
        !std::isfinite(p.z) || p.z < kNearZ) return false;
    out = {
        (float)width()  * 0.5f + p.x * kFocal / p.z,
        (float)height() * 0.5f - p.y * kFocal / p.z,   // note the minus: y is up
    };
    return std::isfinite(out.x) && std::isfinite(out.y);
}

void Renderer::drawQuad3D(const Texture* tex, const Vec3 corners[4], const Color& tint,
                          float alphaTop, float alphaBottom,
                          bool flipV, int strips, const float uv[4])
{
    if (!corners) return;

    // Reject rather than clamp geometry that crosses the near plane. Clamping
    // is useful for project3D as a last-resort safety guard, but accepting one
    // point behind the eye would turn an invalid face into a huge trapezoid.
    // Non-finite values are rejected for the same reason. (C1, acceptance §6)
    for (const Vec3& p : {corners[0], corners[1], corners[2], corners[3]}) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.z) || p.z < kNearZ) return;
    }
    if (!std::isfinite(tint.r) || !std::isfinite(tint.g) ||
        !std::isfinite(tint.b) || !std::isfinite(tint.a) ||
        !std::isfinite(alphaTop) || !std::isfinite(alphaBottom)) return;
    for (const Vec3& p : {corners[0], corners[1], corners[2], corners[3]}) {
        Vec2 projected;
        if (!project3D(p, projected)) return;
    }

    strips = std::clamp(strips, 1, kMaxStrips);

    const float u0 = uv ? uv[0] : 0.f;
    const float v0 = uv ? uv[1] : 0.f;
    const float u1 = uv ? uv[2] : 1.f;
    const float v1 = uv ? uv[3] : 1.f;
    if (!std::isfinite(u0) || !std::isfinite(v0) ||
        !std::isfinite(u1) || !std::isfinite(v1)) return;

    // corners: 0 = TL, 1 = TR, 2 = BR, 3 = BL.
    //
    // (Q3) Seam placement across the quad by perspective rather than by even
    // steps along the edge in 3D. Stepping the object-space parameter evenly
    // spends the subdivision in the wrong place: the near half of a turned face
    // covers far more of the screen than the far half, so its cells would be
    // several times wider and carry several times the error. For a fraction u
    // across the projected face the object-space parameter is
    //     t = (u/zR) / ((1-u)/zL + u/zR)
    // which is the standard perspective-correct inverse - interpolate 1/z
    // linearly in screen space, then divide it back out.
    const float zL = 0.5f * (corners[0].z + corners[3].z);
    const float zR = 0.5f * (corners[1].z + corners[2].z);
    const float zl = (zL > kMinZ) ? zL : kMinZ;
    const float zr = (zR > kMinZ) ? zR : kMinZ;
    auto param = [zl, zr](float u) {
        const float iz = (1.f - u) / zl + u / zr;
        if (iz <= 1e-6f) return u;
        return (u / zr) / iz;
    };

    // (Q2) Subdivide in BOTH directions, not just across.
    //
    // Strips alone fix the horizontal squeeze and leave the vertical error
    // untouched, and the vertical error is the one that shows: inside a strip
    // the two triangles interpolate v affinely across a trapezoid whose near
    // edge is taller than its far edge, so a horizontal line in the texture is
    // pulled off true by an amount that grows to the middle of the strip and
    // returns to zero at each seam. Repeated across the strips that is a
    // sawtooth, most obvious on flat artwork full of straight edges.
    //
    // Splitting each strip into rows as well makes every cell close to a
    // parallelogram, where affine and projective agree, and the error falls
    // away with the square of the cell size.
    const float zT = 0.5f * (corners[0].z + corners[1].z);
    const float zB = 0.5f * (corners[3].z + corners[2].z);
    const float zmin = std::min(std::min(zL, zR), std::min(zT, zB));
    const float zmax = std::max(std::max(zL, zR), std::max(zT, zB));

    int rows = 1;
    if (zmin > kMinZ) {
        const float bend = (zmax / zmin) - 1.f;
        rows = std::clamp((int)std::ceil(bend * kRowsPerUnitBend), 1, kMaxRows);
    }

    // One face is atomic. flush() starts a new GPU draw but never reclaims this
    // frame's vertex arena, so emitting until addVertex starts dropping would
    // tear the face and desynchronise the triangle stream. A worst-case face is
    // bounded at 32*16*6 = 3072 vertices; reject the whole thing up front when
    // the remaining capacity cannot hold it. (Renderer acceptance §6)
    const uint32_t required = (uint32_t)strips * (uint32_t)rows * 6u;
    if (required > GpuDevice::MAX_VERTICES - m_vtxCount) return;

    // Rounded-mask state must never leak into arbitrary geometry. addVertex
    // stamps the active mask on every vertex, so clear it for this run and
    // restore it afterwards in case a nested caller legitimately armed one.
    // Public draw paths normally leave it clear, but this makes the primitive
    // correct by construction rather than by convention. Nothing below this
    // point may return early before the restoration at the end.
    const Vec2 savedShapeCentre = m_shapeCentre;
    const Vec2 savedShapeHalf = m_shapeHalf;
    const float savedShapeRadius = m_shapeRadius;
    const float savedShapeThickness = m_shapeThickness;
    m_shapeCentre = {};
    m_shapeHalf = {};
    m_shapeRadius = 0.f;
    m_shapeThickness = 0.f;
    const EmitSiteScope emitSite{*this, EmitSite::Quad3D};

    // Install the surface once all validation and capacity checks have passed.
    // (Q1) A null/invalid texture is a flat-colour draw: deko3d's -1 slot binds
    // its built-in white image; SDL2 flush() substitutes m_whiteSdlTex.
#ifdef NXUI_BACKEND_DEKO3D
    bindTexture((tex && tex->valid()) ? tex->descriptorSlot() : -1);
#else
    SDL_Texture* nextTexture = (tex && tex->valid()) ? tex->sdlTexture() : nullptr;
    if (nextTexture != m_boundTex || (nextTexture != nullptr) != m_texturing) {
        flush();
        m_boundTex = nextTexture;
        m_texturing = (nextTexture != nullptr);
    }
#endif

    // The alpha gradient is applied to the tint rather than replacing it, so a
    // caller that already faded a surface keeps that fade. (Reflections pass
    // alphaBottom = 0 to sink into the floor.)
    const float aTop = std::clamp(tint.a * alphaTop, 0.f, 1.f);
    const float aBot = std::clamp(tint.a * alphaBottom, 0.f, 1.f);

    auto emit = [&](const Vec3& p, float u, float vtex, float a) {
        Vec2 projected;
        // Every point is bilinear between already-validated corners on one
        // planar face, so this cannot fail unless arithmetic itself overflows.
        // The corner projection preflight below rejects that before emission.
        (void)project3D(p, projected);
        const float vv = flipV ? (1.f - vtex) : vtex;
        addVertex(projected.x, projected.y,
                  u0 + (u1 - u0) * u,
                  v0 + (v1 - v0) * vv,
                  Color{tint.r, tint.g, tint.b, a});
    };

    for (int ry = 0; ry < rows; ++ry) {
        const float b0 = (float)ry / (float)rows;
        const float b1 = (float)(ry + 1) / (float)rows;
        const float a0 = aTop + (aBot - aTop) * b0;
        const float a1 = aTop + (aBot - aTop) * b1;

        for (int s = 0; s < strips; ++s) {
            const float t0 = param((float)s / (float)strips);
            const float t1 = param((float)(s + 1) / (float)strips);

            // The cell's four corners, bilinear in 3D. Four coplanar points
            // interpolate to a coplanar point, so every cell stays on the face;
            // this is the step that keeps the mapping honest.
            const Vec3 topA = Vec3::lerp(corners[0], corners[1], t0);
            const Vec3 topB = Vec3::lerp(corners[0], corners[1], t1);
            const Vec3 botA = Vec3::lerp(corners[3], corners[2], t0);
            const Vec3 botB = Vec3::lerp(corners[3], corners[2], t1);

            const Vec3 p00 = Vec3::lerp(topA, botA, b0);
            const Vec3 p01 = Vec3::lerp(topA, botA, b1);
            const Vec3 p10 = Vec3::lerp(topB, botB, b0);
            const Vec3 p11 = Vec3::lerp(topB, botB, b1);

            emit(p00, t0, b0, a0);
            emit(p10, t1, b0, a0);
            emit(p11, t1, b1, a1);

            emit(p00, t0, b0, a0);
            emit(p11, t1, b1, a1);
            emit(p01, t0, b1, a1);
        }
    }

    m_shapeCentre = savedShapeCentre;
    m_shapeHalf = savedShapeHalf;
    m_shapeRadius = savedShapeRadius;
    m_shapeThickness = savedShapeThickness;
}

} // namespace nxui
