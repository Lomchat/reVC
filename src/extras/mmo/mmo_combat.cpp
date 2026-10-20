// The shared world's combat (Orthros' docs/mmo.md, section 7 "Combat authority"; decision 1's deaths; combat
// everywhere): a hit is decided by the shooter's client and applied by the victim's owner. A hit on a puppet by
// something simulated here (the local player, its vehicle, an entity we own) goes to the puppet's owner as a TARGETED
// record (raw damage, method, piece, direction, attacker, the hit reaction's parameters); the puppet only shows it
// (blood, sparks). The owner applies it with the game's own InflictDamage / BurstTyre / VehicleDamage / KnockOffRider,
// the attacker's puppet as culprit (armour, the player's x0.33, death and credit as in the game). The shots others see
// (bullet traces, sniper bullets, flames, projectiles, explosions) are EVENTs: replayed for their effects only, each
// client damaging what it owns and its own player (area effects skip puppets). A death of the local player is told to
// everyone (the kill line). Pickups dropped by dead peds and pickups taken are told too.
// Wire (engine-owned, docs/mmo.md section 5, subs 0x20-0x3F):
//   TARGETED 0x20 ped damage   u32 attacker, u8 method, f32 damage, u8 piece, u8 direction, u8 reaction, u8 hitLevel,
//                              u8 strength, f32x3 attacker position, u32 server ms
//            0x21 vehicle      u32 attacker, u8 weapon, f32 damage, f32x3 position, u32 server ms
//            0x22 tyre         u32 attacker, u8 wheel (CAR_PIECE_WHEEL_*), u32 server ms
//            0x23 impulse      u32 attacker, f32 impulse, u16 piece, u32 server ms (melee on a car)
//            0x24 knock off    u32 attacker, u8 weapon, u8 direction, u32 server ms (a rider off his bike)
//            0x25 car hit      u32 car, f32 impulse, f32x3 car speed, u32 server ms (a ped run over)
//   EVENT    0x20 shot         u32 shooter, u8 weapon, u8 flags (1 muzzle), f32x3 start, f32x3 end
//            0x21 sniper       u32 shooter, u8 weapon, f32x3 position, f32x3 speed
//            0x22 flame        u32 shooter, f32x3 start, f32x3 end
//            0x23 projectile   u32 shooter, u32 id, u8 weapon, f32x3 position, forward, up, velocity
//            0x24 projectile gone  u32 id
//            0x25 explosion    u32 culprit, u32 exploding, u8 type, f32x3 position, u32 lifetime, u32 server ms
//            0x26 death        u32 victim, u32 killer, u8 weapon
//            0x27 pickup made  u32 id, u16 model, u8 type, u32 quantity, f32x3 position
//            0x28 pickup taken u8 kind: 0 u32 id (one made by a client); 1 u16 index, u16 model, f32x3 position (the
//                              save's own, the same index everywhere)
//            0x29 melee swing  u32 shooter, u8 weapon
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "General.h"
#include "Timer.h"
#include "Frontend.h"
#include "World.h"
#include "Pools.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Ped.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Bike.h"
#include "Weapon.h"
#include "WeaponInfo.h"
#include "WeaponType.h"
#include "BulletInfo.h"
#include "ShotInfo.h"
#include "ProjectileInfo.h"
#include "Projectile.h"
#include "Explosion.h"
#include "Fire.h"
#include "Pickups.h"
#include "Particle.h"
#include "PointLights.h"
#include "SpecialFX.h"
#include "Camera.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "Streaming.h"
#include "WeaponModelInfo.h"
#include "Game.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "DMAudio.h"

extern FightMove tFightMoves[NUM_FIGHTMOVES];
extern CShotInfo gaShotInfo[NUMSHOTINFOS];

namespace OrthrosMmo
{
// ---------------------------------------------------------------- the records
enum {
	TG_PED_DAMAGE = SUB_COMBAT, TG_VEHICLE_DAMAGE, TG_TYRE, TG_VEHICLE_IMPULSE, TG_KNOCK_OFF, TG_CAR_HIT,
};
enum {
	EV_SHOT = SUB_COMBAT, EV_SNIPER, EV_FLAME, EV_PROJECTILE, EV_PROJECTILE_GONE, EV_EXPLOSION, EV_DEATH,
	EV_PICKUP_MADE, EV_PICKUP_TAKEN, EV_MELEE,
};
// (the hit reactions the shooter saw: the owner plays the game's own for the method, a fist fight's from its numbers)
enum { REACT_WEAPON, REACT_FIGHT };

enum {
	STALE_HIT = 20000,     // (ms of server time: a hit older is dropped - a tab hidden meanwhile, its queue replayed;
	STALE_EFFECT = 15000,  // a blast older is not replayed. Generous: a slow machine's frame may take seconds)
	NEAR = 300,            // (m: an event is sent only when a remote player is this near)
	SHOTS_PER_SECOND = 40, // (traces, sniper bullets, flames told at most this often)
	DEATH_MEMORY = 30000,  // (ms: the last network hit names the killer for this long)
	PENDING_PICKUPS = 16,
};

// ---------------------------------------------------------------- state
// (projectiles by their slot in gaProjectileInfo: a local one and its id, or a remote one's copy)
struct ProjectileSlot { bool used, remote, told; uint8 from; uint32 id; };
static ProjectileSlot projectiles[NUM_PROJECTILES];
static uint32 projectileCounter;
static bool bMakingProjectile; // (AddProjectile called for a remote one)
static uint8 makingFrom;
static uint32 makingId;

// (pickups made by a client, by their slot in CPickups::aPickUps: its id and handle)
struct PickupSlot { uint32 id; int32 handle; };
static PickupSlot pickups[NUMPICKUPS];
static bool bMakingPickup;
struct PendingPickup { bool used; uint32 id; int16 model; uint8 type; uint32 quantity; CVector pos; double until; };
static PendingPickup pendingPickups[PENDING_PICKUPS];

// (the last network hit applied to the local player: the killer if it dies of it)
static struct { uint32 attacker; int32 weapon; CEntity *culprit; double at; } lastHit;
static bool bDeathTold;
static bool bApplying;  // (a remote hit being applied here: the hooks let it through)
static bool bWrecking;  // (a puppet vehicle blown up as its owner's was: its occupants are not killed here)

// (the shots' budget, the last shooter of a muzzle flash)
static double shotWindow;
static int32 shotsInWindow;
static const CEntity *lastMuzzleShooter;
static uint32 lastMuzzleFrame;
static double lastMelee;

// (debug)
static struct { int32 sent, received, applied, dropped; } hits;
static struct { uint32 target; int32 method; float damage; double at; } lastSent, lastReceived;
static int32 eventsSent, eventsReceived, kills, deaths;
static int32 fireCommands, firedCommands;
static int32 seen[16]; // (events received, by sub - SUB_COMBAT)
static CVector lastTraceStart, lastTraceEnd;
static char lastKiller[24], lastVictim[24];
static uint32 lastDeathKiller;

// ---------------------------------------------------------------- helpers
static inline bool
Playing(void)
{
	return Mode() == MODE_ACTIVE && LocalSlot() != NO_SLOT && FindPlayerPed() != nil;
}

static uint32
Stamp(void)
{
	return (uint32)(int64)ServerNow();
}

// (a record's server-time age in ms)
static int32
Age(uint32 stamp)
{
	return (int32)(Stamp() - stamp);
}

// (simulated here: the local player, its vehicle, an entity we own, an entity not networked)
static bool
Local(const CEntity *e)
{
	return e != nil && OwnedHere(e);
}

// (the network id the others know an entity by: a remote player's puppet is that player)
static uint32
IdOf(const CEntity *e)
{
	if (e == nil)
		return 0;
	if (e == FindPlayerPed())
		return LocalSlot() != NO_SLOT ? PlayerNetId(LocalSlot()) : 0;
	const NetEntity *ne = Find(e);
	if (ne == nil)
		return 0;
	return ne->player >= 0 ? PlayerNetId(ne->player) : ne->netid;
}

// (a pointer the game keeps without a reference - m_lastDamEntity - still a live ped or vehicle: read from the pools,
// never through the pointer)
template<typename T, typename U>
static bool
InPool(CPool<T, U> *pool, const CEntity *e)
{
	if (pool == nil)
		return false;
	const int32 i = pool->GetJustIndex_NoFreeAssert((T *)e);
	return i >= 0 && i < pool->GetSize() && (const CEntity *)pool->GetSlot(i) == e;
}

static bool
Live(const CEntity *e)
{
	return e != nil && (InPool(CPools::GetPedPool(), e) || InPool(CPools::GetVehiclePool(), e));
}

// (the one who did it: a vehicle's driver)
static CEntity *
Doer(CEntity *e)
{
	if (e && e->IsVehicle() && ((CVehicle *)e)->pDriver)
		return ((CVehicle *)e)->pDriver;
	return e;
}

// (is a remote player, present and not away, within this distance of pos?)
static bool
AnyoneNear(const CVector &pos, float radius)
{
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		const RemotePlayer *p = Player(i);
		if (p && p->hasState && !p->away && (p->pos - pos).MagnitudeSqr() < radius * radius)
			return true;
	}
	return false;
}

