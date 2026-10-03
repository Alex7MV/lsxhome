# lsxhome — Chat UI + Inference MVP (Design)

- **Date:** 2026-10-03
- **Branch:** `feat/chat-ui-inference-mvp`
- **Status:** Approved

## Goal

Turn the shell from a one-shot `--run` demo into a usable chat surface: the
user types a question in the welcome input card, the shell dispatches it to the
logestix inference engine, and the answer streams back into the main panel as the
model decodes it. The button «Stop» aborts an in-flight generation.

Today the input card's buffer (`src/gui_renderer.cpp`) is never read, the compute
thread publishes tokens exactly once at startup from the `--run` prompt, and the
only consumer is a sidebar debug button that logs the pool. The MVP closes the
loop end to end.

Decisions taken during brainstorming:

| Question | Decision |
|---|---|
| Response delivery | Streaming per token, plus a Stop button |
| Conversation | Multi-turn — history is framed back to the model |
| Model source | `--model <path>` only; the UI model picker stays a stub |
| Answer placement | The welcome screen becomes the chat panel |

## Architecture

Three new headers keep engine knowledge out of the UI, and one new `.cpp` is the
only place that knows `lsxcommon`:

- `include/lsxhome/chat_state.h` — the UI-thread transcript. `std::vector` of
  `{role, text, interrupted}` plus an "assistant reply is open" flag and the last
  error string. Operations: `BeginTurn(user_text)`, `AppendDelta(seq, text)`,
  `EndTurn(interrupted)`, `SetError()`, `Clear()`. No engine, no ImGui, no
  threads.
- `include/lsxhome/generation_backend.h` — the injected engine boundary:
  `Prepare()` (load the model, called once) and `Generate(history, sink, abort)`
  returning a status. `sink` receives decoded text deltas.
- `include/lsxhome/chat_session.h` — request queue (a `SpscTokenRing` of fixed
  `QueuedPrompt` slots), the worker thread, the phase/error atomics, and the
  bridge hand-off. Owns a `GuiBridge` reference; never touches ImGui.
- `src/lsx_generation_backend.cpp` — `LsxGenerationBackend`, the real adapter:
  `InferenceEngine::Session`, `LsxModelFactory::Create`/`Load`,
  `BuildConversationInputIds` over `lsxcommon::ChatConversation`, `Infer` with a
  `token_callback`, and `IncrementalDetokenizer` to turn token ids into UTF-8-safe
  deltas.
- `include/lsxhome/text_splitter.h` — gains `SplitUtf8Chunks()`, because the
  fixed 32-byte `TokenPayload::text` cannot carry an arbitrary delta. The
  existing space splitter drops over-long words with a warning; the new chunker
  splits on codepoint boundaries so nothing is lost and no multibyte character is
  cut.
- `src/gui_renderer.cpp` — `DrawChatPanel(session, state, bridge)` replaces
  `DrawClaudeWelcomeInterface()` as the `##main` content.

Chosen shape: **chat session + existing SPSC bridge, with an injected backend.**
Rejected alternatives: calling `Infer` on the UI thread (freezes the window for
the minutes a model load takes) and `std::async` per request (breaks the
project's deterministic zero-allocation SPSC contract and forces a mutex around
the model).

## Data flow

Phases: `kIdle → kLoading` (first `Prepare()`, UI shows «Загрузка модели…») `→
kGenerating → kIdle`, or `kError` with text. The model stays loaded between
questions and across errors; it is torn down when the app exits.

1. UI: `Submit(text)` → the turn is refused if a reply is open or the queue is
   full (the draft stays in the input box, the transcript is untouched) →
   `BeginTurn()` appends the user message and an open assistant message → the
   prompt is pushed into the request ring.
2. Worker: pops the prompt, sets `kLoading`, calls `Prepare()` once, then
   `kGenerating` and `Generate(history, sink, &abort)`. The worker keeps **its
   own** copy of the transcript (append the user message on pop, the accumulated
   answer on completion) — the UI's `ChatState` is display-only and is never
   read from another thread, so there is no lock and no race.
3. `sink(delta)` → `SplitUtf8Chunks` → each chunk is published to the
   `GuiBridge` with an increasing `stream_seq` through `Submit` (retry-until-
   accepted): the ring may be full while the UI renders, and answer text must
   never be dropped. The wait honours the abort flag.
4. UI, every frame: `session.DrainInto(state)` appends drained chunks into the
   open reply and renders them; the phase decides the form (input / Stop /
   «Загрузка модели…»). The drain must run **before** the phase is evaluated:
   the worker publishes the last chunk before flipping to a terminal phase, so
   drain-first is what keeps the tail of the answer from being closed out.
