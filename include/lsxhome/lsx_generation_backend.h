#pragma once

#include <memory>
#include <string>

#include "generation_backend.h"

namespace lsxhome {

/// Builds the production backend for the checkpoint at @p model_path (the
/// `--model` flag value). An empty path yields a backend whose `Prepare` fails
/// with a user-facing message instead of failing at the first question.
std::unique_ptr<GenerationBackend> MakeLsxGenerationBackend(std::string model_path);

}  // namespace lsxhome