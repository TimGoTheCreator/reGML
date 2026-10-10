#include "regml/render/assets.hpp"
#include <png.h>
#include <cmath>
#include <algorithm>
namespace regml {
GameAssets::GameAssets(const Package& package):package_(package) {
    const auto data=package.view();
    auto list=[&](std::size_t at,const Chunk& c){
        if(at<c.data_offset || at>c.data_offset+c.size || c.data_offset+c.size-at<4)throw Error(ErrorKind::malformed,"asset list outside chunk",at);
        const auto count=static_cast<std::size_t>(data.u32(at));if(count>1'000'000 || count>(c.data_offset+c.size-at-4)/4)throw Error(ErrorKind::malformed,"asset list count",at);
        std::vector<std::size_t> out;for(std::size_t i=0;i<count;++i){const auto p=data.u32(at+4+i*4);if(p && (p<c.data_offset || p>=c.data_offset+c.size))throw Error(ErrorKind::malformed,"asset record outside chunk",p);out.push_back(p);}return out;
    };
    auto rec=[&](std::size_t at,const Chunk& c,std::size_t bytes){if(at<c.data_offset || at>c.data_offset+c.size || bytes>c.data_offset+c.size-at)throw Error(ErrorKind::malformed,"asset record range",at);return data.slice(at,bytes);};
    const auto* tx=package.find_chunk("TXTR");const auto* tp=package.find_chunk("TPAG");const auto* sp=package.find_chunk("SPRT");const auto* fo=package.find_chunk("FONT");
    if(!tx || !tp || !sp || !fo)throw Error(ErrorKind::unsupported,"graphics resources missing");
    for(auto p:list(tx->data_offset,*tx)){const auto r=rec(p,*tx,8);const auto image=r.u32(4);if(image<tx->data_offset || image>=tx->data_offset+tx->size)throw Error(ErrorKind::malformed,"PNG outside TXTR",p+4);pages_.push_back({0,0,image,{}});}
    for(auto p:list(tp->data_offset,*tp)){const auto r=rec(p,*tp,22);Region region{r.u16(0),r.u16(2),r.u16(4),r.u16(6),r.u16(8),r.u16(10),r.u16(12),r.u16(14),r.i16(20)};if(region.page<0 || static_cast<std::size_t>(region.page)>=pages_.size())throw Error(ErrorKind::malformed,"invalid texture page ID",p+20);regions_.emplace(p,region);}
    for(auto p:list(sp->data_offset,*sp)){
        const auto r=rec(p,*sp,60);Sprite sprite;sprite.width=r.i32(4);sprite.height=r.i32(8);sprite.ox=r.i32(48);sprite.oy=r.i32(52);const auto count=r.u32(56);
        sprite.left=r.i32(12);sprite.right=r.i32(16);sprite.bottom=r.i32(20);sprite.top=r.i32(24);
        if(count>10000)throw Error(ErrorKind::unsupported,"special sprite profile is unsupported",p+56);
        const auto frames=rec(p+60,*sp,static_cast<std::size_t>(count)*4);for(std::size_t i=0;i<count;++i){const auto region=frames.u32(i*4);if(region && !regions_.contains(region))throw Error(ErrorKind::malformed,"sprite TPAG reference invalid",p+60+i*4);sprite.frames.push_back(region);}sprites_.push_back(std::move(sprite));
    }
    if(const auto* bg=package.find_chunk("BGND"))for(auto p:list(bg->data_offset,*bg)){const auto r=rec(p,*bg,20);const auto ptr=r.u32(16);if(ptr && !regions_.contains(ptr))throw Error(ErrorKind::malformed,"background TPAG reference invalid",p+16);backgrounds_.push_back(ptr);}
    for(auto p:list(fo->data_offset,*fo)){
        const auto r=rec(p,*fo,44);Font font;font.size=r.i32(8);font.region=r.u32(28);if(!regions_.contains(font.region))throw Error(ErrorKind::malformed,"font TPAG reference invalid",p+28);
        for(auto q:list(p+40,*fo)){const auto g=rec(q,*fo,16);font.glyphs[g.u16(0)]={g.u16(2),g.u16(4),g.u16(6),g.u16(8),g.i16(10),g.i16(12)};}fonts_.push_back(std::move(font));
    }
}
const GameAssets::Sprite& GameAssets::sprite(int id)const {if(id<0 || static_cast<std::size_t>(id)>=sprites_.size())throw Error(ErrorKind::vm,"sprite ID out of range");return sprites_[static_cast<std::size_t>(id)];}
void GameAssets::load_page(int id) {
    auto& page=pages_.at(static_cast<std::size_t>(id));if(!page.pixels.empty())return;
    const auto* chunk=package_.find_chunk("TXTR");const auto bytes=package_.view().slice(page.png,chunk->data_offset+chunk->size-page.png).bytes();
    png_image image{};image.version=PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_memory(&image,bytes.data(),bytes.size()))throw Error(ErrorKind::malformed,"texture PNG header invalid",page.png);
    struct Guard{png_image& image;~Guard(){png_image_free(&image);}}guard{image};
    if(image.width>8192 || image.height>8192 || !image.width || !image.height)throw Error(ErrorKind::unsupported,"texture dimensions exceed limit",page.png);
    image.format=PNG_FORMAT_RGBA;std::vector<unsigned char> rgba(PNG_IMAGE_SIZE(image));
    if(!png_image_finish_read(&image,nullptr,rgba.data(),0,nullptr))throw Error(ErrorKind::malformed,"texture PNG decode failed",page.png);
    page.width=static_cast<int>(image.width);page.height=static_cast<int>(image.height);page.pixels.resize(static_cast<std::size_t>(page.width)*static_cast<std::size_t>(page.height));
    for(std::size_t i=0;i<page.pixels.size();++i)page.pixels[i]=(static_cast<unsigned>(rgba[i*4+3])<<24)|(static_cast<unsigned>(rgba[i*4])<<16)|(static_cast<unsigned>(rgba[i*4+1])<<8)|rgba[i*4+2];
}
void GameAssets::prefetch(int id){for(auto r:sprite(id).frames)if(r)load_page(regions_.at(r).page);}
void GameAssets::draw_background(Canvas& canvas,int id,double x,double y){
    if(id<0 || static_cast<std::size_t>(id)>=backgrounds_.size())throw Error(ErrorKind::vm,"background ID out of range");const auto ptr=backgrounds_[static_cast<std::size_t>(id)];if(!ptr)return;const auto& r=regions_.at(ptr);blit(canvas,r,x+r.tx,y+r.ty,1,1,0,0xffffff,1);
}
void GameAssets::draw_tile(Canvas& canvas,int id,int source_x,int source_y,int width,int height,double x,double y,double sx,double sy,unsigned color){
    if(id<0 || static_cast<std::size_t>(id)>=backgrounds_.size())throw Error(ErrorKind::vm,"tile background ID out of range");const auto ptr=backgrounds_[static_cast<std::size_t>(id)];if(!ptr)return;const auto& r=regions_.at(ptr);
    const int left=std::max(source_x,r.tx),top=std::max(source_y,r.ty),right=std::min(source_x+width,r.tx+r.tw),bottom=std::min(source_y+height,r.ty+r.th);if(right<=left || bottom<=top)return;
    Region cut{r.x+(left-r.tx),r.y+(top-r.ty),right-left,bottom-top,0,0,right-left,bottom-top,r.page};blit(canvas,cut,x+(left-source_x)*sx,y+(top-source_y)*sy,sx,sy,0,color&0xffffff,static_cast<double>(color>>24)/255);
}
void GameAssets::view(double x,double y,double sx,double sy,double px,double py){if(!std::isfinite(x+y+sx+sy+px+py))throw Error(ErrorKind::vm,"nonfinite view transform");view_x_=x;view_y_=y;view_sx_=sx;view_sy_=sy;port_x_=px;port_y_=py;}
void GameAssets::rectangle(Canvas& canvas,double x1,double y1,double x2,double y2,bool outline)const {
    canvas.rectangle((x1-view_x_)*view_sx_+port_x_,(y1-view_y_)*view_sy_+port_y_,(x2+1-view_x_)*view_sx_+port_x_,(y2+1-view_y_)*view_sy_+port_y_,outline);
}
void GameAssets::blit(Canvas& canvas,const Region& r,double x,double y,double sx,double sy,double angle,unsigned color,double alpha) {
    x=(x-view_x_)*view_sx_+port_x_;y=(y-view_y_)*view_sy_+port_y_;sx*=view_sx_;sy*=view_sy_;
    if(!std::isfinite(x+y+sx+sy+angle+alpha))throw Error(ErrorKind::vm,"nonfinite sprite transform");
    if(sx==0 || sy==0 || r.w==0 || r.h==0)return;load_page(r.page);const auto& page=pages_.at(static_cast<std::size_t>(r.page));
    if(r.x<0 || r.y<0 || r.x+r.w>page.width || r.y+r.h>page.height)throw Error(ErrorKind::malformed,"texture region outside page");
    const auto radians=angle*3.141592653589793/180;const auto c=std::cos(radians),s=std::sin(radians);
    const double w=r.tw*sx,h=r.th*sy;const double xx[]={x,x+w*c,x+h*s,x+w*c+h*s};const double yy[]={y,y-w*s,y+h*c,y-w*s+h*c};
    const auto left=static_cast<int>(std::clamp(std::floor(*std::min_element(xx,xx+4)),0.0,static_cast<double>(canvas.width())));const auto right=static_cast<int>(std::clamp(std::ceil(*std::max_element(xx,xx+4)),0.0,static_cast<double>(canvas.width())));
    const auto top=static_cast<int>(std::clamp(std::floor(*std::min_element(yy,yy+4)),0.0,static_cast<double>(canvas.height())));const auto bottom=static_cast<int>(std::clamp(std::ceil(*std::max_element(yy,yy+4)),0.0,static_cast<double>(canvas.height())));
    const unsigned tint=((color&0xff)<<16)|(color&0xff00)|((color>>16)&0xff);
    for(int dy=top;dy<bottom;++dy)for(int dx=left;dx<right;++dx){const auto px=dx+0.5-x,py=dy+0.5-y;const auto tx=(px*c-py*s)/sx,ty=(px*s+py*c)/sy;if(tx<0 || ty<0 || tx>=r.tw || ty>=r.th)continue;
        const int ux=r.x+static_cast<int>(tx*r.w/r.tw),uy=r.y+static_cast<int>(ty*r.h/r.th);canvas.rgba_pixel_tinted(dx,dy,page.pixels[static_cast<std::size_t>(uy)*static_cast<std::size_t>(page.width)+static_cast<std::size_t>(ux)],tint,alpha);}
}
void GameAssets::draw_sprite(Canvas& canvas,int id,double frame,double x,double y,double sx,double sy,double angle,unsigned color,double alpha) {
    const auto& spr=sprite(id);if(spr.frames.empty())return;if(!std::isfinite(frame))throw Error(ErrorKind::vm,"nonfinite sprite frame");const auto index=static_cast<std::size_t>(std::fmod(std::floor(frame)+static_cast<double>(spr.frames.size())*1'000'000,static_cast<double>(spr.frames.size())));const auto pointer=spr.frames.at(index);if(!pointer)return;const auto& r=regions_.at(pointer);
    const double ox=(r.tx-spr.ox)*sx,oy=(r.ty-spr.oy)*sy,rad=angle*3.141592653589793/180;blit(canvas,r,x+ox*std::cos(rad)+oy*std::sin(rad),y-ox*std::sin(rad)+oy*std::cos(rad),sx,sy,angle,color,alpha);
}
void GameAssets::draw_sprite_part(Canvas& canvas,int id,double frame,double left,double top,double width,double height,double x,double y,double sx,double sy,unsigned color,double alpha) {
    const auto& spr=sprite(id);if(spr.frames.empty())return;
    if(!std::isfinite(frame+left+top+width+height+x+y+sx+sy+alpha))throw Error(ErrorKind::vm,"nonfinite sprite-part argument");
    if(width<=0 || height<=0 || sx==0 || sy==0)return;
    const auto index=static_cast<std::size_t>(std::fmod(std::floor(frame)+static_cast<double>(spr.frames.size())*1'000'000,static_cast<double>(spr.frames.size())));
    const auto pointer=spr.frames.at(index);if(!pointer)return;const auto& r=regions_.at(pointer);
    const double l=std::max({left,0.0,static_cast<double>(r.tx)}),t=std::max({top,0.0,static_cast<double>(r.ty)});
    const double rr=std::min({left+width,static_cast<double>(spr.width),static_cast<double>(r.tx+r.tw)}),bb=std::min({top+height,static_cast<double>(spr.height),static_cast<double>(r.ty+r.th)});
    if(rr<=l || bb<=t)return;
    const int ix=static_cast<int>(std::floor(l)),iy=static_cast<int>(std::floor(t)),ir=static_cast<int>(std::ceil(rr)),ib=static_cast<int>(std::ceil(bb));
    Region crop{r.x+static_cast<int>((ix-r.tx)*r.w/r.tw),r.y+static_cast<int>((iy-r.ty)*r.h/r.th),static_cast<int>((ir-ix)*r.w/r.tw),static_cast<int>((ib-iy)*r.h/r.th),0,0,ir-ix,ib-iy,r.page};
    blit(canvas,crop,x+(ix-spr.ox)*sx,y+(iy-spr.oy)*sy,sx,sy,0,color,alpha);
}
double GameAssets::text_width(int font,std::string_view text)const {
    if(font<0 || static_cast<std::size_t>(font)>=fonts_.size())return static_cast<double>(text.size()*6);const auto& f=fonts_[static_cast<std::size_t>(font)];double line=0,max=0;for(char raw:text){const auto c=static_cast<unsigned char>(raw);if(c=='\n' || c=='#'){max=std::max(max,line);line=0;}else if(auto it=f.glyphs.find(c);it!=f.glyphs.end())line+=it->second.shift;}return std::max(line,max);
}
double GameAssets::text_height(int font)const{return font<0 || static_cast<std::size_t>(font)>=fonts_.size()?8:fonts_[static_cast<std::size_t>(font)].size;}
void GameAssets::draw_text(Canvas& canvas,int font,double x,double y,std::string_view text,unsigned color,double alpha,int halign,int valign,double sx,double sy,double angle) {
    if(font<0 || static_cast<std::size_t>(font)>=fonts_.size()){canvas.color(color);canvas.alpha(alpha);canvas.text(x,y,text);return;}const auto& f=fonts_[static_cast<std::size_t>(font)];const auto& atlas=regions_.at(f.region);
    const double start=-text_width(font,text)*halign/2.0;double lx=start,ly=-text_height(font)*valign/2.0;const auto rad=angle*3.141592653589793/180,c=std::cos(rad),s=std::sin(rad);
    for(char raw:text){const auto ch=static_cast<unsigned char>(raw);if(ch=='\n' || ch=='#'){lx=start;ly+=text_height(font)+2;continue;}const auto it=f.glyphs.find(ch);if(it==f.glyphs.end())continue;const auto& g=it->second;Region r{atlas.x+g.x,atlas.y+g.y,g.w,g.h,0,0,g.w,g.h,atlas.page};const auto ox=(lx+g.offset)*sx,oy=ly*sy;blit(canvas,r,x+ox*c+oy*s,y-ox*s+oy*c,sx,sy,angle,color,alpha);lx+=g.shift;}
}
} // namespace regml
