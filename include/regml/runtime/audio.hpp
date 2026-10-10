#pragma once
#include "regml/formats/package.hpp"
#include <memory>
#include <unordered_map>
namespace regml {
class GameAudio {
public:
    GameAudio(const Package& package,std::filesystem::path directory);
    int create_stream(std::string_view filename);
    int play(int sound,bool loop,double priority);
    void stop(int id);void pause(int id,bool paused);bool playing(int id)const;
    double gain(int id);void gain(int id,double value);void pitch(int id,double value);void channel_limit(int count);
    double length(int id);double position(int id)const;void position(int id,double seconds);
    std::vector<float> mix(std::size_t frames);
private:
    struct Clip {std::vector<float> stereo;double gain=1;};
    struct Sound {int embedded=-1;std::string filename;double gain=1;std::shared_ptr<Clip> clip;};
    struct Voice {int sound=0;std::shared_ptr<Clip> clip;double frame=0,gain=1,pitch=1,priority=0;bool loop=false,paused=false;};
    const Package& package_;std::filesystem::path directory_;std::vector<std::size_t> embedded_;std::unordered_map<int,Sound> sounds_;std::unordered_map<int,Voice> voices_;int next_stream_=100000,next_voice_=1000000,channels_=128;
    std::shared_ptr<Clip> load(int id);
    std::shared_ptr<Clip> decode(std::span<const std::uint8_t> bytes);
};
} // namespace regml
