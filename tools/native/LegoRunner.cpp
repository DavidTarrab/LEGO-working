#include "LegoInference.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static std::string quoted(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
static std::string csv(const std::string& s) {
    std::string result = "\"";
    for (char c : s) { if (c == '"') result += '"'; result += c; }
    return result + '"';
}
struct Event {
    std::string kind, node;
    int decode;
    uint64_t chunks;
    double ms, wait;
};
struct Run {
    lego_inference* inference;
    bool verify = false;
    std::atomic<bool> active{false}, finished{false};
    std::atomic<uint64_t> completed{0};
    std::mutex mutex;
    std::condition_variable condition;
    int pauses = 0, resumes = 0;
    int last_tested_decode = -1, nodes_in_decode = 0;
    bool frozen = true, pauses_inside_decode = true;
    HANDLE paused_event = nullptr;
    std::vector<Event> events;
    std::vector<int> tokens;
    std::string text;
    size_t dropped_events = 0;
    double elapsed_ms = 0;
    explicit Run(lego_inference* ptr) : inference(ptr) {}
    static void event(const lego_event* event, void* ptr) {
        auto& self = *static_cast<Run*>(ptr);
        if (self.events.size() < 100000) self.events.push_back({event->kind, event->node, event->decode_index, event->completed_chunks, event->elapsed_ms, event->wait_ms});
        else ++self.dropped_events;
        const std::string kind = event->kind;
        if (kind == "decode_begin") { self.active = true; self.nodes_in_decode = 0; }
        if (kind == "decode_end") self.active = false;
        if (kind == "chunk_complete") {
            self.completed = event->completed_chunks;
            ++self.nodes_in_decode;
            if (self.verify && event->decode_index <= 1 && self.last_tested_decode != event->decode_index && self.nodes_in_decode == 5) {
                self.last_tested_decode = event->decode_index;
                lego_pause(self.inference);
            }
        }
        if (kind == "paused") {
            if (self.paused_event) SetEvent(self.paused_event);
            std::lock_guard<std::mutex> lock(self.mutex);
            self.pauses_inside_decode = self.pauses_inside_decode && self.active;
            ++self.pauses; self.condition.notify_all();
        }
        if (kind == "resumed") {
            if (self.paused_event) ResetEvent(self.paused_event);
            std::lock_guard<std::mutex> lock(self.mutex);
            ++self.resumes; self.condition.notify_all();
        }
    }
    static void token(int id, const char* piece, void* ptr) {
        auto& self = *static_cast<Run*>(ptr);
        self.tokens.push_back(id); self.text += piece;
    }
};
struct Controls {
    HANDLE handles[4]{};
    HANDLE paused = nullptr;
    std::thread worker;
    Controls(const std::string& name, lego_inference* inference) {
        if (name.empty()) return;
        for (unsigned char c : name) if (!std::isalnum(c) && c != '-' && c != '_') throw std::runtime_error("Invalid control name.");
        const auto base = "Local\\LEGO-" + name + "-";
        const char* suffixes[] = {"Pause", "Resume", "Stop", "Finished", "Paused"};
        try {
            for (int i = 0; i < 5; ++i) {
                HANDLE h = CreateEventA(nullptr, i == 4, FALSE, (base + suffixes[i]).c_str());
                if (!h) throw std::runtime_error("Cannot create control event.");
                if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); throw std::runtime_error("Control name already in use."); }
                if (i < 4) handles[i] = h; else paused = h;
            }
            worker = std::thread([this, inference] {
                for (;;) {
                    DWORD result = WaitForMultipleObjects(4, handles, FALSE, INFINITE);
                    if (result == WAIT_OBJECT_0) lego_pause(inference);
                    else if (result == WAIT_OBJECT_0 + 1) lego_resume(inference);
                    else if (result == WAIT_OBJECT_0 + 2) lego_request_stop(inference);
                    else break;
                }
            });
        } catch (...) {
            for (auto h : handles) if (h) CloseHandle(h);
            if (paused) CloseHandle(paused);
            throw;
        }
    }
    ~Controls() {
        if (worker.joinable()) { SetEvent(handles[3]); worker.join(); }
        for (auto h : handles) if (h) CloseHandle(h);
        if (paused) CloseHandle(paused);
    }
};
static void save_events(const std::filesystem::path& path, const Run& run) {
    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot write checkpoint CSV.");
    file << "event,node,decode_index,completed_chunks,elapsed_ms,wait_ms\n" << std::fixed << std::setprecision(6);
    for (const auto& e : run.events) file << csv(e.kind) << ',' << csv(e.node) << ',' << e.decode << ',' << e.chunks << ',' << e.ms << ',' << e.wait << '\n';
    if (!file) throw std::runtime_error("Checkpoint CSV write failed.");
}
static int execute(Run& run, const std::string& prompt, int count, std::string& error) {
    char buffer[1024]{};
    std::thread verifier;
    if (run.verify) verifier = std::thread([&] {
        int handled = 0;
        std::unique_lock<std::mutex> lock(run.mutex);
        while (run.condition.wait_for(lock, std::chrono::seconds(30), [&] { return run.pauses > handled || run.finished; })) {
            if (run.pauses <= handled) break;
            ++handled;
            lock.unlock();
            auto before = run.completed.load();
            bool active_before = run.active && lego_wait_paused(run.inference, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            bool stable = active_before && run.active && before == run.completed && lego_wait_paused(run.inference, 0);
            lock.lock(); run.frozen = run.frozen && stable; lock.unlock();
            lego_resume(run.inference);
            lock.lock();
            if (!run.condition.wait_for(lock, std::chrono::seconds(5), [&] { return run.resumes >= handled || run.finished; })) break;
        }
        if (!run.finished) { run.frozen = false; lock.unlock(); lego_resume(run.inference); }
    });
    const auto started = std::chrono::steady_clock::now();
    int result = lego_run(run.inference, prompt.c_str(), count, Run::event, Run::token, &run, buffer, sizeof(buffer));
    run.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    run.finished = true; run.condition.notify_all();
    if (verifier.joinable()) verifier.join();
    error = buffer;
    return result;
}
int main(int argc, char** argv) {
    try {
        std::string model, prompt = "You control a fighter. Health 60, enemy health 40, distance 3 meters. Available actions: dodge, attack, retreat. Choose an action and explain briefly.";
        std::string output, control;
        int count = 16, nodes = 1, context = 2048;
        bool verify = false;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--verify") { verify = true; continue; }
            if (key == "--help") {
                std::cout << "lego-infer --model PATH --output NEW_DIRECTORY [--tokens 16] [--prompt TEXT] [--nodes 1] [--context 2048] [--verify | --control-name NAME]\n"
                    << "--nodes 0 disables checkpoints. N >= 1 observes every N graph nodes. Pause waits between completed groups, never inside a CUDA kernel.\n";
                return 0;
            }
            if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
            std::string value = argv[++i];
            if (key == "--model") model = value;
            else if (key == "--output") output = value;
            else if (key == "--prompt") prompt = value;
            else if (key == "--control-name") control = value;
            else if (key == "--tokens") count = std::stoi(value);
            else if (key == "--nodes") nodes = std::stoi(value);
            else if (key == "--context") context = std::stoi(value);
            else throw std::runtime_error("Unknown argument: " + key);
        }
        if (model.empty() || output.empty() || count < 1 || nodes < 0 || context < 32) throw std::runtime_error("Specify --model and --output; use --help for options.");
        if ((!control.empty() || verify) && !nodes) throw std::runtime_error("Pause control requires --nodes >= 1.");
        if (verify && (!control.empty() || count < 2)) throw std::runtime_error("Verification requires at least two tokens and no external controller.");
        const std::filesystem::path out(output);
        if (!std::filesystem::create_directories(out)) throw std::runtime_error("Output directory already exists; choose a new directory.");
        char buffer[1024]{};
        lego_config config{model.c_str(), context, 99, nodes};
        std::unique_ptr<lego_inference, decltype(&lego_destroy)> inference(lego_create(&config, buffer, sizeof(buffer)), lego_destroy);
        if (!inference) throw std::runtime_error(buffer);
        std::ofstream metadata(out / "config.json");
        metadata << "{\"upstream_commit\":" << quoted(lego_upstream_commit()) << ",\"model\":" << quoted(model)
            << ",\"prompt\":" << quoted(prompt) << ",\"tokens\":" << count << ",\"checkpoint_nodes\":" << nodes
            << ",\"cuda_graphs\":false,\"timing_source\":\"upstream-host-wall\",\"control_name\":" << quoted(control) << "}\n";
        if (!metadata) throw std::runtime_error("Cannot write metadata.");
        std::string error;
        Run reference(inference.get());
        if (verify) {
            if (execute(reference, prompt, count, error)) throw std::runtime_error("Reference run failed: " + error);
            save_events(out / "reference-checkpoints.csv", reference);
        }
        Run run(inference.get()); run.verify = verify;
        int result;
        {
            Controls controls(control, inference.get()); run.paused_event = controls.paused;
            std::cout << "READY" << (control.empty() ? "" : " control=" + control) << std::endl;
            result = execute(run, prompt, count, error);
        }
        save_events(out / "checkpoints.csv", run);
        bool matches = !verify || reference.tokens == run.tokens;
        bool passed = result == 0 && (!verify || (matches && run.pauses == 2 && run.resumes == 2 && run.frozen && run.pauses_inside_decode));
        std::ofstream summary(out / "summary.json");
        summary << "{\"status\":" << quoted(result < 0 ? "error" : result ? "stopped" : "complete") << ",\"error\":" << quoted(error)
            << ",\"verification_requested\":" << (verify ? "true" : "false") << ",\"verification_passed\":" << (verify ? passed ? "true" : "false" : "null")
            << ",\"tokens_match_reference\":" << (verify ? matches ? "true" : "false" : "null")
            << ",\"progress_frozen_while_paused\":" << (verify ? run.frozen ? "true" : "false" : "null")
            << ",\"pauses_inside_decode\":" << (verify ? run.pauses_inside_decode ? "true" : "false" : "null")
            << ",\"pauses\":" << run.pauses << ",\"resumes\":" << run.resumes << ",\"completed_chunks\":" << run.completed.load()
            << ",\"host_elapsed_ms\":" << run.elapsed_ms
            << ",\"reference_host_elapsed_ms\":" << (verify ? std::to_string(reference.elapsed_ms) : "null")
            << ",\"dropped_checkpoint_events\":" << run.dropped_events << ",\"text\":" << quoted(run.text) << ",\"token_ids\":[";
        for (size_t i = 0; i < run.tokens.size(); ++i) { if (i) summary << ','; summary << run.tokens[i]; }
        summary << "]}\n";
        if (!summary) throw std::runtime_error("Summary write failed.");
        std::cout << run.text << "\n" << (verify ? passed ? "VERIFICATION PASSED" : "VERIFICATION FAILED" : "Run finished") << "\nArtifacts: " << out.string() << '\n';
        return result < 0 || (verify && !passed) ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
