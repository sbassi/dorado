#include "CudaModelRunner.h"

#include "CudaCaller.h"
#include "decode/Decoder.h"
#include "utils/cuda_utils.h"
#include "utils/math_utils.h"

#include <c10/cuda/CUDAStream.h>

#include <sstream>

namespace dorado::basecall {

CudaModelRunner::CudaModelRunner(std::shared_ptr<CudaCaller> caller)
        : m_caller(std::move(caller)),
          m_input(m_caller->create_input_tensor()),
          m_output(m_caller->create_output_tensor()),
          m_stream(c10::cuda::getStreamFromPool(false, m_caller->device().index())) {}

void CudaModelRunner::accept_chunk(int chunk_idx, const at::Tensor &chunk) {
    m_input.index_put_({chunk_idx, torch::indexing::Ellipsis}, chunk);
}

std::vector<decode::DecodedChunk> CudaModelRunner::call_chunks(int num_chunks) {
    ++m_num_batches_called;
    stats::Timer timer;
    auto decoded_chunks = m_caller->call_chunks(m_input, m_output, num_chunks, m_stream);
    return decoded_chunks;
}

const CRFModelConfig &CudaModelRunner::config() const { return m_caller->config(); }
size_t CudaModelRunner::model_stride() const { return m_caller->config().stride; }
size_t CudaModelRunner::chunk_size() const { return m_input.size(2); }
size_t CudaModelRunner::batch_size() const { return m_input.size(0); }
void CudaModelRunner::terminate() { m_caller->terminate(); }
void CudaModelRunner::restart() { m_caller->restart(); }

std::string CudaModelRunner::get_name() const {
    // The name must be unique across multiple instances.
    // We could take a unique ID at setup time, but for now just use the address.
    std::ostringstream name_stream;
    name_stream << "CudaModelRunner_" << this;
    return name_stream.str();
}

stats::NamedStats CudaModelRunner::sample_stats() const {
    // We don't have direct access to the caller object when the pipeline is set up,
    // so pass through stats here.
    // Each runner will retrieve stats from the caller.
    // Only the last retrieved version will appear, but they should be very similar.
    stats::NamedStats stats = stats::from_obj(*m_caller);
    stats["batches_called"] = double(m_num_batches_called);
    return stats;
}

}  // namespace dorado::basecall
