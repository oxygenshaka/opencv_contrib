#include "../precomp.hpp"
#include "opencv2/imgproc.hpp"
#include <opencv2/dnn/shape_utils.hpp>

namespace cv
{
namespace dnn
{

using std::abs;
using std::exp;
using std::tanh;
using std::pow;

template<typename Func>
class ElementWiseLayer : public Func::Layer
{
public:
    class PBody : public cv::ParallelLoopBody
    {
    public:
        const Func* func_;
        const Mat* src_;
        Mat* dst_;
        int nstripes_;

        PBody(const Func &func, const Mat &src, Mat& dst, int nstripes)
        {
            func_ = &func;
            src_ = &src;
            dst_ = &dst;
            nstripes_ = nstripes;
        }

        void operator()(const Range &r) const
        {
            int nstripes = nstripes_, nsamples, outCn;
            size_t planeSize;

            if( src_->dims == 4 )
            {
                nsamples = src_->size[0];
                outCn = src_->size[1];
                planeSize = (size_t)src_->size[2]*src_->size[3];
            }
            else
            {
                nsamples = outCn = 1;
                planeSize = (size_t)src_->total();
            }

            size_t stripeSize = (planeSize + nstripes - 1)/nstripes;
            size_t stripeStart = r.start*stripeSize;
            size_t stripeEnd = std::min(r.end*stripeSize, planeSize);

            for( int i = 0; i < nsamples; i++ )
            {
                const float* srcptr = src_->ptr<float>(i) + stripeStart;
                float* dstptr = dst_->ptr<float>(i) + stripeStart;
                func_->apply(srcptr, dstptr, (int)(stripeEnd - stripeStart), planeSize, 0, outCn);
            }
        }
    };

    ElementWiseLayer(const Func &f=Func()) { func = f; }

    bool getMemoryShapes(const std::vector<MatShape> &inputs,
                         const int requiredOutputs,
                         std::vector<MatShape> &outputs,
                         std::vector<MatShape> &internals) const
    {
        Layer::getMemoryShapes(inputs, requiredOutputs, outputs, internals);
        return true;
    }

    void forward(std::vector<Mat*> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
    {
        for (size_t i = 0; i < inputs.size(); i++)
        {
            const Mat &src = *inputs[i];
            Mat &dst = outputs[i];
            CV_Assert(src.size == dst.size && src.type() == dst.type() &&
                      src.isContinuous() && dst.isContinuous() && src.type() == CV_32F);

            const int nstripes = getNumThreads();
            PBody body(func, src, dst, nstripes);
            parallel_for_(Range(0, nstripes), body, nstripes);
        }
    }

    void forwardSlice(const float* src, float* dst, int len, size_t planeSize, int cn0, int cn1) const
    {
        func.apply(src, dst, len, planeSize, cn0, cn1);
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const
    {
        long flops = 0;
        for (int i = 0; i < outputs.size(); i++)
        {
            flops += total(outputs[i]) * func.getFLOPSPerElement();
        }
        return flops;
    }

    Func func;
    bool run_parallel;
};

struct ReLUFunctor
{
    typedef ReLULayer Layer;
    float slope;

    explicit ReLUFunctor(float slope_=1.f) : slope(slope_) {}

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        float s = slope;
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            int i = 0;
#if CV_SIMD128
            v_float32x4 s4 = v_setall_f32(s), z = v_setzero_f32();
            for( ; i <= len - 16; i += 16 )
            {
                v_float32x4 x0 = v_load(srcptr + i);
                v_float32x4 x1 = v_load(srcptr + i + 4);
                v_float32x4 x2 = v_load(srcptr + i + 8);
                v_float32x4 x3 = v_load(srcptr + i + 12);
                x0 = v_select(x0 >= z, x0, x0*s4);
                x1 = v_select(x1 >= z, x1, x1*s4);
                x2 = v_select(x2 >= z, x2, x2*s4);
                x3 = v_select(x3 >= z, x3, x3*s4);
                v_store(dstptr + i, x0);
                v_store(dstptr + i + 4, x1);
                v_store(dstptr + i + 8, x2);
                v_store(dstptr + i + 12, x3);
            }
#endif
            for( ; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = x >= 0.f ? x : s*x;
            }
        }
    }

    int64 getFLOPSPerElement() const { return 1; }
};

struct TanHFunctor
{
    typedef TanHLayer Layer;

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = tanh(x);
            }
        }
    }

    int64 getFLOPSPerElement() const { return 1; }
};

struct SigmoidFunctor
{
    typedef SigmoidLayer Layer;

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = 1.f/(1.f + exp(-x));
            }
        }
    }

    int64 getFLOPSPerElement() const { return 3; }
};

struct AbsValFunctor
{
    typedef AbsLayer Layer;

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = abs(x);
            }
        }
    }

    int64 getFLOPSPerElement() const { return 1; }
};

struct BNLLFunctor
{
    typedef BNLLLayer Layer;

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            for( int i = 0; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = log(1.f + exp(-abs(x)));
            }
        }
    }

    int64 getFLOPSPerElement() const { return 5; }
};

