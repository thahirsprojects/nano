// ===================================================================
//  NANO pocket case  -  Developed by Mohamed Thahir
//  https://github.com/thahirsprojects/nano
//
//  Parts (export each one as STL, see README.md):
//    part = "shell"      front shell: screen window, speaker grille, touch spot
//    part = "plate"      battery plate: keeps the ESP32 in place, separates the battery
//    part = "lid"        back lid with the kickstand hinge
//    part = "kickstand"  fold-out stand (hinge pin: 1.75 mm filament, 42 mm long)
//  Views:
//    part = "assembly"   everything closed, with dummy electronics
//    part = "standing"   kickstand open
//    part = "exploded"   exploded view
//    part = "check_*"    collision checks (must render EMPTY)
//
//  Axes: X = width, Y = height (top is +Y), Z = depth from the front face (0)
//  to the back. Layers from front to back:
//    front wall | L1: OLED, buzzer, touch | L2: ESP32 | plate | L3: battery, charger | lid
// ===================================================================

part = "assembly";
open_angle = 66;          // kickstand opening (the stop is designed for ~66 deg)

$fn = 48;

// ---------------- electronics (change these if yours differ) ----------------
esp_l = 51.5;   esp_w = 28.4;   esp_pcb = 1.6;   // ESP32 DevKit V1, 30 pins
esp_top = 3.6;      // tallest part on the top side (module, USB, buttons)
esp_under = 6.0;    // header plastic + trimmed pins + soldered wires
esp_row_l = 38.1;   // header row length (15 pins)
esp_row_y = 12.7;   // header row offset from the board centre line

oled_w = 27.3;  oled_h = 27.8;  oled_t = 4.2;     // 0.96" SSD1306 module (glass + PCB)
oled_win_w = 25;  oled_win_h = 15;  oled_win_dy = -1.0;   // window over the active area

buz_d = 12.0;  buz_h = 6.0;                       // KC1206 passive buzzer
ttp_w = 11.0;  ttp_h = 15.0;  ttp_t = 3.0;        // TTP223 touch module

bat_w = 30;  bat_l = 35;  bat_t = 5.0;            // 503035 LiPo, 500 mAh
chg_w = 17.5;  chg_l = 28.0;  chg_t = 5.0;        // TP4056 USB-C charger
sw_l = 8.6;  sw_w = 3.6;  sw_h = 3.5;             // SS12D00 slide switch

tol = 0.3;     // clearance around parts

// ---------------- case ----------------
wall = 1.8;  front_t = 1.6;  lid_t = 3.6;
in_w = 56;   in_h = 42;
l1 = 7.0;    l3 = 6.0;  plate_t = 1.2;
r_xy = 5;    r_e = 1.5;            // corner radius / edge rounding
r_in = 1.0;                         // inner corner radius (square boards fit the corners)

out_w = in_w + 2 * wall;            // 59.6
out_h = in_h + 2 * wall;            // 45.6

z_pcb_front = front_t + l1 + esp_top;   // ESP32 PCB front face (components face the screen)
z_pcb_back  = z_pcb_front + esp_pcb;
z_plate     = z_pcb_back + esp_under;   // plate front face
z_l3        = z_plate + plate_t;
z_lid       = z_l3 + l3;
depth       = z_lid + lid_t;            // 30.6

// ESP32 position: USB end at the left wall, antenna end to the right
esp_x0 = -in_w / 2 + 0.8;
esp_x1 = esp_x0 + esp_l;
esp_cx = (esp_x0 + esp_x1) / 2;

