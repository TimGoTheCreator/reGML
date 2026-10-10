#pragma once
#include "regml/formats/package.hpp"
#include "regml/render/canvas.hpp"
#include <unordered_map>
namespace regml {
class GameAssets {
public:
    explicit GameAssets(const Package& package);
    struct Sprite { int width=0,height=0,ox=0,oy=0,left=0,right=0,top=0,bottom=0; std::vector<std::size_t> frames; };
    const Sprite& sprite(int id)const;
    void prefetch(int id);
    void draw_background(Canvas& canvas,int id,double x,double y);
    void draw_tile(Canvas& canvas,int id,int source_x,int source_y,int width,int height,double x,double y,double sx,double sy,unsigned color);
    void draw_sprite(Canvas& canvas,int id,double frame,double x,double y,double sx=1,double sy=1,double angle=0,unsigned color=0xffffff,double alpha=1);
    void draw_sprite_part(Canvas& canvas,int id,double frame,double left,double top,double width,double height,double x,double y,double sx,double sy,unsigned color,double alpha);
    void draw_text(Canvas& canvas,int font,double x,double y,std::string_view text,unsigned color,double alpha,int halign=0,int valign=0,double sx=1,double sy=1,double angle=0);
    double text_width(int font,std::string_view text)const;
    double text_height(int font)const;
    void view(double x,double y,double sx,double sy,double px,double py);
    void rectangle(Canvas& canvas,double x1,double y1,double x2,double y2,bool outline)const;
private:
    struct Page {int width=0,height=0;std::size_t png=0;std::vector<std::uint32_t> pixels;};
    struct Region {int x=0,y=0,w=0,h=0,tx=0,ty=0,tw=0,th=0,page=0;};
    struct Glyph {int x=0,y=0,w=0,h=0,shift=0,offset=0;};
    struct Font {std::size_t region=0;int size=0;std::unordered_map<unsigned,Glyph> glyphs;};
    const Package& package_;
    double view_x_=0,view_y_=0,view_sx_=1,view_sy_=1,port_x_=0,port_y_=0;
    std::vector<Sprite> sprites_;std::vector<Page> pages_;std::unordered_map<std::size_t,Region> regions_;std::vector<Font> fonts_;
    std::vector<std::size_t> backgrounds_;
    void load_page(int id);
    void blit(Canvas& canvas,const Region& r,double x,double y,double sx,double sy,double angle,unsigned color,double alpha);
};
} // namespace regml
