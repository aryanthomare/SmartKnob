FeatureScript 3083;
import(path : "onshape/std/common.fs", version : "3083.0");

// ===== Settings (edit here; the part rebuilds when the Feature Studio is saved) =====
// Kept as constants rather than feature-dialog parameters so code updates never invalidate the feature.
const WIRE_GAP = 14;         // mm, PCB edge to inner wall on the J3/J4 (hand) side
const NOSE_HEIGHT = 11;      // mm, height of the front nose above the base plate
const BOTTOM_STANDOFF = 6;   // mm, standoff under the PCB
const TOP_STANDOFF = 10;     // mm, standoff from PCB to the AS5600 board
const ENC_SPACER = 2;        // mm, printed spacer under the AS5600 board (0 = none)
const COVER_CLEARANCE = 3.8; // mm, AS5600 board top to cover underside (screw heads)
const MOTOR_HOLE = 37;       // mm, hole in the cover for the motor
const PEG_D = 2.4;           // mm, knob pegs that press into the motor's M2.5 holes (tune 2.3-2.5 for your printer)
const PEG_LEN = 2.5;         // mm, peg length (rotor holes are 3 mm deep)
const EXPLODE = false;       // lift the cover and knob off for viewing
const SHOW_PCB = true;       // driver PCB model (reference, not for printing)
const SHOW_STACK = true;     // standoffs / AS5600 board / motor (reference)

// SmartKnob driver enclosure (sleek one-piece cover + base plate) + driver PCB model.
// Frame matches the KiCad STEP export (kicad-cli pcb export step --user-origin 127x127mm):
// origin = PCB centre = motor axis, Z = 0 at the PCB bottom face, +Y = J3/J4 (JST) edge, -Y = barrel jack / USB-C edge.
// Component envelopes are the bounding boxes of the real KiCad 3D models (exported per component),
// except U3/U4/J2/SW1/SW2 which have no model and use their footprint F.Fab outlines.

