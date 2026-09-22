// ============================================================
//  LCD Touch Screen Case
//  Fits: 8" LCD panel with Touch Panel
//
//  Dimensions sourced from manufacturer drawing:
//    正面图 (Front), 侧面图 (Side), 背面图 (Back)
//
//  Print front_bezel() and back_cover() separately.
//  Use M2x6 screws through back cover into front bezel posts.
// ============================================================

// ---- Screen Dimensions (from drawing) ----------------------
pcb_w       = 104.00;   // PCB width
pcb_h       = 165.00;   // PCB height
pcb_t       =   1.60;   // PCB thickness

tp_w        = 100.30;   // Touch Panel width
tp_h        = 165.00;   // Touch Panel height
tp_t        =   1.60;   // Touch Panel glass thickness

lcd_aa_w    =  85.92;   // LCD Active Area width
lcd_aa_h    = 154.21;   // LCD Active Area height
lcd_t       =   3.50;   // LCD module thickness

glass_total =   6.70;   // Total display stack (TP + LCD glass, front to back)
// (+ 0.3mm tolerance per drawing)

// LCD AA offset from PCB edges (from front view)
//  - Width:  PCB 104, TP 100.30 → TP offset = 1.85 each side
//            TP 100.30, LCD AA 85.92 → centered on TP → offset = 7.19
//            Total from PCB left = 1.85 + 7.19 = 9.04 mm
//  - Height: 3.80mm from PCB top (per drawing dimension)
lcd_aa_x_off = 9.04;    // from PCB left edge
lcd_aa_y_off = 3.80;    // from PCB top edge

// Mounting holes – 4 × Φ2.10 (from back view)
hole_d       =  2.10;
// Hole positions (from back view): 35.45 from right, 52.00 span, 5.37/14.30 from top
hole_right   = pcb_w - 35.45;   // = 68.55 from left
hole_left    = hole_right - 52.00; // = 16.55 from left
hole_top     =  5.37;
hole_bot     = pcb_h - 14.30;   // ≈ 150.70 from top

holes = [
    [hole_left,  hole_top],
    [hole_right, hole_top],
    [hole_left,  hole_bot],
    [hole_right, hole_bot]
];

// FPC / flex connector at bottom center of PCB (back view shows connector)
fpc_w   = 20.0;
fpc_h   =  8.0;   // slot height through wall
fpc_x   = (pcb_w - fpc_w) / 2;   // centered on PCB

// ---- Case Design Parameters --------------------------------
wall        =  3.0;    // shell wall thickness
tolerance   =  0.4;    // per-side clearance around PCB
bezel_lip   =  2.5;    // front bezel lip overlapping TP glass edge
corner_r    =  3.0;    // exterior corner radius
screw_d     =  2.2;    // M2 clearance hole diameter
boss_d      =  5.5;    // boss OD for M2 screw
boss_h      =  4.0;    // boss height inside back cover
back_extra  =  4.0;    // interior depth behind PCB for components/cables

// ---- Derived -----------------------------------------------
inner_w  = pcb_w + 2*tolerance;
inner_h  = pcb_h + 2*tolerance;
outer_w  = inner_w + 2*wall;
outer_h  = inner_h + 2*wall;

// Front bezel total depth: enough for full display stack + lip
front_depth = glass_total + bezel_lip + 1.0;

// Back cover depth: PCB + rear component space + wall
back_depth  = pcb_t + back_extra + wall;

// Display window — show full LCD AA, with thin bezel around it
// Window origin in outer_w/outer_h coordinate space:
win_x = wall + tolerance + lcd_aa_x_off;
win_y = wall + tolerance + lcd_aa_y_off;
win_w = lcd_aa_w;
win_h = lcd_aa_h;

// ============================================================
//  Helper: rounded rectangular prism
// ============================================================
module rbox(w, h, d, r=corner_r) {
    hull() {
        for (xi = [r, w-r], yi = [r, h-r])
            translate([xi, yi, 0])
                cylinder(r=r, h=d, $fn=40);
    }
}

