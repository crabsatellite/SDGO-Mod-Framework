#pragma once
#include <algorithm>

namespace sdgo::friend_unit_gift::client::ui {
struct CardRect {
    int x=0,y=0,width=0,height=0;
    constexpr int right() const {return x+width;}
    constexpr int bottom() const {return y+height;}
};
struct CardLayout {
    CardRect background,panel,label,note,primary,cancel;
    bool valid=false;
};
// Anchor to the shipped skill row and level gauge, rather than the dialog's
// large invisible canvas. The original information-card border is smaller.
inline CardLayout InformationCardLayout(CardRect firstSkill,CardRect lastSkill,
                                      CardRect skillTitle,CardRect levelGauge,bool received) {
    CardLayout layout;
    const int left=firstSkill.x-15,right=lastSkill.right()+15;
    const int footerBottom=levelGauge.bottom()+7;
    const int buttonY=footerBottom-31;
    const int top=received?skillTitle.y:std::max(firstSkill.bottom(),lastSkill.bottom())+12;
    layout.panel={left,top,right-left,buttonY-8-top};
    // Replace the whole stock custom-parts area, including the gaps behind
    // the footer. A note-sized overlay leaves the baked-in PARTS slots visible.
    layout.background={left,top,right-left,footerBottom+8-top};
    layout.label={left+12,top+6,right-left-24,15};
    layout.note={left+12,top+25,right-left-24,layout.panel.height-33};
    layout.primary={right-174,buttonY,82,31};
    layout.cancel={right-82,buttonY,82,31};
    layout.valid=left>=0 && layout.panel.width>=200 && layout.panel.height>=52 &&
        layout.note.height>=19 && layout.label.bottom()+4<=layout.note.y;
    return layout;
}
}
