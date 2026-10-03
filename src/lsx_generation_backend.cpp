// lsxhome — the only translation unit that talks to the logestix engine.
//
// Translates the shell's engine-free vocabulary (ChatTurn / GenerationStatus)
// into lsxcommon calls: model load, chat-template framing, streaming Infer with
// per-token detokenization, and cooperative abort. Everything upstream of this
// file stays free of engine types, so the session logic is unit-testable.

#include "lsxhome/lsx_generation_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsxcommon/chat_conversation.h"
#include "lsxcommon/incremental_detokenizer.h"
#include "lsxcommon/model.h"
#include "lsxcommon/model_engine.h"
#include "lsxcommon/model_factory.h"

namespace lsxhome {
namespace {

/// Tokens generated per answer. A chat surface budget, not a benchmark knob.
constexpr int kMaxGenTokens = 512;

class LsxGenerationBackend final : public GenerationBackend {
public:
    explicit LsxGenerationBackend(std::string model_path)
        : model_path_(std::move(model_path)) {}

    bool Prepare(std::string& out_error) override {
        if (model_) {
            return true;
        }
        if (model_path_.empty()) {
            out_error = "model path is required: pass --model <path>";
            return false;
        }
        // The engine session is reference-counted and lives on the worker thread
        // that drives generation; it is torn down with the backend.
        session_ = std::make_unique<lsxcommon::InferenceEngine::Session>();

        lsxcommon::ModelInitConfig config;
        config.model_path = model_path_;
        config.max_gen_tokens = kMaxGenTokens;
        model_ = lsxcommon::LsxModelFactory::Create(config);
        if (!model_) {
            out_error = "failed to open the model: " + model_path_;
            session_.reset();
            return false;
        }
        if (!model_->Load()) {
            out_error = "failed to load the model: " + model_path_;
            model_.reset();
            session_.reset();
            return false;
        }
        return true;
    }

    GenerationStatus Generate(const std::vector<ChatTurn>& history,
                             const Sink& sink,
                             const std::atomic<bool>& abort,
                             std::string& out_error) override {
        if (!model_) {
            out_error = "the model is not loaded";
            return GenerationStatus::kError;
        }

        // Chat-template framing: the model's own BuildConversationInputIds
        // renders the roles (and the system prompt, when a family has one); the
        // generic Hermes fallback flattens the transcript.
        lsxcommon::ChatConversation conversation;
        conversation.messages.reserve(history.size());
        for (const ChatTurn& turn : history) {
            lsxcommon::ChatMessage message;
            message.role = turn.role;
            message.content = turn.text;
            conversation.messages.push_back(std::move(message));
        }

        std::vector<std::int32_t> input_ids;
        if (!model_->BuildConversationInputIds(model_->Tokenizer(),
                                               conversation, input_ids) ||
            input_ids.empty()) {
            out_error = "failed to frame the prompt for the model";
            return GenerationStatus::kError;
        }

        // Per-token detokenization with UTF-8 boundary safety: every delta is
        // valid text, never half a codepoint.
        auto detokenizer = lsxcommon::IncrementalDetokenizer::ArrowDecoder(
            model_->Tokenizer());

        lsxcommon::ModelRequest request{std::move(input_ids), kMaxGenTokens};
        request.abort_flag = &abort;
        request.token_callback =
            [&detokenizer, &sink](const lsxcommon::TokenChunk& chunk) {
                if (chunk.is_special) {
                    return;
                }
                const std::string delta = detokenizer.Push(
                    static_cast<std::int32_t>(chunk.token_id));
                if (!delta.empty()) {
                    sink(delta);
                }
            };

        const lsxcommon::gpu::InferenceResult result = model_->Infer(request);

        // Release any bytes the detokenizer withheld at a stream boundary.
        const std::string tail = detokenizer.Flush();
        if (!tail.empty()) {
            sink(tail);
        }

        if (abort.load(std::memory_order_acquire)) {
            return GenerationStatus::kAborted;
        }
        if (!result.ok) {
            out_error = result.error_msg.empty()
                            ? std::string("inference failed")
                            : result.error_msg;
            return GenerationStatus::kError;
        }
        if (result.output_text.empty()) {
            out_error = "the model returned an empty answer";
            return GenerationStatus::kError;
        }
        return GenerationStatus::kOk;
    }

private:
    std::string model_path_;
    std::unique_ptr<lsxcommon::InferenceEngine::Session> session_;
    std::unique_ptr<lsxcommon::IModel> model_;
};

}  // namespace

std::unique_ptr<GenerationBackend> MakeLsxGenerationBackend(
    std::string model_path) {
    return std::make_unique<LsxGenerationBackend>(std::move(model_path));
}

}  // namespace lsxhome