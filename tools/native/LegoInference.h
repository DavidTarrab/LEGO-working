#pragma once
#include <stdint.h>
#ifdef _WIN32
#ifdef LEGO_INFERENCE_EXPORTS
#define LEGO_API __declspec(dllexport)
#else
#define LEGO_API __declspec(dllimport)
#endif
#else
#define LEGO_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lego_inference lego_inference;
typedef struct lego_config {
    const char* model_path;
    int context_tokens;
    int gpu_layers;
    // 0 = upstream execution without observer; N >= 1 = observe every N nodes.
    int checkpoint_nodes;
} lego_config;
typedef struct lego_event {
    const char* kind;
    const char* node;
    int decode_index; // 0 = prompt processing, 1+ = generated-token decode
    uint64_t completed_chunks;
    double elapsed_ms;
    double wait_ms;
} lego_event;
// Called on the inference thread. Copy strings before returning. Do not call
// run/destroy from a callback. pause/resume/request_stop are safe here.
typedef void (*lego_event_callback)(const lego_event*, void*);
typedef void (*lego_token_callback)(int token, const char* text, void*);
LEGO_API const char* lego_upstream_commit(void);
LEGO_API lego_inference* lego_create(const lego_config*, char* error, int error_size);
// Owner-thread query; returns -1 for invalid inputs. Does not execute GPU work.
LEGO_API int lego_prompt_token_count(lego_inference*, const char* prompt);
// Single caller owns run/destroy. run is synchronous; control calls below can
// come from another thread. Returns 0 success, 1 stopped at a decode boundary,
// -1 error. Stop releases the gate and lets the current decode safely finish.
LEGO_API int lego_run(lego_inference*, const char* prompt, int tokens,
    lego_event_callback, lego_token_callback, void* user, char* error, int error_size);
LEGO_API int lego_pause(lego_inference*); // request, not acknowledgement
LEGO_API void lego_resume(lego_inference*);
LEGO_API int lego_wait_paused(lego_inference*, int timeout_ms);
LEGO_API void lego_request_stop(lego_inference*);
LEGO_API void lego_destroy(lego_inference*);
#ifdef __cplusplus
}
#endif