static bool
ShotBudget(void)
{
	const double now = Now();
	if (now - shotWindow >= 1000.0) {
		shotWindow = now;
		shotsInWindow = 0;
	}
	return shotsInWindow++ < SHOTS_PER_SECOND;
}

// (hits sent as they are: bullets, pellets, melee, sniper bullets, drive-bys, the helicopters' guns. Not the hits the
// victim's client sees itself - falls, drowning - nor the area effects every owner applies to its own: explosions,
// fire, flames, gas)
static bool
DirectHit(int32 method)
{
	return (method >= WEAPONTYPE_UNARMED && method <= WEAPONTYPE_CHAINSAW) ||
	       (method >= WEAPONTYPE_COLT45 && method <= WEAPONTYPE_ROCKETLAUNCHER) || method == WEAPONTYPE_M60 ||
	       method == WEAPONTYPE_MINIGUN || method == WEAPONTYPE_HELICANNON || method == WEAPONTYPE_UZI_DRIVEBY;
}

// (the name of a slot's player: ours, or a present remote player's)
static const char *
NameOf(int32 slot)
{
	if (slot == LocalSlot())
		return Me().name;
	const RemotePlayer *p = Player(slot);
	return p ? p->name : nil;
}

static void
CopyName(char *out, const char *name, int32 size)
{
	strncpy(out, name ? name : "", size - 1);
	out[size - 1] = '\0';
}

// (a TARGETED record to a victim's owner)
static void
Targeted(uint32 target, int32 sub, Out &o)
{
	o.U32(target);
	o.U8(sub);
}

// ---------------------------------------------------------------- hits on puppets: told to their owner
static void
SendPedDamage(CPed *victim, CEntity *damagedBy, int32 method, float damage, int32 piece, uint8 direction)
{
	const uint32 target = IdOf(victim);
	if (target == 0)
		return;
	if (IsPlayerNetId(target)) {
		const RemotePlayer *p = Player(SlotOfNetId(target));
		if (p == nil || p->away) // (a player away is not hurt)
			return;
	}
	// (a fist fight's reaction is the attacker's move: its level, its strength)
	int32 reaction = REACT_WEAPON, hitLevel = 0, strength = 0;
	CEntity *doer = Doer(damagedBy);
	if (doer && doer->IsPed() && ((CPed *)doer)->m_nPedState == PED_FIGHT && method <= WEAPONTYPE_CHAINSAW) {
		CPed *attacker = (CPed *)doer;
		reaction = REACT_FIGHT;
		if (attacker->m_curFightMove < NUM_FIGHTMOVES)
			hitLevel = tFightMoves[attacker->m_curFightMove].hitLevel;
		strength = (int32)(method == WEAPONTYPE_UNARMED ? damage / 3.0f : damage);
		strength = Clamp(strength, 0, 255);
	}
	const CVector from = doer ? doer->GetPosition() : victim->GetPosition();
	Out o(R_TARGETED);
	Targeted(target, TG_PED_DAMAGE, o);
	o.U32(IdOf(doer));
	o.U8(method);
	o.F32(damage);
	o.U8(piece);
	o.U8(direction);
	o.U8(reaction);
	o.U8(hitLevel);
	o.U8(strength);
	o.Vec(from);
	o.U32(Stamp());
	hits.sent++;
	lastSent.target = target;
	lastSent.method = method;
	lastSent.damage = damage;
	lastSent.at = Now();
}

// ---------------------------------------------------------------- hits on what we own: applied
// (the attacker's entity here: a remote player's puppet, an entity's copy; nil if none)
static CEntity *
Culprit(uint32 attacker)
{
	CEntity *e = EntityOf(attacker);
	return e && e != FindPlayerPed() ? e : nil;
}

// (the hit reactions of CWeapon::DoBulletImpact, FireShotgun, FireMelee, CBulletInfo::Update, the drive-by, then the
// game's InflictDamage)
static void
ApplyPedHit(CPed *victim, CEntity *culprit, int32 method, float damage, int32 piece, int32 dir, int32 reaction,
            int32 hitLevel, int32 strength, const CVector &from)
{
	const bool isPlayer = victim == FindPlayerPed();
	if (victim->DyingOrDead())
		return;
	CVector2D posOffset(from.x - victim->GetPosition().x, from.y - victim->GetPosition().y);
	const float oldHealth = victim->m_fHealth;
	if (culprit && culprit->IsPed())
		victim->ReactToAttack(culprit);

	if (reaction == REACT_FIGHT) {
		victim->StartFightDefend(dir, hitLevel, strength);
		victim->InflictDamage(culprit, (eWeaponType)method, damage, (ePedPieceTypes)piece, dir);
		return;
	}
	if (method <= WEAPONTYPE_CHAINSAW) {
		// (FireMelee)
		const bool isHeavy = method >= WEAPONTYPE_GOLFCLUB && method <= WEAPONTYPE_KATANA && method != WEAPONTYPE_HAMMER;
		int32 level = HITLEVEL_HIGH;
		if (isHeavy && (victim->OnGround() || victim->m_nWaitState == WAITSTATE_SUN_BATHE_IDLE))
			level = HITLEVEL_GROUND;
		victim->StartFightDefend(dir, level, 10);
		victim->InflictDamage(culprit, (eWeaponType)method, damage, PEDPIECE_TORSO, dir);
		if (!victim->OnGround()) {
			if (victim->m_fHealth > 0.0f &&
			    ((victim->m_fHealth < 30.0f && oldHealth > 30.0f) || ((isHeavy || method == WEAPONTYPE_BRASSKNUCKLE) && !isPlayer))) {
				posOffset.Normalise();
				victim->bIsStanding = false;
				if (method == WEAPONTYPE_CHAINSAW)
					victim->ApplyMoveForce(posOffset.x * -2.0f, posOffset.y * -2.0f, 2.0f);
				else
					victim->ApplyMoveForce(posOffset.x * -5.0f, posOffset.y * -5.0f, 3.0f);
				victim->SetFall(isHeavy && isPlayer ? 3000 : 1500, AnimationId(ANIM_STD_HIGHIMPACT_FRONT + dir), false);
			}
		}
		return;
	}
	if (CWeapon::IsShotgun(method)) {
		// (FireShotgun's pellets)
		bool cantStandup = true;
		if (victim->m_getUpTimer > CTimer::GetTimeInMilliseconds() - 3000 || !victim->bCanBeShotInVehicle)
			cantStandup = false;
		posOffset.Normalise();
		if (victim->IsPedInControl()) {
			if (victim->bIsStanding && cantStandup) {
				victim->bIsStanding = false;
				victim->ApplyMoveForce(posOffset.x * -6.0f, posOffset.y * -6.0f, 5.0f);
			} else
				victim->ApplyMoveForce(posOffset.x * -2.0f, posOffset.y * -2.0f, 0.0f);
			if (cantStandup)
				victim->SetFall(1500, AnimationId(ANIM_STD_HIGHIMPACT_FRONT + dir), false);
		}
		victim->InflictDamage(culprit, (eWeaponType)method, damage, (ePedPieceTypes)piece, dir);
		return;
	}
	// (the gun's hit animation: DoBulletImpact; the drive-by's and the sniper bullet's are the same one)
	if (!victim->IsPedInControl() || victim->bIsDucking) {
		victim->InflictDamage(culprit, (eWeaponType)method, damage, (ePedPieceTypes)piece, dir);
		return;
	}
	if (victim->bCanBeShotInVehicle && !isPlayer &&
	    (method == WEAPONTYPE_HELICANNON || method == WEAPONTYPE_M60 || method == WEAPONTYPE_PYTHON)) {
		posOffset.Normalise();
		victim->bIsStanding = false;
		victim->ApplyMoveForce(posOffset.x * -5.0f, posOffset.y * -5.0f, 5.0f);
		victim->SetFall(1500, AnimationId(ANIM_STD_HIGHIMPACT_FRONT + dir), false);
		victim->InflictDamage(culprit, (eWeaponType)method, damage, (ePedPieceTypes)piece, dir);
		return;
	}
	bool animate = true;
	if (isPlayer) {
		CPlayerPed *player = (CPlayerPed *)victim;
		animate = player->m_nHitAnimDelayTimer < CTimer::GetTimeInMilliseconds() && victim->m_nPedState != PED_DRIVING;
		if (animate)
			player->m_nHitAnimDelayTimer = CTimer::GetTimeInMilliseconds() + (method == WEAPONTYPE_M4 ? 2500 : 1000);
	}
	if (animate) {
		victim->ClearAttackByRemovingAnim();
		CAnimBlendAssociation *assoc = CAnimManager::AddAnimation(victim->GetClump(), ASSOCGRP_STD, AnimationId(ANIM_STD_HITBYGUN_FRONT + dir));
		if (assoc) {
			assoc->blendAmount = 0.0f;
			assoc->blendDelta = 8.0f;
		}
	}
	victim->InflictDamage(culprit, (eWeaponType)method, damage, (ePedPieceTypes)piece, dir);
}