// part positions
buz_x = -in_w / 2 + buz_d / 2 + tol + 0.1;
ttp_x = 21.5;
chg_x0 = -in_w / 2;  chg_x1 = chg_x0 + chg_w + 2 * tol;   chg_cx = (chg_x0 + chg_x1) / 2;
chg_y1 = in_h / 2;   chg_y0 = chg_y1 - chg_l - 2 * tol;
bat_x0 = chg_x1 + 0.4;  bat_x1 = bat_x0 + bat_w + 2 * tol;
bat_y0 = -in_h / 2 + 1.4;   // battery sits low to leave room for the top screw
sw_y = -12.3;  sw_z = z_l3 + l3 / 2;   // clear of the screw boss below it

boss_r = 2.4;
// three corners + top middle (the top-left corner is taken by the charger)
boss_pos = [[-(in_w / 2 - 2.4), -(in_h / 2 - 1.9)], [in_w / 2 - 2.4, -(in_h / 2 - 1.9)],
            [in_w / 2 - 2.4, in_h / 2 - 1.9], [5, in_h / 2 - 2.4]];

// ESP32 corner supports (front posts at the antenna end, plate pegs at all corners)
post = 2.5;
post_pos_front = [[esp_x1 - 1.5, esp_w / 2 - 1.5], [esp_x1 - 1.5, -(esp_w / 2 - 1.5)]];
peg_pos = [for (x = [esp_x0 + 1.5, esp_x1 - 1.5], y = [-1, 1]) [x, y * (esp_w / 2 - 1.5)]];

// kickstand
leg_w = 30;  leg_L = 34;  leg_t = 2.4;
knuckle_r = 2.0;  pin_d = 1.9;
axis_y = out_h / 2 - 5.6;          // hinge axis height
axis_dz = 0.8;                      // axis below the back surface
recess_d = leg_t + 0.2;
groove_d = 3.1;
// Stop: a tab above the hinge, in line with the leg. Its inner corner swings into
// the groove floor at open_angle and would go 0.2 mm deeper by 90 deg, so it
// still stops reliably with normal print tolerances.
stop_r = (groove_d - axis_dz) / sin(open_angle);
tail = stop_r;
tail_drop = 0;

// ===================================================================
//  helpers
// ===================================================================
module torus(R, r) rotate_extrude() translate([R, 0]) circle(r, $fn = 16);

module rbox(w, h, d) {
    hull() for (sx = [-1, 1], sy = [-1, 1])
        translate([sx * (w / 2 - r_xy), sy * (h / 2 - r_xy), 0]) {
            translate([0, 0, r_e]) torus(r_xy - r_e, r_e);
            translate([0, 0, d - r_e]) torus(r_xy - r_e, r_e);
        }
}

module rrect(w, h, r) offset(r) square([w - 2 * r, h - 2 * r], center = true);

module cavity(z0, z1) translate([0, 0, z0]) linear_extrude(z1 - z0) rrect(in_w, in_h, r_in);

// rectangular frame of ribs around a pocket
module frame(w, h, rib, height) linear_extrude(height) difference() {
    square([w + 2 * rib, h + 2 * rib], center = true);
    square([w, h], center = true);
}

// ===================================================================
//  SHELL
// ===================================================================
module shell() {
    difference() {
        union() {
            difference() {
                intersection() {
                    rbox(out_w, out_h, depth);
                    translate([-50, -50, 0]) cube([100, 100, z_lid]);
                }
                cavity(front_t, z_lid + 1);
            }
            intersection() {
                cavity(front_t - 0.01, z_lid);
                shell_inside();
            }
        }
        shell_cuts();
    }
}

module shell_inside() {
    // screw bosses
    for (p = boss_pos) translate([p[0], p[1], 0]) difference() {
        cylinder(r = boss_r, h = z_lid);
        translate([0, 0, z_lid - 10]) cylinder(d = 1.7, h = 11);
    }

    // OLED frame (gap in the top rib for the pin wires)
    translate([0, 0, front_t]) difference() {
        frame(oled_w + 2 * tol, oled_h + 2 * tol, 1.0, 3.0);
        translate([-7, oled_h / 2 - 1, -1]) cube([14, 4, 5]);
    }

