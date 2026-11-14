#pragma once
#include "sdgo/rgb_paint.h"
#include <array>
#include <string>

namespace sdgo::rgb_paint::ui {
struct Rect {
    float x=0,y=0,w=0,h=0;
    bool Contains(float px,float py) const {return px>=x&&py>=y&&px<x+w&&py<y+h;}
};
struct Layout {
    Rect square{770,164,208,208},hue{986,164,18,208};
    std::array<Rect,3> bars{{{795,405,150,10},{795,443,150,10},{795,481,150,10}}};
    std::array<Rect,3> fields{{{952,396,52,28},{952,434,52,28},{952,472,52,28}}};
    Rect hex{812,520,192,28},clear{770,566,112,28},restore{892,566,112,28},save{770,632,112,26},toggle{892,632,112,26};
    std::array<Rect,6> regions{};
};
class Panel {
public:
    bool Open(const Snapshot&);
    void Close();
    bool Down(float x,float y);
    bool Contains(float x,float y) const;
    bool Move(float x,float y);
    void Up();
    bool Character(wchar_t);
    bool Key(unsigned);
    bool Commit();
    bool Select(unsigned);
    void Restore();
    Layout layout;
    Editor editor;
    Picker picker;
    int focus=-1;
    bool editing=true,invalid=false;
    std::wstring buffer;
    unsigned changes=0;
    bool Dragging() const {return drag_>=0;}
private:
    int drag_=-1;
    bool replace_=false;
    void Reload();
    void Focus(int);
    void Drag(float,float);
};
}
