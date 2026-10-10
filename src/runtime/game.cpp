#include "regml/runtime/game.hpp"
#include "regml/bytecode/linker.hpp"
#include "regml/vm/interpreter.hpp"
#include "regml/render/assets.hpp"
#include "regml/runtime/audio.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <map>
#include <cctype>
#include <fstream>
#include <chrono>
namespace regml {
namespace {
int number(const Value& v) {
    const auto n=value_number(v);
    if (!std::isfinite(n) || n<INT32_MIN || n>INT32_MAX) throw Error(ErrorKind::vm,"integer value out of range");
    return static_cast<int>(n);
}
struct Object {
    std::string name; int sprite=-1,mask=-1,parent=-1,depth=0; bool visible=false,persistent=false,solid=false;
    std::map<std::pair<int,int>,std::vector<int>> events;
};
struct Placement { int object=-1,id=0,create=-1,precreate=-1; double x=0,y=0,sx=1,sy=1; };
struct Background {int id=-1,x=0,y=0;bool foreground=false;};
struct Tile {int id=-1,x=0,y=0,source_x=0,source_y=0,width=0,height=0,depth=0;double sx=1,sy=1;unsigned color=0xffffffff;};
struct Room { std::string name; int width=640,height=480,speed=30,create=-1; bool persistent=false;unsigned color=0; std::vector<Placement> instances;VariableStore views;std::vector<Background> backgrounds;std::vector<Tile> tiles; };
struct PathPoint {double x=0,y=0,speed=100,distance=0;};
struct Path {bool smooth=false;unsigned precision=4;double length=0;std::vector<PathPoint> points;};
struct Instance { int id=0,object=0; bool alive=true,outside_room=false; VariableStore vars; };
}
struct GameSession::Impl {
    Package package; LinkedPackage linked; GameAssets assets; GameAudio audio; Canvas canvas{640,480};std::vector<float> audio_frame;
    std::vector<Path> paths;std::vector<Object> objects; std::vector<Room> rooms; std::vector<int> order,scripts;
    std::unordered_map<std::string,int> script_names;
    std::vector<std::shared_ptr<Instance>> instances;
    struct SavedRoom {std::vector<std::shared_ptr<Instance>> instances;VariableStore engine;};std::unordered_map<int,SavedRoom> saved_rooms;
    VariableStore globals,engine; KeyboardState held{},pressed{},previous_held{},released{};
    std::unordered_map<int,VariableStore> maps;
    std::unordered_map<int,std::vector<Value>> lists;
    int next_ds=0;
    std::unordered_map<int,std::unique_ptr<std::fstream>> files;int next_file=0;
    std::filesystem::path save_dir=std::filesystem::current_path()/"build"/"saves"/"undertale",ini_path;
    std::map<std::string,std::map<std::string,std::string>> ini;
    bool ini_dirty=false;
    unsigned draw_color=0xffffff;double draw_alpha=1;int draw_font=-1,halign=0,valign=0;
    std::size_t frame=0,instruction_count=0,depth=0,event_budget=0;
    int current_room=-1,pending_room=-1,next_id=200000; std::mt19937 random{0x7265474d};
    bool game_started=false;
    struct EventFrame {int type=0,subtype=0,object=0;};std::vector<EventFrame> event_stack;
    std::filesystem::path game_dir;
    std::optional<std::uint32_t> fixed_seed;
    explicit Impl(const std::filesystem::path& path,std::optional<std::uint32_t> seed,const std::filesystem::path& save_directory):package(Package::load(path)),linked(link_legacy_package(package)),assets(package),audio(package,path.parent_path()),game_dir(path.parent_path()),fixed_seed(seed) {
        if(seed)random.seed(*seed);if(!save_directory.empty())save_dir=save_directory;
        load_world();
        engine={{"os_type",std::int32_t{0}},{"os_version",std::int32_t{0}},{"debug_mode",std::int32_t{0}},
            {"working_directory",game_dir.string()+"/"},{"program_directory",game_dir.string()+"/"},
            {"display_aa",std::int32_t{0}},{"application_surface",std::int32_t{0}},
            {"current_time",0.0},{"delta_time",33333.0},{"background_color",std::int32_t{0}},{"fps",30.0},{"fps_real",30.0},
            {"keyboard_key",std::int32_t{0}},{"keyboard_lastkey",std::int32_t{0}},{"keyboard_string",std::string{}},
            {"mouse_x",0.0},{"mouse_y",0.0},{"mouse_button",std::int32_t{0}}};
        enter(order.at(0));
    }
    std::vector<std::size_t> list(std::size_t at,const Chunk& c) const {
        if (at<c.data_offset || at>c.data_offset+c.size || c.data_offset+c.size-at<4) throw Error(ErrorKind::malformed,"resource list outside chunk",at);
        const auto data=package.view(); const auto count=static_cast<std::size_t>(data.u32(at));
        if (count>1'000'000 || count>(c.data_offset+c.size-at-4)/4) throw Error(ErrorKind::malformed,"invalid resource list",at);
        std::vector<std::size_t> out;
        for (std::size_t i=0;i<count;++i) { const auto p=data.u32(at+4+i*4); if(p<c.data_offset || p>=c.data_offset+c.size) throw Error(ErrorKind::malformed,"resource pointer outside chunk",at+4+i*4); out.push_back(p); }
        return out;
    }
    BinaryView record(std::size_t at,const Chunk& c,std::size_t length) const {
        if(at<c.data_offset || at>c.data_offset+c.size || length>c.data_offset+c.size-at) throw Error(ErrorKind::malformed,"resource record outside chunk",at);
        return package.view().slice(at,length);
    }
    void code_valid(int id) const { if(id < -1 || (id>=0 && static_cast<std::size_t>(id)>=linked.programs.size())) throw Error(ErrorKind::malformed,"invalid resource code ID"); }
    void load_world() {
        const auto* sc=package.find_chunk("SCPT"); const auto* ob=package.find_chunk("OBJT"); const auto* ro=package.find_chunk("ROOM"); const auto* ge=package.find_chunk("GEN8");
        if(!sc || !ob || !ro || !ge) throw Error(ErrorKind::unsupported,"game requires SCPT/OBJT/ROOM/GEN8 resources");
        for(auto p:list(sc->data_offset,*sc)) { const auto r=record(p,*sc,8); const auto name=std::string(package.string_pointer(r.u32(0))); const auto id=r.i32(4); code_valid(id); scripts.push_back(id); script_names.emplace(name,id); }
        if(const auto* chunk=package.find_chunk("PATH"))for(auto p:list(chunk->data_offset,*chunk)){
            const auto header=record(p,*chunk,20);Path path;path.smooth=header.u32(4)!=0;path.precision=std::clamp(header.u32(12),1u,8u);const auto count=header.u32(16);if(count>65536)throw Error(ErrorKind::malformed,"path point limit",p);
            const auto points=record(p+20,*chunk,static_cast<std::size_t>(count)*12);
            for(std::size_t j=0;j<count;++j){PathPoint point;point.x=std::bit_cast<float>(points.u32(j*12));point.y=std::bit_cast<float>(points.u32(j*12+4));point.speed=std::bit_cast<float>(points.u32(j*12+8));if(!path.points.empty())path.length+=std::hypot(point.x-path.points.back().x,point.y-path.points.back().y);point.distance=path.length;path.points.push_back(point);}
            const bool closed=header.u32(8)!=0;
            if(closed && path.points.size()>1)path.points.push_back(path.points.front());
            // Smooth GMS paths are cubic curves through their control points. Store
            // a small arc-length lookup table so path movement stays distance based.
            if(path.smooth && path.points.size()>1){
                const auto controls=std::move(path.points);path.points.clear();path.length=0;const int subdivisions=static_cast<int>(path.precision*8);path.points.reserve((controls.size()-1)*static_cast<std::size_t>(subdivisions+1)+1);
                for(std::size_t segment=0;segment+1<controls.size();++segment){
                    const auto& p1=controls[segment];const auto& p2=controls[segment+1];
                    const auto& p0=segment?controls[segment-1]:(closed?controls[controls.size()-2]:p1);
                    const auto& p3=segment+2<controls.size()?controls[segment+2]:(closed?controls[1]:p2);
                    for(int sample=0;sample<subdivisions;++sample){const double t=static_cast<double>(sample)/subdivisions,t2=t*t,t3=t2*t;
                        const double x=0.5*((2*p1.x)+(-p0.x+p2.x)*t+(2*p0.x-5*p1.x+4*p2.x-p3.x)*t2+(-p0.x+3*p1.x-3*p2.x+p3.x)*t3);
                        const double y=0.5*((2*p1.y)+(-p0.y+p2.y)*t+(2*p0.y-5*p1.y+4*p2.y-p3.y)*t2+(-p0.y+3*p1.y-3*p2.y+p3.y)*t3);
                        const double speed=p1.speed+(p2.speed-p1.speed)*t;
                        PathPoint point{x,y,speed,0};if(!path.points.empty())path.length+=std::hypot(x-path.points.back().x,y-path.points.back().y);point.distance=path.length;path.points.push_back(point);
                    }
                }
                const auto& last=controls.back();if(!path.points.empty())path.length+=std::hypot(last.x-path.points.back().x,last.y-path.points.back().y);path.points.push_back({last.x,last.y,last.speed,path.length});
            }else{
                path.length=0;for(std::size_t j=0;j<path.points.size();++j){if(j)path.length+=std::hypot(path.points[j].x-path.points[j-1].x,path.points[j].y-path.points[j-1].y);path.points[j].distance=path.length;}
            }
            paths.push_back(std::move(path));
        }
        for(auto p:list(ob->data_offset,*ob)) {
            auto r=record(p,*ob,80); Object o; o.name=package.string_pointer(r.u32(0)); o.sprite=r.i32(4);o.visible=r.u32(8)!=0;o.depth=r.i32(16);o.persistent=r.u32(20)!=0;o.parent=r.i32(24);
            o.mask=r.i32(28);o.solid=r.u32(12)!=0;
            const auto vertices=r.u32(64); if(vertices>4096) throw Error(ErrorKind::unsupported,"physics vertices exceed limit",p+64);
            const auto groups=list(p+80+static_cast<std::size_t>(vertices)*8,*ob);
            for(std::size_t type=0;type<groups.size();++type) for(auto e:list(groups[type],*ob)) {
                const auto er=record(e,*ob,8); const auto subtype=er.i32(0);
                for(auto a:list(e+4,*ob)) { const auto ar=record(a,*ob,56); const auto code=ar.i32(32);code_valid(code);if(code>=0)o.events[{static_cast<int>(type),subtype}].push_back(code); }
            }
            objects.push_back(std::move(o));
        }
        for(auto p:list(ro->data_offset,*ro)) {
            const auto r=record(p,*ro,88);Room room;room.name=package.string_pointer(r.u32(0));room.width=r.i32(8);room.height=r.i32(12);room.speed=r.i32(16);room.color=r.u32(24);room.create=r.i32(32);code_valid(room.create);
            room.persistent=r.u32(20)!=0;room.views["view_enabled"]=std::int32_t{(r.u32(36)&1)!=0};room.views["view_current"]=std::int32_t{0};
            const auto view_list=list(r.u32(44),*ro);const std::string_view fields[]={"view_visible","view_xview","view_yview","view_wview","view_hview","view_xport","view_yport","view_wport","view_hport","view_hborder","view_vborder","view_hspeed","view_vspeed","view_object"};
            for(std::size_t v=0;v<view_list.size();++v){const auto vr=record(view_list[v],*ro,56);for(std::size_t f=0;f<14;++f)room.views[std::string(fields[f])+"["+std::to_string(v)+"]"]=vr.i32(f*4);}
            if(room.speed<1 || room.speed>1000 || room.width<1 || room.height<1) throw Error(ErrorKind::malformed,"invalid room dimensions/speed",p);
            for(auto q:list(r.u32(48),*ro)) {
                const auto ir=record(q,*ro,40);Placement placement;placement.x=ir.i32(0);placement.y=ir.i32(4);placement.object=ir.i32(8);placement.id=ir.i32(12);placement.create=ir.i32(16);placement.precreate=ir.i32(36);placement.sx=std::bit_cast<float>(ir.u32(20));placement.sy=std::bit_cast<float>(ir.u32(24));
                if(placement.object<0 || static_cast<std::size_t>(placement.object)>=objects.size())throw Error(ErrorKind::malformed,"invalid placed object",q+8);
                code_valid(placement.create);code_valid(placement.precreate);room.instances.push_back(placement);
            }
            for(auto q:list(r.u32(40),*ro)){const auto b=record(q,*ro,40);if(b.i32(0) && b.i32(8)>=0)room.backgrounds.push_back({b.i32(8),b.i32(12),b.i32(16),b.i32(4)!=0});}
            for(auto q:list(r.u32(52),*ro)){const auto t=record(q,*ro,48);room.tiles.push_back({t.i32(8),t.i32(0),t.i32(4),t.i32(12),t.i32(16),t.i32(20),t.i32(24),t.i32(28),std::bit_cast<float>(t.u32(36)),std::bit_cast<float>(t.u32(40)),t.u32(44)});}
            std::stable_sort(room.tiles.begin(),room.tiles.end(),[](const Tile& a,const Tile& b){return a.depth>b.depth;});
            rooms.push_back(std::move(room));
        }
        const auto gr=record(ge->data_offset,*ge,ge->size);const auto count=gr.u32(128);
        if(count==0 || count>rooms.size())throw Error(ErrorKind::malformed,"invalid room order",ge->data_offset+128);
        for(std::size_t i=0;i<count;++i){const auto id=gr.i32(132+i*4);if(id<0 || static_cast<std::size_t>(id)>=rooms.size())throw Error(ErrorKind::malformed,"invalid ordered room");order.push_back(id);}
    }
    std::shared_ptr<Instance> make(const Placement& p) {
        if(instances.size()>=10000)throw Error(ErrorKind::vm,"instance limit reached");
        const auto& obj=objects.at(static_cast<std::size_t>(p.object));auto inst=std::make_shared<Instance>();inst->id=p.id;inst->object=p.object;
        inst->vars={{"x",p.x},{"y",p.y},{"xprevious",p.x},{"yprevious",p.y},{"xstart",p.x},{"ystart",p.y},
            {"id",std::int32_t{p.id}},{"object_index",std::int32_t{p.object}},{"sprite_index",std::int32_t{obj.sprite}},
            {"mask_index",std::int32_t{obj.mask}},{"visible",std::int32_t{obj.visible}},{"persistent",std::int32_t{obj.persistent}},
            {"solid",std::int32_t{obj.solid}},{"depth",std::int32_t{obj.depth}},{"image_index",0.0},{"image_speed",1.0},
            {"image_xscale",p.sx},{"image_yscale",p.sy},{"image_angle",0.0},{"image_alpha",1.0},{"image_blend",std::int32_t{0xffffff}},
            {"hspeed",0.0},{"vspeed",0.0},{"speed",0.0},{"direction",0.0},{"friction",0.0},{"gravity",0.0},{"gravity_direction",270.0},
            {"path_index",std::int32_t{-1}},{"path_position",0.0},{"path_speed",0.0}};
        for(int a=0;a<12;++a)inst->vars["alarm["+std::to_string(a)+"]"]=std::int32_t{-1};
        instances.push_back(inst);return inst;
    }
    struct Bounds {double left,right,top,bottom;bool valid;};
    Bounds bounds(const Instance& i,double x,double y,bool full_sprite=false) const {
        int id=full_sprite?-1:number(i.vars.at("mask_index"));if(id<0)id=number(i.vars.at("sprite_index"));if(id<0)return {x,x,y,y,false};
        const auto& s=assets.sprite(id);const double sx=value_number(i.vars.at("image_xscale")),sy=value_number(i.vars.at("image_yscale"));
        const double a=value_number(i.vars.at("image_angle"))*3.141592653589793/180,c=std::cos(a),sn=std::sin(a);
        Bounds out{INFINITY,-INFINITY,INFINITY,-INFINITY,true};
        const int left=full_sprite?0:s.left,right=full_sprite?s.width:s.right+1,top=full_sprite?0:s.top,bottom=full_sprite?s.height:s.bottom+1;
        for(double px:{double(left-s.ox),double(right-s.ox)})for(double py:{double(top-s.oy),double(bottom-s.oy)}){
            const double tx=x+px*sx*c+py*sy*sn,ty=y-px*sx*sn+py*sy*c;
            out.left=std::min(out.left,tx);out.right=std::max(out.right,tx);out.top=std::min(out.top,ty);out.bottom=std::max(out.bottom,ty);
        }
        return out;
    }
    Bounds bounds(const Instance& i) const {return bounds(i,value_number(i.vars.at("x")),value_number(i.vars.at("y")));}
    bool matches(const Instance& i,int object) const {
        if(object==-3 || object==i.id)return true;
        int n=i.object;
        for(std::size_t guard=0;n>=0 && static_cast<std::size_t>(n)<objects.size() && guard<objects.size();++guard){if(n==object)return true;n=objects[static_cast<std::size_t>(n)].parent;}
        return false;
    }
    std::vector<std::shared_ptr<Instance>> targets(int target,const std::shared_ptr<Instance>& self,const std::shared_ptr<Instance>& other) {
        if(target==-1)return self?std::vector{self}:std::vector<std::shared_ptr<Instance>>{};
        if(target==-2)return other && other->alive?std::vector{other}:std::vector<std::shared_ptr<Instance>>{};
        std::vector<std::shared_ptr<Instance>> out;for(auto& i:instances)if(i->alive && matches(*i,target))out.push_back(i);return out;
    }
    void event(std::shared_ptr<Instance> inst,int type,int subtype,int object=-1,std::shared_ptr<Instance> other={}) {
        if(!inst->alive)return;int n=object<0?inst->object:object;
        for(std::size_t guard=0;n>=0 && static_cast<std::size_t>(n)<objects.size() && guard<objects.size();++guard) {
            const auto& obj=objects[static_cast<std::size_t>(n)];const auto it=obj.events.find({type,subtype});
            if(it!=obj.events.end()){event_stack.push_back({type,subtype,n});struct Guard{std::vector<EventFrame>& frames;~Guard(){frames.pop_back();}}guard{event_stack};for(auto code:it->second)execute(code,inst,{},other);return;}n=obj.parent;
        }
    }
    void enter(int id) {
        if(id<0 || static_cast<std::size_t>(id)>=rooms.size())throw Error(ErrorKind::vm,"room ID out of range");
        const auto previous=instances;for(auto& i:previous)if(i->alive)event(i,7,5);
        if(current_room>=0){
            if(number(engine.at("room_persistent"))!=0){SavedRoom saved;for(const auto& i:instances)if(i->alive && number(i->vars.at("persistent"))==0)saved.instances.push_back(i);
                for(const auto& [key,value]:engine)if(key.starts_with("view_") || key=="room_width" || key=="room_height" || key=="room_speed" || key=="room_persistent")saved.engine[key]=value;saved_rooms[current_room]=std::move(saved);
            }else saved_rooms.erase(current_room);
        }
        std::erase_if(instances,[](const auto& i){return !i->alive || number(i->vars.at("persistent"))==0;});
        current_room=id;pending_room=-1;const auto& room=rooms[static_cast<std::size_t>(id)];
        engine["room"]=std::int32_t{id};engine["room_width"]=std::int32_t{room.width};engine["room_height"]=std::int32_t{room.height};engine["room_speed"]=std::int32_t{room.speed};
        engine["room_persistent"]=std::int32_t{room.persistent};
        for(const auto& [key,value]:room.views)engine[key]=value;
        event_budget=0;std::vector<std::shared_ptr<Instance>> placed;
        const auto saved=saved_rooms.find(id);
        if(saved!=saved_rooms.end()){
            instances.insert(instances.end(),saved->second.instances.begin(),saved->second.instances.end());for(const auto& [key,value]:saved->second.engine)engine[key]=value;saved_rooms.erase(saved);
        }else{
            for(const auto& p:room.instances)placed.push_back(make(p));
            for(std::size_t i=0;i<placed.size();++i){const auto& p=room.instances[i];if(p.precreate>=0)execute(p.precreate,placed[i],{});event(placed[i],0,0);if(p.create>=0)execute(p.create,placed[i],{});}
            if(room.create>=0 && !placed.empty())execute(room.create,placed[0],{});
        }
        const auto snapshot=instances;
        if(!game_started){game_started=true;for(auto& i:snapshot)event(i,7,2);}
        for(auto& i:snapshot)event(i,7,4);
        std::cout<<"Entered room "<<id<<": "<<room.name<<" ("<<instances.size()<<" instances)"<<std::endl;
    }
    Value execute(int code,std::shared_ptr<Instance> self,std::span<const Value> arguments,std::shared_ptr<Instance> other={}) {
        if(code<0)return std::monostate{};
        if(depth>=128)throw Error(ErrorKind::vm,"script call depth exceeded");
        ++depth;struct Guard {std::size_t& d;~Guard(){--d;}}guard{depth};
        const auto& program=linked.programs.at(static_cast<std::size_t>(code));VariableStore locals;
        locals["argument_count"]=static_cast<std::int32_t>(arguments.size());
        for(std::size_t i=0;i<arguments.size();++i){locals["argument"+std::to_string(i)]=arguments[i];locals["argument["+std::to_string(i)+"]"]=arguments[i];}
        std::vector<Value> stack;std::size_t pc=0;
        struct Environment {std::shared_ptr<Instance> saved,saved_other;std::vector<std::shared_ptr<Instance>> choices;std::size_t index=0;};std::vector<Environment> environments;
        auto pop=[&](){if(stack.empty())throw Error(ErrorKind::vm,"native VM stack underflow",program.origin+pc);auto v=std::move(stack.back());stack.pop_back();return v;};
        auto push=[&](Value v){if(stack.size()>=4096)throw Error(ErrorKind::vm,"native VM stack limit",program.origin+pc);stack.push_back(std::move(v));};
        try {
        while(pc<program.byte_length) {
            if(++event_budget>2'000'000)throw Error(ErrorKind::vm,"event instruction budget exhausted",program.origin+pc);++instruction_count;
            const auto& ins=program.instructions.at(program.index_by_offset.at(pc));std::size_t next=pc+ins.size;
            auto variable=[&](bool writing,Value value=std::monostate{})->Value {
                const auto address=program.origin+pc;const auto id=linked.variable_at.at(address);const auto& symbol=linked.variables[id];std::string name=symbol.name;
                const auto kind=(*ins.reference>>24)&0xf8;int scope=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));int array_index=-1;
                if(kind==0){array_index=number(pop());if(array_index<0 || array_index>=32000*32000)throw Error(ErrorKind::vm,"array index out of range");scope=number(pop());name+="["+std::to_string(array_index)+"]";}
                else if(kind==0x80)scope=number(pop());
                else if(kind!=0xa0)throw Error(ErrorKind::unsupported,"unsupported variable reference kind "+std::to_string(kind));
                if(scope==-9)scope=number(pop());
                if(!writing && name.starts_with("bbox_")){
                    const auto found=targets(scope,self,other);if(found.empty())throw Error(ErrorKind::vm,"bounding box instance missing");const auto b=bounds(*found[0]);
                    if(name=="bbox_left")return std::floor(b.left);if(name=="bbox_right")return std::ceil(b.right)-1;if(name=="bbox_top")return std::floor(b.top);if(name=="bbox_bottom")return std::ceil(b.bottom)-1;
                }
                if(!writing && (name=="sprite_width" || name=="sprite_height" || name=="sprite_xoffset" || name=="sprite_yoffset" || name=="image_number")){
                    const auto found=targets(scope,self,other);if(found.empty())throw Error(ErrorKind::vm,"sprite variable instance missing");const int id=number(found[0]->vars.at("sprite_index"));if(id<0)return 0.0;const auto& spr=assets.sprite(id);
                    if(name=="sprite_width")return static_cast<double>(spr.width)*std::abs(value_number(found[0]->vars.at("image_xscale")));if(name=="sprite_height")return static_cast<double>(spr.height)*std::abs(value_number(found[0]->vars.at("image_yscale")));if(name=="sprite_xoffset")return static_cast<double>(spr.ox);if(name=="sprite_yoffset")return static_cast<double>(spr.oy);return static_cast<double>(spr.frames.size());
                }
                VariableStore* store=nullptr;
                std::vector<VariableStore*> stores;
                if(scope==-5)store=&globals;
                else if(scope==-7 || name.starts_with("argument"))store=&locals;
                else if(engine.contains(name) || (kind==0xa0 && ins.opcode==Opcode::push_builtin && !self->vars.contains(name)))store=&engine;
                else {const auto found=targets(scope,self,other);if(found.empty())throw Error(ErrorKind::vm,"variable instance not found: "+name);store=&found[0]->vars;for(const auto& target:found)stores.push_back(&target->vars);}
                auto access=[&](VariableStore* store)->Value {
                if(array_index>=0 && !store->contains(name)){
                    auto base=store->find(symbol.name);
                    if(base==store->end()){if(!writing)throw Error(ErrorKind::vm,"array variable is uninitialized: "+symbol.name);base=store->emplace(symbol.name,std::make_shared<ArrayValue>()).first;}
                    auto* array=std::get_if<std::shared_ptr<ArrayValue>>(&base->second);if(!array || !*array)throw Error(ErrorKind::vm,"indexed variable is not an array: "+symbol.name);
                    const int row=array_index/32000,col=array_index%32000;
                    if(writing){if(array->use_count()>1)*array=std::make_shared<ArrayValue>(**array);if((*array)->elements.size()>=1'000'000)throw Error(ErrorKind::vm,"array element limit reached");(*array)->elements[array_index]=value;auto& length=(*array)->row_lengths[row];length=std::max(length,col+1);return value;}
                    const auto length=(*array)->row_lengths.find(row);if(length==(*array)->row_lengths.end() || col>=length->second)throw Error(ErrorKind::vm,"array read exceeds allocated row: "+symbol.name);
                    const auto element=(*array)->elements.find(array_index);return element==(*array)->elements.end()?Value{std::int32_t{0}}:element->second;
                }
                if(writing){(*store)[name]=value;if(store==&engine && name=="room")pending_room=number(value);
                    if(store!=&engine && store!=&globals && store!=&locals){
                        if(name=="speed" || name=="direction"){const double rad=value_number(store->at("direction"))*3.141592653589793/180,speed=value_number(store->at("speed"));(*store)["hspeed"]=speed*std::cos(rad);(*store)["vspeed"]=-speed*std::sin(rad);}
                        if(name=="hspeed" || name=="vspeed"){const double h=value_number(store->at("hspeed")),v=value_number(store->at("vspeed"));(*store)["speed"]=std::hypot(h,v);if(h!=0 || v!=0)(*store)["direction"]=std::fmod(std::atan2(-v,h)*180/3.141592653589793+360,360);}
                    }return value;}
                if(name=="instance_count")return static_cast<std::int32_t>(instances.size());
                const auto it=store->find(name);if(it==store->end())throw Error(ErrorKind::vm,"uninitialized variable: "+name);return it->second;
                };
                if(writing && !stores.empty()){for(auto* target:stores)access(target);return value;}
                return access(store);
            };
            switch(ins.opcode) {
            case Opcode::push_i:case Opcode::push:case Opcode::push_local:case Opcode::push_global:case Opcode::push_builtin:
                if(ins.type1==DataType::variable)push(variable(false));
                else if(ins.type1==DataType::string)push(std::string(package.string_at(*ins.reference)));
                else if(auto v=std::get_if<std::int32_t>(&ins.immediate))push(*v);
                else if(auto v=std::get_if<double>(&ins.immediate))push(*v);
                else if(auto v=std::get_if<std::int64_t>(&ins.immediate))push(static_cast<double>(*v));
                else throw Error(ErrorKind::unsupported,"unsupported native push");break;
            case Opcode::pop: {
                if(ins.type1==DataType::int16)throw Error(ErrorKind::unsupported,"native stack swap pending");
                // For store.v.* the value is above the scope/index operands; store.*.v has it below.
                if(ins.type1!=DataType::variable){auto v=pop();variable(true,v);}
                else {const auto kind=(*ins.reference>>24)&0xf8;std::vector<Value> address;
                    if(kind==0){auto index=pop();auto scope=pop();address={scope,index};}else if(kind==0x80)address={pop()};
                    auto v=pop();for(auto& a:address)push(a);variable(true,v);}
                break;
            }
            case Opcode::conv:{auto v=pop();if(ins.type2==DataType::int32)push(static_cast<std::int32_t>(number(v)));else if(ins.type2==DataType::real)push(value_number(v));else if(ins.type2==DataType::boolean)push(value_number(v)>0.5);else push(v);break;}
            case Opcode::dup:{const auto count=static_cast<std::size_t>(ins.extra)+1;if(count>stack.size())throw Error(ErrorKind::vm,"DUP stack underflow");const auto first=stack.size()-count;for(std::size_t i=0;i<count;++i)push(stack[first+i]);break;}
            case Opcode::popz:pop();break;
            case Opcode::add:case Opcode::sub:case Opcode::mul:case Opcode::div:case Opcode::rem:case Opcode::mod:case Opcode::cmp:case Opcode::bit_and:case Opcode::bit_or:case Opcode::bit_xor:case Opcode::shl:case Opcode::shr:{
                const auto right=pop(),left=pop();
                if(std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
                    const auto& a=std::get<std::string>(left);const auto& b=std::get<std::string>(right);
                    if(ins.opcode==Opcode::add){if(a.size()+b.size()>1'048'576)throw Error(ErrorKind::vm,"string limit");push(a+b);}
                    else if(ins.opcode==Opcode::cmp){const int c=a.compare(b);push(ins.comparison==1?c<0:ins.comparison==2?c<=0:ins.comparison==3?c==0:ins.comparison==4?c!=0:ins.comparison==5?c>=0:c>0);}else throw Error(ErrorKind::unsupported,"string arithmetic");break;
                }
                const double a=value_number(left),b=value_number(right);
                switch(ins.opcode){case Opcode::add:push(a+b);break;case Opcode::sub:push(a-b);break;case Opcode::mul:push(a*b);break;
                case Opcode::div:if(b==0)throw Error(ErrorKind::vm,"division by zero");push(a/b);break;
                case Opcode::rem:if(b==0)throw Error(ErrorKind::vm,"integer division by zero");push(std::trunc(a/b));break;
                case Opcode::mod:if(b==0)throw Error(ErrorKind::vm,"modulo by zero");push(std::fmod(a,b));break;
                case Opcode::cmp:push(ins.comparison==1?a<b:ins.comparison==2?a<=b:ins.comparison==3?a==b:ins.comparison==4?a!=b:ins.comparison==5?a>=b:a>b);break;
                case Opcode::bit_and:push(static_cast<std::int32_t>(number(left)&number(right)));break;case Opcode::bit_or:push(static_cast<std::int32_t>(number(left)|number(right)));break;case Opcode::bit_xor:push(static_cast<std::int32_t>(number(left)^number(right)));break;
                case Opcode::shl:push(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(number(left))<<(static_cast<unsigned>(number(right))&31)));break;case Opcode::shr:push(static_cast<std::int32_t>(number(left)>>(static_cast<unsigned>(number(right))&31)));break;default:break;}break;
            }
            case Opcode::neg:push(-value_number(pop()));break;
            case Opcode::bit_not:if(ins.type1==DataType::boolean)push(!(value_number(pop())>0.5));else push(static_cast<std::int32_t>(~number(pop())));break;
            case Opcode::call:{const auto& name=linked.functions.at(linked.function_at.at(program.origin+pc)).name;const auto count=static_cast<std::size_t>(ins.word&0xffff);std::vector<Value> args;for(std::size_t i=0;i<count;++i)args.push_back(pop());push(call(name,args,self,other));break;}
            case Opcode::ret:return pop();case Opcode::exit:return std::monostate{};
            case Opcode::branch:next=*ins.branch_target;break;
            case Opcode::branch_true:case Opcode::branch_false:{const bool condition=value_number(pop())>0.5;if(condition==(ins.opcode==Opcode::branch_true))next=*ins.branch_target;break;}
            case Opcode::push_env:{const auto target=number(pop());auto choices=targets(target,self,other);if(choices.empty()){next=*ins.branch_target;const auto found=program.index_by_offset.find(next);if(found!=program.index_by_offset.end()){const auto& end=program.instructions.at(found->second);if(end.opcode==Opcode::pop_env)next+=end.size;}}else {environments.push_back({self,other,choices,0});other=self;self=choices[0];}break;}
            case Opcode::pop_env:{if(environments.empty())throw Error(ErrorKind::vm,"environment underflow");auto& e=environments.back();if(!ins.environment_exit && ++e.index<e.choices.size()){self=e.choices[e.index];next=*ins.branch_target;}else{self=e.saved;other=e.saved_other;environments.pop_back();}break;}
            case Opcode::extended:{const auto sub=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(ins.word));if(sub!=-1)throw Error(ErrorKind::unsupported,"native extended opcode pending: "+std::to_string(sub));if(stack.empty() || number(stack.back())<0 || number(stack.back())>=32000)throw Error(ErrorKind::vm,"legacy two-dimensional array index out of range");break;}
            default:throw Error(ErrorKind::unsupported,"native opcode pending: "+std::string(opcode_name(ins.opcode)));
            }
            pc=next;
        }
        }catch(const Error& e){throw Error(e.kind(),std::string(e.what())+" in "+std::string(package.code_name(static_cast<std::size_t>(code))),e.offset().value_or(program.origin+pc));}
        return std::monostate{};
    }
    Value call(std::string_view name,std::span<const Value> args,std::shared_ptr<Instance> self,std::shared_ptr<Instance> other) {
        auto n=[&](std::size_t i){if(i>=args.size())throw Error(ErrorKind::vm,"missing built-in argument");return value_number(args[i]);};
        auto text=[&](std::size_t i){if(i>=args.size())throw Error(ErrorKind::vm,"missing built-in argument");return value_text(args[i]);};
        auto set_velocity=[&](std::string_view component,double value){self->vars[std::string(component)]=value;const double h=value_number(self->vars.at("hspeed")),v=value_number(self->vars.at("vspeed"));self->vars["speed"]=std::hypot(h,v);if(h!=0 || v!=0)self->vars["direction"]=std::fmod(std::atan2(-v,h)*180/3.141592653589793+360,360);};
        if(const auto it=script_names.find(std::string(name));it!=script_names.end())return execute(it->second,self,args,other);
        if(name=="script_execute"){const auto id=static_cast<std::size_t>(n(0));if(id>=scripts.size())throw Error(ErrorKind::vm,"script ID out of range");return execute(scripts[id],self,args.subspan(1),other);}
        if(name=="os_get_language")return std::string("en");if(name=="os_get_region")return std::string("US");
        if(name=="string")return text(0);
        if(name=="real"){if(std::holds_alternative<std::string>(args[0])){try{return std::stod(text(0));}catch(const std::exception&){throw Error(ErrorKind::vm,"real conversion requires numeric string");}}return n(0);}
        if(name=="is_undefined"){if(args.empty())throw Error(ErrorKind::vm,"is_undefined argument missing");return std::holds_alternative<std::monostate>(args[0]);}
        if(name=="is_string")return std::holds_alternative<std::string>(args[0]);
        if(name=="is_array")return std::holds_alternative<std::shared_ptr<ArrayValue>>(args[0]);
        if(name=="is_real")return !std::holds_alternative<std::string>(args[0]) && !std::holds_alternative<std::monostate>(args[0]);
        if(name=="string_upper" || name=="string_lower"){auto str=text(0);for(auto& c:str)c=static_cast<char>(name=="string_upper"?std::toupper(static_cast<unsigned char>(c)):std::tolower(static_cast<unsigned char>(c)));return str;}
        if(name=="string_copy"){const auto str=text(0);const auto start=std::clamp(static_cast<int>(n(1))-1,0,static_cast<int>(str.size()));return str.substr(static_cast<std::size_t>(start),static_cast<std::size_t>(std::max(0.0,n(2))));}
        if(name=="string_length" || name=="string_byte_length" || name=="strlen")return static_cast<double>(text(0).size());
        if(name=="string_replace_all" || name=="string_replace"){auto str=text(0);const auto from=text(1),to=text(2);if(from.empty())return str;std::size_t at=0;while((at=str.find(from,at))!=std::string::npos){str.replace(at,from.size(),to);at+=to.size();if(str.size()>1'048'576)throw Error(ErrorKind::vm,"string replacement exceeds limit");if(name=="string_replace")break;}return str;}
        if(name=="string_pos"){const auto pos=text(1).find(text(0));return pos==std::string::npos?0.0:static_cast<double>(pos+1);}
        if(name=="string_delete"){auto str=text(0);const int at=std::max(0,static_cast<int>(n(1))-1);if(static_cast<std::size_t>(at)<str.size())str.erase(static_cast<std::size_t>(at),static_cast<std::size_t>(std::max(0.0,n(2))));return str;}
        if(name=="string_insert"){auto str=text(1);const auto at=static_cast<std::size_t>(std::clamp(n(2)-1,0.0,static_cast<double>(str.size())));str.insert(at,text(0));return str;}
        if(name=="string_char_at"){auto str=text(0);const int at=static_cast<int>(n(1))-1;return at>=0 && static_cast<std::size_t>(at)<str.size()?str.substr(static_cast<std::size_t>(at),1):std::string{};}
        if(name=="ord"){const auto str=text(0);return static_cast<std::int32_t>(str.empty()?0:static_cast<unsigned char>(str[0]));}
        if(name=="chr")return std::string(1,static_cast<char>(static_cast<int>(n(0))));
        if(name=="variable_global_exists")return globals.contains(text(0));
        if(name=="variable_instance_exists"){const auto found=targets(static_cast<int>(n(0)),self,other);return !found.empty() && found[0]->vars.contains(text(1));}
        if(name=="variable_global_get")return globals.at(text(0));if(name=="variable_global_set"){globals[text(0)]=args[1];return std::monostate{};}
        if(name=="ds_map_create"){const int id=next_ds++;maps[id]={};return std::int32_t{id};}
        if(name=="ds_map_set" || name=="ds_map_replace" || name=="ds_map_add"){auto& map=maps.at(static_cast<int>(n(0)));const auto key=(std::holds_alternative<std::string>(args[1])?"s:":"n:")+text(1);if(name=="ds_map_add" && map.contains(key))return false;map[key]=args[2];return true;}
        if(name=="ds_map_exists" || name=="ds_map_find_value"){const auto& map=maps.at(static_cast<int>(n(0)));const auto key=(std::holds_alternative<std::string>(args[1])?"s:":"n:")+text(1);if(name=="ds_map_exists")return map.contains(key);const auto it=map.find(key);return it==map.end()?Value{std::monostate{}}:it->second;}
        if(name=="ds_map_destroy"){maps.erase(static_cast<int>(n(0)));return std::monostate{};}
        if(name=="ds_list_create"){const int id=next_ds++;lists[id]={};return std::int32_t{id};}
        if(name=="ds_list_add"){auto& list=lists.at(static_cast<int>(n(0)));list.insert(list.end(),args.begin()+1,args.end());return std::monostate{};}
        if(name=="ds_list_size")return static_cast<double>(lists.at(static_cast<int>(n(0))).size());
        if(name=="ds_list_find_value")return lists.at(static_cast<int>(n(0))).at(static_cast<std::size_t>(n(1)));
        if(name=="ds_list_destroy"){lists.erase(static_cast<int>(n(0)));return std::monostate{};}
        auto save_path=[&](std::string str){const std::filesystem::path p(str);if(p.is_absolute() || p.has_parent_path())throw Error(ErrorKind::unsupported,"save path must be a filename");return save_dir/p;};
        if(name=="file_exists")return std::filesystem::is_regular_file(save_path(text(0))) || std::filesystem::is_regular_file(game_dir/text(0));
        if(name=="file_delete"){std::filesystem::remove(save_path(text(0)));return std::monostate{};}
        if(name=="file_text_open_write" || name=="file_text_open_read" || name=="file_text_open_append"){
            const auto path=save_path(text(0));std::filesystem::create_directories(save_dir);const auto mode=name=="file_text_open_read"?std::ios::in:name=="file_text_open_append"?std::ios::out|std::ios::app:std::ios::out|std::ios::trunc;
            auto file=std::make_unique<std::fstream>(path,mode);if(!*file)return std::int32_t{-1};const auto id=next_file++;files.emplace(id,std::move(file));return std::int32_t{id};
        }
        if(name=="file_text_close"){files.erase(static_cast<int>(n(0)));return std::monostate{};}
        if(name=="file_text_write_string" || name=="file_text_write_real"){auto& file=*files.at(static_cast<int>(n(0)));file<<text(1);if(!file)throw Error(ErrorKind::io,"guest text write failed");return std::monostate{};}
        if(name=="file_text_read_real"){auto& file=*files.at(static_cast<int>(n(0)));double value=0;file>>value;if(file.fail() && !file.eof())throw Error(ErrorKind::io,"guest numeric text read failed");return value;}
        if(name=="file_text_read_string"){auto& file=*files.at(static_cast<int>(n(0)));std::string value;while(file.peek()!=std::char_traits<char>::eof() && file.peek()!='\r' && file.peek()!='\n')value.push_back(static_cast<char>(file.get()));return value;}
        if(name=="file_text_readln"){std::string line;std::getline(*files.at(static_cast<int>(n(0))),line);return std::monostate{};}
        if(name=="file_text_eof")return files.at(static_cast<int>(n(0)))->peek()==std::char_traits<char>::eof();
        if(name=="file_text_eoln"){const auto ch=files.at(static_cast<int>(n(0)))->peek();return ch=='\r' || ch=='\n' || ch==std::char_traits<char>::eof();}
        if(name=="file_text_writeln"){*files.at(static_cast<int>(n(0)))<<'\n';return std::monostate{};}
        if(name=="ini_open"){
            if(!ini_path.empty())call("ini_close",{},self,other);ini_path=save_path(text(0));ini.clear();ini_dirty=false;
            std::ifstream file(ini_path);std::string line,section;while(std::getline(file,line)){if(!line.empty() && line.back()=='\r')line.pop_back();if(line.size()>1 && line.front()=='[' && line.back()==']')section=line.substr(1,line.size()-2);else if(const auto eq=line.find('=');eq!=std::string::npos)ini[section][line.substr(0,eq)]=line.substr(eq+1);}return std::monostate{};
        }
        if(name=="ini_read_real" || name=="ini_read_string"){
            if(ini_path.empty())throw Error(ErrorKind::vm,"INI read without open");const auto section=ini.find(text(0));if(section==ini.end())return args[2];const auto entry=section->second.find(text(1));if(entry==section->second.end())return args[2];
            if(name=="ini_read_string")return entry->second;try{return std::stod(entry->second);}catch(const std::exception&){return args[2];}
        }
        if(name=="ini_write_real" || name=="ini_write_string"){if(ini_path.empty())throw Error(ErrorKind::vm,"INI write without open");ini[text(0)][text(1)]=text(2);ini_dirty=true;return std::monostate{};}
        if(name=="ini_section_exists")return ini.contains(text(0));
        if(name=="ini_key_exists"){const auto section=ini.find(text(0));return section!=ini.end() && section->second.contains(text(1));}
        if(name=="ini_section_delete"){ini.erase(text(0));ini_dirty=true;return std::monostate{};}
        if(name=="ini_key_delete"){if(auto section=ini.find(text(0));section!=ini.end())section->second.erase(text(1));ini_dirty=true;return std::monostate{};}
        if(name=="ini_close"){
            if(ini_dirty){std::filesystem::create_directories(save_dir);std::ofstream file(ini_path);if(!file)throw Error(ErrorKind::io,"guest INI open failed");for(const auto& [section,entries]:ini){file<<'['<<section<<"]\n";for(const auto& [key,value]:entries)file<<key<<'='<<value<<'\n';}if(!file)throw Error(ErrorKind::io,"guest INI write failed");}
            ini_path.clear();ini.clear();ini_dirty=false;return std::monostate{};
        }
        if(name=="steam_initialised" || name=="steam_is_overlay_enabled" || name=="steam_file_exists")return false;
        if(name=="randomize"){if(!fixed_seed)random.seed(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));return std::monostate{};}
        if(name=="audio_channel_num"){audio.channel_limit(static_cast<int>(n(0)));return std::monostate{};}
        if(name=="audio_create_stream")return std::int32_t{audio.create_stream(text(0))};
        if(name=="audio_play_sound")return std::int32_t{audio.play(static_cast<int>(n(0)),n(2)>0.5,n(1))};
        if(name=="audio_stop_sound"){audio.stop(static_cast<int>(n(0)));return std::monostate{};}
        if(name=="audio_stop_all"){audio.stop(-1);return std::monostate{};}
        if(name=="audio_pause_sound" || name=="audio_resume_sound"){audio.pause(static_cast<int>(n(0)),name=="audio_pause_sound");return std::monostate{};}
        if(name=="audio_is_playing")return audio.playing(static_cast<int>(n(0)));
        if(name=="audio_sound_get_gain")return audio.gain(static_cast<int>(n(0)));
        if(name=="audio_sound_gain"){audio.gain(static_cast<int>(n(0)),n(1));return std::monostate{};}
        if(name=="audio_sound_pitch"){audio.pitch(static_cast<int>(n(0)),n(1));return std::monostate{};}
        if(name=="audio_sound_length")return audio.length(static_cast<int>(n(0)));
        if(name=="audio_sound_get_track_position")return audio.position(static_cast<int>(n(0)));
        if(name=="audio_sound_set_track_position"){audio.position(static_cast<int>(n(0)),n(1));return std::monostate{};}
        if(name=="sprite_prefetch"){assets.prefetch(static_cast<int>(n(0)));return std::int32_t{0};}
        if(name=="sprite_get_number")return static_cast<double>(assets.sprite(static_cast<int>(n(0))).frames.size());
        if(name=="sprite_get_width")return std::int32_t{assets.sprite(static_cast<int>(n(0))).width};
        if(name=="sprite_get_height")return std::int32_t{assets.sprite(static_cast<int>(n(0))).height};
        if(name=="draw_set_color"){draw_color=static_cast<unsigned>(n(0));canvas.color(draw_color);return std::monostate{};}
        if(name=="draw_get_color")return static_cast<double>(draw_color);
        if(name=="draw_set_alpha"){draw_alpha=n(0);canvas.alpha(draw_alpha);return std::monostate{};}
        if(name=="draw_get_alpha")return draw_alpha;
        if(name=="draw_set_font"){draw_font=static_cast<int>(n(0));return std::monostate{};}
        if(name=="draw_set_halign"){halign=static_cast<int>(n(0));return std::monostate{};}
        if(name=="draw_set_valign"){valign=static_cast<int>(n(0));return std::monostate{};}
        if(name=="draw_text"){assets.draw_text(canvas,draw_font,n(0),n(1),text(2),draw_color,draw_alpha,halign,valign);return std::monostate{};}
        if(name=="draw_text_transformed"){assets.draw_text(canvas,draw_font,n(0),n(1),text(2),draw_color,draw_alpha,halign,valign,n(3),n(4),n(5));return std::monostate{};}
        if(name=="surface_get_width" || name=="surface_get_height"){if(n(0)!=0)throw Error(ErrorKind::vm,"surface ID unavailable");return std::int32_t{name=="surface_get_width"?canvas.width():canvas.height()};}
        if(name=="surface_exists")return n(0)==0;
        if(name=="string_width")return assets.text_width(draw_font,text(0));if(name=="string_height")return assets.text_height(draw_font);
        if(name=="draw_rectangle"){assets.rectangle(canvas,n(0),n(1),n(2),n(3),n(4)!=0);return std::monostate{};}
        if(name=="ossafe_fill_rectangle"){assets.rectangle(canvas,n(0),n(1),n(2),n(3),false);return std::monostate{};}
        if(name=="draw_circle"){canvas.circle(n(0),n(1),n(2),n(3)!=0);return std::monostate{};}
        if(name=="draw_clear"){canvas.clear(static_cast<unsigned>(n(0)));return std::monostate{};}
        if(name=="draw_background"){assets.draw_background(canvas,static_cast<int>(n(0)),n(1),n(2));return std::monostate{};}
        if(name=="draw_sprite"){const auto f=n(1)<0?value_number(self->vars.at("image_index")):n(1);assets.draw_sprite(canvas,static_cast<int>(n(0)),f,n(2),n(3),1,1,0,0xffffff,draw_alpha);return std::monostate{};}
        if(name=="draw_sprite_ext"){assets.draw_sprite(canvas,static_cast<int>(n(0)),n(1)<0?value_number(self->vars.at("image_index")):n(1),n(2),n(3),n(4),n(5),n(6),static_cast<unsigned>(n(7)),n(8));return std::monostate{};}
        if(name=="draw_sprite_part_ext"){assets.draw_sprite_part(canvas,static_cast<int>(n(0)),n(1)<0?value_number(self->vars.at("image_index")):n(1),n(2),n(3),n(4),n(5),n(6),n(7),n(8),n(9),static_cast<unsigned>(n(10)),n(11));return std::monostate{};}
        if(name=="collision_line"){
            const double x=n(0),y=n(1),dx=n(2)-x,dy=n(3)-y;const bool exclude=n(6)!=0;
            for(const auto& i:targets(static_cast<int>(n(4)),self,other)){if(exclude && i==self)continue;const auto b=bounds(*i);if(!b.valid)continue;double lo=0,hi=1;
                auto clip=[&](double origin,double delta,double low,double high){if(delta==0)return origin>=low && origin<high;double a=(low-origin)/delta,z=(high-origin)/delta;if(a>z)std::swap(a,z);lo=std::max(lo,a);hi=std::min(hi,z);return lo<=hi;};
                if(clip(x,dx,b.left,b.right) && clip(y,dy,b.top,b.bottom))return std::int32_t{i->id};
            }return std::int32_t{-4};
        }
        if(name=="collision_point" || name=="collision_rectangle"){
            const bool point=name=="collision_point";const double x1=n(0),y1=n(1),x2=point?x1:n(2),y2=point?y1:n(3);
            const auto object=static_cast<int>(n(point?2:4));const bool exclude=n(point?4:6)!=0;
            for(const auto& i:targets(object,self,other)){if(exclude && i==self)continue;const auto b=bounds(*i);if(!b.valid)continue;
                if(point?(x1>=b.left && x1<b.right && y1>=b.top && y1<b.bottom):(std::max(x1,x2)>=b.left && std::min(x1,x2)<b.right && std::max(y1,y2)>=b.top && std::min(y1,y2)<b.bottom))return std::int32_t{i->id};
            }return std::int32_t{-4};
        }
        if(name=="place_meeting" || name=="instance_place"){
            const auto b=bounds(*self,n(0),n(1));for(const auto& i:targets(static_cast<int>(n(2)),self,other))if(i!=self){const auto o=bounds(*i);if(b.valid && o.valid && b.left<o.right && b.right>o.left && b.top<o.bottom && b.bottom>o.top)return name=="place_meeting"?Value{true}:Value{std::int32_t{i->id}};}
            return name=="place_meeting"?Value{false}:Value{std::int32_t{-4}};
        }
        if(name=="keyboard_clear"){const auto key=static_cast<int>(n(0));if(key>=0 && key<256){const auto k=static_cast<std::size_t>(key);held[k]=pressed[k]=released[k]=false;}return 0.0;}
        if(name=="action_move"){
            const auto directions=text(0);if(directions.size()!=9)throw Error(ErrorKind::vm,"action_move requires nine directions");std::vector<int> choices;for(int k=0;k<9;++k)if(directions[static_cast<std::size_t>(k)]=='1')choices.push_back(k);if(choices.empty())return 0.0;
            const int chosen=choices[std::uniform_int_distribution<std::size_t>(0,choices.size()-1)(random)];const double angles[]={225,270,315,180,0,0,135,90,45};const double speed=chosen==4?0:n(1),angle=angles[chosen],rad=angle*3.141592653589793/180;self->vars["speed"]=speed;self->vars["direction"]=angle;self->vars["hspeed"]=speed*std::cos(rad);self->vars["vspeed"]=-speed*std::sin(rad);return 0.0;
        }
        if(name=="action_set_alarm"){const int alarm=static_cast<int>(n(1));if(alarm<0 || alarm>=12)throw Error(ErrorKind::vm,"alarm index out of range");self->vars["alarm["+std::to_string(alarm)+"]"]=std::int32_t{static_cast<int>(n(0))};return 0.0;}
        if(name=="action_set_friction"){double value=n(0);if(auto it=globals.find("__argument_relative");it!=globals.end() && value_number(it->second)!=0)value+=value_number(self->vars.at("friction"));self->vars["friction"]=value;return 0.0;}
        if(name=="action_set_gravity"){double direction=n(0),gravity=n(1);if(auto it=globals.find("__argument_relative");it!=globals.end() && value_number(it->second)!=0){direction+=value_number(self->vars.at("gravity_direction"));gravity+=value_number(self->vars.at("gravity"));}self->vars["gravity_direction"]=direction;self->vars["gravity"]=gravity;return std::monostate{};}
        if(name=="action_set_hspeed" || name=="action_set_vspeed"){const auto component=name=="action_set_hspeed"?"hspeed":"vspeed";double value=n(0);if(auto it=globals.find("__argument_relative");it!=globals.end() && value_number(it->second)!=0)value+=value_number(self->vars.at(component));set_velocity(component,value);return std::monostate{};}
        if(name=="action_set_relative"){globals["__argument_relative"]=n(0)!=0;return std::monostate{};}
        if(name=="path_end"){self->vars["path_index"]=std::int32_t{-1};return std::monostate{};}
        if(name=="path_start"){
            const int id=number(args[0]);if(id<0 || static_cast<std::size_t>(id)>=paths.size())throw Error(ErrorKind::vm,"invalid path ID");const auto& path=paths[static_cast<std::size_t>(id)];if(path.points.empty())return std::monostate{};
            const auto& first=path.points.front();self->vars["path_index"]=std::int32_t{id};self->vars["path_speed"]=n(1);self->vars["path_position"]=n(1)<0?1.0:0.0;self->vars["path_endaction"]=args[2];self->vars["path_scale"]=1.0;self->vars["path_orientation"]=0.0;self->vars["path_xstart"]=n(3)!=0?0.0:value_number(self->vars.at("x"))-first.x;self->vars["path_ystart"]=n(3)!=0?0.0:value_number(self->vars.at("y"))-first.y;return std::monostate{};
        }
        if(name=="move_towards_point"){const double dx=n(0)-value_number(self->vars.at("x")),dy=n(1)-value_number(self->vars.at("y")),length=std::hypot(dx,dy),speed=n(2);self->vars["speed"]=speed;self->vars["direction"]=std::fmod(std::atan2(-dy,dx)*180/3.141592653589793+360,360);self->vars["hspeed"]=length!=0?dx*speed/length:0;self->vars["vspeed"]=length!=0?dy*speed/length:0;return 0.0;}
        if(name=="distance_to_point"){const auto b=bounds(*self);return std::hypot(std::max({b.left-n(0),0.0,n(0)-b.right}),std::max({b.top-n(1),0.0,n(1)-b.bottom}));}
        if(name=="point_distance")return std::hypot(n(2)-n(0),n(3)-n(1));
        if(name=="point_direction")return std::fmod(std::atan2(n(1)-n(3),n(2)-n(0))*180/3.141592653589793+360,360);
        if(name=="lengthdir_x")return n(0)*std::cos(n(1)*3.141592653589793/180);
        if(name=="lengthdir_y")return -n(0)*std::sin(n(1)*3.141592653589793/180);
        if(name=="instance_exists")return !targets(static_cast<int>(n(0)),self,other).empty();
        if(name=="event_inherited"){if(event_stack.empty())throw Error(ErrorKind::vm,"event_inherited outside an event");const auto f=event_stack.back();const auto parent=objects.at(static_cast<std::size_t>(f.object)).parent;if(parent>=0)event(self,f.type,f.subtype,parent,other);return std::monostate{};}
        if(name=="event_perform"){
            const int type=static_cast<int>(n(0)),subtype=static_cast<int>(n(1));
            if(type<0 || type>12)throw Error(ErrorKind::vm,"event_perform event type out of range");
            // This runs the requested event code on the current instance. It does not
            // synthesize input, advance alarms, or otherwise trigger engine behavior.
            event(self,type,subtype);
            return std::monostate{};
        }
        if(name=="event_user"){event(self,7,10+static_cast<int>(n(0)),-1,other);return std::monostate{};}
        if(name=="action_move_to"){self->vars["x"]=n(0);self->vars["y"]=n(1);return std::monostate{};}
        if(name=="instance_number")return static_cast<double>(targets(static_cast<int>(n(0)),self,other).size());
        if(name=="instance_find"){const auto found=targets(static_cast<int>(n(0)),self,other);const auto index=static_cast<std::size_t>(n(1));return std::int32_t{index<found.size()?found[index]->id:-4};}
        if(name=="instance_create"){Placement p;p.x=n(0);p.y=n(1);p.object=static_cast<int>(n(2));p.id=next_id++;const auto i=make(p);event(i,0,0);return std::int32_t{i->id};}
        if(name=="instance_change"){const int object=static_cast<int>(n(0));const bool perform=n(1)!=0;if(object<0 || static_cast<std::size_t>(object)>=objects.size())throw Error(ErrorKind::vm,"instance_change object ID out of range");if(perform){event(self,1,0);if(self->alive)event(self,12,0);if(!self->alive)return std::monostate{};}self->object=object;self->vars["object_index"]=std::int32_t{object};if(perform){const auto& replacement=objects[static_cast<std::size_t>(object)];self->vars["sprite_index"]=std::int32_t{replacement.sprite};self->vars["mask_index"]=std::int32_t{replacement.mask};self->vars["visible"]=std::int32_t{replacement.visible};self->vars["depth"]=std::int32_t{replacement.depth};self->vars["solid"]=std::int32_t{replacement.solid};self->vars["image_index"]=0.0;self->vars["image_speed"]=1.0;event(self,0,0);}return std::monostate{};}
        if(name=="instance_destroy" || name=="action_kill_object"){event(self,1,0);self->alive=false;return std::monostate{};}
        if(name=="room_set_persistent"){const int id=static_cast<int>(n(0));if(id<0 || static_cast<std::size_t>(id)>=rooms.size())throw Error(ErrorKind::vm,"room ID out of range");const bool persistent=n(1)!=0;rooms[static_cast<std::size_t>(id)].persistent=persistent;if(id==current_room)engine["room_persistent"]=std::int32_t{persistent};if(auto saved=saved_rooms.find(id);saved!=saved_rooms.end())saved->second.engine["room_persistent"]=std::int32_t{persistent};return std::monostate{};}
        if(name=="room_get_name"){const int id=number(args[0]);if(id<0 || static_cast<std::size_t>(id)>=rooms.size())throw Error(ErrorKind::vm,"room ID out of range");return rooms[static_cast<std::size_t>(id)].name;}
        if(name=="room_goto"){pending_room=static_cast<int>(n(0));return std::monostate{};}
        if(name=="room_goto_next"){const auto it=std::find(order.begin(),order.end(),current_room);if(it==order.end() || it+1==order.end())throw Error(ErrorKind::vm,"no next room");pending_room=*(it+1);return std::monostate{};}
        if(name=="room_previous" || name=="room_next"){const int id=args.empty()?current_room:static_cast<int>(n(0));const auto it=std::find(order.begin(),order.end(),id);if(it==order.end())return std::int32_t{-1};if(name=="room_previous")return std::int32_t{it==order.begin()?-1:*(it-1)};return std::int32_t{it+1==order.end()?-1:*(it+1)};}
        if(name=="keyboard_check" || name=="keyboard_check_direct" || name=="keyboard_check_pressed" || name=="keyboard_check_released"){const auto key=static_cast<int>(n(0));if(key<0 || key>=256)return false;return name=="keyboard_check_pressed"?pressed[static_cast<std::size_t>(key)]:name=="keyboard_check_released"?released[static_cast<std::size_t>(key)]:held[static_cast<std::size_t>(key)];}
        if(name=="keyboard_key_press" || name=="keyboard_key_release"){const auto key=static_cast<int>(n(0));if(key>=0 && key<256){const auto k=static_cast<std::size_t>(key);held[k]=name=="keyboard_key_press";}return std::monostate{};}
        if(name=="gamepad_get_device_count")return std::int32_t{0};if(name=="gamepad_is_connected")return false;
        if(name=="joystick_exists")return false;
        if(name=="sqr")return n(0)*n(0);if(name=="power")return std::pow(n(0),n(1));
        if(name=="abs")return std::abs(n(0));if(name=="floor")return std::floor(n(0));if(name=="ceil")return std::ceil(n(0));if(name=="round")return std::nearbyint(n(0));if(name=="sin")return std::sin(n(0));if(name=="cos")return std::cos(n(0));if(name=="sqrt")return std::sqrt(n(0));
        if(name=="min" || name=="max"){double v=n(0);for(std::size_t i=1;i<args.size();++i)v=name=="min"?std::min(v,n(i)):std::max(v,n(i));return v;}
        if(name=="clamp")return std::clamp(n(0),n(1),n(2));
        if(name=="random")return std::generate_canonical<double,53>(random)*n(0);if(name=="irandom")return std::floor(std::generate_canonical<double,53>(random)*(n(0)+1));
        if(name=="choose"){if(args.empty())throw Error(ErrorKind::vm,"choose requires at least one argument");return args[std::uniform_int_distribution<std::size_t>(0,args.size()-1)(random)];}
        if(name=="window_set_caption"){engine["window_caption"]=text(0);return std::monostate{};}
        if(name=="show_debug_message"){std::cout<<"Guest: "<<text(0)<<'\n';return std::monostate{};}
        throw Error(ErrorKind::unsupported,"engine built-in pending: "+std::string(name));
    }
    void tick(const KeyboardState& input,const KeyboardState& edges) {
        held=input;pressed=edges;for(std::size_t k=0;k<256;++k){pressed[k]=pressed[k] || (held[k] && !previous_held[k]);released[k]=!held[k] && previous_held[k];}previous_held=held;event_budget=0;
        for(int transitions=0;pending_room>=0 && transitions<16;++transitions)enter(pending_room);
        const auto snapshot=instances;
        for(auto& i:snapshot){event(i,3,1);if(i->alive){i->vars["xprevious"]=i->vars["x"];i->vars["yprevious"]=i->vars["y"];}}
        for(auto& i:snapshot)if(i->alive)for(int a=0;a<12;++a){auto& v=i->vars.at("alarm["+std::to_string(a)+"]");const auto timer=number(v);if(timer>0){v=std::int32_t{timer-1};if(timer==1)event(i,2,a);}}
        for(auto& i:snapshot)event(i,3,0);
        for(auto& i:snapshot)if(i->alive){
            double h=value_number(i->vars.at("hspeed")),v=value_number(i->vars.at("vspeed"));const double speed=std::hypot(h,v),friction=value_number(i->vars.at("friction"));
            if(speed!=0){const double next=std::max(0.0,speed-friction);h*=next/speed;v*=next/speed;}
            const double gravity=value_number(i->vars.at("gravity")),angle=value_number(i->vars.at("gravity_direction"))*3.141592653589793/180;h+=gravity*std::cos(angle);v-=gravity*std::sin(angle);
            i->vars["hspeed"]=h;i->vars["vspeed"]=v;i->vars["speed"]=std::hypot(h,v);if(h!=0 || v!=0)i->vars["direction"]=std::fmod(std::atan2(-v,h)*180/3.141592653589793+360,360);
            const int path_id=number(i->vars.at("path_index"));if(path_id>=0){
                const auto& path=paths.at(static_cast<std::size_t>(path_id));if(path.length>0){
                    const double scale=value_number(i->vars.at("path_scale"));double position=value_number(i->vars.at("path_position")),distance=position*path.length;
                    auto sample=[&](double d){auto next=std::lower_bound(path.points.begin()+1,path.points.end(),d,[](const PathPoint& point,double target){return point.distance<target;});if(next==path.points.end())return path.points.back();const auto& before=*(next-1);const double length=next->distance-before.distance,t=length>0?(d-before.distance)/length:0;return PathPoint{before.x+(next->x-before.x)*t,before.y+(next->y-before.y)*t,before.speed+(next->speed-before.speed)*t,d};};
                    distance+=value_number(i->vars.at("path_speed"))*sample(distance).speed/100/std::max(std::abs(scale),0.000001);const bool ended=distance<0 || distance>=path.length;const int action=number(i->vars.at("path_endaction"));
                    if(ended){if(action==1)distance=std::fmod(std::fmod(distance,path.length)+path.length,path.length);else if(action==3){distance=std::clamp(distance,0.0,path.length);i->vars["path_speed"]=-value_number(i->vars.at("path_speed"));}else {distance=std::clamp(distance,0.0,path.length);i->vars["path_index"]=std::int32_t{-1};}}
                    const auto point=sample(distance);const auto before=sample(position*path.length);const double dx=(point.x-before.x)*scale,dy=(point.y-before.y)*scale;
                    if(dx!=0 || dy!=0)i->vars["direction"]=std::fmod(std::atan2(-dy,dx)*180/3.141592653589793+360,360);
                    i->vars["path_positionprevious"]=position;i->vars["path_position"]=distance/path.length;i->vars["x"]=value_number(i->vars.at("path_xstart"))+point.x*scale;i->vars["y"]=value_number(i->vars.at("path_ystart"))+point.y*scale;h=0;v=0;if(ended)event(i,7,8);
                }
            }
            i->vars["x"]=value_number(i->vars["x"])+h;i->vars["y"]=value_number(i->vars["y"])+v;for(std::size_t k=0;k<256;++k){if(held[k])event(i,5,static_cast<int>(k));if(pressed[k])event(i,9,static_cast<int>(k));}
        }
        // Outside-room events fire when the entire sprite leaves the room.
        // Guest code uses this event to advance attacks after projectiles are dodged.
        const double room_width=value_number(engine.at("room_width")),room_height=value_number(engine.at("room_height"));
        for(const auto& i:snapshot)if(i->alive){const auto b=bounds(*i,value_number(i->vars.at("x")),value_number(i->vars.at("y")),true);
            const bool outside=b.valid?(b.right<=0 || b.left>=room_width || b.bottom<=0 || b.top>=room_height):(b.left<0 || b.left>room_width || b.top<0 || b.top>room_height);
            const bool entered=outside && !i->outside_room;i->outside_room=outside;if(entered)event(i,7,0);
        }
        // Resolve each object's most specific collision handler, including inherited targets.
        for(const auto& i:snapshot)if(i->alive)for(const auto& j:snapshot)if(j->alive && i!=j){
            const auto b=bounds(*i),o=bounds(*j);if(!b.valid || !o.valid || b.left>=o.right || b.right<=o.left || b.top>=o.bottom || b.bottom<=o.top)continue;
            bool handled=false;int source=i->object;
            for(std::size_t guard=0;source>=0 && static_cast<std::size_t>(source)<objects.size() && guard<objects.size() && !handled;++guard){
                int target=j->object;for(std::size_t tg=0;target>=0 && static_cast<std::size_t>(target)<objects.size() && tg<objects.size();++tg){
                    if(objects[static_cast<std::size_t>(source)].events.contains({4,target})){event(i,4,target,source,j);handled=true;break;}target=objects[static_cast<std::size_t>(target)].parent;
                }source=objects[static_cast<std::size_t>(source)].parent;
            }
        }
        for(const auto& i:snapshot)event(i,3,2);
        if(number(engine.at("view_enabled"))!=0 && number(engine.at("view_visible[0]"))!=0){
            const int target=number(engine.at("view_object[0]"));const double vw=value_number(engine.at("view_wview[0]")),vh=value_number(engine.at("view_hview[0]"));
            if(target>=0 && vw>0 && vh>0){const auto tracked=std::find_if(instances.begin(),instances.end(),[&](const auto& i){return i->alive && i->object==target;});
                if(tracked!=instances.end()){const auto& room=rooms.at(static_cast<std::size_t>(current_room));
                    auto follow=[&](const char* view_key,const char* speed_key,const char* coordinate,double extent,double border,double room_extent){
                        double origin=value_number(engine.at(view_key));const double pos=value_number((*tracked)->vars.at(coordinate));double desired=origin;
                        if(pos<origin+border)desired=pos-border;else if(pos>origin+extent-border)desired=pos-extent+border;
                        const double rate=value_number(engine.at(speed_key));if(rate<0)origin=desired;else origin+=std::clamp(desired-origin,-rate,rate);
                        const double maximum=std::max(0.0,room_extent-extent);origin=std::clamp(origin,0.0,maximum);engine[view_key]=origin;
                    };
                    follow("view_xview[0]","view_hspeed[0]","x",vw,value_number(engine.at("view_hborder[0]")),room.width);
                    follow("view_yview[0]","view_vspeed[0]","y",vh,value_number(engine.at("view_vborder[0]")),room.height);
                }
            }
        }
        canvas.clear(rooms.at(static_cast<std::size_t>(current_room)).color);
        if(number(engine.at("view_enabled"))!=0 && number(engine.at("view_visible[0]"))!=0){const auto w=value_number(engine.at("view_wview[0]")),h=value_number(engine.at("view_hview[0]"));if(w<=0 || h<=0)throw Error(ErrorKind::vm,"invalid view dimensions");assets.view(value_number(engine.at("view_xview[0]")),value_number(engine.at("view_yview[0]")),value_number(engine.at("view_wport[0]"))/w,value_number(engine.at("view_hport[0]"))/h,value_number(engine.at("view_xport[0]")),value_number(engine.at("view_yport[0]")));}else assets.view(0,0,1,1,0,0);
        const auto& room=rooms.at(static_cast<std::size_t>(current_room));for(const auto& b:room.backgrounds)if(!b.foreground)assets.draw_background(canvas,b.id,b.x,b.y);
        auto draw_tile=[&](const Tile& t){assets.draw_tile(canvas,t.id,t.source_x,t.source_y,t.width,t.height,t.x,t.y,t.sx,t.sy,t.color);};std::size_t tile=0;
        auto drawing=snapshot;std::stable_sort(drawing.begin(),drawing.end(),[](const auto& a,const auto& b){return value_number(a->vars.at("depth"))>value_number(b->vars.at("depth"));});
        for(auto& i:drawing)if(i->alive && number(i->vars.at("visible"))){
            while(tile<room.tiles.size() && room.tiles[tile].depth>=value_number(i->vars.at("depth")))draw_tile(room.tiles[tile++]);
            int obj=i->object;bool custom=false;
            for(std::size_t guard=0;obj>=0 && static_cast<std::size_t>(obj)<objects.size() && guard<objects.size();++guard){if(objects[static_cast<std::size_t>(obj)].events.contains({8,0})){custom=true;break;}obj=objects[static_cast<std::size_t>(obj)].parent;}
            if(custom)event(i,8,0);else {const auto sprite=number(i->vars.at("sprite_index"));if(sprite>=0)assets.draw_sprite(canvas,sprite,value_number(i->vars.at("image_index")),value_number(i->vars.at("x")),value_number(i->vars.at("y")),value_number(i->vars.at("image_xscale")),value_number(i->vars.at("image_yscale")),value_number(i->vars.at("image_angle")),static_cast<unsigned>(number(i->vars.at("image_blend"))),value_number(i->vars.at("image_alpha")));}
            const int animated_object=i->object,animated_sprite=number(i->vars.at("sprite_index"));const double old_index=value_number(i->vars.at("image_index")),image_speed=value_number(i->vars.at("image_speed"));
            if(animated_sprite>=0 && image_speed!=0){const auto frame_count=assets.sprite(animated_sprite).frames.size();if(frame_count){const double count=static_cast<double>(frame_count),next_index=old_index+image_speed;const bool ended=(image_speed>0 && old_index<count && next_index>=count)||(image_speed<0 && old_index>=0 && next_index<0);i->vars["image_index"]=next_index;if(ended)event(i,7,7);if(i->alive && i->object==animated_object && number(i->vars.at("sprite_index"))==animated_sprite && value_number(i->vars.at("image_index"))==next_index)i->vars["image_index"]=std::fmod(std::fmod(next_index,count)+count,count);}}
        }
        while(tile<room.tiles.size())draw_tile(room.tiles[tile++]);for(const auto& b:room.backgrounds)if(b.foreground)assets.draw_background(canvas,b.id,b.x,b.y);
        std::erase_if(instances,[](const auto& i){return !i->alive;});++frame;engine["current_time"]=static_cast<double>(frame)*1000.0/value_number(engine["room_speed"]);audio_frame=audio.mix(static_cast<std::size_t>(48000/value_number(engine["room_speed"])));
    }
};
GameSession::GameSession(const std::filesystem::path& path,std::optional<std::uint32_t> seed,const std::filesystem::path& save_directory):impl_(std::make_unique<Impl>(path,seed,save_directory)){}
GameSession::~GameSession()=default;
void GameSession::tick(const KeyboardState& h,const KeyboardState& p){impl_->tick(h,p);}
const Canvas& GameSession::canvas()const{return impl_->canvas;}
std::size_t GameSession::frames()const{return impl_->frame;}
std::size_t GameSession::instructions()const{return impl_->instruction_count;}
std::string GameSession::room_name()const{return impl_->rooms.at(static_cast<std::size_t>(impl_->current_room)).name;}
std::span<const float> GameSession::audio()const{return impl_->audio_frame;}
double GameSession::speed()const{return value_number(impl_->engine.at("room_speed"));}
std::vector<GameSession::InstanceState> GameSession::instance_states()const {
    std::vector<InstanceState> result;for(const auto& i:impl_->instances)if(i->alive)result.push_back({i->id,impl_->objects.at(static_cast<std::size_t>(i->object)).name,i->vars});return result;
}
} // namespace regml