// (the victim's entity here, ours to apply to: the local player, or an entity we own; nil otherwise. The local player is
// not hurt while away - the pause menu open)
static CEntity *
OwnVictim(uint32 target)
{
	if (IsPlayerNetId(target)) {
		if (SlotOfNetId(target) != LocalSlot())
			return nil;
		CPlayerPed *ped = FindPlayerPed();
		if (ped == nil || FrontEndMenuManager.m_bMenuActive)
			return nil;
		return ped;
	}
	CEntity *e = EntityOf(target);
	return e && !IsPuppet(e) ? e : nil;
}

static void
BloodAt(CPed *victim, const CVector &from)
{
	if (!CGame::nastyGame || !victim->GetIsOnScreen())
		return;
	CVector pos = victim->GetPosition();
	pos.z += 0.3f;
	CVector dir = (from - pos) * 0.01f;
	dir.z = 0.01f;
	for (int32 i = 0; i < 4; i++)
		CParticle::AddParticle(PARTICLE_BLOOD_SMALL, pos, dir);
}

static void
OnPedDamage(int32 from, uint32 target, In &in)
{
	const uint32 attacker = in.U32();
	const int32 method = in.U8();
	const float damage = in.F32();
	const int32 piece = in.U8(), dir = in.U8() & 3, reaction = in.U8(), hitLevel = in.U8(), strength = in.U8();
	const CVector source = in.Vec();
	const uint32 stamp = in.U32();
	if (!in.Ok() || !(damage >= 0.0f && damage <= 5000.0f) || method > WEAPONTYPE_ANYWEAPON)
		return;
	hits.received++;
	lastReceived.target = attacker;
	lastReceived.method = method;
	lastReceived.damage = damage;
	lastReceived.at = Now();
	CEntity *e = OwnVictim(target);
	if (e == nil || !e->IsPed() || Age(stamp) > STALE_HIT) {
		hits.dropped++;
		return;
	}
	CPed *victim = (CPed *)e;
	CEntity *culprit = Culprit(attacker);
	if (victim == FindPlayerPed() && attacker != 0) {
		lastHit.attacker = attacker;
		lastHit.weapon = method;
		lastHit.culprit = culprit;
		lastHit.at = Now();
	}
	bApplying = true;
	ApplyPedHit(victim, culprit, method, damage, piece, dir, reaction, hitLevel, strength, source);
	bApplying = false;
	BloodAt(victim, source);
	hits.applied++;
}

static void
OnVehicleDamage(uint32 target, int32 sub, In &in)
{
	const uint32 attacker = in.U32();
	CEntity *e = OwnVictim(target);
	CVehicle *veh = e && e->IsVehicle() ? (CVehicle *)e : nil;
	hits.received++;
	switch (sub) {
	case TG_VEHICLE_DAMAGE: {
		const int32 weapon = in.U8();
		const float damage = in.F32();
		const CVector pos = in.Vec();
		const uint32 stamp = in.U32();
		if (!in.Ok() || veh == nil || Age(stamp) > STALE_HIT || !(damage >= 0.0f && damage <= 5000.0f))
			break;
		bApplying = true;
		veh->InflictDamage(Culprit(attacker), (eWeaponType)weapon, damage, pos);
		bApplying = false;
		hits.applied++;
		return;
	}
	case TG_TYRE: {
		const int32 wheel = in.U8();
		const uint32 stamp = in.U32();
		if (!in.Ok() || veh == nil || Age(stamp) > STALE_HIT || wheel < CAR_PIECE_WHEEL_LF || wheel > CAR_PIECE_WHEEL_RR)
			break;
		bApplying = true;
		veh->BurstTyre(wheel, true);
		bApplying = false;
		hits.applied++;
		return;
	}
	case TG_VEHICLE_IMPULSE: {
		const float impulse = in.F32();
		const int32 piece = in.U16();
		const uint32 stamp = in.U32();
		if (!in.Ok() || veh == nil || !veh->IsCar() || Age(stamp) > STALE_HIT || !(impulse > 0.0f && impulse < 100000.0f))
			break;
		const float oldHealth = veh->m_fHealth;
		bApplying = true;
		((CAutomobile *)veh)->VehicleDamage(impulse, piece);
		bApplying = false;
		CEntity *culprit = Culprit(attacker);
		if (veh->m_fHealth < oldHealth && culprit) {
			veh->m_nLastWeaponDamage = WEAPONTYPE_BASEBALLBAT;
			veh->m_pLastDamageEntity = culprit;
		}
		hits.applied++;
		return;
	}
	}
	hits.dropped++;
}

static void
OnKnockOff(uint32 target, In &in)
{
	const uint32 attacker = in.U32();
	const int32 weapon = in.U8(), dir = in.U8() & 3;
	const uint32 stamp = in.U32();
	hits.received++;
	CEntity *e = OwnVictim(target);
	if (!in.Ok() || e == nil || !e->IsPed() || Age(stamp) > STALE_HIT) {
		hits.dropped++;
		return;
	}
	CPed *rider = (CPed *)e;
	if (!rider->bInVehicle || rider->m_pMyVehicle == nil || !rider->m_pMyVehicle->IsBike()) {
		hits.dropped++;
		return;
	}
	CEntity *culprit = Culprit(attacker);
	bApplying = true;
	((CBike *)rider->m_pMyVehicle)->KnockOffRider((eWeaponType)weapon, dir, rider, false);
	if (culprit && rider != FindPlayerPed())
		rider->ReactToAttack(culprit);
	bApplying = false;
	hits.applied++;
}

// (a car of another client's ran into one of ours: the local player as its own collisions with a car do it - a fall and
// 20 -, another ped as the game's KillPedWithCar does)
static void
OnCarHit(uint32 target, In &in)
{
	const uint32 carId = in.U32();
	const float impulse = in.F32();
	const CVector speed = in.Vec();
	const uint32 stamp = in.U32();
	hits.received++;
	CEntity *e = OwnVictim(target);
	if (!in.Ok() || e == nil || !e->IsPed() || Age(stamp) > STALE_HIT || !(impulse >= 0.0f && impulse < 100000.0f)) {
		hits.dropped++;
		return;
	}
	CPed *victim = (CPed *)e;
	CEntity *carEntity = EntityOf(carId);
	CVehicle *car = carEntity && carEntity->IsVehicle() ? (CVehicle *)carEntity : nil;
	if (victim->bInVehicle || victim->DyingOrDead()) {
		hits.dropped++;
		return;
	}
	bApplying = true;
	if (victim == FindPlayerPed() || car == nil) {
		CVector2D push(-speed.x, -speed.y);
		const int32 dir = victim->GetLocalDirection(push);
		victim->bIsStanding = false;
		if (!victim->OnGround())
			victim->SetFall(1000, AnimationId(ANIM_STD_HIGHIMPACT_FRONT + dir), false);
		const float damage = victim == FindPlayerPed() ? 20.0f : (impulse > 12.0f ? 1000.0f : 30.0f);
		victim->InflictDamage(car, WEAPONTYPE_RAMMEDBYCAR, damage, PEDPIECE_TORSO, dir);
		if (victim == FindPlayerPed() && carId != 0) {
			CEntity *driver = car ? Doer(car) : nil;
			lastHit.attacker = driver && driver != car ? IdOf(driver) : carId;
			lastHit.weapon = WEAPONTYPE_RAMMEDBYCAR;
			lastHit.culprit = car;
			lastHit.at = Now();
		}
	} else
		victim->KillPedWithCar(car, impulse);
	bApplying = false;
	hits.applied++;
}

// ---------------------------------------------------------------- shots seen: effects only
static void
MuzzleFlash(int32 weapon, const CVector &start, const CVector &end)
{
	switch (weapon) {
	case WEAPONTYPE_M4:
	case WEAPONTYPE_RUGER:
	case WEAPONTYPE_M60:
	case WEAPONTYPE_MINIGUN:
	case WEAPONTYPE_HELICANNON:
	case WEAPONTYPE_SHOTGUN:
	case WEAPONTYPE_SPAS12_SHOTGUN:
	case WEAPONTYPE_STUBBY_SHOTGUN:
		CWeapon::AddGunFlashBigGuns(start, end);
		break;
	default: {
		CPointLights::AddLight(CPointLights::LIGHT_POINT, start, CVector(0.0f, 0.0f, 0.0f), 5.0f, 1.0f, 0.8f, 0.0f,
		                       CPointLights::FOG_NONE, false);
		CVector ahead = end - start;
		ahead.z = 0.0f;
		ahead.Normalise();
		CVector pos = start;
		CParticle::AddParticle(PARTICLE_GUNFLASH_NOANIM, pos, CVector(0.0f, 0.0f, 0.0f), nil, 0.06f);
		pos += 0.06f * ahead;
		CParticle::AddParticle(PARTICLE_GUNFLASH_NOANIM, pos, CVector(0.0f, 0.0f, 0.0f), nil, 0.04f);
		pos += 0.04f * ahead;
		CParticle::AddParticle(PARTICLE_GUNFLASH_NOANIM, pos, CVector(0.0f, 0.0f, 0.0f), nil, 0.02f);
		CParticle::AddParticle(PARTICLE_GUNSMOKE2, start, 0.1f * ahead, nil, 0.005f);
		break;
	}
	}
}