// [ref, x0, y0, z0, x1, y1, z1, kind]  kind: 0 resistor, 1 capacitor, 2 IC/diode, 3 inductor, 4 LED, 5 fuse
const COMPONENTS = [
        ["C2", -16, -3.7, 1.6, -12.8, -2.1, 3.2, 1],
        ["C3", -15.4, -0.9, 1.6, -13.8, -0.1, 2.4, 1],
        ["C4", -14.5, 4.15, 1.6, -13.5, 4.65, 2.1, 1],
        ["C5", -21.82, 5.6, 1.6, -20.57, 7.6, 2.85, 1],
        ["C6", -19.82, 5.6, 1.6, -18.57, 7.6, 2.85, 1],
        ["C7", -10.7, 2.85, 1.6, -9.7, 3.35, 2.1, 1],
        ["C8", -19, 11.15, 1.6, -18, 11.65, 2.1, 1],
        ["C9", -19.3, 16.7, 1.6, -17.7, 17.5, 2.4, 1],
        ["C10", 10.45, 17.2, 1.6, 10.95, 18.2, 2.1, 1],
        ["C20", 2.5, 13.3, 1.6, 3.3, 14.9, 2.4, 1],
        ["C21", 4.1, 13.3, 1.6, 4.9, 14.9, 2.4, 1],
        ["C22", 5.7, 13.3, 1.6, 6.5, 14.9, 2.4, 1],
        ["C23", 2.9, 16.4, 1.6, 6.1, 18, 3.2, 1],
        ["C24", 0.9, 12.2, 1.6, 1.7, 13.8, 2.4, 1],
        ["C25", 2.6, 10.2, 1.6, 3.4, 11.8, 2.4, 1],
        ["C26", 4.17, 9, 1.6, 5.42, 11, 2.85, 1],
        ["C27", -7.9, 10.7, 1.6, -7.1, 12.3, 2.4, 1],
        ["C28", -0.75, 4, 1.6, -0.25, 5, 2.1, 1],
        ["C40", 21.15, -9.9, 1.6, 21.65, -8.9, 2.1, 1],
        ["C41", 18.5, 8.7, 1.6, 20.1, 9.5, 2.4, 1],
        ["C42", 18.8, 10.35, 1.6, 19.8, 10.85, 2.1, 1],
        ["C43", 3.6, 7.25, 1.6, 4.6, 7.75, 2.1, 1],
        ["C44", 3.6, 5.25, 1.6, 4.6, 5.75, 2.1, 1],
        ["C45", 3.6, 4.2, 1.6, 4.6, 4.7, 2.1, 1],
        ["D1", -7.8, -16.1, 1.6, -6.2, -12.3, 2.86, 2],
        ["D2", -3.5, -10.35, 1.6, 1.5, -7.65, 3.82, 2],
        ["D3", -24.9, 12.2, 1.6, -21.1, 13.8, 2.86, 2],
        ["D4", -24.9, 15.2, 1.6, -21.1, 16.8, 2.86, 2],
        ["D5", -24.3, 9.4, 1.6, -22.7, 10.2, 2.7, 4],
        ["F1", 20.38, -15.44, 1.6, 21.62, -13.44, 2.05, 5],
        ["L1", -21.9, -0.5, 1.6, -16.9, 4.5, 5.6, 3],
        ["Q1", -7.75, -11, 1.6, -5.25, -8, 2.8, 2],
        ["R1", -9.75, -8.5, 1.6, -9.25, -7.5, 1.95, 0],
        ["R2", -10.7, 1.75, 1.6, -9.7, 2.25, 1.95, 0],
        ["R3", -10.7, 0.65, 1.6, -9.7, 1.15, 1.95, 0],
        ["R4", 8.25, 17.2, 1.6, 8.75, 18.2, 1.95, 0],
        ["R5", 9.35, 17.2, 1.6, 9.85, 18.2, 1.95, 0],
        ["R6", -21, 9.55, 1.6, -20, 10.05, 1.95, 0],
        ["R10", 0.9, 8.3, 1.6, 1.7, 9.9, 2.05, 0],
        ["R11", -7.5, 16.15, 1.6, -6.5, 16.65, 1.95, 0],
        ["R12", 1.4, 3.15, 1.6, 2.4, 3.65, 1.95, 0],
        ["R13", -7.5, 15.05, 1.6, -6.5, 15.55, 1.95, 0],
        ["R14", -7.5, 17.25, 1.6, -6.5, 17.75, 1.95, 0],
        ["R20", 19.95, -9.9, 1.6, 20.45, -8.9, 1.95, 0],
        ["R21", 17.05, 9.1, 1.6, 17.55, 10.1, 1.95, 0],
        ["R22", 1.4, 6.75, 1.6, 2.4, 7.25, 1.95, 0],
        ["R23", 1.4, 5.25, 1.6, 2.4, 5.75, 1.95, 0],
        ["R24", 1.4, 4.2, 1.6, 2.4, 4.7, 1.95, 0],
        ["R30", 11.1, -17.75, 1.6, 12.1, -17.25, 1.95, 0],
        ["R31", 18.9, -17.15, 1.6, 19.9, -16.65, 1.95, 0],
        ["R32", 14, 17.55, 1.6, 15, 18.05, 1.95, 0],
        ["R33", 14, 16.45, 1.6, 15, 16.95, 1.95, 0],
        ["U1", -15.4, 0.55, 1.6, -12.6, 3.45, 3.15, 2],
        ["U2", -19.9, 13.05, 1.6, -17.1, 15.95, 3.15, 2],
        ["U5", 16.15, -14, 1.6, 19.05, -11.2, 3.15, 2],
        ["U3", -5, 7.5, 1.6, 0, 14.5, 2.6, 2]];

function mmv(x, y, z) { return vector(x, y, z) * millimeter; }

// Box with rounded vertical (Z) edges
function rboxZ(context is Context, id is Id, x0, y0, x1, y1, z0, z1, r) returns Query
{
    fCuboid(context, id + "a", { "corner1" : mmv(x0 + r, y0, z0), "corner2" : mmv(x1 - r, y1, z1) });
    fCuboid(context, id + "b", { "corner1" : mmv(x0, y0 + r, z0), "corner2" : mmv(x1, y1 - r, z1) });
    const cs = [[x0 + r, y0 + r], [x1 - r, y0 + r], [x0 + r, y1 - r], [x1 - r, y1 - r]];
    for (var i = 0; i < 4; i += 1)
        fCylinder(context, id + ("k" ~ i), { "bottomCenter" : mmv(cs[i][0], cs[i][1], z0), "topCenter" : mmv(cs[i][0], cs[i][1], z1), "radius" : r * millimeter });
    opBoolean(context, id + "u", { "tools" : qCreatedBy(id, EntityType.BODY), "operationType" : BooleanOperationType.UNION });
    return qCreatedBy(id, EntityType.BODY);
}

