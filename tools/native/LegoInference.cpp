#include "LegoInference.h"
#include "llama.h"
#include "ggml-backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
struct lego_inference {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    llama_sampler* sampler = nullptr;
    llama_batch_ext* batch = nullptr;
    int checkpoint_nodes = 1, decode_index = 0;
    uint64_t asked = 0, completed = 0;
    std::mutex mutex;
    std::condition_variable condition;
    bool pause_requested = false, paused = false;
    std::atomic<bool> stop{false}, running{false};
    Clock::time_point origin;
    lego_event_callback callback = nullptr;
    void* user = nullptr;

    double elapsed() const { return std::chrono::duration<double, std::milli>(Clock::now() - origin).count(); }
    void emit(const char* kind, const char* node = "", double wait = 0) {
        if (callback) {
            lego_event event{kind, node, decode_index, completed, elapsed(), wait};
            callback(&event, user);
        }
    }
    void checkpoint(const char* node) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!pause_requested || stop.load()) return;
        const auto begin = Clock::now();
        paused = true;
        condition.notify_all();
        lock.unlock();
        emit("paused", node);
        lock.lock();
        condition.wait(lock, [&] { return !pause_requested || stop.load(); });
        paused = false;
        condition.notify_all();
        lock.unlock();
        emit("resumed", node, std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    }
    static bool evaluate(ggml_tensor* tensor, bool ask, void* data) {
        auto& self = *static_cast<lego_inference*>(data);
        const char* name = ggml_get_name(tensor);
        if (ask) {
            // This is called while upstream chooses the next graph range,
            // before launching it. The previous observed range is synchronized.
            // Upstream may scan several nodes before choosing a range. Every
            // query is an admission opportunity, not a measured kernel start.
            self.emit("admission_check", name);
            self.checkpoint(name);
            return ++self.asked % self.checkpoint_nodes == 0;
        }
        // Upstream synchronizes this backend before invoking ask=false.
        ++self.completed;
        self.emit("chunk_complete", name);
        self.checkpoint(name);
        // Returning false cancels graph evaluation; it is NOT a yield/resume API.
        return true;
    }
    ~lego_inference() {
        if (batch) llama_batch_ext_free(batch);
        if (sampler) llama_sampler_free(sampler);
        if (context) llama_free(context);
        if (model) llama_model_free(model);
    }
};
static void error_text(char* dest, int size, const char* message) {
    if (dest && size > 0) std::snprintf(dest, size, "%s", message);
}
const char* lego_upstream_commit() { return LEGO_UPSTREAM_COMMIT; }
lego_inference* lego_create(const lego_config* config, char* error, int size) {
    lego_inference* result = nullptr;
    try {
        if (!config || !config->model_path || config->context_tokens < 32 || config->gpu_layers < 1 || config->checkpoint_nodes < 0)
            throw std::runtime_error("Invalid configuration; GPU inference is required.");
        // Upstream backend initialization is process global. Keep it initialized
        // until process exit; never free it while another instance may use it.
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            // Upstream defaults include extremely verbose graph diagnostics.
            // Preserve actionable messages; opt into debug logs explicitly.
            static bool debug = std::getenv("LEGO_INFERENCE_DEBUG") != nullptr;
            llama_log_set([](ggml_log_level level, const char* text, void* data) {
                if (level != GGML_LOG_LEVEL_DEBUG || *static_cast<bool*>(data)) std::fputs(text, stderr);
            }, &debug);
            llama_backend_init();
        });
        bool gpu = false;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i)
            if (ggml_backend_dev_type(ggml_backend_dev_get(i)) == GGML_BACKEND_DEVICE_TYPE_GPU) gpu = true;
        if (!gpu) throw std::runtime_error("No upstream GPU backend registered.");
        result = new lego_inference;
        result->checkpoint_nodes = config->checkpoint_nodes;
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = config->gpu_layers;
        result->model = llama_model_load_from_file(config->model_path, mp);
        if (!result->model) throw std::runtime_error("Model loading failed; see upstream diagnostics.");
        auto cp = llama_context_default_params();
        cp.n_ctx = config->context_tokens;
        cp.n_batch = std::min(512, config->context_tokens);
        cp.n_ubatch = cp.n_batch;
        cp.n_threads = 4; cp.n_threads_batch = 4;
        if (config->checkpoint_nodes) { cp.cb_eval = lego_inference::evaluate; cp.cb_eval_user_data = result; }
        result->context = llama_init_from_model(result->model, cp);
        if (!result->context) throw std::runtime_error("Context creation failed.");
        result->sampler = llama_sampler_init_greedy();
        result->batch = llama_batch_ext_init(result->context);
        if (!result->sampler || !result->batch) throw std::runtime_error("Sampler/batch creation failed.");
        return result;
    } catch (const std::exception& e) { error_text(error, size, e.what()); delete result; return nullptr; }
}
int lego_prompt_token_count(lego_inference* self, const char* prompt) {
    if (!self || !prompt) return -1;
    return -llama_tokenize(llama_model_get_vocab(self->model), prompt, static_cast<int>(std::strlen(prompt)), nullptr, 0, true, true);
}
int lego_pause(lego_inference* self) {
    if (!self || !self->checkpoint_nodes) return 0;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pause_requested = true;
    return 1;
}
void lego_resume(lego_inference* self) {
    if (!self) return;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->pause_requested = false;
    self->condition.notify_all();
}
int lego_wait_paused(lego_inference* self, int timeout_ms) {
    if (!self || timeout_ms < 0) return 0;
    std::unique_lock<std::mutex> lock(self->mutex);
    return self->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return self->paused; }) ? 1 : 0;
}
void lego_request_stop(lego_inference* self) {
    if (!self) return;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->stop = true;
    self->pause_requested = false;
    self->condition.notify_all();
}
int lego_run(lego_inference* self, const char* prompt, int count, lego_event_callback cb, lego_token_callback token_cb,
             void* user, char* error, int size) {
    if (!self || !prompt || count < 1) { error_text(error, size, "Invalid run arguments."); return -1; }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        bool expected = false;
        if (!self->running.compare_exchange_strong(expected, true)) { error_text(error, size, "Instance already running."); return -1; }
        self->stop = false;
    }
    struct Reset { lego_inference* s; ~Reset() { s->callback = nullptr; s->running = false; } } reset{self};
    self->callback = cb; self->user = user;
    self->origin = Clock::now(); self->asked = 0; self->completed = 0; self->decode_index = 0;
    try {
        const auto* vocab = llama_model_get_vocab(self->model);
        const int length = -llama_tokenize(vocab, prompt, static_cast<int>(std::strlen(prompt)), nullptr, 0, true, true);
        if (length < 1 || static_cast<int64_t>(length) + count > llama_n_ctx(self->context)) throw std::runtime_error("Prompt/output exceeds context.");
        std::vector<llama_token> input(length);
        if (llama_tokenize(vocab, prompt, static_cast<int>(std::strlen(prompt)), input.data(), length, true, true) != length)
            throw std::runtime_error("Prompt tokenization failed.");
        llama_memory_clear(llama_get_memory(self->context), true);
        llama_sampler_reset(self->sampler);
        int position = 0;
        const int batch_size = static_cast<int>(llama_n_batch(self->context));
        for (int step = 0; step < count; ++step) {
            if (self->stop) return 1;
            self->decode_index = step;
            int offset = 0;
            do {
                llama_batch_ext_clear(self->batch);
                const int n = std::min(batch_size, static_cast<int>(input.size()) - offset);
                for (int i = 0; i < n; ++i) {
                    int index = llama_batch_ext_add_token(self->batch, 0, input[offset + i]);
                    llama_pos p = position + i;
                    if (!llama_batch_ext_set_pos(self->batch, index, &p)) throw std::runtime_error("Invalid batch position.");
                }
                if (!llama_batch_ext_set_output_logits(self->batch, n - 1, true)) throw std::runtime_error("Cannot request logits.");
                self->checkpoint("decode_start");
                self->emit("decode_begin");
                if (llama_process(self->context, LLAMA_PROCESS_TYPE_DECODE, self->batch)) throw std::runtime_error("Upstream decode failed.");
                llama_synchronize(self->context);
                self->emit("decode_end");
                if (self->stop) return 1;
                position += n; offset += n;
            } while (offset < static_cast<int>(input.size()));
            const auto next = llama_sampler_sample(self->sampler, self->context, -1);
            llama_sampler_accept(self->sampler, next);
            if (llama_vocab_is_eog(vocab, next)) break;
            char buffer[256];
            int bytes = llama_token_to_piece(vocab, next, buffer, sizeof(buffer), 0, true);
            std::string piece;
            if (bytes < 0) {
                piece.resize(-bytes);
                bytes = llama_token_to_piece(vocab, next, piece.data(), static_cast<int>(piece.size()), 0, true);
                if (bytes < 0) throw std::runtime_error("Token conversion failed.");
                piece.resize(bytes);
            } else piece.assign(buffer, bytes);
            if (token_cb) token_cb(next, piece.c_str(), user);
            input.assign(1, next);
        }
        return 0;
    } catch (const std::exception& e) { error_text(error, size, e.what()); return -1; }
}
void lego_destroy(lego_inference* self) { delete self; }
