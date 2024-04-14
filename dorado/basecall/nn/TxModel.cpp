#include "basecall/nn/TxModel.h"

#include "basecall/CRFModelConfig.h"
#include "basecall/nn/CRFModel.h"
#include "spdlog/spdlog.h"
#include "tensor_utils.h"
#include "utils/gpu_profiling.h"

#include <ATen/TensorIndexing.h>
#include <ATen/ops/arange.h>
#include <ATen/ops/cat.h>
#include <ATen/ops/rsqrt.h>
#include <c10/core/ScalarType.h>
#include <c10/core/TensorOptions.h>
#include <torch/nn.h>
#include <torch/nn/functional/padding.h>
#include <torch/nn/options/padding.h>
#include <torch/serialize.h>
#include <torch/types.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dorado::basecall {

namespace nn {

using namespace dorado::basecall::tx;
using namespace torch::nn;
namespace Idx = torch::indexing;
using Slice = torch::indexing::Slice;

at::Tensor load_synthetic(const std::string &filename, const at::TensorOptions &options) {
    spdlog::warn("Loading syntheic model result: {}", filename);
    at::Tensor out;
    try {
        torch::load(out, filename);
    } catch (const c10::Error &e) {
        spdlog::error("Error loading tensor from file: {}", e.msg());
    }
    out = out.to(options);
    spdlog::warn(shape(out, "synthetic_out.shape"));
    return out;
}

RMSNormImpl::RMSNormImpl(int lrno_, int hidden_size_) : lrno(lrno_), hidden_size(hidden_size_) {
    weight = at::ones({hidden_size});
    register_parameter("weight", weight, false);
}

at::Tensor RMSNormImpl::forward(at::Tensor x) {
    x = x.to(torch::kFloat32);
    at::Tensor rstd = torch::rsqrt(x.square().mean(-1, true, torch::kFloat32) + eps);
    return (x * rstd * weight).to(torch::kHalf);
}

GatedMLPImpl::GatedMLPImpl(int lrno_, int in_features, int hidden_features) : lrno(lrno_) {
    fc1 = register_module("fc1",
                          Linear(LinearOptions(in_features, 2 * hidden_features).bias(false)));
    fc2 = register_module("fc2", Linear(LinearOptions(hidden_features, in_features).bias(false)));
};

at::Tensor GatedMLPImpl::forward(at::Tensor x) {
    const at::Tensor fc1_ = fc1(x);
    const std::vector<at::Tensor> chunks = fc1_.chunk(2, -1);
    const at::Tensor &y = chunks[0];
    const at::Tensor &gate = chunks[1];
    const at::Tensor out = fc2(functional::silu(gate).mul(y));

    dump_tensor(fc1_, "m.encoder.transformer_encoder_" + std::to_string(lrno) + ".ff.fc1");
    dump_tensor(out, "m.encoder.transformer_encoder_" + std::to_string(lrno) + ".ff.fc2");
    return out;
}

RotaryEmbeddingImpl::RotaryEmbeddingImpl(int lrno_,
                                         int dim_,
                                         float theta_,
                                         int max_seq_len_,
                                         const at::TensorOptions &options_)
        : lrno(lrno_), dim(dim_), max_seq_len(max_seq_len_), theta(theta_), options(options_) {
    // To maintain precision we use float32 here
    const at::Tensor inv_freq =
            torch::pow(theta, torch::arange(0, dim, 2, options.dtype(torch::kFloat32)) / dim)
                    .reciprocal();

    // freqs.shape := {max_seq_len, 1, 1, dim/2}
    const at::Tensor freqs = torch::arange(max_seq_len, options.dtype(torch::kFloat32))
                                     .reshape({max_seq_len, 1, 1, 1}) *
                             inv_freq;

    register_buffer("cos_freqs", torch::cos(freqs).to(options.dtype(torch::kHalf)));
    register_buffer("sin_freqs", torch::sin(freqs).to(options.dtype(torch::kHalf)));
};

at::Tensor RotaryEmbeddingImpl::forward(at::Tensor qkv) {
    const std::string name = "m.encoder.transformer_encoder_0.self_attn.rotary_emb";
    // Expected shape: N, seq_len, 3, nhead, head_dim
    const long int seq_len = qkv.size(1);

    const at::Tensor cos_buf = named_buffers()["cos_freqs"].index({Slice(Idx::None, seq_len)});
    const at::Tensor sin_buf = named_buffers()["sin_freqs"].index({Slice(Idx::None, seq_len)});

    at::Tensor out = qkv.clone();

    using Slices = std::vector<Idx::TensorIndex>;
    const Slices evens = {Idx::Ellipsis, Slice(Idx::None, 2), Slice(), Slice(Idx::None, dim / 2)};
    const Slices odds = {Idx::Ellipsis, Slice(Idx::None, 2), Slice(), Slice(dim / 2, dim)};

    const at::Tensor qk_evens = qkv.index(evens).to(torch::kFloat32);
    const at::Tensor qk_odds = qkv.index(odds).to(torch::kFloat32);

    out.index(evens) = (cos_buf * qk_evens) - (sin_buf * qk_odds);
    out.index(odds) = (sin_buf * qk_evens) + (cos_buf * qk_odds);

    return out.to(torch::kHalf);
}

MultiHeadAttentionImpl::MultiHeadAttentionImpl(int lrno_,
                                               int d_model_,
                                               int nhead_,
                                               bool qkv_bias_,
                                               bool out_bias_,
                                               const at::Tensor &attn_window_mask_,
                                               const at::TensorOptions &options_)
        : lrno(lrno_),
          d_model(d_model_),
          nhead(nhead_),
          head_dim(d_model_ / nhead_),
          attn_window_mask(attn_window_mask_),
          options(options_) {
    wqkv = register_module("wqkv", Linear(LinearOptions(d_model, 3 * d_model).bias(qkv_bias_)));
    out_proj = register_module("out_proj", Linear(LinearOptions(d_model, d_model).bias(out_bias_)));
    // TODO: can max_seq_len 1000 -> attn_window.size()?
    rotary_emb = register_module("rotary_emb", RotaryEmbedding(lrno, head_dim, 10000.0,
                                                               attn_window_mask.size(0), options));
};

at::Tensor MultiHeadAttentionImpl::forward(at::Tensor x) {
    const long int N = x.size(0);
    const long int T = x.size(1);
    const long int C = x.size(2);

    const std::string name = "m.encoder.transformer_encoder_" + std::to_string(lrno) + ".self_attn";
    spdlog::debug(shape(x, name + ".x"));
    at::Tensor qkv;
    at::Tensor attn_output;
    {
        utils::ScopedProfileRange spr("QKV", 2);
        // in_feat=512, out_feat=1536 (3*in), nhead=8, head_dim=64=(512/8), dim_ff=2048
        qkv = wqkv(x).view({N, T, 3, nhead, head_dim});
        dump_tensor(qkv, name + ".qkv");
    }
    {
        utils::ScopedProfileRange spr("ROTE", 2);
        qkv = rotary_emb(qkv);
        dump_tensor(qkv, name + ".rotary_emb");
    }
    {
        utils::ScopedProfileRange spr("MEA", 2);
        // NT3HD -> N3HTD -> N[1]HTD
        const auto qkv_ = qkv.permute({0, 2, 3, 1, 4}).chunk(3, 1);
        // spdlog::debug(shape(attn_window_mask, "MHA.attn_mask"));
        attn_output = at::scaled_dot_product_attention(qkv_[0], qkv_[1], qkv_[2], attn_window_mask)
                              .permute({0, 1, 3, 2, 4})
                              .reshape({N, T, C});
        dump_tensor(attn_output, name + ".attn_output");
    }
    {
        utils::ScopedProfileRange spr("OUTP", 2);
        x = out_proj(attn_output);
        dump_tensor(x, name + ".out_proj");
    }
    return x;
};

TxEncoderImpl::TxEncoderImpl(int lrno_,
                             const TxEncoderParams &params,
                             const at::Tensor &attn_window_mask_,
                             const at::TensorOptions &options_)
        : lrno(lrno_), attn_window_mask(attn_window_mask_), options(options_) {
    self_attn = register_module("self_attn",
                                MultiHeadAttention(lrno, params.d_model, params.nhead, false, true,
                                                   attn_window_mask, options));
    ff = register_module("ff", GatedMLP(lrno, params.d_model, params.dim_feedforward));
    norm1 = register_module("norm1", RMSNorm(lrno, params.d_model));
    norm2 = register_module("norm2", RMSNorm(lrno, params.d_model));

    const at::Tensor deepnorm_alpha = at::tensor(params.deepnorm_alpha);
    register_buffer("deepnorm_alpha", deepnorm_alpha);
};

at::Tensor TxEncoderImpl::forward(at::Tensor x) {
    const std::string t_name = "m.encoder.transformer_encoder_" + std::to_string(lrno);
    // spdlog::debug(shape(x, t_name + ".x"));
    at::Tensor attn, f;
    {
        utils::ScopedProfileRange spr("MHE", 2);
        attn = self_attn(x);
        dump_tensor(attn, t_name + ".self_attn");
    }
    {
        utils::ScopedProfileRange spr("LNORM1", 2);
        x = norm1(attn + (x * named_buffers()["deepnorm_alpha"]));
        dump_tensor(x, t_name + ".norm1");
    }
    {
        utils::ScopedProfileRange spr("FF", 2);
        f = ff(x);
        dump_tensor(f, t_name + ".ff");
    }
    {
        utils::ScopedProfileRange spr("LNORM2", 2);
        x = norm2(f + (x * named_buffers()["deepnorm_alpha"]));
        dump_tensor(x, t_name + ".norm2");
    }

    dump_tensor(x, t_name);
    return x;
}

TxEncoderStackImpl::TxEncoderStackImpl(const basecall::CRFModelConfig &config,
                                       const at::TensorOptions &options)
        : attn_window_mask(build_attn_window_mask(config, options)) {
    const auto &tx_enc_params = config.tx->tx;
    stack = Sequential();
    for (int i = 0; i < tx_enc_params.depth; ++i) {
        stack->push_back(register_module("transformer_encoder" + std::to_string(i),
                                         TxEncoder(i, tx_enc_params, attn_window_mask, options)));
    }
};

at::Tensor TxEncoderStackImpl::build_attn_window_mask(const basecall::CRFModelConfig &config,
                                                      const at::TensorOptions &options) const {
    const int size = static_cast<int>(config.basecaller.chunksize /
                                      (config.stride * config.tx->upsample.scale_factor));
    const auto [win_upper, win_lower] = config.tx->tx.attn_window;

    at::Tensor mask = at::triu(at::ones({size, size}), -win_upper);
    mask *= at::tril(mask, win_lower);
    mask = mask.to(at::kBool).to(options.device());
    return mask;
};

LinearUpsampleImpl::LinearUpsampleImpl(const EncoderUpsampleParams &params)
        : scale_factor(params.scale_factor) {
    linear = register_module(
            "linear",
            Linear(LinearOptions(params.d_model, scale_factor * params.d_model).bias(true)));
};

at::Tensor LinearUpsampleImpl::forward(at::Tensor x) {
    const long int N = x.size(0);
    const long int T = x.size(1);
    const long int C = x.size(2);
    const at::Tensor out = linear(x).reshape({N, scale_factor * T, C});
    dump_tensor(linear->weight, "upsample.linear.weight.tensor");
    dump_tensor(linear->bias, "upsample.linear.bias.tensor");
    return out;
};

LinearScaledCRFImpl::LinearScaledCRFImpl(const tx::CRFEncoderParams &params) {
    m_params = params;
    linear = register_module(
            "linear", Linear(LinearOptions(m_params.insize, m_params.outsize()).bias(false)));
};

at::Tensor LinearScaledCRFImpl::forward(at::Tensor x) { return linear(x) * m_params.scale; }

TxModelImpl::TxModelImpl(const basecall::CRFModelConfig &config, const at::TensorOptions &options)
        : m_options(options) {
    convs = register_module("convs", basecall::nn::ConvStack(config.convs));
    tx_encoder = register_module("transformer_encoder", TxEncoderStack(config, m_options));
    tx_decoder = register_module("transformer_decoder", LinearUpsample(config.tx->upsample));
    crf = register_module("crf", LinearScaledCRF(config.tx->crf));
}

at::Tensor TxModelImpl::forward(at::Tensor x) {
    dump_tensor(x, "TxModel.x");
    at::Tensor h;
    {
        utils::ScopedProfileRange spr("Conv", 1);
        h = convs->forward(x);
        spdlog::debug(shape(h, "m.encoder.conv"));
        dump_tensor(h, "m.encoder.conv");
    }
    {
        utils::ScopedProfileRange spr("TransEnc", 1);
        h = tx_encoder(h);
        spdlog::debug(shape(h, "m.encoder.transformer_encoder"));
        dump_tensor(h, "m.encoder.transformer_encoder");
    }

    // h = load_synthetic(
    //         "/home/OXFORDNANOLABS/rharris/dev/dorado/synthetic_tensor/"
    //         "m.encoder.transformer_encoder.pt",
    //         m_options);
    // h = torch::ones({x.size(0), 833, 512}, m_options);
    {
        utils::ScopedProfileRange spr("TransDec", 1);
        h = tx_decoder(h);
        spdlog::debug(shape(h, "m.encoder.upsample"));
        dump_tensor(h, "m.encoder.upsample.ones");
    }

    // h = load_synthetic(
    //         "/home/OXFORDNANOLABS/rharris/dev/dorado/synthetic_tensor/"
    //         "m.encoder.upsample.pt",
    //         m_options);

    {
        utils::ScopedProfileRange spr("CRF", 1);
        h = crf(h);
        spdlog::debug(shape(h, "m.encoder.crf"));
        dump_tensor(h, "m.encoder.crf");
    }

    // h = load_synthetic(
    //         "/home/OXFORDNANOLABS/rharris/dev/dorado/synthetic_tensor/"
    //         "m.encoder.crf.pt",
    //         m_options);

    return h;
}

}  // namespace nn

}  // namespace dorado::basecall