// Rounded rectangle in XZ, extruded along Y (wall openings, USB-C shell)
function rslotY(context is Context, id is Id, x0, z0, x1, z1, y0, y1, r) returns Query
{
    fCuboid(context, id + "a", { "corner1" : mmv(x0 + r, y0, z0), "corner2" : mmv(x1 - r, y1, z1) });
    fCuboid(context, id + "b", { "corner1" : mmv(x0, y0, z0 + r), "corner2" : mmv(x1, y1, z1 - r) });
    const cs = [[x0 + r, z0 + r], [x1 - r, z0 + r], [x0 + r, z1 - r], [x1 - r, z1 - r]];
    for (var i = 0; i < 4; i += 1)
        fCylinder(context, id + ("k" ~ i), { "bottomCenter" : mmv(cs[i][0], y0, cs[i][1]), "topCenter" : mmv(cs[i][0], y1, cs[i][1]), "radius" : r * millimeter });
    opBoolean(context, id + "u", { "tools" : qCreatedBy(id, EntityType.BODY), "operationType" : BooleanOperationType.UNION });
    return qCreatedBy(id, EntityType.BODY);
}

function cylZ(context is Context, id is Id, x, y, z0, z1, d) returns Query
{
    fCylinder(context, id, { "bottomCenter" : mmv(x, y, z0), "topCenter" : mmv(x, y, z1), "radius" : d / 2 * millimeter });
    return qCreatedBy(id, EntityType.BODY);
}

function cylY(context is Context, id is Id, x, z, y0, y1, d) returns Query
{
    fCylinder(context, id, { "bottomCenter" : mmv(x, y0, z), "topCenter" : mmv(x, y1, z), "radius" : d / 2 * millimeter });
    return qCreatedBy(id, EntityType.BODY);
}

function block(context is Context, id is Id, x0, y0, z0, x1, y1, z1) returns Query
{
    fCuboid(context, id, { "corner1" : mmv(x0, y0, z0), "corner2" : mmv(x1, y1, z1) });
    return qCreatedBy(id, EntityType.BODY);
}

function cut(context is Context, id is Id, target is Query, tools is Query)
{
    opBoolean(context, id, { "targets" : target, "tools" : tools, "operationType" : BooleanOperationType.SUBTRACTION });
}

function nameAndColor(context is Context, q is Query, name is string, c)
{
    setProperty(context, { "entities" : q, "propertyType" : PropertyType.NAME, "value" : name });
    if (c != undefined)
        setProperty(context, { "entities" : q, "propertyType" : PropertyType.APPEARANCE, "value" : c });
}

