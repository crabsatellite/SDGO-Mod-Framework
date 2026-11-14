#include "sdgo/rgb_paint.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace sdgo::rgb_paint;
namespace {
int checks = 0;
void Check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
void Colors() {
    Rgb color{1, 2, 3};
    Check(ParseHex("#FF0048", color) && color == Rgb{255, 0, 72}, "reference RGB color");
    Check(Hex(color) == "#FF0048", "uppercase hex output");
    for (auto bad : {"", "#fff", "#FF00480", "#FF00GG", " FF0048", "-F0048", "##0048"}) {
        Check(!ParseHex(bad, color) && color == Rgb{255, 0, 72}, "invalid hex is non-mutating");
    }
    Check(ParseHex("aBcDeF", color) && color == Rgb{171,205,239}, "case insensitive hex");
    Check(!FromNative(0), "native zero means reset");
    Check(FromNative(0x7fff) == Rgb{248,248,248}, "stock RGB555 must not be expanded to 255");
    Check(FromNative(0x8000) == Rgb{0,0,0}, "nonzero native black");
    for (int r = 0; r <= 255; ++r) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
        const Rgb source{static_cast<std::uint8_t>(r),static_cast<std::uint8_t>(g),static_cast<std::uint8_t>(b)};
        Check(FromHsv(ToHsv(source), color) && color == source, "RGB/HSV roundtrip precision");
        Check(ParseHex(Hex(source), color) && color == source, "RGB/hex roundtrip precision");
    }
    for (auto invalid : {Hsv{-1,1,1}, Hsv{361,1,1}, Hsv{0,-.1,1}, Hsv{0,1,1.1},
                        Hsv{std::numeric_limits<double>::quiet_NaN(),1,1},
                        Hsv{0,std::numeric_limits<double>::infinity(),1}}) {
        color = {4,5,6}; Check(!FromHsv(invalid,color) && color == Rgb{4,5,6}, "invalid HSV is non-mutating");
    }
    Check(FromHsv({360,1,1},color) && color == Rgb{255,0,0}, "hue wrap");
}
void Transactions() {
    Snapshot initial{{11,0x100000003ull},9,{}};
    initial.colors[2] = Rgb{10,20,30};
    Editor e;
    Check(!e.Set({255,0,72}) && !e.Submit(), "closed editor cannot mutate or send");
    Check(!e.Open({}) && e.Open(initial), "authoritative identity required");
    Check(!e.Open(initial) && !e.Select(6), "cannot overwrite live draft or select absent region");
    Check(!e.Confirm() && e.Select(2) && e.Set({0,0,0}), "edit selected region");
    Check(e.preview()[2] == Rgb{0,0,0} && e.saved().colors == initial.colors, "black is preview only");
    Check(e.Clear() && !e.preview()[2] && e.Restore() && !e.dirty(), "clear differs from black; restore cancels draft");
    Check(e.Set({255,0,72}) && e.Confirm() && !e.Set({0,0,1}), "confirmation freezes preview");
    Check(e.CancelConfirmation() && e.Confirm(), "confirmation is cancelable");
    const auto request = e.Submit();
    Check(request && request->identity.unit == initial.identity.unit && !e.Submit(), "one pending save; preserve 64-bit UID");
    Snapshot accepted = initial; accepted.revision++; accepted.colors = request->colors;
    auto stale = *request; ++stale.sequence;
    Check(!e.Reply(stale, accepted, true), "wrong sequence ignored");
    auto wrong = accepted; wrong.identity.session++;
    Check(!e.Reply(*request, wrong, true), "wrong session ignored");
    wrong = accepted; wrong.colors[0] = Rgb{1,1,1};
    Check(!e.Reply(*request, wrong, true), "mismatching saved colors are not success");
    Check(!e.Reply(*request, initial, true), "non-advancing save revision rejected");
    Check(e.Timeout(*request) && !e.Restore() && !e.Confirm(), "timeout freezes writes until authoritative resolution");
    wrong = accepted; wrong.revision = initial.revision;
    Check(!e.Reconcile(wrong) && !e.Reply(*request,wrong,false), "a revision cannot describe two different palettes");
    Check(e.Reply(*request, accepted, true) && !e.dirty(), "late authenticated success resolves timeout");
    Check(e.Set({1,2,3}) && e.Confirm(), "second draft");
    auto second = e.Submit();
    Check(e.Reply(*second, accepted, false) && e.dirty(), "rejection retains unsaved draft");
    Check(e.Confirm(), "rejected draft can be explicitly retried");
    second = e.Submit();
    Check(e.Timeout(*second) && e.Reconcile(accepted) && !e.dirty(), "readback resolves uncertain write without resend");
    e.Close(); Check(e.Open(initial) && e.Set({2,3,4}) && e.Confirm(), "new page lifetime");
    auto third = e.Submit();
    Check(third->sequence > second->sequence && !e.Reply(*request, accepted, true), "old page reply cannot commit new draft");
    e.Close(); Check(!e.Reply(*third, accepted, true), "closed page ignores reply");
}
void PickerInputs() {
    Picker picker;
    picker.Set({0,0,255});
    Check(picker.Square(1,1) && picker.color() == Rgb{0,0,0}, "square bottom is black");
    Check(picker.Square(1,0) && picker.color() == Rgb{0,0,255}, "hue survives the black endpoint");
    Check(picker.HexInput("#000000") && picker.Square(1,0) && picker.color() == Rgb{0,0,255}, "black RGB input retains hue");
    Check(picker.HexInput("#FFFFFF") && picker.Square(1,0) && picker.color() == Rgb{0,0,255}, "white RGB input retains hue");
    Check(picker.Hue(0) && picker.color() == Rgb{255,0,0}, "hue strip red endpoint");
    Check(picker.Channel(2,72) && picker.color() == Rgb{255,0,72}, "reference color through numeric RGB input");
    const auto saved = picker.color();
    Check(!picker.Channel(3,0) && !picker.Channel(0,-1) && !picker.Channel(0,256), "channel/range validation");
    Check(!picker.Hue(-.1) && !picker.Square(1.1,0) && !picker.HexInput("#FF004"), "incomplete input does not change preview");
    Check(picker.color() == saved, "invalid picker input is non-mutating");
}
}
int main() { Colors(); Transactions(); PickerInputs(); std::cout << checks << " RGB paint checks passed\n"; }