// (where a shot ended: blood on a ped, sparks and smoke elsewhere)
static void
Impact(const CVector &start, const CVector &end)
{
	CVector dir = end - start;
	if (dir.MagnitudeSqr() < 0.0001f)
		return;
	dir.Normalise();
	CColPoint point;
	CEntity *hit = nil;
	if (CWorld::ProcessLineOfSight(end - 0.5f * dir, end + 0.5f * dir, point, hit, false, false, true, false, false, true) &&
	    hit && hit->IsPed()) {
		if (CGame::nastyGame && hit->GetIsOnScreen()) {
			CVector bloodDir = (point.point - hit->GetPosition()) * 0.01f;
			bloodDir.z = 0.01f;
			for (int32 i = 0; i < 8; i++)
				CParticle::AddParticle(PARTICLE_BLOOD_SMALL, point.point, bloodDir);
		}
		return;
	}
	for (int32 i = 0; i < 8; i++)
		CParticle::AddParticle(PARTICLE_SPARK, end, -0.05f * dir);
	CParticle::AddParticle(PARTICLE_BULLETHIT_SMOKE, end, CVector(0.0f, 0.0f, 0.0f));
}

static void
ShotSound(CEntity *shooter)
{
	if (shooter && (shooter->IsPed() || shooter->IsVehicle()))
		DMAudio.PlayOneShot(((CPhysical *)shooter)->m_audioEntityId, SOUND_WEAPON_SHOT_FIRED, 0.0f);
}

static void
OnShot(In &in)
{
	const uint32 shooterId = in.U32();
	const int32 weapon = in.U8(), flags = in.U8();
	CVector start = in.Vec(), end = in.Vec();
	if (!in.Ok() || weapon >= WEAPONTYPE_TOTALWEAPONS)
		return;
	CEntity *shooter = Culprit(shooterId);
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	if (flags & 1) {
		MuzzleFlash(weapon, start, end);
		ShotSound(shooter);
	}
	CBulletTraces::AddTrace(&start, &end, weapon, shooter);
	Impact(start, end);
	bReplaying = wasReplaying;
}

static void
OnSniper(In &in)
{
	const uint32 shooterId = in.U32();
	const int32 weapon = in.U8();
	const CVector pos = in.Vec(), speed = in.Vec();
	if (!in.Ok() || speed.MagnitudeSqr() < 0.0001f)
		return;
	CEntity *shooter = Culprit(shooterId);
	CVector dir = speed;
	dir.Normalise();
	CVector end = pos + CWeaponInfo::GetWeaponInfo((eWeaponType)weapon)->m_fRange * dir;
	CColPoint point;
	CEntity *hit = nil;
	CWorld::pIgnoreEntity = shooter;
	if (CWorld::ProcessLineOfSight(pos, end, point, hit, true, true, true, true, true, false, false, true))
		end = point.point;
	CWorld::pIgnoreEntity = nil;
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	if (shooter) {
		MuzzleFlash(weapon, shooter->GetPosition() + CVector(0.0f, 0.0f, 0.6f), end);
		ShotSound(shooter);
	}
	Impact(pos, end);
	bReplaying = wasReplaying;
}

// (a remote flame: its particles; its shot, which sets on fire what we own as it goes - PostWorld)
static void
OnFlame(In &in)
{
	const uint32 shooterId = in.U32();
	const CVector start = in.Vec(), end = in.Vec();
	if (!in.Ok())
		return;
	CEntity *shooter = Culprit(shooterId);
	CVector dir = end - start;
	if (dir.MagnitudeSqr() < 0.0001f)
		return;
	dir.Normalise();
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	if (shooter && shooter->IsPed())
		CShotInfo::AddShot(shooter, WEAPONTYPE_FLAMETHROWER, start, end);
	CWeapon::GenerateFlameThrowerParticles(start, dir);
	bReplaying = wasReplaying;
}

// (the remote flames' shots: what they reach of ours catches fire, as CShotInfo::Update does with the shooter's near
// peds - a puppet has none)
static void
RemoteFlames(void)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil)
		return;
	for (int32 slot = 0; slot < NUMSHOTINFOS; slot++) {
		CShotInfo &shot = gaShotInfo[slot];
		if (!shot.m_inUse || shot.m_weapon != WEAPONTYPE_FLAMETHROWER || shot.m_sourceEntity == nil || !IsPuppet(shot.m_sourceEntity))
			continue;
		const float radius = Max(1.0f, shot.m_radius);
		for (int32 i = -1; i < player->m_numNearPeds; i++) {
			CPed *ped = i < 0 ? (CPed *)player : player->m_nearPeds[i];
			if (ped == nil || IsPuppet(ped) || ped->bInVehicle || ped->m_pFire || ped->bFireProof || !ped->IsPedInControl())
				continue;
			if ((ped->GetPosition() - shot.m_startPos).MagnitudeSqr() < radius) {
				if (ped != player) {
					ped->SetFindPathAndFlee(shot.m_sourceEntity, 10000);
					ped->SetMoveState(PEDMOVE_SPRINT);
				}
				gFireManager.StartFire(ped, shot.m_sourceEntity, 0.8f, true);
			}
		}
	}
}

static void
OnMelee(In &in)
{
	const uint32 shooterId = in.U32();
	const int32 weapon = in.U8();
	CEntity *shooter = Culprit(shooterId);
	if (!in.Ok() || shooter == nil || !shooter->IsPed() || weapon == WEAPONTYPE_CHAINSAW)
		return;
	DMAudio.PlayOneShot(((CPed *)shooter)->m_audioEntityId, SOUND_MELEE_ATTACK_START, weapon << 8);
}

// ---------------------------------------------------------------- projectiles
static int32
RemoteProjectile(int32 from, uint32 id)
{
	for (int32 i = 0; i < NUM_PROJECTILES; i++)
		if (projectiles[i].used && projectiles[i].remote && projectiles[i].from == from && projectiles[i].id == id &&
		    gaProjectileInfo[i].m_bInUse && CProjectileInfo::ms_apProjectile[i])
			return i;
	return -1;
}

static void
OnProjectile(int32 from, In &in)
{
	const uint32 shooterId = in.U32(), id = in.U32();
	const int32 weapon = in.U8();
	const CVector pos = in.Vec(), forward = in.Vec(), up = in.Vec(), velocity = in.Vec();
	if (!in.Ok() || RemoteProjectile(from, id) >= 0)
		return;
	if (weapon != WEAPONTYPE_GRENADE && weapon != WEAPONTYPE_MOLOTOV && weapon != WEAPONTYPE_TEARGAS && weapon != WEAPONTYPE_ROCKET)
		return;
	CEntity *shooter = Culprit(shooterId);
	if (shooter == nil || (!shooter->IsPed() && !shooter->IsVehicle()))
		return;
	const int32 model = weapon == WEAPONTYPE_ROCKET ? MI_MISSILE : weapon == WEAPONTYPE_MOLOTOV ? MI_MOLOTOV :
	                    weapon == WEAPONTYPE_TEARGAS ? MI_TEARGAS : MI_GRENADE;
	if (!CStreaming::HasModelLoaded(model)) {
		CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
		return;
	}
	bMakingProjectile = true;
	makingFrom = from;
	makingId = id;
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	const bool made = CProjectileInfo::AddProjectile(shooter, (eWeaponType)weapon, pos, 0.0f);
	bReplaying = wasReplaying;
	bMakingProjectile = false;
	if (!made)
		return;
	const int32 slot = RemoteProjectile(from, id);
	if (slot < 0)
		return;
	// (as the owner threw it)
	CProjectile *p = CProjectileInfo::ms_apProjectile[slot];
	CMatrix &m = p->GetMatrix();
	m.GetForward() = forward;
	m.GetUp() = up;
	m.GetRight() = CrossProduct(up, forward);
	m.GetPosition() = pos;
	p->SetMoveSpeed(velocity);
	m.UpdateRW();
	p->UpdateRwFrame();
	gaProjectileInfo[slot].m_vecPos = pos;
	if (weapon == WEAPONTYPE_ROCKET)
		ShotSound(shooter);
}

// (a remote projectile gone: its copy too, without an explosion - its owner's comes as its own event)
static void
OnProjectileGone(int32 from, In &in)
{
	const uint32 id = in.U32();
	if (!in.Ok())
		return;
	const int32 slot = RemoteProjectile(from, id);
	if (slot >= 0)
		CProjectileInfo::RemoveProjectile(&gaProjectileInfo[slot], CProjectileInfo::ms_apProjectile[slot]);
}

static void
RemoveRemoteProjectiles(void)
{
	for (int32 i = 0; i < NUM_PROJECTILES; i++) {
		if (projectiles[i].used && projectiles[i].remote && gaProjectileInfo[i].m_bInUse && CProjectileInfo::ms_apProjectile[i])
			CProjectileInfo::RemoveProjectile(&gaProjectileInfo[i], CProjectileInfo::ms_apProjectile[i]);
		projectiles[i].used = false;
	}
}