// Driver PCB with all components. Returns the barrel jack body (for the clearance check).
function buildPcb(context is Context, id is Id) returns Query
{
    const t = 1.6;
    const board = rboxZ(context, id + "board", -27, -27, 27, 27, 0, t, 1.5);
    var holes = [];
    var n = 0;
    for (var sx in [-12, 12])
        for (var sy in [-12, 12])
        {
            holes = append(holes, cylZ(context, id + ("hole" ~ n), sx, sy, -1, t + 1, 3.2));
            n += 1;
        }
    cut(context, id + "boardHoles", board, qUnion(holes));
    nameAndColor(context, board, "PCB board", color(0.05, 0.35, 0.15));

    const colors = [color(0.12, 0.12, 0.12), color(0.72, 0.58, 0.4), color(0.08, 0.08, 0.08), color(0.2, 0.2, 0.22), color(0.95, 0.95, 0.9), color(0.6, 0.6, 0.5)];
    for (var i = 0; i < size(COMPONENTS); i += 1)
    {
        const c = COMPONENTS[i];
        nameAndColor(context, block(context, id + ("cmp" ~ i), c[1], c[2], c[3], c[4], c[5], c[6]), "PCB " ~ c[0], colors[c[7]]);
    }

    const black = color(0.1, 0.1, 0.1);
    const cream = color(0.95, 0.92, 0.82);
    const metal = color(0.78, 0.78, 0.8);

    // J1 barrel jack DC-005C-20A: model envelope (-5.15..4.15, -27.09..-12.89, top 12.3), 6.3 mm bore, 2 mm pin
    const j1 = block(context, id + "j1", -5.15, -27.09, t, 4.15, -12.89, 12.3);
    cut(context, id + "j1Bore", j1, cylY(context, id + "j1b", -0.65, t + 6.5, -27.5, -18.0, 6.3));
    const j1All = qUnion([j1, cylY(context, id + "j1p", -0.65, t + 6.5, -26.0, -18.0, 2.0)]);
    opBoolean(context, id + "j1PinU", { "tools" : j1All, "operationType" : BooleanOperationType.UNION });
    nameAndColor(context, j1All, "PCB J1 barrel jack", black);

    // J3 / J4 JST PH side-entry headers, opening facing +Y (board edge)
    for (var j in [["J3", 11.55, 21.45], ["J4", -6.45, 1.45]])
    {
        const b = block(context, id + ("hdr" ~ j[0]), j[1], 21.0, t, j[2], 27.0, 6.45);
        cut(context, id + ("hdrCut" ~ j[0]), b, block(context, id + ("hdrPocket" ~ j[0]), j[1] + 0.6, 23.0, t + 1.6, j[2] - 0.6, 27.5, 5.85));
        nameAndColor(context, b, "PCB " ~ j[0] ~ " JST PH", cream);
    }

    // J5 2x5 2.54 mm pin header: housing + pins
    const j5 = block(context, id + "j5h", -25.97, -15.43, t, -20.89, -2.73, t + 2.54);
    var pins = [j5];
    var p = 0;
    for (var px in [-24.7, -22.16])
        for (var k = 0; k < 5; k += 1)
        {
            pins = append(pins, block(context, id + ("j5p" ~ p), px - 0.32, -4 - 2.54 * k - 0.32, -1.4, px + 0.32, -4 - 2.54 * k + 0.32, 10.14));
            p += 1;
        }
    opBoolean(context, id + "j5U", { "tools" : qUnion(pins), "operationType" : BooleanOperationType.UNION });
    nameAndColor(context, qUnion(pins), "PCB J5 pin header", black);

    // C1 6.3 x 7.7 electrolytic: plastic base + can
    const c1 = block(context, id + "c1base", -22.8, 18.7, t, -16.2, 25.3, t + 1.0);
    const c1All = qUnion([c1, cylZ(context, id + "c1can", -19.5, 22, t + 1.0, 9.3, 6.3)]);
    opBoolean(context, id + "c1U", { "tools" : c1All, "operationType" : BooleanOperationType.UNION });
    nameAndColor(context, c1All, "PCB C1 electrolytic", metal);

    // No 3D model in the KiCad library: footprint F.Fab outlines
    nameAndColor(context, block(context, id + "u4", 6.5, -7.7, t, 26.5, 7.7, t + 2.55), "PCB U4 ESP32-S3-MINI-1 (outline)", metal);
    const usb = rslotY(context, id + "j2", 14 - 4.47, t, 14 + 4.47, t + 3.26, -27.0, -19.7, 1.3);
    cut(context, id + "j2Cut", usb, rslotY(context, id + "j2in", 14 - 4.17, t + 0.3, 14 + 4.17, t + 2.96, -27.5, -21.0, 1.0));
    nameAndColor(context, usb, "PCB J2 USB-C (outline)", metal);
    for (var s in [["SW1", -22.0], ["SW2", -13.5]])
    {
        const sw = block(context, id + ("sw" ~ s[0]), s[1] - 2.55, -23.5 - 2.55, t, s[1] + 2.55, -23.5 + 2.55, t + 0.9);
        const swAll = qUnion([sw, cylZ(context, id + ("swa" ~ s[0]), s[1], -23.5, t + 0.9, t + 1.5, 2.6)]);
        opBoolean(context, id + ("swU" ~ s[0]), { "tools" : swAll, "operationType" : BooleanOperationType.UNION });
        nameAndColor(context, swAll, "PCB " ~ s[0] ~ " TS-1187A (outline)", black);
    }
    return j1All;
}