    // buzzer ring
    translate([buz_x, 0, front_t]) difference() {
        cylinder(r = buz_d / 2 + tol + 1.0, h = 3.0);
        translate([0, 0, -1]) cylinder(r = buz_d / 2 + tol, h = 5);
    }

    // TTP223 frame
    translate([ttp_x, 0, front_t]) frame(ttp_w + 2 * tol, ttp_h + 2 * tol, 1.0, 2.5);

    // ESP32 supports on the antenna end (touch the PCB only at its free corners)
    for (p = post_pos_front)
        translate([p[0] - post / 2, p[1] - post / 2, 0]) cube([post, post, z_pcb_front]);

    // USB-end ledges (thin strip along the board edge, beside the USB socket)
    for (s = [-1, 1])
        translate([-in_w / 2 - 1, s > 0 ? 5.5 : -(esp_w / 2 - 0.3), 0])
            cube([esp_x0 + 0.8 + in_w / 2 + 1, esp_w / 2 - 0.3 - 5.5, z_pcb_front]);
}

module shell_cuts() {
    // OLED window, chamfered outwards
    translate([0, oled_win_dy, 0]) hull() {
        translate([0, 0, front_t / 2 + 0.01]) linear_extrude(front_t) square([oled_win_w, oled_win_h], center = true);
        translate([0, 0, -0.01]) linear_extrude(0.01) square([oled_win_w + 2 * front_t, oled_win_h + 2 * front_t], center = true);
    }

    // speaker grille
    for (a = [0 : 60 : 300]) translate([buz_x + 3 * cos(a), 3 * sin(a), -1]) cylinder(d = 1.6, h = front_t + 2, $fn = 16);
    translate([buz_x, 0, -1]) cylinder(d = 1.6, h = front_t + 2, $fn = 16);

    // touch spot: thinner wall behind it + engraved ring outside
    translate([ttp_x - (ttp_w + 2 * tol) / 2, -(ttp_h + 2 * tol) / 2, front_t - 0.7])
        cube([ttp_w + 2 * tol, ttp_h + 2 * tol, 1]);
    translate([ttp_x, 0, -0.01]) difference() {
        cylinder(r = 4.6, h = 0.41);
        translate([0, 0, -1]) cylinder(r = 4.0, h = 3);
    }
    translate([ttp_x, 0, -0.01]) cylinder(r = 1.0, h = 0.41, $fn = 16);

