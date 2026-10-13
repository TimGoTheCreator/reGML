#pragma once
#include "audio/v1/audio.hpp"
#include <SDL2/SDL.h>
#include <unordered_set>

namespace regml {
struct GameAudio::AsyncState {
    struct Task {
        AsyncState *state;
        int sound;
        std::filesystem::path path;
        const Package *package;
        std::size_t embedded_offset = 0, embedded_size = 0;
        bool has_embedded = false;
    };
    struct Completion {
        int sound;
        Clip *clip;
        std::string error;
    };

    AsyncState();
    ~AsyncState();
    bool pending(int sound) const;
    void finish(int sound);
    void post(Completion completion);
    void request(int sound, std::filesystem::path path, const Package &package, std::size_t embedded_offset = 0,
                 std::size_t embedded_size = 0, bool has_embedded = false);
    std::vector<Completion> take_completed();

  private:
    static int thread_entry(void *context);
    static int decode_thread(void *context);
    SDL_mutex *mutex_ = nullptr;
    std::vector<SDL_Thread *> threads_;
    std::vector<Completion> completed_;
    std::unordered_set<int> pending_;
};
} // namespace regml
