/*
Copyright (c) 2012 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <limits>


#include "oslexec_pvt.h"
#include <OSL/oslnoise.h>
#include <OSL/dual_vec.h>
#include <OSL/Imathx.h>

#include <OpenImageIO/fmath.h>

OSL_NAMESPACE_ENTER

namespace pvt {


static const float Gabor_Frequency = 2.0;
//static const float Gabor_Impulse_Weight = 1;
static constexpr float Gabor_Impulse_Weight = 1.0f;

// The Gabor kernel in theory has infinite support (its envelope is
// a Gaussian).  To restrict the distance at which we must sum the
// kernels, we only consider those whose Gaussian envelopes are
// above the truncation threshold, as a portion of the Gaussian's
// peak value.
static const float Gabor_Truncate = 0.02f;



// Very fast random number generator based on [Borosh & Niederreiter 1983]
// linear congruential generator.
class fast_rng {
public:
    // seed based on the cell containing P
    fast_rng (const Vec3 &p, int seed=0) {
        // Use guts of cellnoise
        unsigned int pi[4] = { unsigned(quick_floor(p[0])),
                               unsigned(quick_floor(p[1])),
                               unsigned(quick_floor(p[2])),
                               unsigned(seed) };
        m_seed = inthash<4>(pi);
        if (! m_seed)
            m_seed = 1;
    }
    // Return uniform on [0,1)
    float operator() () {
        return (m_seed *= 3039177861u) / float(UINT_MAX);
    }
    // Return poisson distribution with the given mean
    int poisson (float mean) {
        float g = expf (-mean);
        unsigned int em = 0;
        float t = (*this)();
        while (t > g) {
            ++em;
            t *= (*this)();
        }
        return em;
    }
private:
    unsigned int m_seed;
};

// The Gabor kernel is a harmonic (cosine) modulated by a Gaussian
// envelope.  This version is augmented with a phase, per [Lagae2011].
//   \param  weight      magnitude of the pulse
//   \param  omega       orientation of the harmonic
//   \param  phi         phase of the harmonic.
//   \param  bandwidth   width of the gaussian envelope (called 'a'
//                          in [Lagae09].
//   \param  x           the position being sampled
template <class VEC>   // VEC should be Vec3 or Vec2
inline Dual2<float>
gabor_kernel (const Dual2<float> &weight, const VEC &omega,
              const Dual2<float> &phi, float bandwidth, const Dual2<VEC> &x)
{
    // see Equation 1
    Dual2<float> g = exp (float(-M_PI) * (bandwidth * bandwidth) * dot(x,x));
    Dual2<float> h = cos (float(M_TWO_PI) * dot(omega,x) + phi);
    return weight * g * h;
}



inline void
slice_gabor_kernel_3d (const Dual2<float> &d, float w, float a,
                       const Vec3 &omega, float phi,
                       Dual2<float> &w_s, Vec2 &omega_s, Dual2<float> &phi_s)
{
    // Equation 6
    w_s = w * exp(float(-M_PI) * (a*a)*(d*d));
    //omega_s[0] = omega[0];
    //omega_s[1] = omega[1];
    //phi_s = phi - float(M_TWO_PI) * d * omega[2];
    omega_s.x = omega.x;
    omega_s.y = omega.y;
    phi_s = phi - float(M_TWO_PI) * d * omega.x;
}


static void
filter_gabor_kernel_2d (const Matrix22 &filter, const Dual2<float> &w, float a,
                        const Vec2 &omega, const Dual2<float> &phi,
                        Dual2<float> &w_f, float &a_f,
                        Vec2 &omega_f, Dual2<float> &phi_f)
{
    //  Equation 10
    Matrix22 Sigma_f = filter;
    Dual2<float> c_G = w;
    Vec2 mu_G = omega;
    Matrix22 Sigma_G = (a * a / float(M_TWO_PI)) * Matrix22();
    float c_F = 1.0f / (float(M_TWO_PI) * sqrtf(determinant(Sigma_f)));
    Matrix22 Sigma_F = float(1.0 / (4.0 * M_PI * M_PI)) * Sigma_f.inverse();
    Matrix22 Sigma_G_Sigma_F = Sigma_G + Sigma_F;
    Dual2<float> c_GF = c_F * c_G
        * (1.0f / (float(M_TWO_PI) * sqrtf(determinant(Sigma_G_Sigma_F))))
        * expf(-0.5f * dot(Sigma_G_Sigma_F.inverse()*mu_G, mu_G));
    Matrix22 Sigma_G_i = Sigma_G.inverse();
    Matrix22 Sigma_GF = (Sigma_F.inverse() + Sigma_G_i).inverse();
    Vec2 mu_GF;
    Matrix22 Sigma_GF_Gi = Sigma_GF * Sigma_G_i;
    Sigma_GF_Gi.multMatrix (mu_G, mu_GF);
    w_f = c_GF;
    a_f = sqrtf(M_TWO_PI * sqrtf(determinant(Sigma_GF)));
    omega_f = mu_GF;
    phi_f = phi;
}


inline float
wrap (float s, float period)
{
    period = floorf (period);
    if (period < 1.0f)
        period = 1.0f;
    return s - period * floorf (s / period);
}



static Vec3
wrap (const Vec3 &s, const Vec3 &period)
{
    return Vec3 (wrap (s[0], period[0]),
                 wrap (s[1], period[1]),
                 wrap (s[2], period[2]));
}


// Normalize v and set a and b to be unit vectors (any two unit vectors)
// that are orthogonal to v and each other.  We get the first
// orthonormal by taking the cross product of v and (1,0,0), unless v
// points roughly toward (1,0,0), in which case we cross with (0,1,0).
// Either way, we get something orthogonal.  Then cross(v,a) is mutually
// orthogonal to the other two.
inline void
make_orthonormals (Vec3 &v, Vec3 &a, Vec3 &b)
{
    v.normalize();
    if (fabsf(v[0]) < 0.9f)
	a.setValue (0.0f, v[2], -v[1]);   // v X (1,0,0)
    else
        a.setValue (-v[2], 0.0f, v[0]);   // v X (0,1,0)
    a.normalize ();
    b = v.cross (a);
//    b.normalize ();  // note: not necessary since v is unit length
}



// Helper function: per-component 'floor' of a Dual2<Vec3>.
inline Vec3
floor (const Dual2<Vec3> &vd)
{
    const Vec3 &v (vd.val());
    return Vec3 (floorf(v[0]), floorf(v[1]), floorf(v[2]));
}


struct DisabledFilterPolicy
{
	static constexpr bool active = false;
};

struct EnabledFilterPolicy
{
	static constexpr bool active = true;
};


// Foward declaration, implementation is in fast_gabor.h
template<int AnisotropicT, typename FilterPolicyT, int WidthT>
__attribute__((noinline)) void
fast_gabor (
		ConstWideAccessor<Dual2<Vec3>,WidthT> wP,
		WideAccessor<Dual2<float>,WidthT> wResult,
		NoiseParams const *opt);

// Foward declaration, implementation is in fast_gabor.h
template<int AnisotropicT, typename FilterPolicyT, int WidthT>
__attribute__((noinline)) void
fast_gabor3 (
		ConstWideAccessor<Dual2<Vec3>, WidthT> wP,
		WideAccessor<Dual2<Vec3>,WidthT> wResult,
		NoiseParams const *opt);

} // namespace pvt

OSL_NAMESPACE_EXIT