// ---------------------------------------------------------------- explosions
// (a puppet vehicle wrecked as its owner's was: BlowUpCar's own effects and blast, its occupants left to their owners)
static void
Wreck(CVehicle *veh, CEntity *culprit, bool explode)
{
	if (veh->GetStatus() == STATUS_WRECKED)
		return;
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	bWrecking = true;
	if (explode)
		veh->BlowUpCar(culprit);
	else {
		veh->SetStatus(STATUS_WRECKED);
		veh->bRenderScorched = true;
		veh->m_nTimeOfDeath = CTimer::GetTimeInMilliseconds();
		veh->m_fHealth = 0.0f;
		veh->bEngineOn = false;
		veh->bLightsOn = false;
		veh->m_bSirenOrAlarm = false;
		if (veh->IsCar()) {
			((CAutomobile *)veh)->bTaxiLight = false;
			((CAutomobile *)veh)->Damage.FuckCarCompletely();
		}
	}
	bWrecking = false;
	bReplaying = wasReplaying;
}

static void
OnExplosion(In &in)
{
	const uint32 culpritId = in.U32(), explodingId = in.U32();
	const int32 type = in.U8();
	const CVector pos = in.Vec();
	const uint32 lifetime = in.U32();
	const uint32 stamp = in.U32();
	if (!in.Ok() || type > EXPLOSION_HELI_BOMB || Age(stamp) > STALE_EFFECT)
		return;
	CEntity *culprit = Culprit(culpritId);
	CEntity *exploding = Culprit(explodingId);
	// (a vehicle's blast: its copy here blows up with it)
	if (exploding && exploding->IsVehicle() && IsPuppet(exploding) && ((CVehicle *)exploding)->GetStatus() != STATUS_WRECKED &&
	    (type == EXPLOSION_CAR || type == EXPLOSION_CAR_QUICK || type == EXPLOSION_BOAT || type == EXPLOSION_HELI || type == EXPLOSION_HELI2) &&
	    (exploding->GetPosition() - pos).MagnitudeSqr() < 25.0f * 25.0f) {
		Wreck((CVehicle *)exploding, culprit, true);
		return;
	}
	const bool wasReplaying = bReplaying;
	bReplaying = true;
	CExplosion::AddExplosion(exploding, culprit, (eExplosionType)type, pos, lifetime);
	bReplaying = wasReplaying;
}

// ---------------------------------------------------------------- deaths
static void
KillLine(int32 victimSlot, uint32 killer)
{
	const char *victim = NameOf(victimSlot);
	if (victim == nil)
		return;
	const char *by = nil;
	if (IsPlayerNetId(killer) && SlotOfNetId(killer) != victimSlot)
		by = NameOf(SlotOfNetId(killer));
	char line[96];
	if (by)
		snprintf(line, sizeof(line), LangFrench() ? "%s a tu\xC3\xA9 %s" : "%s killed %s", by, victim);
	else
		snprintf(line, sizeof(line), LangFrench() ? "%s est mort(e)" : "%s died", victim);
	Social_AddLine(LINE_KILL, line);
	CopyName(lastVictim, victim, sizeof(lastVictim));
	CopyName(lastKiller, by, sizeof(lastKiller));
	lastDeathKiller = killer;
}

// (the local player died: its killer - the last network hit if it is still the last damage, else the game's own
// record - told to everyone)
static void
TellDeath(void)
{
	if (bDeathTold || !Playing())
		return;
	bDeathTold = true;
	CPlayerPed *ped = FindPlayerPed();
	uint32 killer = 0;
	int32 weapon = ped->m_lastWepDam;
	const bool recent = lastHit.attacker != 0 && Now() - lastHit.at < DEATH_MEMORY;
	if (recent && (ped->m_lastDamEntity == lastHit.culprit || ped->m_lastDamEntity == nil)) {
		killer = lastHit.attacker;
		weapon = lastHit.weapon;
	} else if (Live(ped->m_lastDamEntity))
		killer = IdOf(Doer(ped->m_lastDamEntity));
	const uint32 me = PlayerNetId(LocalSlot());
	{
		Out o(R_EVENT);
		o.U8(EV_DEATH);
		o.U32(me);
		o.U32(killer);
		o.U8(weapon < 0 ? WEAPONTYPE_UNIDENTIFIED : weapon);
	}
	eventsSent++;
	deaths++;
	KillLine(LocalSlot(), killer);
}

static void
OnDeath(int32 from, In &in)
{
	const uint32 victim = in.U32(), killer = in.U32();
	in.U8();
	if (!in.Ok() || !IsPlayerNetId(victim) || SlotOfNetId(victim) != from)
		return;
	if (LocalSlot() != NO_SLOT && killer == PlayerNetId(LocalSlot()))
		kills++;
	KillLine(from, killer);
}

// ---------------------------------------------------------------- pickups
static int32
PickupSlotOf(int32 handle)
{
	const int32 slot = handle & 0xFFFF;
	return handle >= 0 && slot < NUMPICKUPS ? slot : -1;
}

// (a slot's entry still its pickup's)
static bool
PickupCurrent(int32 slot)
{
	return pickups[slot].id != 0 && CPickups::GetActualPickupIndex(pickups[slot].handle) == slot;
}

static bool
PickupModelReady(int32 model)
{
	if (model < 0 || model >= MODELINFOSIZE || CModelInfo::GetModelInfo(model) == nil)
		return false;
	bool ready = CStreaming::HasModelLoaded(model);
	if (!ready)
		CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
	if (CModelInfo::GetModelInfo(model)->GetModelType() == MITYPE_WEAPON) {
		const int32 other = CWeaponInfo::GetWeaponInfo(((CWeaponModelInfo *)CModelInfo::GetModelInfo(model))->GetWeaponInfo())->m_nModel2Id;
		if (other > 0 && other != model && !CStreaming::HasModelLoaded(other)) {
			CStreaming::RequestModel(other, STREAMFLAGS_DEPENDENCY);
			ready = false;
		}
	}
	return ready;
}

static void
MakeRemotePickup(uint32 id, int32 model, int32 type, uint32 quantity, const CVector &pos)
{
	bMakingPickup = true;
	const int32 handle = CPickups::GenerateNewOne(pos, model, type, quantity);
	bMakingPickup = false;
	const int32 slot = PickupSlotOf(handle);
	if (slot < 0)
		return;
	pickups[slot].id = id;
	pickups[slot].handle = handle;
}

static void
OnPickupMade(In &in)
{
	const uint32 id = in.U32();
	const int32 model = in.U16(), type = in.U8();
	const uint32 quantity = in.U32();
	const CVector pos = in.Vec();
	if (!in.Ok() || id == 0 || (type != PICKUP_ONCE_TIMEOUT && type != PICKUP_MONEY))
		return;
	if (PickupModelReady(model)) {
		MakeRemotePickup(id, model, type, quantity, pos);
		return;
	}
	for (int32 i = 0; i < PENDING_PICKUPS; i++)
		if (!pendingPickups[i].used) {
			PendingPickup &p = pendingPickups[i];
			p.used = true;
			p.id = id;
			p.model = model;
			p.type = type;
			p.quantity = quantity;
			p.pos = pos;
			p.until = Now() + 10000.0;
			return;
		}
}

static void
MakePendingPickups(void)
{
	const double now = Now();
	for (int32 i = 0; i < PENDING_PICKUPS; i++) {
		PendingPickup &p = pendingPickups[i];
		if (!p.used)
			continue;
		if (now > p.until)
			p.used = false;
		else if (PickupModelReady(p.model)) {
			p.used = false;
			MakeRemotePickup(p.id, p.model, p.type, p.quantity, p.pos);
		}
	}
}

