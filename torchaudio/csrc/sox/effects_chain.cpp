#include <torchaudio/csrc/sox/effects_chain.h>
#include <torchaudio/csrc/sox/utils.h>

using namespace torch::indexing;
using namespace torchaudio::sox_utils;

namespace torchaudio {
namespace sox_effects_chain {

namespace {

/// helper classes for passing the location of input tensor and output buffer
///
/// drain/flow callback functions require plaing C style function signature and
/// the way to pass extra data is to attach data to sox_effect_t::priv pointer.
/// The following structs will be assigned to sox_effect_t::priv pointer which
/// gives sox_effect_t an access to input Tensor and output buffer object.
struct TensorInputPriv {
  size_t index;
  torch::Tensor* waveform;
  int64_t sample_rate;
  bool channels_first;
};
struct TensorOutputPriv {
  std::vector<sox_sample_t>* buffer;
};
struct FileOutputPriv {
  sox_format_t* sf;
};

/// Callback function to feed Tensor data to SoxEffectChain.
int tensor_input_drain(sox_effect_t* effp, sox_sample_t* obuf, size_t* osamp) {
  // Retrieve the input Tensor and current index
  auto priv = static_cast<TensorInputPriv*>(effp->priv);
  auto index = priv->index;
  auto tensor = *(priv->waveform);
  auto num_channels = effp->out_signal.channels;

  // Adjust the number of samples to read
  const size_t num_samples = tensor.numel();
  if (index + *osamp > num_samples) {
    *osamp = num_samples - index;
  }
  // Ensure that it's a multiple of the number of channels
  *osamp -= *osamp % num_channels;

  // Slice the input Tensor
  auto chunk = [&]() {
    auto i_frame = index / num_channels;
    auto num_frames = *osamp / num_channels;
    auto t = (priv->channels_first)
        ? tensor.index({Slice(), Slice(i_frame, i_frame + num_frames)}).t()
        : tensor.index({Slice(i_frame, i_frame + num_frames), Slice()});
    return t.reshape({-1});
  }();

  // Convert to sox_sample_t (int32_t)
  switch (chunk.dtype().toScalarType()) {
    case c10::ScalarType::Float: {
      // Need to convert to 64-bit precision so that
      // values around INT32_MIN/MAX are handled correctly.
      chunk = chunk.to(c10::ScalarType::Double);
      chunk *= 2147483648.;
      chunk.clamp_(INT32_MIN, INT32_MAX);
      chunk = chunk.to(c10::ScalarType::Int);
      break;
    }
    case c10::ScalarType::Int: {
      break;
    }
    case c10::ScalarType::Short: {
      chunk = chunk.to(c10::ScalarType::Int);
      chunk *= 65536;
      break;
    }
    case c10::ScalarType::Byte: {
      chunk = chunk.to(c10::ScalarType::Int);
      chunk -= 128;
      chunk *= 16777216;
      break;
    }
    default:
      throw std::runtime_error("Unexpected dtype.");
  }
  // Write to buffer
  chunk = chunk.contiguous();
  memcpy(obuf, chunk.data_ptr<int32_t>(), *osamp * 4);
  priv->index += *osamp;
  return (priv->index == num_samples) ? SOX_EOF : SOX_SUCCESS;
}

/// Callback function to fetch data from SoxEffectChain.
int tensor_output_flow(
    sox_effect_t* effp,
    sox_sample_t const* ibuf,
    sox_sample_t* obuf LSX_UNUSED,
    size_t* isamp,
    size_t* osamp) {
  *osamp = 0;
  // Get output buffer
  auto out_buffer = static_cast<TensorOutputPriv*>(effp->priv)->buffer;
  // Append at the end
  out_buffer->insert(out_buffer->end(), ibuf, ibuf + *isamp);
  return SOX_SUCCESS;
}

int file_output_flow(
    sox_effect_t* effp,
    sox_sample_t const* ibuf,
    sox_sample_t* obuf LSX_UNUSED,
    size_t* isamp,
    size_t* osamp) {
  *osamp = 0;
  if (*isamp) {
    auto sf = static_cast<FileOutputPriv*>(effp->priv)->sf;
    if (sox_write(sf, ibuf, *isamp) != *isamp) {
      if (sf->sox_errno) {
        std::ostringstream stream;
        stream << sf->sox_errstr << " " << sox_strerror(sf->sox_errno) << " "
               << sf->filename;
        throw std::runtime_error(stream.str());
      }
      return SOX_EOF;
    }
  }
  return SOX_SUCCESS;
}

sox_effect_handler_t* get_tensor_input_handler() {
  static sox_effect_handler_t handler{
      /*name=*/"input_tensor",
      /*usage=*/NULL,
      /*flags=*/SOX_EFF_MCHAN,
      /*getopts=*/NULL,
      /*start=*/NULL,
      /*flow=*/NULL,
      /*drain=*/tensor_input_drain,
      /*stop=*/NULL,
      /*kill=*/NULL,
      /*priv_size=*/sizeof(TensorInputPriv)};
  return &handler;
}

sox_effect_handler_t* get_tensor_output_handler() {
  static sox_effect_handler_t handler{
      /*name=*/"output_tensor",
      /*usage=*/NULL,
      /*flags=*/SOX_EFF_MCHAN,
      /*getopts=*/NULL,
      /*start=*/NULL,
      /*flow=*/tensor_output_flow,
      /*drain=*/NULL,
      /*stop=*/NULL,
      /*kill=*/NULL,
      /*priv_size=*/sizeof(TensorOutputPriv)};
  return &handler;
}

sox_effect_handler_t* get_file_output_handler() {
  static sox_effect_handler_t handler{
      /*name=*/"output_file",
      /*usage=*/NULL,
      /*flags=*/SOX_EFF_MCHAN,
      /*getopts=*/NULL,
      /*start=*/NULL,
      /*flow=*/file_output_flow,
      /*drain=*/NULL,
      /*stop=*/NULL,
      /*kill=*/NULL,
      /*priv_size=*/sizeof(FileOutputPriv)};
  return &handler;
}

} // namespace

SoxEffect::SoxEffect(sox_effect_t* se) noexcept : se_(se) {}

SoxEffect::~SoxEffect() {
  if (se_ != nullptr) {
    free(se_);
  }
}

SoxEffect::operator sox_effect_t*() const {
  return se_;
}

auto SoxEffect::operator->() noexcept -> sox_effect_t* {
  return se_;
}

SoxEffectsChain::SoxEffectsChain(
    sox_encodinginfo_t input_encoding,
    sox_encodinginfo_t output_encoding)
    : in_enc_(input_encoding),
      out_enc_(output_encoding),
      in_sig_(),
      interm_sig_(),
      out_sig_(),
      sec_(sox_create_effects_chain(&in_enc_, &out_enc_)) {
  if (!sec_) {
    throw std::runtime_error("Failed to create effect chain.");
  }
}

SoxEffectsChain::~SoxEffectsChain() {
  if (sec_ != nullptr) {
    sox_delete_effects_chain(sec_);
  }
}

void SoxEffectsChain::run() {
  sox_flow_effects(sec_, NULL, NULL);
}

void SoxEffectsChain::addInputTensor(
    torch::Tensor* waveform,
    int64_t sample_rate,
    bool channels_first) {
  in_sig_ = get_signalinfo(waveform, sample_rate, "wav", channels_first);
  interm_sig_ = in_sig_;
  SoxEffect e(sox_create_effect(get_tensor_input_handler()));
  auto priv = static_cast<TensorInputPriv*>(e->priv);
  priv->index = 0;
  priv->waveform = waveform;
  priv->sample_rate = sample_rate;
  priv->channels_first = channels_first;
  if (sox_add_effect(sec_, e, &interm_sig_, &in_sig_) != SOX_SUCCESS) {
    throw std::runtime_error(
        "Internal Error: Failed to add effect: input_tensor");
  }
}

void SoxEffectsChain::addOutputBuffer(
    std::vector<sox_sample_t>* output_buffer) {
  SoxEffect e(sox_create_effect(get_tensor_output_handler()));
  static_cast<TensorOutputPriv*>(e->priv)->buffer = output_buffer;
  if (sox_add_effect(sec_, e, &interm_sig_, &in_sig_) != SOX_SUCCESS) {
    throw std::runtime_error(
        "Internal Error: Failed to add effect: output_tensor");
  }
}

void SoxEffectsChain::addInputFile(sox_format_t* sf) {
  in_sig_ = sf->signal;
  interm_sig_ = in_sig_;
  SoxEffect e(sox_create_effect(sox_find_effect("input")));
  char* opts[] = {(char*)sf};
  sox_effect_options(e, 1, opts);
  if (sox_add_effect(sec_, e, &interm_sig_, &in_sig_) != SOX_SUCCESS) {
    std::ostringstream stream;
    stream << "Internal Error: Failed to add effect: input " << sf->filename;
    throw std::runtime_error(stream.str());
  }
}

void SoxEffectsChain::addOutputFile(sox_format_t* sf) {
  out_sig_ = sf->signal;
  SoxEffect e(sox_create_effect(get_file_output_handler()));
  static_cast<FileOutputPriv*>(e->priv)->sf = sf;
  if (sox_add_effect(sec_, e, &interm_sig_, &out_sig_) != SOX_SUCCESS) {
    std::ostringstream stream;
    stream << "Internal Error: Failed to add effect: output " << sf->filename;
    throw std::runtime_error(stream.str());
  }
}

void SoxEffectsChain::addEffect(const std::vector<std::string> effect) {
  const auto num_args = effect.size();
  if (num_args == 0) {
    throw std::runtime_error("Invalid argument: empty effect.");
  }
  const auto name = effect[0];
  if (UNSUPPORTED_EFFECTS.find(name) != UNSUPPORTED_EFFECTS.end()) {
    std::ostringstream stream;
    stream << "Unsupported effect: " << name;
    throw std::runtime_error(stream.str());
  }

  auto returned_effect = sox_find_effect(name.c_str());
  if (!returned_effect) {
    std::ostringstream stream;
    stream << "Unsupported effect: " << name;
    throw std::runtime_error(stream.str());
  }
  SoxEffect e(sox_create_effect(returned_effect));
  const auto num_options = num_args - 1;

  std::vector<char*> opts;
  for (size_t i = 1; i < num_args; ++i) {
    opts.push_back((char*)effect[i].c_str());
  }
  if (sox_effect_options(e, num_options, num_options ? opts.data() : nullptr) !=
      SOX_SUCCESS) {
    std::ostringstream stream;
    stream << "Invalid effect option:";
    for (const auto& v : effect) {
      stream << " " << v;
    }
    throw std::runtime_error(stream.str());
  }

  if (sox_add_effect(sec_, e, &interm_sig_, &in_sig_) != SOX_SUCCESS) {
    std::ostringstream stream;
    stream << "Internal Error: Failed to add effect: \"" << name;
    for (size_t i = 1; i < num_args; ++i) {
      stream << " " << effect[i];
    }
    stream << "\"";
    throw std::runtime_error(stream.str());
  }
}

int64_t SoxEffectsChain::getOutputNumChannels() {
  return interm_sig_.channels;
}

int64_t SoxEffectsChain::getOutputSampleRate() {
  return interm_sig_.rate;
}

} // namespace sox_effects_chain
} // namespace torchaudio