    // name under the screen
    translate([0, -18.4, -0.01]) linear_extrude(0.5) mirror([1, 0, 0])
        text("NANO", size = 3.2, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");

    // micro-USB (ESP32, for uploading) on the left wall
    translate([-out_w / 2 - 1, 0, z_pcb_front - 1.3]) rotate([0, 90, 0])
        linear_extrude(wall + 2) rrect(7.5, 11, 1.5);

    // slide switch on the left wall (battery layer)
    translate([-out_w / 2 - 1, sw_y, sw_z]) rotate([0, 90, 0])
        linear_extrude(wall + 2) rrect(2.4, 5.6, 0.6);

    // USB-C (charging) on the top wall
    translate([chg_cx, out_h / 2 + 1, z_l3 + 3]) rotate([90, 0, 0])
        linear_extrude(wall + 2) rrect(10, 4.8, 1.6);
}

// ===================================================================
//  BATTERY PLATE (pegs press the ESP32 corners towards the front)
// ===================================================================
module plate() {
    difference() {
        translate([0, 0, z_plate]) linear_extrude(plate_t) rrect(in_w - 0.8, in_h - 0.8, r_in);
        for (p = boss_pos) translate([p[0], p[1], z_plate - 1]) cylinder(r = boss_r + 0.4, h = plate_t + 2);
        // wire passage (battery/charger to ESP32 VIN and GND)
        translate([-in_w / 2 - 1, -5, z_plate - 1]) cube([7, 10, plate_t + 2]);
    }
    for (p = peg_pos)
        translate([p[0] - post / 2, p[1] - post / 2, z_pcb_back + 0.1]) cube([post, post, z_plate - z_pcb_back - 0.1 + 0.01]);
}

// ===================================================================
//  LID with kickstand recess and hinge
// ===================================================================
module lid() {
    difference() {
        union() {
            intersection() {
                rbox(out_w, out_h, depth);
                translate([-50, -50, z_lid]) cube([100, 100, depth]);
            }
            // hinge knuckles beside the recess
            for (s = [-1, 1])
                translate([s > 0 ? leg_w / 2 + tol : -(leg_w / 2 + tol) - 5.7, axis_y, depth - axis_dz])
                    rotate([0, 90, 0]) cylinder(r = knuckle_r, h = 5.7);
        }
        // recess for the kickstand
        translate([-(leg_w / 2 + tol), axis_y - leg_L - 0.5, depth - recess_d]) cube([leg_w + 2 * tol, leg_L + 0.5, 5]);
        // deeper groove where the hinge turns
        translate([-(leg_w / 2 + tol), axis_y - 2.6, depth - groove_d]) cube([leg_w + 2 * tol, 2.6 + 2.9, 5]);
        // finger scoop to open it
        translate([0, axis_y - leg_L - 0.5, depth + 6 - 2.2]) rotate([0, 90, 0]) cylinder(r = 6, h = 16, center = true);
        // hinge pin hole
        translate([0, axis_y, depth - axis_dz]) rotate([0, 90, 0]) cylinder(d = pin_d, h = leg_w + 16, center = true, $fn = 20);
        // screws (M2), counterbored
        for (p = boss_pos) translate([p[0], p[1], z_lid - 1]) {
            cylinder(d = 2.3, h = lid_t + 2, $fn = 20);
            translate([0, 0, lid_t + 1 - 1.6]) cylinder(d = 4.2, h = 3, $fn = 24);
        }
        // charge LED peek hole
        translate([chg_cx, chg_y0 + 3, z_lid - 1]) cylinder(d = 2.0, h = lid_t + 2, $fn = 16);
    }
}

// ===================================================================
//  KICKSTAND (local frame: hinge axis on the X axis, leg hangs down -Y)
// ===================================================================
module leg_local() {
    z_in = -(leg_t - axis_dz);   // inner face (towards the lid)
    z_out = axis_dz;             // outer face
    difference() {
        union() {
            translate([-leg_w / 2, -leg_L, z_in]) cube([leg_w, leg_L, leg_t]);
            translate([-leg_w / 2, -0.01, -tail_drop]) cube([leg_w, tail + 0.01, tail_drop + z_out]);   // stop tab
            intersection() {
                rotate([0, 90, 0]) cylinder(r = knuckle_r, h = leg_w, center = true);
                translate([-leg_w / 2, -5, z_in]) cube([leg_w, 10, 10]);
            }
        }
        rotate([0, 90, 0]) cylinder(d = pin_d, h = leg_w + 2, center = true, $fn = 20);
        // round the foot corners
        for (s = [-1, 1]) translate([s * leg_w / 2, -leg_L, z_in - 1])
            difference() {
                translate([-2, -2, 0]) cube([4, 4, leg_t + 2]);
                translate([-s * 2, 2, -1]) cylinder(r = 2, h = leg_t + 4);
            }
    }
}

module kickstand(angle = 0) translate([0, axis_y, depth - axis_dz]) rotate([-angle, 0, 0]) leg_local();

// ===================================================================
//  dummy electronics (for the assembly view and collision checks)
// ===================================================================
module esp32() {
    color("teal") {
        translate([esp_x0, -esp_w / 2, z_pcb_front]) cube([esp_l, esp_w, esp_pcb]);
        // top-side parts, leaving the corners and USB-end edge free (where the supports touch)
        difference() {
            translate([esp_x0, -esp_w / 2, z_pcb_front - esp_top]) cube([esp_l, esp_w, esp_top]);
            for (p = post_pos_front) translate([p[0] - 2, p[1] - 2, z_pcb_front - esp_top - 1]) cube([4, 4, esp_top + 2]);
            translate([esp_x0 - 1, -esp_w / 2 - 1, z_pcb_front - esp_top - 1]) cube([2, esp_w + 2, esp_top + 2]);
        }
        // header rows + trimmed pins + wires
        for (s = [-1, 1]) translate([esp_cx - esp_row_l / 2, s * esp_row_y - 1.5, z_pcb_back]) cube([esp_row_l, 3, esp_under - 0.2]);
    }
}

module electronics() {
    esp32();
    color("dimgray") translate([-oled_w / 2, -oled_h / 2, front_t]) cube([oled_w, oled_h, oled_t]);
    color("black") translate([buz_x, 0, front_t]) cylinder(d = buz_d, h = buz_h + 1.0);
    color("royalblue") translate([ttp_x - ttp_w / 2, -ttp_h / 2, front_t - 0.7]) cube([ttp_w, ttp_h, ttp_t]);
    color("silver") translate([bat_x0 + tol, bat_y0 + tol, z_l3]) cube([bat_w, bat_l, bat_t]);
    color("darkblue") translate([chg_x0 + tol, chg_y0 + tol, z_l3]) cube([chg_w, chg_l, chg_t]);
    color("red") translate([-in_w / 2 + 0.1, sw_y - sw_l / 2, sw_z - sw_h / 2]) cube([sw_w, sw_l, sw_h]);
}

// ===================================================================
//  output
// ===================================================================
module case_closed(angle = 0) {
    color("whitesmoke") shell();
    color("orange") plate();
    color("lightgray") lid();
    color("gold") kickstand(angle);
}

if (part == "shell") shell();
else if (part == "plate") translate([0, 0, z_plate + plate_t]) rotate([180, 0, 0]) plate();   // pegs up
else if (part == "lid") translate([0, 0, -z_lid]) lid();                                         // back face up
else if (part == "kickstand") translate([0, 0, leg_t - axis_dz]) leg_local();                     // flat
else if (part == "assembly") { case_closed(0); electronics(); }
else if (part == "standing") case_closed(open_angle);
else if (part == "scene_standing" || part == "scene_upright") {
    // on a table: leaning back on the kickstand (40 deg) or standing upright
    tilt = part == "scene_standing" ? 40 : 0;
    color("tan") translate([-60, -70, -2]) cube([120, 120, 2]);
    translate([0, -depth, 0]) rotate([tilt, 0, 0]) translate([0, depth, 0])
        rotate([90, 0, 0]) translate([0, out_h / 2, 0]) {
            case_closed(tilt > 0 ? open_angle : 0);
            color("black") translate([0, oled_win_dy, front_t - 0.2]) cube([oled_win_w, oled_win_h, 0.2], center = true);
        }
}
else if (part == "exploded") {
    color("whitesmoke") shell();
    translate([0, 0, 25]) electronics();
    translate([0, 0, 50]) color("orange") plate();
    translate([0, 0, 70]) color("lightgray") lid();
    translate([0, 0, 85]) color("gold") kickstand(0);
}
// collision checks: each must render as an EMPTY object
else if (part == "check_shell") intersection() { shell(); electronics(); }
else if (part == "check_plate") intersection() { plate(); union() { electronics(); shell(); } }
else if (part == "check_lid") intersection() { lid(); union() { electronics(); plate(); shell(); } }
else if (part == "check_leg_closed") intersection() { kickstand(0); union() { lid(); shell(); } }
else if (part == "check_leg_open") intersection() { kickstand(open_angle - 2); union() { lid(); shell(); } }
else if (part == "check_leg_stop") intersection() { kickstand(open_angle + 3); lid(); }   // must NOT be empty