// (a pickup taken by another: gone here too; one of the save's waits for its own time to come back)
static void
OnPickupTaken(In &in)
{
	const int32 kind = in.U8();
	if (kind == 0) {
		const uint32 id = in.U32();
		if (!in.Ok() || id == 0)
			return;
		for (int32 i = 0; i < PENDING_PICKUPS; i++)
			if (pendingPickups[i].used && pendingPickups[i].id == id)
				pendingPickups[i].used = false;
		for (int32 slot = 0; slot < NUMPICKUPS; slot++)
			if (pickups[slot].id == id) {
				if (PickupCurrent(slot))
					CPickups::RemovePickUp(pickups[slot].handle);
				pickups[slot].id = 0;
			}
		return;
	}
	const int32 slot = in.U16(), model = in.U16();
	const CVector pos = in.Vec();
	if (!in.Ok() || slot >= NUMPICKUPS)
		return;
	CPickup &p = CPickups::aPickUps[slot];
	if (p.m_bRemoved || p.m_eModelIndex != model || (p.m_vecPos - pos).MagnitudeSqr() > 1.0f)
		return;
	switch (p.m_eType) {
	case PICKUP_ON_STREET:
	case PICKUP_ON_STREET_SLOW:
		p.GetRidOfObjects();
		p.m_bRemoved = true;
		if (p.m_eType == PICKUP_ON_STREET)
			p.m_nTimer = CTimer::GetTimeInMilliseconds() + 30000;
		else
			p.m_nTimer = CTimer::GetTimeInMilliseconds() + (model == MI_PICKUP_BRIBE ? 300000 : 720000);
		break;
	case PICKUP_ONCE:
	case PICKUP_ONCE_TIMEOUT:
	case PICKUP_ONCE_TIMEOUT_SLOW:
	case PICKUP_MONEY:
	case PICKUP_COLLECTABLE1:
		p.GetRidOfObjects();
		p.m_bRemoved = true;
		p.m_eType = PICKUP_NONE;
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------- the game's hooks (mmo.h)
bool
PedDamage(CPed *victim, CEntity *damagedBy, int32 method, float damage, int32 pedPiece, uint8 direction, bool *result)
{
	if (Mode() == MODE_OFF)
		return false;
	if (!IsPuppet(victim)) {
		// (ours run into by another client's car: that car's owner tells it - TG_CAR_HIT)
		if (!bApplying && damagedBy && damagedBy->IsVehicle() && IsPuppet(damagedBy) &&
		    (method == WEAPONTYPE_RAMMEDBYCAR || method == WEAPONTYPE_RUNOVERBYCAR)) {
			*result = false;
			return true;
		}
		return false;
	}
	*result = false;
	if (!bReplaying && Playing() && DirectHit(method) && Local(damagedBy))
		SendPedDamage(victim, damagedBy, method, damage, pedPiece, direction);
	return true;
}

bool
PedHitByCar(CPed *victim, CVehicle *car, float impulse)
{
	if (Mode() == MODE_OFF)
		return false;
	if (IsPuppet(victim)) {
		static const CPed *hitVictim;
		static double lastAt;
		const double now = Now();
		if (!bReplaying && Playing() && Local(car) && impulse > 3.0f && !victim->DyingOrDead() &&
		    (victim != hitVictim || now - lastAt > 1000.0)) {
			const uint32 target = IdOf(victim);
			const RemotePlayer *p = IsPlayerNetId(target) ? Player(SlotOfNetId(target)) : nil;
			if (target != 0 && (!IsPlayerNetId(target) || (p && !p->away))) {
				hitVictim = victim;
				lastAt = now;
				Out o(R_TARGETED);
				Targeted(target, TG_CAR_HIT, o);
				o.U32(IdOf(car));
				o.F32(impulse);
				o.Vec(car->m_vecMoveSpeed);
				o.U32(Stamp());
				hits.sent++;
				lastSent.target = target;
				lastSent.method = WEAPONTYPE_RAMMEDBYCAR;
				lastSent.damage = impulse;
				lastSent.at = now;
			}
		}
		return true;
	}
	// (one of ours run into by another client's car: told by that car's owner)
	return !bApplying && IsPuppet(car);
}

bool
VehicleDamage(CVehicle *veh, CEntity *damagedBy, int32 weaponType, float damage, const CVector &pos)
{
	if (!IsPuppet(veh))
		return false;
	if (!bReplaying && Playing() && DirectHit(weaponType) && Local(damagedBy)) {
		const uint32 target = IdOf(veh);
		if (target != 0) {
			Out o(R_TARGETED);
			Targeted(target, TG_VEHICLE_DAMAGE, o);
			o.U32(IdOf(Doer(damagedBy)));
			o.U8(weaponType);
			o.F32(damage);
			o.Vec(pos);
			o.U32(Stamp());
			hits.sent++;
			lastSent.target = target;
			lastSent.method = weaponType;
			lastSent.damage = damage;
			lastSent.at = Now();
		}
	}
	return true;
}

bool
TyreBurst(CVehicle *veh, uint8 wheel)
{
	if (!IsPuppet(veh))
		return false;
	const uint32 target = IdOf(veh);
	if (!bReplaying && Playing() && target != 0) {
		Out o(R_TARGETED);
		Targeted(target, TG_TYRE, o);
		o.U32(IdOf(FindPlayerPed()));
		o.U8(wheel);
		o.U32(Stamp());
		hits.sent++;
	}
	return true;
}

bool
VehicleImpulse(CVehicle *veh, float impulse, uint16 piece)
{
	if (!IsPuppet(veh))
		return false;
	const uint32 target = IdOf(veh);
	if (!bReplaying && Playing() && impulse != 0.0f && target != 0) {
		Out o(R_TARGETED);
		Targeted(target, TG_VEHICLE_IMPULSE, o);
		o.U32(IdOf(FindPlayerPed()));
		o.F32(impulse);
		o.U16(piece);
		o.U32(Stamp());
		hits.sent++;
	}
	return true;
}

bool
KnockOff(CVehicle *bike, CPed *rider, int32 weapon, uint8 direction)
{
	if (rider == nil || !IsPuppet(rider))
		return false;
	const uint32 target = IdOf(rider);
	if (!bReplaying && Playing() && weapon <= WEAPONTYPE_CHAINSAW && target != 0) {
		Out o(R_TARGETED);
		Targeted(target, TG_KNOCK_OFF, o);
		o.U32(IdOf(FindPlayerPed()));
		o.U8(weapon);
		o.U8(direction);
		o.U32(Stamp());
		hits.sent++;
	}
	return true;
}

bool
BlockBlowUp(CVehicle *veh)
{
	return IsPuppet(veh) && !bWrecking;
}

bool
BlockPuppetDeath(CPed *ped)
{
	if (!IsPuppet(ped))
		return false;
	return !bReplaying || bWrecking;
}

bool
BlockFire(CEntity *shooter)
{
	if (shooter == nil || Mode() == MODE_OFF)
		return false;
	if (IsPuppet(shooter))
		return true;
	if (shooter->IsVehicle() && ((CVehicle *)shooter)->pDriver && IsPuppet(((CVehicle *)shooter)->pDriver))
		return true;
	// (the local player's melee swing: heard by the others)
	CPlayerPed *player = FindPlayerPed();
	if (shooter == player && Playing()) {
		CWeapon *weapon = player->GetWeapon();
		const double now = Now();
		if (CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType)->m_eWeaponFire == WEAPON_FIRE_MELEE &&
		    weapon->m_eWeaponState != WEAPONSTATE_FIRING && now - lastMelee > 250.0 && AnyoneNear(player->GetPosition(), 60.0f)) {
			lastMelee = now;
			Out o(R_EVENT);
			o.U8(EV_MELEE);
			o.U32(IdOf(player));
			o.U8(weapon->m_eWeaponType);
			eventsSent++;
		}
	}
	return false;
}

void
BulletTrace(const CVector &start, const CVector &end, int32 weaponType, CEntity *shooter)
{
	if (bReplaying || !Playing() || !Local(shooter))
		return;
	if (shooter == FindPlayerPed()) {
		lastTraceStart = start;
		lastTraceEnd = end;
	}
	if (!AnyoneNear(start, NEAR) || !ShotBudget())
		return;
	// (the muzzle once a shot: a shotgun's pellets share it)
	const bool muzzle = shooter != lastMuzzleShooter || CTimer::GetFrameCounter() != lastMuzzleFrame;
	lastMuzzleShooter = shooter;
	lastMuzzleFrame = CTimer::GetFrameCounter();
	Out o(R_EVENT);
	o.U8(EV_SHOT);
	o.U32(IdOf(shooter));
	o.U8(weaponType);
	o.U8(muzzle ? 1 : 0);
	o.Vec(start);
	o.Vec(end);
	eventsSent++;
}

void
BulletAdded(int32 weaponType, CEntity *source, const CVector &pos, const CVector &speed)
{
	if (bReplaying || !Playing() || !Local(source) || !AnyoneNear(pos, NEAR) || !ShotBudget())
		return;
	Out o(R_EVENT);
	o.U8(EV_SNIPER);
	o.U32(IdOf(source));
	o.U8(weaponType);
	o.Vec(pos);
	o.Vec(speed);
	eventsSent++;
}

void
FlameShot(CEntity *source, const CVector &start, const CVector &end)
{
	if (bReplaying || !Playing() || !Local(source) || !AnyoneNear(start, NEAR))
		return;
	static double lastFlame;
	const double now = Now();
	if (now - lastFlame < 80.0)
		return;
	lastFlame = now;
	Out o(R_EVENT);
	o.U8(EV_FLAME);
	o.U32(IdOf(source));
	o.Vec(start);
	o.Vec(end);
	eventsSent++;
}

void
ProjectileAdded(int32 slot)
{
	if (slot < 0 || slot >= NUM_PROJECTILES || Mode() == MODE_OFF)
		return;
	ProjectileSlot &s = projectiles[slot];
	s.used = true;
	s.told = false;
	if (bMakingProjectile) {
		s.remote = true;
		s.from = makingFrom;
		s.id = makingId;
		return;
	}
	s.remote = false;
	s.from = (uint8)LocalSlot();
	s.id = ++projectileCounter;
	CProjectileInfo &info = gaProjectileInfo[slot];
	CProjectile *p = CProjectileInfo::ms_apProjectile[slot];
	if (bReplaying || !Playing() || p == nil || !Local(info.m_pSource) || !AnyoneNear(p->GetPosition(), NEAR))
		return;
	const uint32 shooter = IdOf(info.m_pSource);
	if (shooter == 0)
		return;
	s.told = true;
	Out o(R_EVENT);
	o.U8(EV_PROJECTILE);
	o.U32(shooter);
	o.U32(s.id);
	o.U8(info.m_eWeaponType);
	o.Vec(p->GetPosition());
	o.Vec(p->GetForward());
	o.Vec(p->GetUp());
	o.Vec(p->m_vecMoveSpeed);
	eventsSent++;
}

bool
ProjectileSilent(int32 slot)
{
	if (slot < 0 || slot >= NUM_PROJECTILES || Mode() == MODE_OFF || !projectiles[slot].used)
		return false;
	ProjectileSlot &s = projectiles[slot];
	s.used = false;
	if (s.remote)
		return true;
	if (s.told && Playing()) {
		Out o(R_EVENT);
		o.U8(EV_PROJECTILE_GONE);
		o.U32(s.id);
		eventsSent++;
	}
	return false;
}

void
ExplosionAdded(CEntity *exploding, CEntity *culprit, int32 type, const CVector &pos, uint32 lifetime)
{
	if (bReplaying || !Playing())
		return;
	// (ours: a vehicle or thing of ours blowing up, or a blast we caused; the world's own - mines, a barrel no one
	// shot - is every client's own)
	const bool ours = exploding ? Local(exploding) : (culprit != nil && Local(culprit));
	if (!ours || !AnyoneNear(pos, NEAR))
		return;
	Out o(R_EVENT);
	o.U8(EV_EXPLOSION);
	o.U32(IdOf(Doer(culprit)));
	o.U32(IdOf(exploding));
	o.U8(type);
	o.Vec(pos);
	o.U32(lifetime);
	o.U32(Stamp());
	eventsSent++;
}

bool
SkipAreaDamage(CEntity *e)
{
	return IsPuppet(e);
}

void
PickupCreated(int32 handle)
{
	const int32 slot = PickupSlotOf(handle);
	if (slot < 0 || bMakingPickup || Mode() == MODE_OFF)
		return;
	pickups[slot].id = 0;
	// (a puppet's death played here: its owner drops the real ones)
	if (bReplaying) {
		CPickups::RemovePickUp(handle);
		return;
	}
	if (!Playing())
		return;
	const CPickup &p = CPickups::aPickUps[slot];
	if ((p.m_eType != PICKUP_ONCE_TIMEOUT && p.m_eType != PICKUP_MONEY) || !AnyoneNear(p.m_vecPos, NEAR))
		return;
	const uint32 id = NewNetId();
	if (id == 0)
		return;
	pickups[slot].id = id;
	pickups[slot].handle = handle;
	Out o(R_EVENT);
	o.U8(EV_PICKUP_MADE);
	o.U32(id);
	o.U16((uint16)p.m_eModelIndex);
	o.U8(p.m_eType);
	o.U32(p.m_nQuantity);
	o.Vec(p.m_vecPos);
	eventsSent++;
}

void
PickupCollected(int32 slot)
{
	if (slot < 0 || slot >= NUMPICKUPS || !Playing())
		return;
	if (PickupCurrent(slot)) {
		Out o(R_EVENT);
		o.U8(EV_PICKUP_TAKEN);
		o.U8(0);
		o.U32(pickups[slot].id);
		pickups[slot].id = 0;
		eventsSent++;
		return;
	}
	pickups[slot].id = 0;
	const CPickup &p = CPickups::aPickUps[slot];
	switch (p.m_eType) {
	case PICKUP_IN_SHOP:
	case PICKUP_IN_SHOP_OUT_OF_STOCK:
	case PICKUP_ASSET_REVENUE:
	case PICKUP_PROPERTY_LOCKED:
	case PICKUP_PROPERTY_FORSALE:
		return;
	}
	Out o(R_EVENT);
	o.U8(EV_PICKUP_TAKEN);
	o.U8(1);
	o.U16(slot);
	o.U16((uint16)p.m_eModelIndex);
	o.Vec(p.m_vecPos);
	eventsSent++;
}

// ---------------------------------------------------------------- the module's handlers (mmo_internal.h)
void
Combat_PreWorld(void)
{
	if (Mode() != MODE_ACTIVE)
		return;
	MakePendingPickups();
}

void
Combat_PostWorld(void)
{
	if (!Playing())
		return;
	CPlayerPed *ped = FindPlayerPed();
	if (ped->DyingOrDead())
		TellDeath();
	RemoteFlames();
}

void
Combat_Reset(void)
{
	RemoveRemoteProjectiles();
	for (int32 slot = 0; slot < NUMPICKUPS; slot++)
		pickups[slot].id = 0;
	for (int32 i = 0; i < PENDING_PICKUPS; i++)
		pendingPickups[i].used = false;
	lastHit.attacker = 0;
	lastHit.culprit = nil;
	bDeathTold = false;
	bApplying = bWrecking = bMakingProjectile = bMakingPickup = false;
	lastMuzzleShooter = nil;
}

void
Combat_OnEvent(int32 from, int32 sub, In &in)
{
	if (Mode() != MODE_ACTIVE || from == LocalSlot())
		return;
	eventsReceived++;
	if (sub - SUB_COMBAT >= 0 && sub - SUB_COMBAT < (int32)ARRAY_SIZE(seen))
		seen[sub - SUB_COMBAT]++;
	switch (sub) {
	case EV_SHOT: OnShot(in); break;
	case EV_SNIPER: OnSniper(in); break;
	case EV_FLAME: OnFlame(in); break;
	case EV_PROJECTILE: OnProjectile(from, in); break;
	case EV_PROJECTILE_GONE: OnProjectileGone(from, in); break;
	case EV_EXPLOSION: OnExplosion(in); break;
	case EV_DEATH: OnDeath(from, in); break;
	case EV_PICKUP_MADE: OnPickupMade(in); break;
	case EV_PICKUP_TAKEN: OnPickupTaken(in); break;
	case EV_MELEE: OnMelee(in); break;
	default: break;
	}
}

void
Combat_OnTargeted(int32 from, uint32 target, int32 sub, In &in)
{
	if (Mode() != MODE_ACTIVE)
		return;
	switch (sub) {
	case TG_PED_DAMAGE: OnPedDamage(from, target, in); break;
	case TG_VEHICLE_DAMAGE:
	case TG_TYRE:
	case TG_VEHICLE_IMPULSE: OnVehicleDamage(target, sub, in); break;
	case TG_KNOCK_OFF: OnKnockOff(target, in); break;
	case TG_CAR_HIT: OnCarHit(target, in); break;
	default: break;
	}
}

void
Combat_PlayerDied(void)
{
	TellDeath();
}

void
Combat_PlayerRespawned(void)
{
	bDeathTold = false;
	lastHit.attacker = 0;
	lastHit.culprit = nil;
}

void
Combat_WreckPuppet(CVehicle *veh, bool explode)
{
	if (veh && IsPuppet(veh))
		Wreck(veh, nil, explode);
}

// ---------------------------------------------------------------- test commands (?mmotest)
// (the remote player's puppet aimed at by "aim", else the nearest one in front within the weapon's range)
static int32 aimSlot = -1;

static CEntity *
AimTarget(CPlayerPed *ped, float range)
{
	if (aimSlot >= 0) {
		const RemotePlayer *p = Player(aimSlot);
		if (p && p->puppet)
			return p->puppet;
	}
	CEntity *best = nil;
	float bestDist = range;
	const CVector &fwd = ped->GetForward();
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		const RemotePlayer *p = Player(i);
		if (p == nil || p->puppet == nil)
			continue;
		CVector to = p->puppet->GetPosition() - ped->GetPosition();
		const float dist = to.Magnitude();
		if (dist < 0.1f || dist > bestDist)
			continue;
		if (DotProduct(to, fwd) / dist < 0.7f)
			continue;
		best = p->puppet;
		bestDist = dist;
	}
	return best;
}

