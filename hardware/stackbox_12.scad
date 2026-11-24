// Stackbox body (54 mm) cut to 45 mm for 12 washers, like short.scad, but by
// squeezing the knurled band (z 27..54) to 18 mm instead of cutting 9 mm out,
// so the knurl stays continuous. Threads (z < 27) are untouched.
// Stackbox_Body.stl from https://makerworld.com/en/models/196485 (Printer_Gobrrr, CC BY-NC-SA)
STL = "Stackbox_Body.stl";
K0 = 27; K1 = 54; D = 9;
union() {
  intersection() { import(STL); translate([-30,-30,-5]) cube([60,60,K0+5]); }
  translate([0,0,K0]) scale([1,1,(K1-K0-D)/(K1-K0)]) translate([0,0,-K0])
    intersection() { import(STL); translate([-30,-30,K0]) cube([60,60,K1-K0]); }
  translate([0,0,-D])
    intersection() { import(STL); translate([-30,-30,K1]) cube([60,60,20]); }
}