// AS5600 breakout (33 x 33, concave R25.6 sides, R4.5 corners, mounting slots on the 24 mm square)
function buildEncoder(context is Context, id is Id, z0) returns Query
{
    const t = 1.6;
    const plate = block(context, id + "sq", -16.5, -16.5, z0, 16.5, 16.5, z0 + t);
    var tools = [];
    var n = 0;
    for (var c in [[0, 39.6], [0, -39.6], [39.6, 0], [-39.6, 0]])
    {
        tools = append(tools, cylZ(context, id + ("cc" ~ n), c[0], c[1], z0 - 1, z0 + t + 1, 51.2));
        n += 1;
    }
    for (var sx in [-1, 1])
        for (var sy in [-1, 1])
        {
            tools = append(tools, block(context, id + ("cb" ~ n), sx * 12, sy * 12, z0 - 1, sx * 17, sy * 17, z0 + t + 1));
            n += 1;
        }
    cut(context, id + "shape", plate, qUnion(tools));
    var corners = [plate];
    var holes = [];
    n = 0;
    for (var sx in [-12, 12])
        for (var sy in [-12, 12])
        {
            corners = append(corners, cylZ(context, id + ("rc" ~ n), sx, sy, z0, z0 + t, 9.0));
            holes = append(holes, cylZ(context, id + ("mh" ~ n), sx, sy, z0 - 1, z0 + t + 1, 3.1));
            n += 1;
        }
    opBoolean(context, id + "corners", { "tools" : qUnion(corners), "operationType" : BooleanOperationType.UNION });
    const pcb = qUnion(corners);
    cut(context, id + "holes", pcb, qUnion(holes));
    // AS5600 SOIC-8 on top, under the magnet
    const all = qUnion([pcb, block(context, id + "chip", -2.0, -2.5, z0 + t, 2.0, 2.5, z0 + t + 1.5)]);
    opBoolean(context, id + "chipU", { "tools" : all, "operationType" : BooleanOperationType.UNION });
    nameAndColor(context, all, "REF AS5600 board", color(0.1, 0.4, 0.5));
    return all;
}

// Plan-view rounded rectangle (back corners rb at y0, front corners rf at y1), extruded z0..z1
function planShape(context is Context, id is Id, x0, y0, x1, y1, z0, z1, rb, rf) returns Query
{
    fCuboid(context, id + "box", { "corner1" : mmv(x0, y0, z0), "corner2" : mmv(x1, y1, z1) });
    var cuts = [];
    var cyls = [qCreatedBy(id + "box", EntityType.BODY)];
    var i = 0;
    for (var c in [[x0, y0, rb, 1, 1], [x1, y0, rb, -1, 1], [x0, y1, rf, 1, -1], [x1, y1, rf, -1, -1]])
    {
        const cx = c[0] + c[3] * c[2];
        const cy = c[1] + c[4] * c[2];
        cuts = append(cuts, block(context, id + ("cc" ~ i), min(c[0], cx), min(c[1], cy), z0 - 1, max(c[0], cx), max(c[1], cy), z1 + 1));
        cyls = append(cyls, cylZ(context, id + ("cy" ~ i), cx, cy, z0, z1, 2 * c[2]));
        i += 1;
    }
    cut(context, id + "corners", qCreatedBy(id + "box", EntityType.BODY), qUnion(cuts));
    opBoolean(context, id + "round", { "tools" : qUnion(cyls), "operationType" : BooleanOperationType.UNION });
    return qCreatedBy(id, EntityType.BODY);
}