static void
Face(CPlayerPed *ped, const CVector &pos)
{
	const float heading = CGeneral::GetRadianAngleBetweenPoints(pos.x, pos.y, ped->GetPosition().x, ped->GetPosition().y);
	ped->m_fRotationCur = ped->m_fRotationDest = CGeneral::LimitRadianAngle(heading);
	ped->SetHeading(ped->m_fRotationCur);
	ped->GetMatrix().UpdateRW();
	ped->UpdateRwFrame();
}

// (the last "fire": its target, what a line from the hand to it meets - the debug state's)
static struct { uint32 target; int32 hit; CVector end; bool fired; int32 state; } lastFire;

// (one shot of the held weapon as the player's attack makes it: at the puppet aimed at as a lock-on does, else straight
// ahead)
static bool
FireOnce(int32 slot)
{
	CPlayerPed *ped = FindPlayerPed();
	if (ped == nil || ped->DyingOrDead() || ped->bInVehicle)
		return false;
	CWeapon *weapon = ped->GetWeapon();
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(weapon->m_eWeaponType);
	CEntity *target = nil;
	if (slot >= 0) {
		const RemotePlayer *p = Player(slot);
		target = p ? p->puppet : nil;
	} else
		target = AimTarget(ped, info->m_fRange);
	if (target)
		Face(ped, target->GetPosition());
	// (the muzzle where an aimed weapon is, whatever the animation of the moment puts the hand at)
	CVector source = ped->GetMatrix() * CVector(0.0f, 0.6f, 0.5f);
	// (what the shot's line meets, for the debug state)
	CVector aimAt = target ? target->GetPosition() : source + info->m_fRange * ped->GetForward();
	CColPoint point;
	CEntity *hit = nil;
	CWorld::pIgnoreEntity = ped;
	CWorld::bIncludeBikers = true;
	lastFire.hit = 0;
	lastFire.end = aimAt;
	if (CWorld::ProcessLineOfSight(source, aimAt, point, hit, true, true, true, true, true, false, false, true) && hit) {
		lastFire.end = point.point;
		lastFire.hit = IsPuppet(hit) ? 9 : hit->GetType();
	}
	CWorld::bIncludeBikers = false;
	CWorld::pIgnoreEntity = nil;
	lastFire.target = IdOf(target);
	lastFire.state = weapon->m_eWeaponState;

	CEntity *oldPointAt = ped->m_pPointGunAt;
	const uint32 oldFlags = ped->m_pedIK.m_flags;
	const bool oldMouse = CCamera::m_bUseMouse3rdPerson;
	if (target) {
		ped->m_pPointGunAt = target;
		ped->m_pedIK.m_flags |= CPedIK::GUN_POINTED_SUCCESSFULLY;
	}
	CCamera::m_bUseMouse3rdPerson = false;
	if (weapon->m_eWeaponState == WEAPONSTATE_FIRING && weapon->m_nTimer < CTimer::GetTimeInMilliseconds())
		weapon->m_eWeaponState = WEAPONSTATE_READY;
	// (one press: the first shot's accuracy; a throw held a moment)
	const float oldCounter = ped->m_fAttackButtonCounter;
	ped->m_fAttackButtonCounter = info->m_eWeaponFire == WEAPON_FIRE_PROJECTILE ? 8.0f : 0.0f;
	const bool fired = weapon->Fire(ped, &source);
	ped->m_fAttackButtonCounter = oldCounter;
	CCamera::m_bUseMouse3rdPerson = oldMouse;
	ped->m_pPointGunAt = oldPointAt;
	ped->m_pedIK.m_flags = oldFlags;
	lastFire.fired = fired;
	return fired;
}

