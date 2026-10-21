#include "audio_async.hpp"
#include "error.hpp"
#include <fstream>

namespace regml {
GameAudio::AsyncState::AsyncState() : mutex_(SDL_CreateMutex()) {
    if (!mutex_)
        throw Error(ErrorKind::io, "audio decode mutex creation failed");
}

GameAudio::AsyncState::~AsyncState() {
    for (auto *thread : threads_)
        SDL_WaitThread(thread, nullptr);
    for (const auto &item : completed_)
        delete item.clip;
    SDL_DestroyMutex(mutex_);
}

bool GameAudio::AsyncState::pending(int sound) const {
    return pending_.contains(sound);
}
void GameAudio::AsyncState::finish(int sound) {
    pending_.erase(sound);
}
void GameAudio::AsyncState::post(Completion completion) {
    SDL_LockMutex(mutex_);
    completed_.push_back(std::move(completion));
    SDL_UnlockMutex(mutex_);
}

void GameAudio::AsyncState::request(int sound,
                                    std::filesystem::path path,
                                    const Package &package,
                                    std::size_t embedded_offset,
                                    std::size_t embedded_size,
                                    bool has_embedded) {
    auto task = std::make_unique<Task>(
        Task{this, sound, std::move(path), &package, embedded_offset, embedded_size, has_embedded});
    auto *thread = SDL_CreateThread(thread_entry, "reGML audio decode", task.get());
    if (!thread)
        throw Error(ErrorKind::io, std::string("audio decoder thread: ") + SDL_GetError());
    pending_.insert(sound);
    threads_.push_back(thread);
    task.release();
}

std::vector<GameAudio::AsyncState::Completion> GameAudio::AsyncState::take_completed() {
    std::vector<Completion> result;
    SDL_LockMutex(mutex_);
    result.swap(completed_);
    SDL_UnlockMutex(mutex_);
    return result;
}

int GameAudio::AsyncState::thread_entry(void *context) {
    return decode_thread(context);
}

int GameAudio::AsyncState::decode_thread(void *context) {
    std::unique_ptr<AsyncState::Task> task(static_cast<AsyncState::Task *>(context));
    Clip *clip = nullptr;
    std::string error;
    try {
        std::vector<std::uint8_t> bytes;
        std::ifstream file(task->path, std::ios::binary | std::ios::ate);
        if (file) {
            const auto size = file.tellg();
            if (size < 0 || size > 100'000'000)
                throw Error(ErrorKind::unsupported, "audio file size exceeds limit");
            bytes.resize(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file)
                throw Error(ErrorKind::io, "audio file read failed");
        } else if (task->has_embedded) {
            bytes = task->package->read_bytes(task->embedded_offset, task->embedded_size);
        } else {
            throw Error(ErrorKind::io, "audio file missing: " + task->path.string());
        }
        clip = decode(bytes).release();
    } catch (const std::exception &exception) {
        error = exception.what();
    } catch (...) {
        error = "audio decode failed";
    }
    task->state->post({task->sound, clip, std::move(error)});
    return 0;
}
} // namespace regml
