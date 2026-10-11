#include "regml/runtime/audio.hpp"
#include <cmath>
#include <iostream>

namespace {
double rms(const std::vector<float> &samples) {
    double energy = 0;
    for (const float sample : samples)
        energy += static_cast<double>(sample) * sample;
    return samples.empty() ? 0 : std::sqrt(energy / samples.size());
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: regml_audio_test <Undertale data.win>\n";
        return 2;
    }
    try {
        const auto path = std::filesystem::path(argv[1]);
        const auto package = regml::Package::load(path);
        regml::GameAudio audio(package, path.parent_path());
        const auto music_id = audio.find_sound_id("mus_story.ogg");
        const auto blip_id = audio.find_sound_id("SND_TXT2.wav");
        if (!music_id || !blip_id) {
            std::cerr << "FAIL: expected mus_story.ogg and SND_TXT2.wav sound resources\n";
            return 1;
        }

        const auto sond = package.find_chunk("SOND");
        const auto data = package.view();
        const auto sound_table = data.slice(sond->data_offset, sond->size);
        const auto music_record = data.u32(sond->data_offset + 4 + static_cast<std::size_t>(*music_id) * 4);
        const int music_group = data.i32(music_record + 28);
        const int music_audio = data.i32(music_record + 32);
        std::cout << "mus_story sound id=" << *music_id << " group=" << music_group << " audio id=" << music_audio
                  << " (SOND entries=" << sound_table.u32(0) << ")\n";

        const double music_length = audio.length(*music_id);
        const int external_music = audio.create_stream("mus_story.ogg");
        const double external_music_length = audio.length(external_music);
        const int music_voice = audio.play(*music_id, false, 100);
        const auto music_block = audio.mix(4800);
        const double music_rms = rms(music_block);
        const double music_position = audio.position(music_voice);
        const bool music_ok = music_length >= 60 && music_length <= 100 && external_music_length >= 60 &&
                              external_music_length <= 100 && music_rms >= 0.01 && music_position >= 0.09;

        audio.stop(-1);
        const double blip_length = audio.length(*blip_id);
        const int blip_voice = audio.play(*blip_id, false, 80);
        const auto blip_block = audio.mix(4800);
        const double blip_rms = rms(blip_block);
        const bool blip_ok = blip_length > 0 && blip_length <= 2 && blip_rms >= 0.001;

        std::cout << (music_ok ? "PASS" : "FAIL") << ": mus_story.ogg resource length=" << music_length
                  << "s, external file length=" << external_music_length << "s, first-block RMS=" << music_rms
                  << ", playback position=" << music_position << "s\n"
                  << (blip_ok ? "PASS" : "FAIL") << ": SND_TXT2.wav length=" << blip_length
                  << "s, first-block RMS=" << blip_rms << '\n'
                  << "This checks decoding and mixing; it does not test SDL output or speaker playback.\n";
        return music_ok && blip_ok ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