struct PowerFunctor
{
    typedef PowerLayer Layer;

    float power;
    float scale;
    float shift;

    explicit PowerFunctor(float power_ = 1.f, float scale_ = 1.f, float shift_ = 0.f)
        : power(power_), scale(scale_), shift(shift_) {}

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        float a = scale, b = shift, p = power;
        if( p == 1.f )
        {
            for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
            {
                for( int i = 0; i < len; i++ )
                {
                    float x = srcptr[i];
                    dstptr[i] = a*x + b;
                }
            }
        }
        else
        {
            for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
            {
                for( int i = 0; i < len; i++ )
                {
                    float x = srcptr[i];
                    dstptr[i] = pow(a*x + b, p);
                }
            }
        }
    }

    int64 getFLOPSPerElement() const { return power == 1 ? 2 : 10; }
};


struct ChannelsPReLUFunctor
{
    typedef ChannelsPReLULayer Layer;
    Mat scale;

    explicit ChannelsPReLUFunctor(const Mat& scale_=Mat()) : scale(scale_)
    {
    }

    void apply(const float* srcptr, float* dstptr, int len, size_t planeSize, int cn0, int cn1) const
    {
        CV_Assert(scale.isContinuous() && scale.type() == CV_32F);

        const float* scaleptr = scale.ptr<float>();
        CV_Assert( 0 <= cn0 && cn0 < cn1 && cn1 <= (int)scale.total() );

        for( int cn = cn0; cn < cn1; cn++, srcptr += planeSize, dstptr += planeSize )
        {
            float s = scaleptr[cn];
            int i = 0;
        #if CV_SIMD128
            v_float32x4 s4 = v_setall_f32(s), z = v_setzero_f32();
            for( ; i <= len - 16; i += 16 )
            {
                v_float32x4 x0 = v_load(ptr + i);
                v_float32x4 x1 = v_load(ptr + i + 4);
                v_float32x4 x2 = v_load(ptr + i + 8);
                v_float32x4 x3 = v_load(ptr + i + 12);
                x0 = v_select(x0 >= z, x0, x0*s4);
                x1 = v_select(x1 >= z, x1, x1*s4);
                x2 = v_select(x2 >= z, x2, x2*s4);
                x3 = v_select(x3 >= z, x3, x3*s4);
                v_store(ptr + i, x0);
                v_store(ptr + i + 4, x1);
                v_store(ptr + i + 8, x2);
                v_store(ptr + i + 12, x3);
            }
        #endif
            for( ; i < len; i++ )
            {
                float x = srcptr[i];
                dstptr[i] = x >= 0.f ? x : s*x;
            }
        }
    }

    int64 getFLOPSPerElement() const { return 1; }
};

#define ACTIVATION_CREATOR_FOR(_Layer, _Functor, ...) \
Ptr<_Layer> _Layer::create() { \
    return return Ptr<_Layer>( new ElementWiseLayer<_Functor>(_Functor()) ); }


Ptr<ReLULayer> ReLULayer::create(const LayerParams& params)
{
    float negativeSlope = params.get<float>("negative_slope", 0.f);
    Ptr<ReLULayer> l(new ElementWiseLayer<ReLUFunctor>(ReLUFunctor(negativeSlope)));
    l->setParamsFrom(params);
    l->negativeSlope = negativeSlope;

    return l;
}

Ptr<TanHLayer> TanHLayer::create(const LayerParams& params)
{
    Ptr<TanHLayer> l(new ElementWiseLayer<TanHFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<SigmoidLayer> SigmoidLayer::create(const LayerParams& params)
{
    Ptr<SigmoidLayer> l(new ElementWiseLayer<SigmoidFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<AbsLayer> AbsLayer::create(const LayerParams& params)
{
    Ptr<AbsLayer> l(new ElementWiseLayer<AbsValFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<BNLLLayer> BNLLLayer::create(const LayerParams& params)
{
    Ptr<BNLLLayer> l(new ElementWiseLayer<BNLLFunctor>());
    l->setParamsFrom(params);

    return l;
}

Ptr<PowerLayer> PowerLayer::create(const LayerParams& params)
{
    float power = params.get<float>("power", 1.0f);
    float scale = params.get<float>("scale", 1.0f);
    float shift = params.get<float>("shift", 0.0f);
    Ptr<PowerLayer> l(new ElementWiseLayer<PowerFunctor>(PowerFunctor(power, scale, shift)));
    l->setParamsFrom(params);
    l->power = power;
    l->scale = scale;
    l->shift = shift;

    return l;
}

Ptr<ChannelsPReLULayer> ChannelsPReLULayer::create(const LayerParams& params)
{
    Ptr<ChannelsPReLULayer> l(new ElementWiseLayer<ChannelsPReLUFunctor>(ChannelsPReLUFunctor(params.blobs[0])));
    l->setParamsFrom(params);

    return l;
}

}
}
