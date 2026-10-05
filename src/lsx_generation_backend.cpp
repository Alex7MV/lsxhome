// lsxhome — the only translation unit that talks to the logestix engine.
//
// Translates the shell's engine-free vocabulary (ChatTurn / GenerationStatus)
// into lsxcommon calls: model load, chat-template framing, streaming Infer with
// per-token detokenization, and cooperative abort. Everything upstream of this
// file stays free of engine types, so the session logic is unit-testable.

#include "lsxhome/history_budget.h"
#include "lsxhome/lsx_generation_backend.h"
#include "lsxhome/model_root.h"
#include "lsxhome/text_normalizer.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsxcommon/chat_conversation.h"
#include "lsxcommon/incremental_detokenizer.h"
#include "lsxcommon/log.h"
#include "lsxcommon/model.h"
#include "lsxcommon/model_engine.h"
#include "lsxcommon/model_factory.h"

#include <absl/strings/str_cat.h>

namespace lsxhome {
namespace {

constexpr int kMaxGenTokens = 512;

/// Prompt budget for the framed history. Answer time grows with the prompt, and
/// a transcript that never forgets made every question slower than the last
/// (measured on the real checkpoint: input_ids 41 → 417 → 468 → 1733 across
/// four turns of one conversation). ~4000 bytes of Cyrillic history is a few
/// turns of context — enough to follow along, bounded enough to stay responsive.
constexpr std::size_t kHistoryBudgetBytes = 4000;

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
        // The factory autodetects the family from <root>/logestix/
        // model.index.json, so a converted-directory spelling is normalized
        // here instead of failing autodetection.
        config.model_path = ResolveModelRoot(model_path_);
        config.max_gen_tokens = kMaxGenTokens;
        model_ = lsxcommon::LsxModelFactory::Create(config);
        if (!model_) {
            out_error = "failed to open the model: " + config.model_path;
            session_.reset();
            return false;
        }
        if (!model_->Load()) {
            out_error = "failed to load the model: " + config.model_path;
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
        // generic Hermes fallback flattens the transcript. Messages are
        // normalized first: a pasted question can carry the same exotic spaces
        // the model's own answers do, and those break the vocabulary encode.
        lsxcommon::ChatConversation conversation;
        // The transcript keeps every turn; the prompt does not. Trimming here —
        // right before framing — keeps the rule in one place and leaves the
        // on-screen history complete.
        const std::vector<ChatTurn> windowed =
            TrimHistoryToBudget(history, kHistoryBudgetBytes);
        conversation.messages.reserve(windowed.size());
        for (const ChatTurn& turn : windowed) {
            lsxcommon::ChatMessage message;
            message.role = turn.role;
            message.content = NormalizeEngineText(turn.text);
            conversation.messages.push_back(std::move(message));
        }

        // One framing attempt: the conversation is rendered with the model's own
        // chat template and encoded with its vocabulary. A rejection here is
        // reported to the user instead of being swallowed.
        std::vector<std::int32_t> input_ids;
        if (!model_->BuildConversationInputIds(model_->Tokenizer(),
                                               conversation,
                                               input_ids) ||
            input_ids.empty()) {
            // Distinguish the two ways this can happen, because they are not the
            // same bug: an empty conversation (the history window dropped
            // everything) is ours, a rejected encode is the vocabulary's.
            if (conversation.messages.empty()) {
                out_error =
                    "internal: the history window produced an empty prompt";
            } else {
                std::size_t chars = 0;
                for (const auto& message : conversation.messages) {
                    chars += message.content.size();
                }
                out_error = "the model's vocabulary rejected the prompt (" +
                            std::to_string(conversation.messages.size()) +
                            " messages, " + std::to_string(chars) +
                            " bytes; tokenizer_kind=" +
                            std::to_string(model_->Tokenizer().tokenizer_kind) +
                            ")";
            }
            lsxcommon::log::error(absl::StrCat("[engine] ", out_error));
            return GenerationStatus::kError;
        }

        lsxcommon::log::info(absl::StrCat(
            "[engine] generate: history=", history.size(),
            " input_ids=", input_ids.size()));


        // Per-token detokenization with UTF-8 boundary safety: every delta is
        // valid text, never half a codepoint. ArrowDecoder supplies the real
        // vocabulary-backed decode; IncrementalDetokenizer owns the boundary
        // logic and emits only newly completed text.
        lsxcommon::IncrementalDetokenizer detokenizer(
            lsxcommon::IncrementalDetokenizer::ArrowDecoder(model_->Tokenizer()));

        lsxcommon::ModelRequest request{std::move(input_ids), kMaxGenTokens};
        request.abort_flag = &abort;
        // Normalize before the sink, so the transcript, the framed history and
        // what the user sees all carry the same plain spaces: this model emits
        // U+00A0, which its own vocabulary cannot encode back.
        request.token_callback =
            [&detokenizer, &sink](const lsxcommon::TokenChunk& chunk) {
                if (chunk.is_special) {
                    return;
                }
                const std::string delta = NormalizeEngineText(
                    detokenizer.Push(static_cast<std::int32_t>(chunk.token_id)));
                if (!delta.empty()) {
                    sink(delta);
                }
            };

        const lsxcommon::gpu::InferenceResult result = model_->Infer(request);

        // Release any bytes the detokenizer withheld at a stream boundary.
        const std::string tail =
            NormalizeEngineText(detokenizer.Flush());
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