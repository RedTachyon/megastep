#include <ATen/ATen.h>
#include <variant>
#include <exception>
#include <iostream>

using TT = at::Tensor;

#define CHECK_CUDA(x) AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) CHECK_CUDA(x); CHECK_CONTIGUOUS(x)

// Define our own copy of RestrictPtrTraits here, as the at::RestrictPtrTraits is 
// only included during NVCC compilation, not plain C++. This would mess things up 
// since this file is included on both the NVCC and Clang sides. 
template <typename T>
struct RestrictPtrTraits {
  typedef T* __restrict__ PtrType;
};

template<typename T>
at::ScalarType dtype() { return at::typeMetaToScalarType(caffe2::TypeMeta::Make<T>()); }

template <typename T, size_t D>
struct TensorProxy {

    using PTA = at::PackedTensorAccessor32<T, D, RestrictPtrTraits>;
    TT t; 

    TensorProxy(const at::Tensor t) : t(t) {
        CHECK_INPUT(t);
        AT_ASSERT(t.scalar_type() == dtype<T>());
        AT_ASSERT(t.ndimension() == D);
    }

    static TensorProxy<T, D> empty(at::IntArrayRef size) { return TensorProxy(at::empty(size, at::device(at::kCUDA).dtype(dtype<T>()))); }
    static TensorProxy<T, D> zeros(at::IntArrayRef size) { return TensorProxy(at::zeros(size, at::device(at::kCUDA).dtype(dtype<T>()))); }
    static TensorProxy<T, D> ones(at::IntArrayRef size) { return TensorProxy(at::ones(size, at::device(at::kCUDA).dtype(dtype<T>()))); }

    PTA pta() const { return t.packed_accessor32<T, D, RestrictPtrTraits>(); }

    size_t size(const size_t i) const { return t.size(i); }
};

template <typename T, size_t D>
struct RaggedPackedTensorAccessor {
    using IPTA = TensorProxy<int, 1>::PTA;
    using PTA = at::PackedTensorAccessor32<T, D, RestrictPtrTraits>;
    using TA = at::TensorAccessor<T, D, RestrictPtrTraits, int32_t>;

    PTA vals;
    const IPTA widths;
    const IPTA starts;
    const IPTA inverse;
    int32_t _sizes[D];
    int32_t _strides[D];

    RaggedPackedTensorAccessor(
        TT vals, TT widths, TT starts, TT inverse) :
        vals(vals.packed_accessor32<T, D, RestrictPtrTraits>()), 
        widths(widths.packed_accessor32<int, 1, RestrictPtrTraits>()), 
        starts(starts.packed_accessor32<int, 1, RestrictPtrTraits>()), 
        inverse(inverse.packed_accessor32<int, 1, RestrictPtrTraits>()) {

        for (auto d=0; d<D; ++d) {
            _sizes[d] = vals.size(d);
            _strides[d] = vals.stride(d);
        }
        // _sizes[0] is going to be wrong, but that's not used by the accessor mechanism. 
        // Alternative is to construct PTAs dynamically on the device, which would be Not Fast. 
        // Just to make sure we notice immediately if it's ever used, let's set it to an illegal value
        _sizes[0] = -1;
    }

    C10_HOST_DEVICE TA operator[](const int n) const {
        return TA(vals.data() + starts[n]*_strides[0], _sizes, _strides);
    }
    
    C10_HOST_DEVICE int64_t size(const int d) const { 
        return (d == 0) ? widths.size(0) : vals.size(d-1);
    }

};

#if defined(__CUDACC__)
TT inverses(const TT& widths);
#else
TT inverses(const TT& widths) {
    at::AutoNonVariableTypeMode nonvar{true};
    const auto starts = widths.cumsum(0) - widths.to(at::kLong);
    const auto flags = at::ones(starts.size(0), at::dtype(at::kInt).device(widths.device()));
    auto indices = at::zeros(widths.sum(0).item<int64_t>(), at::dtype(at::kInt).device(widths.device()));
    auto inverse = indices.scatter(0, starts, flags).cumsum(0).to(at::kInt)-1;
    return inverse;
}  
#endif


template <typename T, size_t D>
struct Ragged {
    const TT vals;
    const TT widths;
    const TT starts;
    const TT inverse;

    using PTA = RaggedPackedTensorAccessor<T, D>;

    Ragged(TT vals, TT widths, bool cuda=true) : 
        vals(vals), widths(widths), 
        starts(widths.cumsum(0).toType(at::kInt) - widths),
        inverse(inverses(widths)) { 
        
        if (cuda) {
            CHECK_INPUT(vals);
            CHECK_INPUT(widths);
        } else {
            CHECK_CONTIGUOUS(vals);
            CHECK_CONTIGUOUS(widths);
        }

        AT_ASSERT(widths.size(0) == starts.size(0));
        AT_ASSERT(widths.sum(0).item<int64_t>() == vals.size(0));
        AT_ASSERT(vals.size(0) == inverse.size(0));
    }

    PTA pta() const { return PTA(vals, widths, starts, inverse); }

    size_t size(const size_t i) const { return vals.size(i); }
};

using Centers = Ragged<float, 3>;
using Radii = Ragged<float, 2>;
using Lowers = Ragged<float, 2>;
using Uppers = Ragged<float, 2>;

struct Respawns {
    const Centers centers;
    const Radii radii;
    const Lowers lowers;
    const Uppers uppers;

    Respawns(TT centers, TT radii, TT lowers, TT uppers, TT widths) :
        centers(centers, widths), radii(radii, widths), lowers(lowers, widths), uppers(uppers, widths) { 

        AT_ASSERT(centers.size(0) == radii.size(0));
        AT_ASSERT(centers.size(0) == radii.size(0));
        AT_ASSERT(centers.size(0) == lowers.size(0));
        AT_ASSERT(centers.size(0) == uppers.size(0)); 
    }
};

using Angles = TensorProxy<float, 2>;
using Positions = TensorProxy<float, 3>;
using AngMomenta = TensorProxy<float, 2>;
using Momenta = TensorProxy<float, 3>;

struct Drones {
    Angles angles;
    Positions positions; 
    AngMomenta angmomenta;
    Momenta momenta; 
};

using Lights = Ragged<float, 2>;
using Lines = Ragged<float, 3>;
using Textures = Ragged<float, 2>;
using Baked = Ragged<float, 1>;
using Frame = TensorProxy<float, 3>;

struct Scene {
    const Lights lights;
    const Lines lines;
    const Frame frame;
    const Textures textures;
    const Baked baked;

    Scene(
        TT lights, TT lightwidths, 
        TT lines, TT linewidths, 
        TT textures, TT texwidths, 
        TT frame) :
        lights(lights, lightwidths), 
        lines(lines, linewidths),
        textures(textures, texwidths),
        // Weird initialization here is to avoid having to create a AutoNonVariableTypeMode 
        // guard, because I still don't understand the Variable/Tensor thing
        baked(1 + 0*textures.select(1, 0).clone(), texwidths),
        frame(frame) { }
};

struct Render {
    const TT indices;
    const TT locations;
    const TT dots;
    const TT distances;
    const TT screen;
};

using Submovement = TensorProxy<int, 2>;

struct Movement {
    const Submovement mesial;
    const Submovement lateral;
    const Submovement yaw;
};

void initialize(float, int, float);
void bake(Scene& scene, int D);
void respawn(const TT reset, const Respawns& respawns, Drones& drones);
void physics(const Movement movement, const Scene& scene, Drones& drones);
Render render(const Drones& drones, Scene& scene);