// One-piece cover: side profile (flat deck over the motor, rounded slope down to a low nose on the JST side)
// intersected with the plan shape, top perimeter rounded, then shelled from the open bottom.
function buildCover(context is Context, id is Id, xh, yb, yf, zb, zt, noseZ, slopeY, rb, rf, wall) returns Query
{
    const sk = newSketchOnPlane(context, id + "sk", { "sketchPlane" : plane(vector(0, 0, 0) * meter, vector(1, 0, 0), vector(0, 1, 0)) });
    skPolyline(sk, "p", { "points" : [vector(yb, zb) * millimeter, vector(yf, zb) * millimeter, vector(yf, noseZ) * millimeter,
                vector(slopeY, zt) * millimeter, vector(yb, zt) * millimeter, vector(yb, zb) * millimeter] });
    skSolve(sk);
    opExtrude(context, id + "prof", { "entities" : qSketchRegion(id + "sk"), "direction" : vector(1, 0, 0),
                "endBound" : BoundingType.BLIND, "endDepth" : (xh + 10) * millimeter, "startBound" : BoundingType.BLIND, "startDepth" : (xh + 10) * millimeter });
    opDeleteBodies(context, id + "delSk", { "entities" : qCreatedBy(id + "sk", EntityType.BODY) });
    const prof = qCreatedBy(id + "prof", EntityType.BODY);
    opFillet(context, id + "fNose", { "entities" : qContainsPoint(qOwnedByBody(prof, EntityType.EDGE), mmv(0, yf, noseZ)), "radius" : 8 * millimeter });
    opFillet(context, id + "fSlope", { "entities" : qContainsPoint(qOwnedByBody(prof, EntityType.EDGE), mmv(0, slopeY, zt)), "radius" : 18 * millimeter });
    opFillet(context, id + "fBack", { "entities" : qContainsPoint(qOwnedByBody(prof, EntityType.EDGE), mmv(0, yb, zt)), "radius" : 3 * millimeter });

    const plan = planShape(context, id + "plan", -xh, yb, xh, yf, zb - 1, zt + 1, rb, rf);
    opBoolean(context, id + "isect", { "tools" : qUnion([prof, plan]), "operationType" : BooleanOperationType.INTERSECTION });
    const body = qCreatedBy(id, EntityType.BODY);

    // Round the top perimeter: non-vertical edges lying on the plan's side walls, excluding the open bottom
    const centers = [[-xh + rb, yb + rb, rb], [xh - rb, yb + rb, rb], [-xh + rf, yf - rf, rf], [xh - rf, yf - rf, rf]];
    var sel = [];
    for (var e in evaluateQuery(context, qOwnedByBody(body, EntityType.EDGE)))
    {
        const p = evEdgeTangentLine(context, { "edge" : e, "parameter" : 0.5 }).origin / millimeter;
        var onSide = abs(abs(p[0]) - xh) < 0.01;
        for (var cc in centers)
            if (abs(sqrt((p[0] - cc[0]) ^ 2 + (p[1] - cc[1]) ^ 2) - cc[2]) < 0.01)
                onSide = true;
        if (abs(p[2] - zb) > 0.01 && onSide && size(evaluateQuery(context, qParallelEdges(e, vector(0, 0, 1)))) == 0)
            sel = append(sel, e);
    }
    opFillet(context, id + "fTop", { "entities" : qUnion(sel), "radius" : 4 * millimeter, "tangentPropagation" : true });
    opShell(context, id + "shell", { "entities" : qCoincidesWithPlane(qOwnedByBody(body, EntityType.FACE), plane(mmv(0, 0, zb), vector(0, 0, -1))),
                "thickness" : -wall * millimeter });
    return body;
}

// Knob cap for the rotor end of the 34.5 mm gimbal motor. Four pegs press into the rotor's M2.5 threaded holes
// (16 mm circle, 3 mm deep). z0 = rotor end face. The skirt hangs over the rotor cup (11 mm long) and stops 1 mm short of the stator.
function buildKnob(context is Context, id is Id, z0)
{
    const od = 40.0;
    const top = 4.0;
    const skirt = 10.0;
    const body = cylZ(context, id + "body", 0, 0, z0 - skirt, z0 + top, od);
    cut(context, id + "bore", body, cylZ(context, id + "boreC", 0, 0, z0 - skirt - 1, z0, 34.9));
    opFillet(context, id + "fTop", { "entities" : qContainsPoint(qOwnedByBody(body, EntityType.EDGE), mmv(od / 2, 0, z0 + top)), "radius" : 2.5 * millimeter });
    opFillet(context, id + "fBot", { "entities" : qContainsPoint(qOwnedByBody(body, EntityType.EDGE), mmv(od / 2, 0, z0 - skirt)), "radius" : 0.8 * millimeter });
    var tools = [];
    // grip flutes
    for (var i = 0; i < 40; i += 1)
    {
        const a = i * 9 * degree;
        tools = append(tools, cylZ(context, id + ("fl" ~ i), 20.6 * cos(a), 20.6 * sin(a), z0 - skirt + 2, z0 + top - 3, 2.4));
    }
    // relief over the hollow shaft / bearing at the rotor centre
    tools = append(tools, cylZ(context, id + "rel", 0, 0, z0 - 1, z0 + 1, 12));
    // pointer dimple
    tools = append(tools, cylZ(context, id + "ind", 0, 15.5, z0 + top - 0.6, z0 + top + 1, 2.5));
    cut(context, id + "cuts", body, qUnion(tools));

    // pegs into the M2.5 holes, with a short lead-in cone at the tip
    var parts = [body];
    for (var k = 0; k < 4; k += 1)
    {
        const a = k * 90 * degree;
        const x = 8 * cos(a);
        const y = 8 * sin(a);
        parts = append(parts, cylZ(context, id + ("peg" ~ k), x, y, z0 - PEG_LEN + 0.3, z0 + 0.5, PEG_D));
        fCone(context, id + ("tip" ~ k), { "bottomCenter" : mmv(x, y, z0 - PEG_LEN), "topCenter" : mmv(x, y, z0 - PEG_LEN + 0.3),
                    "bottomRadius" : (PEG_D / 2 - 0.3) * millimeter, "topRadius" : PEG_D / 2 * millimeter });
        parts = append(parts, qCreatedBy(id + ("tip" ~ k), EntityType.BODY));
    }
    opBoolean(context, id + "pegsU", { "tools" : qUnion(parts), "operationType" : BooleanOperationType.UNION });
    nameAndColor(context, qUnion(parts), "Knob", color(0.22, 0.22, 0.25));
}