5. Worker completion: success → `kIdle`, and the UI closes the reply
   (`EndTurn(interrupted=false)`); abort → the reply is closed with
   `interrupted=true` and a muted «остановлено» marker is drawn; error →
   `SetError()` + `kError`.

CLI compatibility: `--run [PROMPT]` still works — the resolved prompt is
submitted automatically as the first chat turn. Without `--run` the shell opens
on the welcome screen with an empty transcript.

The sidebar «Drain pool» button is removed: the chat panel is now the bridge's
consumer, so the button would steal tokens from the transcript. The
`Published/Dropped/PoolSlots` counters stay.

## UI

- Empty transcript: the existing welcome screen (greeting, input card, quick
  actions). Clicking a quick action fills the input box instead of doing nothing.
- Non-empty transcript: a scrollable conversation area filling the height, with
  auto-scroll to the tail as text grows. User turns use the muted accent colour,
  answers use the default text colour.
- The form is pinned to the bottom: multiline input plus «Send». Enter submits,
  Shift+Enter inserts a newline.
- While generating, «Send» is replaced by «Stop»; the field stays editable but
  the next question cannot be submitted.
- The error line renders between the conversation and the form.

## Errors and threading invariants

- No `--model` → `Prepare()` fails with «модель не задана — передайте --model
  <путь>»; the form stays usable and the error is visible. A question is never
  silently swallowed.
- `Load()` false, `Infer` `!ok`, or an empty `output_text` → `SetError(text)`,
  `kError`, and the open reply is closed as interrupted so the transcript cannot
  stay stuck in the "generating" visual state.
- `~ChatSession` sets shutdown + abort and joins the worker.
- The worker never touches ImGui and shares only the lock-free bridge plus
  atomic phase/error state with the UI.
- The hot path stays allocation-free: `ChatState` reserves a reply's capacity on
  `BeginTurn`, and a test asserts 1024 drained deltas perform zero `operator new`
  calls (same counting-allocator pattern as the existing bridge test).

## TDD plan

New test file `tests/test_lsxhome_chat.cpp` and a new ctest target
`test_lsxhome_chat` labelled `fast`. It links Catch2 only — no `lsxcommon`, no
CUDA — and drives a fake `GenerationBackend`. Each step is RED first, then the
minimal GREEN:

1. `SplitUtf8Chunks`: chunks never exceed `kTokenTextCapacity - 1` bytes, never
   split a codepoint (Cyrillic, 4-byte emoji), concatenating them reproduces the
   input exactly, empty input yields zero chunks.
2. `ChatState`: `BeginTurn` refuses an empty question and an open reply;
   `AppendDelta` ignores a duplicate and a stale `stream_seq` and accepts the
   next one; `EndTurn(interrupted)` closes the reply; `SetError`/`Clear`.
3. `ChatSession` with a fake backend: `Submit` hands the backend a one-message
   history; deltas reach the state in order; phases walk
   `kLoading → kGenerating → kIdle`; a second question receives
   `[user1, assistant1, user2]`; a `Submit` during generation is refused; a full
   request queue refuses without touching the transcript; `RequestStop()` sets
   the abort flag the backend observes and closes the reply as interrupted;
   `Prepare`/`Infer` failures surface as `kError` plus error text; the destructor
   joins the worker.
4. Zero-allocation drain: 1024 deltas through `DrainInto` leave the allocation
   counter unchanged.
5. `DrawChatPanel` and `LsxGenerationBackend` land after the tests are green and
   stay untested by unit tests, on the same boundary as the device-side D3D12
   code.

## Files

New: `include/lsxhome/chat_state.h`, `include/lsxhome/chat_session.h`,
`include/lsxhome/generation_backend.h`, `src/lsx_generation_backend.cpp`,
`tests/test_lsxhome_chat.cpp`.

Modified: `include/lsxhome/text_splitter.h`, `include/lsxhome/gui_renderer.h`,
`src/gui_renderer.cpp`, `src/main_win32.cpp` (construct the session, auto-submit
`--run`), `CMakeLists.txt` (new source in the executable), `tests/CMakeLists.txt`,
`README.md`, `AGENTS.md`.

## Out of scope

Tabs / parallel conversations, model selection in the UI, system prompts, tool
calling, history persistence, network streaming.

## Acceptance

- `ctest --preset windows-debug` green for label `fast`.
- Manual run of `lsxhome.exe --model <path>`: a typed question reaches the engine,
  the answer streams into the conversation, «Stop» interrupts, and a second
  question is answered with the first exchange in context.