static bool
Word(const char *cmd, const char *word, const char **args)
{
	const size_t k = strlen(word);
	if (strncmp(cmd, word, k) != 0 || (cmd[k] != ' ' && cmd[k] != '\0'))
		return false;
	*args = cmd + k;
	while (**args == ' ')
		(*args)++;
	return true;
}

bool
Combat_TestCommand(const char *cmd)
{
	const char *args;
	if (Word(cmd, "fire", &args)) {
		// fire [slot]: at that player's puppet, else ahead (the puppet in front locked on)
		const bool fired = Playing() && FireOnce(*args ? atoi(args) : -1);
		fireCommands++;
		if (fired)
			firedCommands++;
		return true;
	}
	if (Word(cmd, "aim", &args)) {
		// (the players module turns the player itself when it has this command; here, the fallback)
		const int32 slot = atoi(args);
		const RemotePlayer *p = Player(slot);
		aimSlot = p ? slot : -1;
		CPlayerPed *ped = Playing() ? FindPlayerPed() : nil;
		if (ped && p && !ped->bInVehicle)
			Face(ped, p->puppet ? p->puppet->GetPosition() : p->pos);
		return true;
	}
	if (Word(cmd, "hit", &args)) {
		// hit <slot> <damage>: a TARGETED damage to that player, no shot (an administrator's, like the cheats)
		int32 slot = -1;
		float damage = 25.0f;
		sscanf(args, "%d %f", &slot, &damage);
		if (!Playing() || !IsAdmin() || Player(slot) == nil || slot == LocalSlot())
			return true;
		CPlayerPed *ped = FindPlayerPed();
		const int32 weapon = ped->GetWeapon()->m_eWeaponType != WEAPONTYPE_UNARMED ? ped->GetWeapon()->m_eWeaponType : WEAPONTYPE_COLT45;
		Out o(R_TARGETED);
		Targeted(PlayerNetId(slot), TG_PED_DAMAGE, o);
		o.U32(PlayerNetId(LocalSlot()));
		o.U8(weapon);
		o.F32(damage);
		o.U8(PEDPIECE_TORSO);
		o.U8(0);
		o.U8(REACT_WEAPON);
		o.U8(0);
		o.U8(0);
		o.Vec(ped->GetPosition());
		o.U32(Stamp());
		hits.sent++;
		lastSent.target = PlayerNetId(slot);
		lastSent.method = weapon;
		lastSent.damage = damage;
		lastSent.at = Now();
		return true;
	}
	if (Word(cmd, "pickup", &args)) {
		// pickup [amount]: money dropped 4 m ahead of the player, as a dead ped's (an administrator's)
		if (!Playing() || !IsAdmin())
			return true;
		CPlayerPed *ped = FindPlayerPed();
		const int32 amount = *args ? atoi(args) : 10;
		CPickups::CreateSomeMoney(ped->GetPosition() + 4.0f * ped->GetForward(), Clamp(amount, 1, 10000));
		return true;
	}
	if (Word(cmd, "explode", &args)) {
		// explode: a grenade's blast 5 m ahead of the player (an administrator's)
		if (!Playing() || !IsAdmin())
			return true;
		CPlayerPed *ped = FindPlayerPed();
		const CVector pos = ped->GetPosition() + 5.0f * ped->GetForward();
		CExplosion::AddExplosion(nil, ped, EXPLOSION_GRENADE, pos, 0);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- debug (?mmotest)
void
Combat_Debug(void)
{
	int32 local = 0, remote = 0, made = 0;
	for (int32 i = 0; i < NUM_PROJECTILES; i++)
		if (projectiles[i].used && gaProjectileInfo[i].m_bInUse)
			projectiles[i].remote ? remote++ : local++;
	for (int32 slot = 0; slot < NUMPICKUPS; slot++)
		if (pickups[slot].id != 0)
			made++;
	const double now = Now();
	DebugAppend(",\"combat\":{\"hitsSent\":%d,\"hitsReceived\":%d,\"hitsApplied\":%d,\"hitsDropped\":%d", hits.sent,
	            hits.received, hits.applied, hits.dropped);
	DebugAppend(",\"lastSent\":{\"target\":%u,\"method\":%d,\"damage\":%.1f,\"ago\":%.0f}", lastSent.target, lastSent.method,
	            lastSent.damage, lastSent.at > 0.0 ? now - lastSent.at : -1.0);
	DebugAppend(",\"lastReceived\":{\"attacker\":%u,\"method\":%d,\"damage\":%.1f,\"ago\":%.0f}", lastReceived.target,
	            lastReceived.method, lastReceived.damage, lastReceived.at > 0.0 ? now - lastReceived.at : -1.0);
	DebugAppend(",\"fires\":%d,\"fired\":%d,\"lastFire\":{\"target\":%u,\"fired\":%s,\"state\":%d,\"hit\":%d,\"end\":[%.2f,%.2f,%.2f]}",
	            fireCommands, firedCommands, lastFire.target, lastFire.fired ? "true" : "false", lastFire.state, lastFire.hit,
	            lastFire.end.x, lastFire.end.y, lastFire.end.z);
	DebugAppend(",\"seen\":{\"shots\":%d,\"snipers\":%d,\"flames\":%d,\"projectiles\":%d,\"projectilesGone\":%d,"
	            "\"explosions\":%d,\"deaths\":%d,\"pickupsMade\":%d,\"pickupsTaken\":%d,\"melee\":%d}",
	            seen[EV_SHOT - SUB_COMBAT], seen[EV_SNIPER - SUB_COMBAT], seen[EV_FLAME - SUB_COMBAT],
	            seen[EV_PROJECTILE - SUB_COMBAT], seen[EV_PROJECTILE_GONE - SUB_COMBAT], seen[EV_EXPLOSION - SUB_COMBAT],
	            seen[EV_DEATH - SUB_COMBAT], seen[EV_PICKUP_MADE - SUB_COMBAT], seen[EV_PICKUP_TAKEN - SUB_COMBAT],
	            seen[EV_MELEE - SUB_COMBAT]);
	DebugAppend(",\"pickups\":[");
	for (int32 slot = 0, k = 0; slot < NUMPICKUPS && k < 8; slot++)
		if (PickupCurrent(slot) && !CPickups::aPickUps[slot].m_bRemoved) {
			const CVector &pos = CPickups::aPickUps[slot].m_vecPos;
			DebugAppend("%s[%.2f,%.2f,%.2f]", k++ ? "," : "", pos.x, pos.y, pos.z);
		}
	DebugAppend("]");
	DebugAppend(",\"lastTrace\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]", lastTraceStart.x, lastTraceStart.y, lastTraceStart.z,
	            lastTraceEnd.x, lastTraceEnd.y, lastTraceEnd.z);
	DebugAppend(",\"eventsSent\":%d,\"eventsReceived\":%d,\"kills\":%d,\"deaths\":%d,\"lastKiller\":\"", eventsSent,
	            eventsReceived, kills, deaths);
	for (const char *c = lastKiller; *c; c++)
		if (*c != '"' && *c != '\\' && (uint8)*c >= 0x20)
			DebugAppend("%c", *c);
	DebugAppend("\",\"lastVictim\":\"");
	for (const char *c = lastVictim; *c; c++)
		if (*c != '"' && *c != '\\' && (uint8)*c >= 0x20)
			DebugAppend("%c", *c);
	DebugAppend("\",\"lastDeathKiller\":%u,\"deathTold\":%s,\"projectiles\":{\"local\":%d,\"remote\":%d},\"pickupsMade\":%d,\"aim\":%d}",
	            lastDeathKiller, bDeathTold ? "true" : "false", local, remote, made, aimSlot);
}
}
#endif