// ============================================================
//  FRONT BEZEL
//  - Printed face-down (display window facing build plate)
//  - Pocket holds PCB + display stack from behind
//  - Lip retains TP glass on front face
// ============================================================
module front_bezel() {
    difference() {
        // ── Outer shell ──────────────────────────────────────
        rbox(outer_w, outer_h, front_depth);

        // ── Main PCB/stack pocket (enters from the BACK) ─────
        translate([wall, wall, bezel_lip + 1.0])
            cube([inner_w, inner_h, front_depth]);

        // ── Display window (through front face) ──────────────
        translate([win_x, win_y, -0.1])
            cube([win_w, win_h, bezel_lip + 1.2]);

        // ── Round off inside corners of window ───────────────
        // (subtractive hull removed — square window is fine for FFF)

        // ── FPC cable exit slot — bottom edge ─────────────────
        translate([wall + tolerance + fpc_x, -0.1, bezel_lip + 1.0 + glass_total - fpc_h/2])
            cube([fpc_w, wall + 0.2, fpc_h]);

        // ── M2 screw holes for assembly ───────────────────────
        // Four holes through the rear face of the bezel, into bosses
        for (h = holes) {
            translate([wall + tolerance + h[0], wall + tolerance + h[1],
                       front_depth - 4.0])
                cylinder(d=screw_d, h=4.1, $fn=20);
        }
    }

    // ── M2 threaded boss stubs inside bezel pocket ───────────
    // Give screws from back cover something to bite into
    for (h = holes) {
        translate([wall + tolerance + h[0], wall + tolerance + h[1],
                   bezel_lip + 1.0 + glass_total + pcb_t])
            difference() {
                cylinder(d=boss_d, h=back_extra - 0.5, $fn=24);
                translate([0,0,-0.1])
                    cylinder(d=hole_d + 0.3, h=back_extra, $fn=20); // PCB hole clearance
                // M2 pilot hole (top 3mm)
                translate([0,0, back_extra - 3.5])
                    cylinder(d=1.8, h=3.6, $fn=16); // tight M2 self-tap
            }
    }
}

// ============================================================
//  BACK COVER
//  - Slides onto rear of front bezel
//  - Four M2 screws through bosses into front bezel
//  - Standoffs align to PCB mounting holes (pass-through or tapped)
//  - Ventilation slots optional — comment out if not needed
// ============================================================
module back_cover() {
    difference() {
        // ── Outer shell ──────────────────────────────────────
        rbox(outer_w, outer_h, back_depth);

        // ── Interior hollow ───────────────────────────────────
        translate([wall, wall, wall])
            cube([inner_w, inner_h, back_depth]);

        // ── M2 screw holes (counter-sunk from outside) ────────
        for (h = holes) {
            tx = wall + tolerance + h[0];
            ty = wall + tolerance + h[1];
            // Clearance shaft
            translate([tx, ty, -0.1])
                cylinder(d=screw_d, h=back_depth + 0.2, $fn=20);
            // M2 pan-head counter-sink (Ø4.5 × 1.5mm)
            translate([tx, ty, -0.1])
                cylinder(d=4.5, h=1.6, $fn=20);
        }

        // ── FPC cable exit slot — bottom wall ─────────────────
        translate([wall + tolerance + fpc_x, -0.1, wall + back_extra - fpc_h])
            cube([fpc_w, wall + 0.2, fpc_h + 0.2]);

        // ── Ventilation slots (rear face) ─────────────────────
        slot_w  = 8.0;
        slot_h  = 30.0;
        n_slots = 3;
        total_slot_span = n_slots*slot_w + (n_slots-1)*4.0;
        slot_start_x = (outer_w - total_slot_span) / 2;
        for (i=[0:n_slots-1]) {
            translate([slot_start_x + i*(slot_w+4), outer_h*0.35, -0.1])
                cube([slot_w, slot_h, wall + 0.2]);
        }
    }

    // ── PCB standoffs (align to PCB mounting holes) ───────────
    // Height: lifts PCB so display stack flush with bezel front
    standoff_h = back_extra - 0.5;
    for (h = holes) {
        translate([wall + tolerance + h[0], wall + tolerance + h[1], wall])
            difference() {
                cylinder(d=boss_d, h=standoff_h, $fn=24);
                // Pass-through for M2 screw
                translate([0,0,-0.1])
                    cylinder(d=screw_d, h=standoff_h + 0.2, $fn=20);
            }
    }

    // ── Interior ribs (stiffen back cover) ────────────────────
    rib_t = 1.5;
    // Horizontal rib
    translate([wall, outer_h/2 - rib_t/2, wall])
        cube([inner_w, rib_t, back_extra * 0.4]);
}

// ============================================================
//  OUTPUT — choose which part to export
//
//  To print the front bezel:
//      Comment out back_cover(), render front_bezel()
//  To print the back cover:
//      Comment out front_bezel(), render back_cover()
//
//  Assembled preview: both shown below (bezel up, cover down).
// ============================================================

// Front bezel (print face-down, no supports needed)
front_bezel();

// Back cover — shown separated below for preview
translate([0, 0, -(back_depth + 5)])
    back_cover();
