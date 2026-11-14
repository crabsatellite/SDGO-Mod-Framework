#include "../mods/unit_appearance_client/rgb/panel.h"
#include <cstdio>
#include <cstdlib>
using namespace sdgo::rgb_paint;
unsigned checks=0;
void CheckAt(bool v,int line){++checks;if(!v){std::fprintf(stderr,"failed check %u at line %d\n",checks,line);std::exit(1);}}
#define Check(...) CheckAt((__VA_ARGS__),__LINE__)
void Text(ui::Panel& p,const wchar_t* text){for(;*text;++text)Check(p.Character(*text));Check(p.Key(13));}
int main(){
    ui::Panel p;Snapshot original;original.identity={1,42};original.colors[0]=Rgb{12,34,56};Check(p.Open(original));
    for(unsigned i=0;i<6;++i)p.layout.regions[i]={20.f,100.f+i*40,40,30};
    Check(p.Down(970,410));Text(p,L"256");Check(p.invalid);Check(p.editor.preview()==original.colors);
    Check(p.Key(27));Check(!p.invalid);Check(p.Down(970,410));Text(p,L"0");Check(p.editor.preview()[0]->r==0);
    Check(p.Down(850,530));Text(p,L"#FF0048");Check(p.picker.color()==Rgb{255,0,72});
    Check(p.Down(850,530));Text(p,L"#FF");Check(p.invalid);Check(p.picker.color()==Rgb{255,0,72});Check(p.Key(27));
    for(unsigned i=1;i<6;++i){Check(p.Down(30,110.f+i*40));Check(p.editor.selected()==i);Check(p.Down(850,530));Text(p,L"123456");Check(p.editor.preview()[i]==Rgb{18,52,86});}
    Check(p.editor.preview()[0]==Rgb{255,0,72});
    Check(p.Down(850,570));Check(!p.editor.preview()[5]);Check(p.editor.preview()[4].has_value());
    Check(p.Down(940,570));Check(p.editor.preview()==original.colors);
    Check(p.Down(30,110));Check(p.Down(850,410));Check(p.Dragging());Check(p.Move(2000,0));Check(p.picker.color().r==255);Check(p.Move(-100,0));Check(p.picker.color().r==0);p.Up();Check(!p.Move(0,0));
    Check(p.Down(990,260));Check(p.Move(990,9999));p.Up();Check(p.Down(900,200));Check(p.Move(900,-10));p.Up();Check(p.picker.hsv().v==1);
    Check(p.Down(940,640));Check(!p.editing);Check(!p.Down(900,200));Check(p.Down(940,640));Check(p.editing);
    Snapshot second;second.identity={1,77};Check(p.Open(second));Check(p.editor.preview()==second.colors);Check(p.editor.selected()==0);
    Check(p.editor.Set(Rgb{1,2,3}));Check(p.editor.Confirm());const auto saving=p.editor.Submit();Check(saving.has_value());
    const auto draft=p.editor.preview();Check(p.Down(850,570));Check(p.Down(940,640));Check(p.Key(27));Check(p.Character(L'1'));Check(p.editing&&p.editor.preview()==draft&&p.editor.state()==State::saving);
    Check(p.editor.Timeout(*saving));Check(p.Down(940,570));Check(p.editor.preview()==draft&&p.editor.state()==State::uncertain);
    std::printf("RGB panel: %u checks passed\n",checks);
}