annotation { "Feature Type Name" : "SmartKnobEnclosure" }
export const smartKnobEnclosure = defineFeature(function(context is Context, id is Id, definition is map)
    precondition {}
    {
        const gap = WIRE_GAP;
        const motorD = MOTOR_HOLE;
        const spacer = ENC_SPACER;

        const half = 27.0;      // PCB is 54 x 54
        const clr = 0.5;        // board-to-wall clearance (non-JST sides)
        const wall = 2.0;
        const baseT = 2.5;
        const pcbT = 1.6;
        const pcbTop = pcbT;
        const fl = -BOTTOM_STANDOFF;            // base top / cover bottom (PCB bottom is Z = 0)
        const soTop = pcbTop + TOP_STANDOFF;    // top of the PCB-to-encoder standoffs
        const encZ0 = soTop + spacer;           // AS5600 board bottom
        const encT = 1.6;
        const topZ = encZ0 + encT + COVER_CLEARANCE + wall; // cover top surface
        const noseZ = fl + NOSE_HEIGHT;

        const xh = half + clr + wall;
        const yb = -half - clr - wall;
        const yf = half + gap + wall;
        const rb = 4.0;         // plan radius, back corners (inner radius must clear the PCB corners)
        const rf = 16.0;        // plan radius, front (hand side) corners
        const slopeY = half;    // deck ends over the board edge, then rounds down towards the nose
        const lipH = 3.0;
        const lipT = 1.5;
        const fit = 0.2;
        const printColor = color(0.22, 0.22, 0.25);

        // ---------- Cover ----------
        const cover = buildCover(context, id + "cover", xh, yb, yf, fl, topZ, noseZ, slopeY, rb, rf, wall);
        var cutters = [cylZ(context, id + "motor", 0, 0, topZ - wall - 2, topZ + 1, motorD)];
        // paper-clip access over SW1 / SW2
        cutters = append(cutters, cylZ(context, id + "sw1", -22.0, -23.5, topZ - wall - 1, topZ + 1, 2.5));
        cutters = append(cutters, cylZ(context, id + "sw2", -13.5, -23.5, topZ - wall - 1, topZ + 1, 2.5));
        // Barrel jack J1 (axis X=-0.65, 6.5 mm above PCB): 11 mm round opening
        cutters = append(cutters, cylY(context, id + "dc", -0.65, pcbTop + 6.5, yb - 1, yb + wall + 1, 11));
        // USB-C J2 (centre X=14, axis 1.63 mm above PCB): 13 x 7.5 opening for plug overmold
        cutters = append(cutters, rslotY(context, id + "usb", 14 - 6.5, pcbTop + 1.63 - 3.75, 14 + 6.5, pcbTop + 1.63 + 3.75, yb - 1, yb + wall + 1, 2.5));
        cut(context, id + "coverCut", cover, qUnion(cutters));
        nameAndColor(context, cover, "Enclosure Cover", printColor);

        // ---------- Base plate with locating lip ----------
        const plate = planShape(context, id + "plate", -xh, yb, xh, yf, fl - baseT, fl, rb, rf);
        opFillet(context, id + "plateFillet", { "entities" : qCoincidesWithPlane(qOwnedByBody(plate, EntityType.EDGE), plane(mmv(0, 0, fl - baseT), vector(0, 0, -1))),
                    "radius" : 1.5 * millimeter });
        const ins = wall + fit;
        const lipO = planShape(context, id + "lipO", -xh + ins, yb + ins, xh - ins, yf - ins, fl - 0.5, fl + lipH, rb - ins, rf - ins);
        const lipI = planShape(context, id + "lipI", -xh + ins + lipT, yb + ins + lipT, xh - ins - lipT, yf - ins - lipT, fl - 1, fl + lipH + 1, rb - ins - lipT, rf - ins - lipT);
        cut(context, id + "lipCut", lipO, lipI);
        const base = qUnion([plate, lipO]);
        opBoolean(context, id + "baseU", { "tools" : base, "operationType" : BooleanOperationType.UNION });
        // M3 standoff holes on the 24 mm square (H1-H4), counterbored from below for screw heads
        var holes = [];
        var n = 0;
        for (var sx in [-12, 12])
            for (var sy in [-12, 12])
            {
                holes = append(holes, cylZ(context, id + ("h" ~ n), sx, sy, fl - baseT - 1, fl + 1, 3.4));
                holes = append(holes, cylZ(context, id + ("cb" ~ n), sx, sy, fl - baseT - 1, fl - baseT + 1.5, 6.5));
                n += 1;
            }
        cut(context, id + "baseHoles", base, qUnion(holes));
        nameAndColor(context, base, "Enclosure Base", printColor);

        if (EXPLODE)
            opTransform(context, id + "explode", { "bodies" : cover, "transform" : transform(mmv(0, 0, 40)) });

        // ---------- Printed spacers: lift the AS5600 board clear of the barrel jack ----------
        if (spacer > 0.01)
        {
            var m = 0;
            for (var sx in [-12, 12])
                for (var sy in [-12, 12])
                {
                    const ring = cylZ(context, id + ("sp" ~ m), sx, sy, soTop, encZ0, 7.0);
                    cut(context, id + ("spHole" ~ m), ring, cylZ(context, id + ("spH" ~ m), sx, sy, soTop - 1, encZ0 + 1, 3.4));
                    nameAndColor(context, ring, "Encoder Spacer " ~ (m + 1), printColor);
                    m += 1;
                }
        }

        // ---------- Driver PCB model ----------
        var j1 = qNothing();
        if (SHOW_PCB)
            j1 = buildPcb(context, id + "pcb");

        // ---------- Reference stack (not for printing) ----------
        var enc = qNothing();
        if (SHOW_STACK)
        {
            enc = buildEncoder(context, id + "enc", encZ0);
            const gold = color(0.8, 0.7, 0.3);
            var m = 0;
            for (var sx in [-12, 12])
                for (var sy in [-12, 12])
                {
                    nameAndColor(context, cylZ(context, id + ("so6" ~ m), sx, sy, fl, 0, 5.0), "REF standoff under PCB", gold);
                    nameAndColor(context, cylZ(context, id + ("so10" ~ m), sx, sy, pcbTop, soTop, 5.0), "REF standoff PCB-AS5600", gold);
                    m += 1;
                }
            // Motor (approx.): magnet boss ~2.5 mm above encoder board, then 15 mm body, dia 34.5
            const bossZ = encZ0 + encT + 2.5;
            nameAndColor(context, cylZ(context, id + "boss", 0, 0, bossZ, bossZ + 4.5, 12), "REF motor boss", color(0.5, 0.5, 0.55));
            nameAndColor(context, cylZ(context, id + "motorRef", 0, 0, bossZ + 4.5, bossZ + 19.5, 34.5), "REF motor", color(0.5, 0.5, 0.55));
        }

        // ---------- Knob on the rotor end face (printable) ----------
        buildKnob(context, id + "knob", encZ0 + encT + 2.5 + 4.5 + 15);
        if (EXPLODE)
            opTransform(context, id + "explodeKnob", { "bodies" : qCreatedBy(id + "knob", EntityType.BODY), "transform" : transform(mmv(0, 0, 70)) });

        // ---------- Clearance checks ----------
        if (SHOW_PCB && SHOW_STACK)
        {
            const dj = evDistance(context, { "side0" : j1, "side1" : enc }).distance;
            if (dj < 0.05 * millimeter)
                reportFeatureWarning(context, id, "Barrel jack J1 hits the AS5600 board - increase the spacer or standoff");
        }
        if (!EXPLODE && (SHOW_PCB || SHOW_STACK))
        {
            const inside = qUnion([qCreatedBy(id + "pcb", EntityType.BODY), qCreatedBy(id + "enc", EntityType.BODY),
                        qCreatedBy(id + "boss", EntityType.BODY), qCreatedBy(id + "motorRef", EntityType.BODY)]);
            const dc = evDistance(context, { "side0" : qUnion([cover, base]), "side1" : inside }).distance;
            if (dc < 0.05 * millimeter)
                reportFeatureWarning(context, id, "Enclosure touches the PCB, AS5600 board or motor");
            else
                reportFeatureInfo(context, id, "Min clearance enclosure to parts: " ~ roundToPrecision(dc / millimeter, 2) ~ " mm");
        }
    }, {});
