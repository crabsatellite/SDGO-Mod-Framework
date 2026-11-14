#include "panel.h"
#include <algorithm>
#include <cmath>

namespace sdgo::rgb_paint::ui {
bool Panel::Open(const Snapshot& value) {
    if(!value.identity.session||!value.identity.unit)return false;
    editor.Close();
    if(!editor.Open(value))return false;
    editing=true;focus=drag_=-1;invalid=false;buffer.clear();Reload();return true;
}
void Panel::Close(){editor.Close();focus=drag_=-1;invalid=false;buffer.clear();}
void Panel::Reload(){picker.Set(editor.preview()[editor.selected()].value_or(Rgb{255,255,255}));}
bool Panel::Select(unsigned region){if(!editor.Select(region))return false;focus=-1;invalid=false;Reload();return true;}
void Panel::Restore(){editor.Restore();Reload();focus=drag_=-1;invalid=false;++changes;}
void Panel::Focus(int value) {
    focus=value;invalid=false;replace_=true;
    if(value==3){const auto hex=Hex(picker.color());buffer.assign(hex.begin(),hex.end());}
    else {const auto c=picker.color();const unsigned v[]={c.r,c.g,c.b};buffer=std::to_wstring(v[value]);}
}
bool Panel::Commit(){
    if(focus<0)return true;
    bool ok=false;
    if(focus==3){std::string text;for(auto ch:buffer){if(ch>127){invalid=true;return false;}text+=static_cast<char>(ch);}ok=picker.HexInput(text);}
    else if(!buffer.empty()&&buffer.size()<=3){
        unsigned value=0;ok=true;
        for(auto ch:buffer){if(ch<L'0'||ch>L'9'){ok=false;break;}value=value*10+ch-L'0';}
        if(ok)ok=value<=255&&picker.Channel(static_cast<unsigned>(focus),static_cast<int>(value));
    }
    invalid=!ok;if(!ok)return false;
    editor.Set(picker.color());++changes;focus=-1;return true;
}
void Panel::Drag(float x,float y){
    if(drag_==0)picker.Square(std::clamp<double>((x-layout.square.x)/layout.square.w,0,1),std::clamp<double>((y-layout.square.y)/layout.square.h,0,1));
    else if(drag_==1)picker.Hue(std::clamp<double>((y-layout.hue.y)/layout.hue.h,0,1));
    else if(drag_>=2&&drag_<5){const auto& r=layout.bars[drag_-2];picker.Channel(drag_-2,static_cast<int>(std::lround(std::clamp<double>((x-r.x)/r.w,0,1)*255)));}
    if(drag_>=0){editor.Set(picker.color());++changes;}
}
bool Panel::Contains(float x,float y) const {
    if(layout.toggle.Contains(x,y))return true;
    if(!editing)return false;
    if(Rect{750,104,274,570}.Contains(x,y))return true;
    for(const auto& r:layout.regions)if(r.Contains(x,y))return true;
    return false;
}
bool Panel::Down(float x,float y){
    if(editor.state()==State::saving||editor.state()==State::uncertain)return true;
    if(editor.state()==State::confirming)editor.CancelConfirmation();
    if(layout.toggle.Contains(x,y)){if(!Commit())return true;editing=!editing;drag_=-1;focus=-1;return true;}
    if(!editing)return false;
    if(!Contains(x,y)){Commit();return false;}
    if(!Commit())return true;
    for(unsigned i=0;i<6;++i)if(layout.regions[i].Contains(x,y)){Select(i);return true;}
    if(layout.clear.Contains(x,y)){editor.Clear();Reload();++changes;return true;}
    if(layout.restore.Contains(x,y)){Restore();return true;}
    if(layout.hex.Contains(x,y)){Focus(3);return true;}
    for(unsigned i=0;i<3;++i){
        if(layout.fields[i].Contains(x,y)){Focus(static_cast<int>(i));return true;}
        auto r=layout.bars[i];r.y-=9;r.h+=18;if(r.Contains(x,y)){drag_=static_cast<int>(i)+2;Drag(x,y);return true;}
    }
    if(layout.square.Contains(x,y))drag_=0;
    else if(layout.hue.Contains(x,y))drag_=1;
    if(drag_>=0)Drag(x,y);return true;
}
bool Panel::Move(float x,float y){if(drag_<0)return false;Drag(x,y);return true;}
void Panel::Up(){drag_=-1;}
bool Panel::Character(wchar_t ch){
    if(editor.state()==State::saving||editor.state()==State::uncertain)return true;
    if(focus<0||!editing)return false;
    if(ch==8){if(replace_)buffer.clear();else if(!buffer.empty())buffer.pop_back();replace_=false;invalid=false;return true;}
    if(ch==13){Commit();return true;}
    if(ch<32)return true;
    const bool allowed=(ch>=L'0'&&ch<=L'9')||(focus==3&&(ch==L'#'||(ch>=L'a'&&ch<=L'f')||(ch>=L'A'&&ch<=L'F')));
    if(allowed){if(replace_)buffer.clear();replace_=false;if(buffer.size()<(focus==3?7u:3u))buffer+=ch;invalid=false;}
    return true;
}
bool Panel::Key(unsigned key){
    if(editor.state()==State::saving||editor.state()==State::uncertain)return true;
    if(!editing||focus<0)return false;
    if(key==27){focus=-1;invalid=false;return true;}
    if(key==13){Commit();return true;}
    if(key==9){const int next=(focus+1)%4;if(Commit())Focus(next);return true;}
    if(key==46){buffer.clear();replace_=false;invalid=false;return true;}
    if((key==38||key==40)&&focus<3){const int channel=focus;if(!Commit())return true;const auto c=picker.color();const int values[]={c.r,c.g,c.b};picker.Channel(channel,std::clamp(values[channel]+(key==38?1:-1),0,255));editor.Set(picker.color());++changes;Focus(channel);return true;}
    return true;
}
}
