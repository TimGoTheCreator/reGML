#include "regml/runtime/audio.hpp"
#include <vorbis/vorbisfile.h>
#include <algorithm>
#include <fstream>
#include <cmath>
#include <cstring>
namespace regml {
GameAudio::GameAudio(const Package& package,std::filesystem::path directory):package_(package),directory_(std::move(directory)) {
    const auto d=package.view();
    auto pointers=[&](const Chunk& c){const auto view=d.slice(c.data_offset,c.size);const auto count=view.u32(0);if(count>1'000'000 || count>(c.size-4)/4)throw Error(ErrorKind::malformed,"audio table range",c.data_offset);std::vector<std::size_t> out;for(std::size_t i=0;i<count;++i){const auto p=view.u32(4+i*4);if(p<c.data_offset || p>=c.data_offset+c.size)throw Error(ErrorKind::malformed,"audio pointer outside chunk",p);out.push_back(p);}return out;};
    if(const auto* c=package.find_chunk("AUDO"))embedded_=pointers(*c);
    if(const auto* c=package.find_chunk("SOND")){int id=0;for(auto p:pointers(*c)){if(c->data_offset+c->size-p<36)throw Error(ErrorKind::malformed,"sound record range",p);Sound sound;sound.filename=package.string_pointer(d.u32(p+12));sound.gain=std::bit_cast<float>(d.u32(p+20));const auto group=d.i32(p+28),audio_id=d.i32(p+32);if(group==0)sound.embedded=audio_id;sounds_.emplace(id++,std::move(sound));}}
}
std::shared_ptr<GameAudio::Clip> GameAudio::decode(std::span<const std::uint8_t> bytes) {
    std::vector<float> source;long rate=0;int channels=0;
    if(bytes.size()>=4 && std::memcmp(bytes.data(),"OggS",4)==0){
        struct Reader {std::span<const std::uint8_t> bytes;std::size_t position=0;}reader{bytes};
        ov_callbacks callbacks{};
        callbacks.read_func=[](void* ptr,std::size_t size,std::size_t count,void* context)->std::size_t{auto& r=*static_cast<Reader*>(context);if(size==0)return 0;const auto items=std::min(count,(r.bytes.size()-r.position)/size);std::memcpy(ptr,r.bytes.data()+r.position,items*size);r.position+=items*size;return items;};
        callbacks.seek_func=[](void* context,ogg_int64_t offset,int origin)->int{auto& r=*static_cast<Reader*>(context);const auto base=origin==SEEK_SET?0:origin==SEEK_CUR?static_cast<ogg_int64_t>(r.position):static_cast<ogg_int64_t>(r.bytes.size());const auto target=base+offset;if(target<0 || static_cast<std::uint64_t>(target)>r.bytes.size())return -1;r.position=static_cast<std::size_t>(target);return 0;};
        callbacks.tell_func=[](void* context)->long{return static_cast<long>(static_cast<Reader*>(context)->position);};
        OggVorbis_File file{};if(ov_open_callbacks(&reader,&file,nullptr,0,callbacks)<0)throw Error(ErrorKind::malformed,"Ogg audio decode initialization failed");struct Guard{OggVorbis_File& f;~Guard(){ov_clear(&f);}}guard{file};
        const auto* info=ov_info(&file,-1);rate=info->rate;channels=info->channels;if(channels<1 || channels>2 || rate<1 || rate>192000)throw Error(ErrorKind::unsupported,"unsupported Ogg channel/rate profile");
        int section=0;float** pcm=nullptr;while(true){const long count=ov_read_float(&file,&pcm,4096,&section);if(count==0)break;if(count<0)throw Error(ErrorKind::malformed,"Ogg decode failed");if(source.size()+static_cast<std::size_t>(count)*static_cast<std::size_t>(channels)>100'000'000)throw Error(ErrorKind::unsupported,"decoded audio exceeds clip limit");for(long i=0;i<count;++i)for(int c=0;c<channels;++c)source.push_back(pcm[c][i]);}
    }else {
        const BinaryView d(bytes);if(d.text(0,4)!="RIFF" || d.text(8,4)!="WAVE")throw Error(ErrorKind::unsupported,"audio format requires WAV or Ogg Vorbis");
        int format=0,bits=0;std::span<const std::uint8_t> pcm;
        for(std::size_t at=12;at+8<=d.size();){const auto size=d.u32(at+4);const auto block=d.slice(at+8,size);if(d.text(at,4)=="fmt "){format=block.u16(0);channels=block.u16(2);const auto sample_rate=block.u32(4);if(sample_rate>192000)throw Error(ErrorKind::unsupported,"WAV sample rate exceeds limit");rate=static_cast<long>(sample_rate);bits=block.u16(14);}else if(d.text(at,4)=="data")pcm=block.bytes();at+=8+size+(size&1);}
        if(channels<1 || channels>2 || rate<1 || rate>192000 || format!=1 || (bits!=8 && bits!=16))throw Error(ErrorKind::unsupported,"unsupported WAV sample profile");
        const BinaryView samples(pcm);for(std::size_t at=0;at+static_cast<std::size_t>(bits/8)<=samples.size();at+=static_cast<std::size_t>(bits/8))source.push_back(bits==16?static_cast<float>(samples.i16(at))/32768.0f:(static_cast<float>(samples.u8(at))-128)/128);
    }
    auto clip=std::make_shared<Clip>();const auto count=source.size()/static_cast<std::size_t>(channels);if(count==0)throw Error(ErrorKind::malformed,"empty audio clip");const auto frames=static_cast<std::size_t>(static_cast<double>(count)*48000/static_cast<double>(rate));if(frames>50'000'000)throw Error(ErrorKind::unsupported,"resampled audio exceeds limit");clip->stereo.resize(frames*2);
    for(std::size_t f=0;f<frames;++f){const double input=static_cast<double>(f)*static_cast<double>(rate)/48000;const auto a=std::min(static_cast<std::size_t>(input),count-1),b=std::min(a+1,count-1);const float t=static_cast<float>(input-std::floor(input));for(std::size_t c=0;c<2;++c){const auto ch=std::min(c,static_cast<std::size_t>(channels-1));clip->stereo[f*2+c]=source[a*static_cast<std::size_t>(channels)+ch]*(1-t)+source[b*static_cast<std::size_t>(channels)+ch]*t;}}return clip;
}
std::shared_ptr<GameAudio::Clip> GameAudio::load(int id) {
    auto found=sounds_.find(id);if(found==sounds_.end())throw Error(ErrorKind::vm,"audio resource does not exist");auto& sound=found->second;if(sound.clip)return sound.clip;
    if(sound.embedded>=0){if(static_cast<std::size_t>(sound.embedded)>=embedded_.size())throw Error(ErrorKind::malformed,"embedded audio ID out of range");const auto at=embedded_[static_cast<std::size_t>(sound.embedded)];const auto* c=package_.find_chunk("AUDO");const auto d=package_.view();const auto size=d.u32(at);if(size>c->data_offset+c->size-at-4)throw Error(ErrorKind::malformed,"embedded audio length out of range",at);sound.clip=decode(d.slice(at+4,size).bytes());}
    else {const auto path=directory_/sound.filename;std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw Error(ErrorKind::io,"audio file missing: "+path.string());const auto size=file.tellg();if(size<0 || size>100'000'000)throw Error(ErrorKind::unsupported,"audio file size exceeds limit");std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!file)throw Error(ErrorKind::io,"audio file read failed");sound.clip=decode(bytes);}
    sound.clip->gain=sound.gain;return sound.clip;
}
int GameAudio::create_stream(std::string_view filename){const int id=next_stream_++;sounds_[id]={-1,std::string(filename),1,{}};load(id);return id;}
int GameAudio::play(int sound,bool loop,double priority){auto clip=load(sound);if(voices_.size()>=static_cast<std::size_t>(channels_)){auto lowest=std::min_element(voices_.begin(),voices_.end(),[](const auto& a,const auto& b){return a.second.priority<b.second.priority;});if(lowest->second.priority>priority)return -1;voices_.erase(lowest);}const int id=next_voice_++;voices_[id]={sound,clip,0,1,1,priority,loop,false};return id;}
void GameAudio::stop(int id){std::erase_if(voices_,[&](const auto& p){return id==-1 || p.first==id || p.second.sound==id;});}
void GameAudio::pause(int id,bool paused){for(auto& [voice,v]:voices_)if(id==-1 || voice==id || v.sound==id)v.paused=paused;}
bool GameAudio::playing(int id)const{for(const auto& [voice,v]:voices_)if((voice==id || v.sound==id) && !v.paused)return true;return false;}
double GameAudio::gain(int id){if(const auto it=voices_.find(id);it!=voices_.end())return it->second.gain;return load(id)->gain;}
void GameAudio::gain(int id,double value){if(!std::isfinite(value))throw Error(ErrorKind::vm,"nonfinite audio gain");if(auto it=voices_.find(id);it!=voices_.end())it->second.gain=value;else load(id)->gain=value;}
void GameAudio::pitch(int id,double value){if(!std::isfinite(value) || value<=0 || value>16)throw Error(ErrorKind::vm,"invalid audio pitch");for(auto& [voice,v]:voices_)if(voice==id || v.sound==id)v.pitch=value;}
void GameAudio::channel_limit(int count){channels_=std::clamp(count,1,1024);}
double GameAudio::length(int id){if(auto it=voices_.find(id);it!=voices_.end())return static_cast<double>(it->second.clip->stereo.size())/96000;return static_cast<double>(load(id)->stereo.size())/96000;}
double GameAudio::position(int id)const{const auto it=voices_.find(id);return it==voices_.end()?0:it->second.frame/48000;}
void GameAudio::position(int id,double seconds){if(!std::isfinite(seconds) || seconds<0)throw Error(ErrorKind::vm,"invalid audio seek");if(auto it=voices_.find(id);it!=voices_.end())it->second.frame=std::min(seconds*48000,static_cast<double>(it->second.clip->stereo.size()/2));}
std::vector<float> GameAudio::mix(std::size_t frames){if(frames>48000)throw Error(ErrorKind::vm,"audio block exceeds limit");std::vector<float> out(frames*2,0);for(auto& [id,v]:voices_){if(v.paused)continue;const auto length=v.clip->stereo.size()/2;for(std::size_t f=0;f<frames;++f){if(v.frame>=static_cast<double>(length)){if(v.loop)v.frame=std::fmod(v.frame,static_cast<double>(length));else break;}const auto source=static_cast<std::size_t>(v.frame);for(std::size_t c=0;c<2;++c)out[f*2+c]+=v.clip->stereo[source*2+c]*static_cast<float>(v.gain*v.clip->gain);v.frame+=v.pitch;}}std::erase_if(voices_,[](const auto& p){return !p.second.loop && p.second.frame>=static_cast<double>(p.second.clip->stereo.size()/2);});for(auto& sample:out)sample=std::clamp(sample,-1.0f,1.0f);return out;}
} // namespace regml
