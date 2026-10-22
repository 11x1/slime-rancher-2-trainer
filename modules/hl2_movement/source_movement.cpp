#include "source_movement.h"

#include <cmath>
#include <format>

namespace sr2::modules::hl2mv {

float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
float Length2D(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

float Normalize(Vec3& v) {
    const float len = Length(v);
    if (len > 1e-6f) {
        const float inv = 1.0f / len;
        v = v * inv;
    } else {
        v = {};
    }
    return len;
}

void ClampMove(float& forwardmove, float& sidemove, float maxspeed) {
    const float spd = std::sqrt(forwardmove * forwardmove + sidemove * sidemove);
    if (spd != 0.0f && spd > maxspeed) {
        const float ratio = maxspeed / spd;
        forwardmove *= ratio;
        sidemove *= ratio;
    }
}

void BuildWish(Vec3 forward, Vec3 right, float forwardmove, float sidemove, float maxspeed, Vec3& wishdir,
               float& wishspeed) {
    // WalkMove / AirMove: zero out z of the view vectors and renormalize.
    forward.z = 0;
    right.z = 0;
    Normalize(forward);
    Normalize(right);
    Vec3 wishvel{forward.x * forwardmove + right.x * sidemove, forward.y * forwardmove + right.y * sidemove, 0.0f};
    wishdir = wishvel;
    wishspeed = Normalize(wishdir);
    if (wishspeed != 0.0f && wishspeed > maxspeed) wishspeed = maxspeed;
}

void Friction(Vec3& vel, const MoveVars& mv, float dt) {
    const float speed = Length(vel);
    if (speed < 0.1f) return;
    // Ground only (the caller decides); water friction is not ported.
    const float friction = mv.friction * mv.surfaceFriction;
    const float control = (speed < mv.stopspeed) ? mv.stopspeed : speed;
    const float drop = control * friction * dt;
    float newspeed = speed - drop;
    if (newspeed < 0) newspeed = 0;
    if (newspeed != speed) vel = vel * (newspeed / speed);
}

void Accelerate(Vec3& vel, Vec3 wishdir, float wishspeed, float accel, float dt, float surfaceFriction) {
    const float currentspeed = Dot(vel, wishdir);
    const float addspeed = wishspeed - currentspeed;
    if (addspeed <= 0) return;
    float accelspeed = accel * dt * wishspeed * surfaceFriction;
    if (accelspeed > addspeed) accelspeed = addspeed;
    vel = vel + wishdir * accelspeed;
}

void AirAccelerate(Vec3& vel, Vec3 wishdir, float wishspeed, float accel, float dt, float surfaceFriction,
                   float airSpeedCap) {
    float wishspd = wishspeed;
    if (wishspd > airSpeedCap) wishspd = airSpeedCap;
    const float currentspeed = Dot(vel, wishdir);
    const float addspeed = wishspd - currentspeed;
    if (addspeed <= 0) return;
    // Note: full wishspeed here, not the capped wishspd (Source behaviour).
    float accelspeed = accel * wishspeed * dt * surfaceFriction;
    if (accelspeed > addspeed) accelspeed = addspeed;
    vel = vel + wishdir * accelspeed;
}

Vec3 ClipVelocity(Vec3 in, Vec3 normal, float overbounce) {
    const float backoff = Dot(in, normal) * overbounce;
    Vec3 out = in - normal * backoff;
    // Iterate once to make sure we aren't still moving through the plane.
    const float adjust = Dot(out, normal);
    if (adjust < 0.0f) out = out - normal * adjust;
    return out;
}

bool CheckVelocity(Vec3& vel, float maxvelocity) {
    bool changed = false;
    float* c[3] = {&vel.x, &vel.y, &vel.z};
    for (float* p : c) {
        if (std::isnan(*p) || std::isinf(*p)) {
            *p = 0;
            changed = true;
        } else if (*p > maxvelocity) {
            *p = maxvelocity;
            changed = true;
        } else if (*p < -maxvelocity) {
            *p = -maxvelocity;
            changed = true;
        }
    }
    return changed;
}

void GroundMoveStep(Vec3& vel, Vec3 wishdir, float wishspeed, const MoveVars& mv, float dt) {
    vel.z = 0;
    Friction(vel, mv, dt);
    Accelerate(vel, wishdir, wishspeed, mv.accelerate, dt, mv.surfaceFriction);
    vel.z = 0;
    // WalkMove: "if (spd < 1.0f) { mv->m_vecVelocity.Init(); return; }"
    if (Length(vel) < 1.0f) vel = {};
}

void AirMoveStep(Vec3& vel, Vec3 wishdir, float wishspeed, const MoveVars& mv, float dt) {
    AirAccelerate(vel, wishdir, wishspeed, mv.airaccelerate, dt, mv.surfaceFriction, mv.airSpeedCap);
}

float JumpImpulse(float gravity) {
    if (gravity <= 0) return 0;
    return kJumpImpulse * std::sqrt(gravity / kSvGravity);
}

float JumpImpulseForHeight(float gravity, float height, float tickDt) {
    if (gravity <= 0 || height <= 0) return 0;
    const float v0 = std::sqrt(2.0f * gravity * height) - (tickDt > 0 ? 0.5f * gravity * tickDt : 0.0f);
    return v0 > 0 ? v0 : 0.0f;
}

float DiscreteJumpApex(float impulse, float gravity, float tickDt, float* airTime) {
    if (airTime) *airTime = 0;
    if (impulse <= 0 || gravity <= 0) return 0;
    if (tickDt <= 0) {
        if (airTime) *airTime = 2.0f * impulse / gravity;
        return impulse * impulse / (2.0f * gravity);
    }
    // Take-off tick: move with the impulse. Later ticks: gravity, then move.
    float v = impulse, y = impulse * tickDt, apex = y;
    int ticks = 1;
    while (y > 0 && ticks < 100000) {
        v -= gravity * tickDt;
        y += v * tickDt;
        apex = y > apex ? y : apex;
        ++ticks;
    }
    if (airTime) *airTime = static_cast<float>(ticks) * tickDt;
    return apex;
}

bool ApplyJumpBoost(Vec3& vel, Vec3 forwardFlat, float forwardmove, float maxspeed, bool sprinting, BoostMode mode) {
    Vec3 fwd = Flat(forwardFlat);
    Normalize(fwd);
    // "We give a certain percentage of the current forward movement as a bonus
    // to the jump speed." 0.5, or 0.1 when sprinting (or ducked; SR2 has no duck).
    const float perc = sprinting ? 0.1f : 0.5f;
    float addition = std::fabs(forwardmove * perc);
    bool clamp = mode == BoostMode::VanillaClamp;
    // Uncapped + ABH: the vanilla formula only while moving backwards
    // relative to the view, which is where the clamp turns into ABH.
    if (mode == BoostMode::UncappedAbh) clamp = Dot(Flat(vel), fwd) < 0.0f;
    // ABH both ways: always the vanilla formula, mirrored for forward travel.
    const bool mirrorForward = mode == BoostMode::AbhBothWays && Dot(Flat(vel), fwd) >= 0.0f;
    if (mode == BoostMode::AbhBothWays) clamp = true;
    bool clamped = false;
    if (clamp) {
        const float maxSpeed = maxspeed + maxspeed * perc;
        const float newSpeed = addition + Length2D(vel);
        // "If we're over the maximum, we want to only boost as much as will get
        // us to the goal speed"
        if (newSpeed > maxSpeed) {
            addition -= newSpeed - maxSpeed;
            clamped = true;
        }
    }
    if (mirrorForward && addition < 0.0f) {
        // Mirrored ABH: the vanilla code would push speed - flMaxSpeed against
        // the view (slowing a forward hop); push it along the view instead, as
        // ABH does along the (backwards) travel direction. The forwardmove sign
        // flip is skipped: holding S here is vanilla AFH, which gives the same.
        addition = -addition;
    } else if (forwardmove < 0.0f) {
        addition *= -1.0f;
    }
    vel = vel + fwd * addition;
    return clamped;
}

bool Jump(Vec3& vel, float impulse, Vec3 forwardFlat, float forwardmove, float maxspeed, bool sprinting,
          BoostMode mode) {
    vel.z += impulse;
    return ApplyJumpBoost(vel, forwardFlat, forwardmove, maxspeed, sprinting, mode);
}

// ------------------------------------------------------------- self-test --

namespace {

// One hop's air phase at 120 Hz (3 substeps per 40 Hz KCC tick, as in game):
// gravity until back on the ground. strafe = perfect air-strafing (wishdir
// perpendicular to the velocity, sidemove only), otherwise no air input.
void AirPhase(Vec3& vel, const MoveVars& mv, bool strafe) {
    const float dt = 1.0f / 120.0f;
    float z = 0;
    int guard = 0;
    do {
        if (strafe) {
            Vec3 h = Flat(vel);
            Normalize(h);
            AirMoveStep(vel, {-h.y, h.x, 0}, kNormSpeed, mv, dt);
        }
        vel.z -= kSvGravity * dt;
        z += vel.z * dt;
    } while (z > 0 && ++guard < 1000);
    vel.z = 0;
}

// Bunnyhop with perfect air-strafing: jump on the landing tick (no friction),
// look along the velocity and hold forward when jumping.
float StrafeJumpSim(BoostMode mode, int hops) {
    MoveVars mv;
    Vec3 vel{kNormSpeed, 0, 0};
    for (int hop = 0; hop < hops; ++hop) {
        Vec3 fwd = Flat(vel);
        Normalize(fwd);
        Jump(vel, kJumpImpulse, fwd, kNormSpeed, kNormSpeed, false, mode);
        CheckVelocity(vel, kSvMaxVelocity);
        AirPhase(vel, mv, true);
    }
    return Length2D(vel);
}

// Back hop (DeSinc's ABH): travelling backwards at 300 u/s (just above the
// 285 u/s clamp speed), the view facing away from the velocity, no keys held
// at take-off and no air input. Returns the speed after `hops` perfect hops.
float BackHopSim(BoostMode mode, int hops, float maxvelocity) {
    MoveVars mv;
    Vec3 vel{-300.0f, 0, 0};
    const Vec3 view{1, 0, 0};
    for (int hop = 0; hop < hops; ++hop) {
        Jump(vel, kJumpImpulse, view, 0.0f, kNormSpeed, false, mode);
        CheckVelocity(vel, maxvelocity);
        AirPhase(vel, mv, false);
    }
    return Length2D(vel);
}

// Forward hop: the mirror of BackHopSim. Travelling forwards at 300 u/s with
// the view along the velocity, no keys at take-off, no air input.
float ForwardHopSim(BoostMode mode, int hops, float maxvelocity) {
    MoveVars mv;
    Vec3 vel{300.0f, 0, 0};
    const Vec3 view{1, 0, 0};
    for (int hop = 0; hop < hops; ++hop) {
        Jump(vel, kJumpImpulse, view, 0.0f, kNormSpeed, false, mode);
        CheckVelocity(vel, maxvelocity);
        AirPhase(vel, mv, false);
    }
    return Length2D(vel);
}

Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// Plane through the origin tilted `deg` degrees about the y axis; its down
// slope points towards +x.
Vec3 SlopeNormal(float deg) {
    const float r = deg * 3.14159265f / 180.0f;
    return {std::sin(r), 0.0f, std::cos(r)};
}

struct RampLanding {
    float before = 0;     // horizontal speed just before touching the ramp
    float after = 0;      // ground speed after the Source landing clip
    float oldModel = 0;   // previous behaviour: KCC keeps |horizontal| along the slope, module flattens it
    float hop = 0;        // horizontal speed after a jump on the landing tick
    bool grounded = false;
    float clipZ = 0;      // z of the clipped velocity (> 140 = launched)
};

// Fall from `height` above a ramp (120 Hz, gravity only) with horizontal
// velocity hv, land with LandingClip like TryPlayerMove + SetGroundEntity.
RampLanding RampLandingSim(float slopeDeg, Vec3 hv, float height) {
    const Vec3 n = SlopeNormal(slopeDeg);
    const float dt = 1.0f / 120.0f;
    Vec3 pos{0, 0, height}, vel = hv;
    RampLanding r;
    for (int i = 0; i < 5000; ++i) {
        vel.z -= kSvGravity * dt;
        pos = pos + vel * dt;
        const float d = Dot(pos, n);
        if (d >= 0) continue;
        pos = pos - n * d;
        r.before = Length2D(vel);
        const Vec3 c = LandingClip(vel, n);
        r.clipZ = c.z;
        r.grounded = IsWalkable(n) && !MovingUpRapidly(c);
        r.after = Length2D(c);
        // Old model: KCC's stable landing keeps the horizontal magnitude along
        // the surface tangent, the module then used its horizontal part.
        Vec3 dir = Flat(vel);
        Normalize(dir);
        Vec3 tangent = Cross(n, Cross(dir, {0, 0, 1}));
        Normalize(tangent);
        r.oldModel = r.before * Length2D(tangent);
        // Hop on the landing tick (no friction, no keys): the boost is kept.
        Vec3 j = Flat(c);
        Jump(j, kJumpImpulse, dir, 0.0f, kNormSpeed, false, BoostMode::Uncapped);
        r.hop = Length2D(j);
        break;
    }
    return r;
}

// Slide along a ramp for `seconds` (120 Hz): AirAccelerate into the ramp
// (strafe key towards it), gravity, move, ClipVelocity on contact, Source
// grounding rule (walkable plane and not moving up rapidly -> ground tick
// with friction). Starts on the surface moving along the ramp (+y).
struct SurfResult {
    float along = 0;   // speed along the ramp (y) at the end
    float speed = 0;   // total speed at the end
    int groundTicks = 0;
    int contacts = 0;
};
SurfResult SurfSim(float slopeDeg, float speed, float seconds) {
    const Vec3 n = SlopeNormal(slopeDeg);
    const float dt = 1.0f / 120.0f;
    MoveVars mv;
    Vec3 pos{}, vel{0, speed, 0};
    Vec3 into = Flat(n) * -1.0f;
    Normalize(into);
    SurfResult r;
    bool onGround = false;
    const int steps = static_cast<int>(seconds / dt);
    for (int i = 0; i < steps; ++i) {
        if (onGround) {
            GroundMoveStep(vel, {}, 0.0f, mv, dt);  // no keys on the ground
            ++r.groundTicks;
        } else {
            AirMoveStep(vel, into, kNormSpeed, mv, dt);
            vel.z -= kSvGravity * dt;
        }
        pos = pos + vel * dt;
        const float d = Dot(pos, n);
        if (d < 0.01f) {
            pos = pos - n * d;
            vel = ClipVelocity(vel, n, 1.0f);
            ++r.contacts;
            onGround = IsWalkable(n) && !MovingUpRapidly(vel);
        } else {
            onGround = false;
        }
    }
    r.along = vel.y;
    r.speed = Length(vel);
    return r;
}

}  // namespace

bool SelfTest(std::string* report) {
    bool ok = true;
    std::string fails;
    auto check = [&](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            fails += what;
            fails += "; ";
        }
    };

    const float uncapped = StrafeJumpSim(BoostMode::Uncapped, 15);
    const float capped = StrafeJumpSim(BoostMode::VanillaClamp, 15);
    const float uncAbh = StrafeJumpSim(BoostMode::UncappedAbh, 15);
    check(uncapped > 1000.0f, "uncapped strafe-jump did not gain speed");
    check(capped < 400.0f, "clamped strafe-jump exceeded 400 u/s");
    check(uncapped > 2.0f * capped, "clamp made no difference");
    check(std::fabs(uncAbh - uncapped) < 1.0f, "Uncapped+ABH strafe-jump differs from uncapped");
    const float both = StrafeJumpSim(BoostMode::AbhBothWays, 15);
    check(both > 1000.0f, "ABH-both-ways strafe-jump did not gain speed");

    const float backUnc = BackHopSim(BoostMode::Uncapped, 8, kSvMaxVelocity);
    const float backVan = BackHopSim(BoostMode::VanillaClamp, 8, kSvMaxVelocity);
    const float backAbh = BackHopSim(BoostMode::UncappedAbh, 8, kSvMaxVelocity);
    check(std::fabs(backUnc - 300.0f) < 1.0f, "uncapped back-hop changed speed");
    // 300 -> 315 -> 345 -> 405 -> 525 -> 765 -> 1245 -> 2205 -> 3500 (sv_maxvelocity)
    check(backVan > 2000.0f, "vanilla back-hop did not accelerate (no ABH)");
    check(std::fabs(backAbh - backVan) < 1.0f, "Uncapped+ABH back-hop differs from vanilla");
    const float backBoth = BackHopSim(BoostMode::AbhBothWays, 8, kSvMaxVelocity);
    check(std::fabs(backBoth - backVan) < 1.0f, "ABH-both-ways back-hop differs from vanilla");

    // Forward hop, no keys: uncapped modes keep 300, the vanilla clamp drops to
    // 285, ABH both ways gains exactly like the back hop (300 -> 315 -> ... -> 3500).
    const float fwdUnc = ForwardHopSim(BoostMode::Uncapped, 8, kSvMaxVelocity);
    const float fwdVan = ForwardHopSim(BoostMode::VanillaClamp, 8, kSvMaxVelocity);
    const float fwdAbh = ForwardHopSim(BoostMode::UncappedAbh, 8, kSvMaxVelocity);
    const float fwdBoth = ForwardHopSim(BoostMode::AbhBothWays, 8, kSvMaxVelocity);
    check(std::fabs(fwdUnc - 300.0f) < 1.0f && std::fabs(fwdAbh - 300.0f) < 1.0f,
          "uncapped forward hop (no keys) changed speed");
    check(std::fabs(fwdVan - 285.0f) < 1.0f, "vanilla forward hop (no keys) did not settle at 285");
    check(std::fabs(fwdBoth - backVan) < 1.0f, "ABH-both-ways forward hop does not match the back hop");
    for (int hops = 1; hops <= 7; ++hops) {
        check(std::fabs(ForwardHopSim(BoostMode::AbhBothWays, hops, kSvMaxVelocity) -
                        BackHopSim(BoostMode::VanillaClamp, hops, kSvMaxVelocity)) < 0.5f,
              "ABH-both-ways forward hop differs from the back hop on some hop");
    }
    // A forward jump from rest holding W gets the same +95 u/s in every mode.
    for (BoostMode m : {BoostMode::Uncapped, BoostMode::VanillaClamp, BoostMode::UncappedAbh, BoostMode::AbhBothWays}) {
        Vec3 j{kNormSpeed, 0, 0};
        Jump(j, kJumpImpulse, {1, 0, 0}, kNormSpeed, kNormSpeed, false, m);
        check(std::fabs(Length2D(j) - 285.0f) < 0.01f, "forward jump boost from walk speed is not +95");
    }

    // Ground acceleration from rest reaches normspeed, friction stops the player.
    MoveVars mv;
    Vec3 v{};
    for (int i = 0; i < 100; ++i) GroundMoveStep(v, {1, 0, 0}, kNormSpeed, mv, 0.01f);
    const float accelSpeed = Length(v);
    check(accelSpeed > 185.0f && accelSpeed < 190.01f, "ground accelerate did not reach normspeed");
    for (int i = 0; i < 100; ++i) GroundMoveStep(v, {}, 0.0f, mv, 0.01f);
    check(Length(v) == 0.0f, "friction did not stop the player within 1 s");

    // AirAccelerate cannot push the velocity component along wishdir above 30.
    Vec3 a{};
    for (int i = 0; i < 200; ++i) AirMoveStep(a, {1, 0, 0}, kNormSpeed, mv, 0.01f);
    check(std::fabs(a.x - kAirSpeedCap) < 0.01f, "air speed cap not 30");

    const Vec3 clipped = ClipVelocity({100, 0, -100}, {0, 0, 1}, 1.0f);
    check(std::fabs(clipped.z) < 1e-4f && std::fabs(clipped.x - 100.0f) < 1e-4f, "ClipVelocity");

    Vec3 big{5000, -5000, NAN};
    CheckVelocity(big, kSvMaxVelocity);
    check(big.x == kSvMaxVelocity && big.y == -kSvMaxVelocity && big.z == 0, "CheckVelocity");

    check(std::fabs(JumpImpulse(kSvGravity) - kJumpImpulse) < 1e-3f, "JumpImpulse");

    // Ramp landing: fall 64 u (3.4 m at 0.0526 m/u) onto a 30 degree down
    // slope moving downhill at 300 u/s. The clip converts fall speed into
    // ground speed (ramp boost); the old horizontal-only model lost speed.
    const RampLanding land = RampLandingSim(30.0f, {300, 0, 0}, 64.0f);
    check(land.grounded, "30 degree ramp landing did not ground");
    check(land.after > 1.3f * land.before, "30 degree down-ramp landing gave no ramp boost");
    check(land.oldModel < land.before && land.after > land.oldModel, "ramp landing not better than the old model");
    check(land.hop >= land.after - 0.01f, "hop on the ramp landing tick lost the ramp boost");
    // Flat landing keeps the horizontal speed exactly (no change from before).
    const RampLanding flat = RampLandingSim(0.0f, {300, 0, 0}, 64.0f);
    check(flat.grounded && std::fabs(flat.after - 300.0f) < 0.01f, "flat landing changed horizontal speed");
    // Fast landing onto a 30 degree up slope: clipped z > 140 -> launched up the ramp.
    const RampLanding up = RampLandingSim(30.0f, {-1000, 0, 0}, 8.0f);
    check(!up.grounded && up.clipZ > kNonJumpVelocity, "fast up-ramp landing did not launch");

    // Surf: 60 degree ramp (normal.z 0.5 < 0.7, not ground) keeps the speed
    // along the ramp and only gains from gravity; the same slide on a walkable
    // 30 degree slope is ground with friction.
    const SurfResult surf = SurfSim(60.0f, 1000.0f, 2.0f);
    check(!IsWalkable(SlopeNormal(60.0f)) && IsWalkable(SlopeNormal(30.0f)), "walkable limit (0.7)");
    check(surf.groundTicks == 0 && surf.contacts > 100, "60 degree surf ramp became ground / lost contact");
    check(std::fabs(surf.along - 1000.0f) < 1.0f && surf.speed >= 1000.0f, "60 degree surf lost speed");
    const SurfResult slide30 = SurfSim(30.0f, 1000.0f, 2.0f);
    check(slide30.groundTicks > 0 && slide30.speed < 500.0f, "30 degree slope did not act as ground");

    // Jump height setting: the impulse reaches the set apex in the 40 Hz KCC
    // tick model (take-off tick moves with v0, then gravity before each move).
    const float scale = 10.0f / kNormSpeed;  // matched vanilla scale, 0.0526 m/u
    float apex2m = 0, air2m = 0;
    for (float mult : {1.0f, 2.0f, 0.5f}) {
        for (float hm : {1.0f, 2.0f, 4.0f}) {
            const float g = kSvGravity * mult, hu = hm / scale;
            float air = 0;
            const float apex = DiscreteJumpApex(JumpImpulseForHeight(g, hu, 0.025f), g, 0.025f, &air);
            check(std::fabs(apex - hu) < 0.01f * hu, "jump apex differs from the jump height setting by > 1%");
            if (mult == 1.0f && hm == 2.0f) {
                apex2m = apex * scale;
                air2m = air;
            }
        }
    }

    if (report) {
        *report = std::format(
            "strafe-jump 15 hops: {:.0f} uncapped / {:.0f} vanilla clamp / {:.0f} uncapped+ABH / {:.0f} ABH both ways "
            "u/s; back-hop 8 hops from 300: {:.0f} / {:.0f} / {:.0f} / {:.0f} u/s; forward-hop 8 hops from 300: "
            "{:.0f} / {:.0f} / {:.0f} / {:.0f} u/s; 30deg down-ramp landing from 64 u: {:.0f} -> {:.0f} u/s "
            "(old model {:.0f}, hop {:.0f}); 30deg up-ramp at 1000: launched, z {:.0f} u/s; 60deg surf 2 s at 1000: "
            "along {:.0f}, total {:.0f} u/s, {} contacts, 0 ground ticks (30deg slide: {:.0f} u/s); jump 2.00 m at "
            "600 u/s^2: apex {:.3f} m, air {:.2f} s; {}",
            uncapped, capped, uncAbh, both, backUnc, backVan, backAbh, backBoth, fwdUnc, fwdVan, fwdAbh, fwdBoth,
            land.before, land.after, land.oldModel, land.hop, up.clipZ, surf.along, surf.speed, surf.contacts,
            slide30.speed, apex2m, air2m, ok ? "all checks passed" : "FAILED: " + fails);
    }
    return ok;
}

}  // namespace sr2::modules::hl2mv
