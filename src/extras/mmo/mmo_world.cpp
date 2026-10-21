// The shared world's own state (docs/mmo.md, section 7 "world"; owner's decisions 4 and 7): the clock the server keeps
// (CClock), the weather it gives (the game's 64-hour list read from the clock, or the admins' forced weather), the
// traffic lights on the server's milliseconds, the world cheats as absolute state (world variables 1 and 2, the clock's
// rate, world events), and the hideouts' garages shared through the server's records and locks.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "Automobile.h"
#include "CarCtrl.h"
#include "Clock.h"
#include "Gangs.h"
#include "Garages.h"
#include "HandlingMgr.h"
#include "Hud.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Ped.h"
#include "PedType.h"
#include "PlayerInfo.h"
#include "Pools.h"
#include "Population.h"
#include "Stats.h"
#include "Streaming.h"
#include "Text.h"
#include "Timer.h"
#include "TrafficLights.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "Weather.h"

#include <ctype.h>
#include <math.h>

// (the game's own: its weather lists, Weather.cpp; the big wheels' changes, Pad.cpp)
extern int16 WeatherTypesList[];
extern int16 WeatherTypesList_WithHurricanes[];
void SpecialCarCheats();

namespace OrthrosMmo
{
// ---------------------------------------------------------------- when the world's state is the game's
// (a shared world loaded in this page — the game's tables exist — and being played, not being loaded again)
static bool bLoaded;

static bool
Ready(void)
{
	return bLoaded && IsActive() && !IsEntering();
}

// (requests go to the server only while the link is open in the world: none queued for a link coming back)
static bool
CanSend(void)
{
	return IsActive() && LinkState() == LINK_OPEN;
}

static const char *
Pick(const char *en, const char *fr)
{
	return LangFrench() ? fr : en;
}

// ---------------------------------------------------------------- the world cheats (variable 2: a bit each)
// Everything that changes a flag the whole world reads (traffic, vehicles' handling, pedestrians) is a world cheat;
// the rest (weapons, health, armour, wanted level, skins, spawned vehicles, the player's own car) is personal.
enum {
	WC_PINK_CARS = 1 << 0, WC_BLACK_CARS = 1 << 1, WC_GREEN_LIGHTS = 1 << 2, WC_MAD_DRIVERS = 1 << 3,
	WC_FLYING_CARS = 1 << 4, WC_FLYING_BOATS = 1 << 5, WC_HOVER_CARS = 1 << 6, WC_STRONG_GRIP = 1 << 7,
	WC_WHEELS_ONLY = 1 << 8, WC_HELI_CARS = 1 << 9, WC_RIOT = 1 << 10, WC_HATE_PLAYER = 1 << 11,
	WC_ARMED_PEDS = 1 << 12, WC_CHICKS_WITH_GUNS = 1 << 13, WC_FANNY_MAGNET = 1 << 14, WC_BIG_WHEELS = 1 << 15,
};
static const struct { const char *code; int32 bit; const char *en, *fr; } worldCheats[] = {
	{ "AHAIRDRESSERSCAR", WC_PINK_CARS, "Pink traffic", "Circulation rose" },
	{ "IWANTITPAINTEDBLACK", WC_BLACK_CARS, "Black traffic", "Circulation noire" },
	{ "GREENLIGHT", WC_GREEN_LIGHTS, "Lights stay green", "Feux toujours verts" },
	{ "MIAMITRAFFIC", WC_MAD_DRIVERS, "Aggressive drivers", "Conducteurs agressifs" },
	{ "COMEFLYWITHME", WC_FLYING_CARS, "Cars fly", "Voitures volantes" },
	{ "AIRSHIP", WC_FLYING_BOATS, "Boats fly", "Bateaux volants" },
	{ "SEAWAYS", WC_HOVER_CARS, "Cars drive on water", "Voitures sur l'eau" },
	{ "GRIPISEVERYTHING", WC_STRONG_GRIP, "Perfect handling", "Tenue de route parfaite" },
	{ "WHEELSAREALLINEED", WC_WHEELS_ONLY, "Only the wheels show", "Seulement les roues" },
	{ "CARSAREHELI", WC_HELI_CARS, "Cars fly like helicopters", "Voitures hélicoptères" },
	{ "FIGHTFIGHTFIGHT", WC_RIOT, "Riot", "Bagarre générale" },
	{ "NOBODYLIKESME", WC_HATE_PLAYER, "Everyone attacks you", "Tout le monde vous attaque" },
	{ "OURGODGIVENRIGHTTOBEARARMS", WC_ARMED_PEDS, "Everyone is armed", "Tout le monde est armé" },
	{ "CHICKSWITHGUNS", WC_CHICKS_WITH_GUNS, "Armed girls in bikinis", "Filles en bikini armées" },
	{ "FANNYMAGNET", WC_FANNY_MAGNET, "Women follow you", "Les femmes vous suivent" },
	// (the game cannot undo it: what it changes is kept before and put back here, BigWheels)
	{ "LOADSOFLITTLETHINGS", WC_BIG_WHEELS, "Big wheels, more power", "Grosses roues, plus de puissance" },
};
static const struct { const char *code; int16 weather; } weatherCheats[] = {
	{ "APLEASANTDAY", WEATHER_SUNNY },
	{ "ALOVELYDAY", WEATHER_EXTRA_SUNNY },
	{ "ABITDRIEG", WEATHER_CLOUDY },
	{ "CATSANDDOGS", WEATHER_RAINY },
	{ "CANTSEEATHING", WEATHER_FOGGY },
};
static const char *weatherNames[WEATHER_TOTAL][2] = {
	{ "Sunny", "Ensoleillé" }, { "Cloudy", "Nuageux" }, { "Rain", "Pluie" }, { "Fog", "Brouillard" },
	{ "Very sunny", "Grand soleil" }, { "Hurricane", "Ouragan" },
};
// (LIFEISPASSINGMEBY: the server's clock between the game's own rate and about a minute a frame, as gbFastTime)
static const float NORMAL_MS_PER_MINUTE = 1000.0f, FAST_MS_PER_MINUTE = 33.0f, FAST_BELOW = 500.0f;

// (what is applied to the game now; the threats and the player's gang as the world's load left them)
static uint32 appliedSerial;
static int32 appliedBits;
static int32 appliedForced = WEATHER_RANDOM;
static bool appliedFast;
static uint32 baseThreats[NUM_PEDTYPES];
static bool threatsChanged;
static int32 baseGangModel1, baseGangModel2, baseGangWeapon1, baseGangWeapon2;
static bool chicksApplied;

// (requests sent and not answered yet: the next one is made from them, so two quick cheats are both kept, until the
// world's state says the same, or for REQUEST_MS: a refused one)
static const double REQUEST_MS = 3000.0;
static bool cheatsAsked, weatherAsked, rateAsked;
static int32 askedCheats, askedWeather;
static bool askedFast;
static double cheatsAskedAt, weatherAskedAt, rateAskedAt;

// (LOADSOFLITTLETHINGS, the game's SpecialCarCheats: the wheel scales and handling values it changes kept before, put
// back when the world's cheat goes or the world is left: the handling is loaded once for the page's life)
static const int32 bigWheelModels[] = { MI_INFERNUS, MI_CHEETAH, MI_PHEONIX, MI_DELUXO };
static const tVehicleType bigWheelHandling[] = {
	HANDLING_LANDSTAL, HANDLING_PATRIOT, HANDLING_BOBCAT, HANDLING_BFINJECT, HANDLING_RANCHER, HANDLING_DESPERAD,
	HANDLING_DELUXO, HANDLING_BAGGAGE, HANDLING_GOLFCART, HANDLING_STINGER, HANDLING_BANSHEE, HANDLING_SABRETUR,
	HANDLING_COMET,
};
static float savedWheelScale[ARRAY_SIZE(bigWheelModels)];
static struct { float acceleration, maxVelocity, damage, mass, turnMass; tGear gears[6]; } savedHandling[ARRAY_SIZE(bigWheelHandling)];
static bool bigWheelsApplied;

static void
BigWheels(bool on)
{
	if (on == bigWheelsApplied)
		return;
	if (on) {
		// (the game's own flag already set: nothing changed by it, nothing to put back)
		if (CVehicle::bCheat9)
			return;
		for (int i = 0; i < (int)ARRAY_SIZE(bigWheelModels); i++)
			savedWheelScale[i] = ((CVehicleModelInfo *)CModelInfo::GetModelInfo(bigWheelModels[i]))->m_wheelScale;
		for (int i = 0; i < (int)ARRAY_SIZE(bigWheelHandling); i++) {
			tHandlingData *hd = mod_HandlingManager.GetHandlingData(bigWheelHandling[i]);
			savedHandling[i].acceleration = hd->Transmission.fEngineAcceleration;
			savedHandling[i].maxVelocity = hd->Transmission.fMaxVelocity;
			savedHandling[i].damage = hd->fCollisionDamageMultiplier;
			savedHandling[i].mass = hd->fMass;
			savedHandling[i].turnMass = hd->fTurnMass;
			memcpy(savedHandling[i].gears, hd->Transmission.Gears, sizeof(savedHandling[i].gears));
		}
		SpecialCarCheats();
		bigWheelsApplied = true;
		return;
	}
	for (int i = 0; i < (int)ARRAY_SIZE(bigWheelModels); i++) {
		CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::GetModelInfo(bigWheelModels[i]);
		if (mi)
			mi->m_wheelScale = savedWheelScale[i];
	}
	for (int i = 0; i < (int)ARRAY_SIZE(bigWheelHandling); i++) {
		tHandlingData *hd = mod_HandlingManager.GetHandlingData(bigWheelHandling[i]);
		hd->Transmission.fEngineAcceleration = savedHandling[i].acceleration;
		hd->Transmission.fMaxVelocity = savedHandling[i].maxVelocity;
		hd->fCollisionDamageMultiplier = savedHandling[i].damage;
		hd->fMass = savedHandling[i].mass;
		hd->fTurnMass = savedHandling[i].turnMass;
		memcpy(hd->Transmission.Gears, savedHandling[i].gears, sizeof(savedHandling[i].gears));
	}
	CVehicle::bCheat9 = false;
	bigWheelsApplied = false;
}

static int32
CheatBits(void)
{
	const WorldState &w = World();
	return w.valid && w.varSet[WV_CHEATS] ? w.vars[WV_CHEATS] : 0;
}

static int32
ForcedWeather(void)
{
	const WorldState &w = World();
	int32 f = w.valid && w.varSet[WV_FORCED_WEATHER] ? w.vars[WV_FORCED_WEATHER] : WEATHER_RANDOM;
	return f >= 0 && f < WEATHER_TOTAL ? f : WEATHER_RANDOM;
}

static void
SendVar(int32 var, int32 value)
{
	Out o(C_WORLD_SET);
	o.U8(1);
	o.U8(var);
	o.I32(value);
}

static void
SetPlayerGang(int32 model1, int32 model2, int32 weapon1, int32 weapon2)
{
	// (as CHICKSWITHGUNS: the gang's current models let go, then the new ones)
	int32 old[2] = { CGangs::GetGangPedModel1(GANG_PLAYER), CGangs::GetGangPedModel2(GANG_PLAYER) };
	for (int i = 0; i < 2; i++)
		if (old[i] > 0 && old[i] < MODELINFOSIZE && CModelInfo::GetModelInfo(old[i])) {
			CStreaming::SetModelIsDeletable(old[i]);
			CStreaming::SetModelTxdIsDeletable(old[i]);
		}
	CStreaming::RemoveCurrentZonesModels();
	CGangs::SetGangPedModels(GANG_PLAYER, model1, model2);
	CGangs::SetGangWeapons(GANG_PLAYER, weapon1, weapon2);
}

static void
ApplyThreats(int32 bits)
{
	if (!(bits & (WC_RIOT | WC_HATE_PLAYER)) && !threatsChanged)
		return;
	for (int i = PEDTYPE_CIVMALE; i < PEDTYPE_SPECIAL; i++) {
		uint32 threats = baseThreats[i];
		if (bits & WC_RIOT) // (FIGHTFIGHTFIGHT's)
			threats = PED_FLAG_PLAYER1 | PED_FLAG_PLAYER2 | PED_FLAG_PLAYER3 | PED_FLAG_PLAYER4 |
				PED_FLAG_CIVMALE | PED_FLAG_CIVFEMALE | PED_FLAG_COP | PED_FLAG_GANG1 |
				PED_FLAG_GANG2 | PED_FLAG_GANG3 | PED_FLAG_GANG4 | PED_FLAG_GANG5 |
				PED_FLAG_GANG6 | PED_FLAG_GANG7 | PED_FLAG_GANG8 | PED_FLAG_GANG9 |
				PED_FLAG_EMERGENCY | PED_FLAG_PROSTITUTE | PED_FLAG_CRIMINAL | PED_FLAG_SPECIAL;
		if (bits & WC_HATE_PLAYER) // (NOBODYLIKESME's)
			threats |= PED_FLAG_PLAYER1;
		CPedType::SetThreats(i, threats);
	}
	threatsChanged = (bits & (WC_RIOT | WC_HATE_PLAYER)) != 0;
}

// (every flag set or cleared from the bits; a change after the world's first state is shown to the player)
static void
ApplyCheatBits(int32 bits, bool initial)
{
	gbPinkCars = (bits & WC_PINK_CARS) != 0;
	gbBlackCars = (bits & WC_BLACK_CARS) != 0 && !gbPinkCars;
	CTrafficLights::bGreenLightsCheat = (bits & WC_GREEN_LIGHTS) != 0;
	CCarCtrl::bMadDriversCheat = (bits & WC_MAD_DRIVERS) != 0;
	CVehicle::bAllDodosCheat = (bits & WC_FLYING_CARS) != 0;
	CVehicle::bCheat8 = (bits & WC_FLYING_BOATS) != 0;
	CVehicle::bHoverCheat = (bits & WC_HOVER_CARS) != 0;
	CVehicle::bCheat3 = (bits & WC_STRONG_GRIP) != 0;
	CVehicle::bWheelsOnlyCheat = (bits & WC_WHEELS_ONLY) != 0;
#ifdef RESTORE_ALLCARSHELI_CHEAT
	bAllCarCheat = (bits & WC_HELI_CARS) != 0;
#endif
	CPopulation::ms_bGivePedsWeapons = (bits & WC_ARMED_PEDS) != 0;
	CPed::bFannyMagnetCheat = (bits & WC_FANNY_MAGNET) != 0;
	if (initial || ((bits ^ appliedBits) & (WC_RIOT | WC_HATE_PLAYER)))
		ApplyThreats(bits);
	bool chicks = (bits & WC_CHICKS_WITH_GUNS) != 0;
	if (chicks && !chicksApplied)
		SetPlayerGang(MI_HFYBE, MI_WFYBE, WEAPONTYPE_M4, WEAPONTYPE_M4);
	else if (!chicks && chicksApplied)
		SetPlayerGang(baseGangModel1, baseGangModel2, baseGangWeapon1, baseGangWeapon2);
	chicksApplied = chicks;
	BigWheels((bits & WC_BIG_WHEELS) != 0);
	int32 changed = bits ^ appliedBits;
	appliedBits = bits;
	if (initial || changed == 0)
		return;
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	for (int i = 0; i < (int)ARRAY_SIZE(worldCheats); i++)
		if (changed & worldCheats[i].bit) {
			char line[96];
			bool on = (bits & worldCheats[i].bit) != 0;
			snprintf(line, sizeof(line), "%s%s", Pick(worldCheats[i].en, worldCheats[i].fr),
				on ? Pick(": on", " : activé") : Pick(": off", " : désactivé"));
			Social_AddLine(LINE_NOTICE, line);
		}
}

// ---------------------------------------------------------------- the clock and the weather
// (the weather of a game hour: the forced one, else the game's list read at that hour — the hour before gives the
// weather it comes from, as the game's own new-hour step)
static void
SetWeather(int64 hours)
{
	int32 forced = ForcedWeather();
	int32 inList = (int32)(hours % 64);
	CWeather::WeatherTypeInList = inList;
	if (forced >= 0) {
		CWeather::OldWeatherType = forced;
		CWeather::NewWeatherType = forced;
		CWeather::ForcedWeatherType = forced;
		return;
	}
	const int16 *list = CStats::NoMoreHurricanes ? WeatherTypesList : WeatherTypesList_WithHurricanes;
	CWeather::ForcedWeatherType = WEATHER_RANDOM;
	CWeather::NewWeatherType = list[inList];
	CWeather::OldWeatherType = list[(inList + 63) % 64];
}

// (the game's clock set from the server's minutes: hours, minutes, seconds, its last tick as if it had counted them,
// the days the world has lived; then that hour's weather)
static bool
SetClock(void)
{
	const WorldState &w = World();
	if (!w.valid)
		return false;
	double minutes = WorldMinutes();
	if (!(minutes >= 0.0))
		minutes = 0.0;
	double whole = floor(minutes);
	int64 total = (int64)whole;
	float frac = (float)(minutes - whole);
	CClock::ms_nGameClockHours = (uint8)((total / 60) % 24);
	CClock::ms_nGameClockMinutes = (uint8)(total % 60);
	CClock::ms_nGameClockSeconds = (uint16)(frac * 60.0f);
	CClock::ms_nLastClockTick = CTimer::GetTimeInMilliseconds() - (uint32)(frac * CClock::ms_nMillisecondsPerGameMinute);
	CStats::DaysPassed = (int32)(total / (24 * 60));
	SetWeather(total / 60);
	return true;
}

bool
ClockUpdate(void)
{
	if (!Ready())
		return false;
	return SetClock();
}

bool
WeatherUpdate(void)
{
	// (the clock's step already gave this hour's weather: the game's own new hour is not taken)
	return Ready() && World().valid;
}

uint32
LightsTime(void)
{
	if (Ready()) {
		double t = ServerNow();
		if (t > 0.0)
			return (uint32)(uint64)t;
	}
	return CTimer::GetTimeInMilliseconds();
}

// ---------------------------------------------------------------- the world's state applied
static void
ApplyWorld(bool initial)
{
	const WorldState &w = World();
	appliedSerial = w.serial;
	ApplyCheatBits(CheatBits(), initial);
	int32 forced = ForcedWeather();
	if (initial || forced != appliedForced) {
		if (forced >= 0)
			CWeather::ForceWeatherNow(forced);
		else
			CWeather::ReleaseWeather();
		if (!initial) {
			char line[64];
			CHud::SetHelpMessage(TheText.Get("CHEAT7"), true);
			if (forced >= 0)
				snprintf(line, sizeof(line), "%s%s", Pick("Weather: ", "Météo : "), Pick(weatherNames[forced][0], weatherNames[forced][1]));
			else
				snprintf(line, sizeof(line), "%s", Pick("Weather: its own cycle", "Météo : son cycle normal"));
			Social_AddLine(LINE_NOTICE, line);
		}
		appliedForced = forced;
	}
	bool fast = w.valid && w.msPerMinute > 0.0f && w.msPerMinute < FAST_BELOW;
	if (!initial && fast != appliedFast) {
		CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
		Social_AddLine(LINE_NOTICE, fast ? Pick("Faster clock: on", "Horloge accélérée : activée") :
			Pick("Faster clock: off", "Horloge accélérée : désactivée"));
	}
	appliedFast = fast;
	SetClock();
}

void
World_Changed(void)
{
	if (Ready())
		ApplyWorld(false);
}

void
World_BeginProcess(void)
{
	// (a WORLD that came while the world was not being played yet)
	if (Ready() && (World().serial != appliedSerial || CheatBits() != appliedBits || ForcedWeather() != appliedForced))
		ApplyWorld(false);
}

// ---------------------------------------------------------------- the world cheats asked for, world events
static bool
SameCode(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (toupper((uint8)*a) != toupper((uint8)*b))
			return false;
	return *a == *b;
}

// (a world cheat is asked for as the absolute state it gives — the world's state toggled — never as a toggle; the
// server checks the admin list and gives everyone the new state; no link: nothing asked, nothing applied here)
bool
World_CheatRequested(const char *code)
{
	if (!IsActive() || code == nil)
		return false;
	bool send = CanSend();
	double now = Now();
	for (int i = 0; i < (int)ARRAY_SIZE(weatherCheats); i++)
		if (SameCode(code, weatherCheats[i].code)) {
			// (the same weather asked again gives the world its own cycle back)
			int32 w = weatherCheats[i].weather;
			int32 forced = ForcedWeather();
			if (weatherAsked && askedWeather != forced && now - weatherAskedAt < REQUEST_MS)
				forced = askedWeather;
			if (send) {
				askedWeather = forced == w ? WEATHER_RANDOM : w;
				weatherAsked = true;
				weatherAskedAt = now;
				SendVar(WV_FORCED_WEATHER, askedWeather);
			}
			return true;
		}
	if (SameCode(code, "LIFEISPASSINGMEBY")) {
		float rate = World().msPerMinute;
		bool fast = rate > 0.0f && rate < FAST_BELOW;
		if (rateAsked && askedFast != fast && now - rateAskedAt < REQUEST_MS)
			fast = askedFast;
		if (send) {
			askedFast = !fast;
			rateAsked = true;
			rateAskedAt = now;
			Out o(C_WORLD_SET);
			o.U8(3);
			o.F32(askedFast ? FAST_MS_PER_MINUTE : NORMAL_MS_PER_MINUTE);
		}
		return true;
	}
	if (SameCode(code, "BIGBANG")) {
		if (send) {
			Out o(C_WORLD_EVENT);
			o.U8(WE_BIGBANG);
			o.I32(0);
		}
		return true;
	}
	for (int i = 0; i < (int)ARRAY_SIZE(worldCheats); i++)
		if (SameCode(code, worldCheats[i].code)) {
			int32 bits = CheatBits();
			if (cheatsAsked && askedCheats != bits && now - cheatsAskedAt < REQUEST_MS)
				bits = askedCheats;
			int32 bit = worldCheats[i].bit;
			bits ^= bit;
			// (pink or black traffic: one of them)
			if (bit == WC_PINK_CARS && (bits & bit))
				bits &= ~WC_BLACK_CARS;
			if (bit == WC_BLACK_CARS && (bits & bit))
				bits &= ~WC_PINK_CARS;
			if (send) {
				askedCheats = bits;
				cheatsAsked = true;
				cheatsAskedAt = now;
				SendVar(WV_CHEATS, bits);
			}
			return true;
		}
	return false;
}

// (BIGBANG: each client blows up what it simulates — its player's vehicle, its own ambient ones — the others' come
// with their owners' explosions)
static void
BlowUpOwnedVehicles(void)
{
	CHud::SetHelpMessage(TheText.Get("CHEAT1"), true);
	int i = CPools::GetVehiclePool()->GetSize();
	while (i-- > 0) {
		CVehicle *veh = CPools::GetVehiclePool()->GetSlot(i);
		if (veh == nil || IsPuppet(veh) || !OwnedHere(veh))
			continue;
		veh->BlowUpCar(nil);
	}
}

void
World_OnWorldEvent(int32 from, int32 id, int32 arg)
{
	if (!Ready())
		return;
	if (id == WE_BIGBANG)
		BlowUpOwnedVehicles();
}

void
World_OnEvent(int32 from, int32 sub, In &in)
{
	// (no world EVENT yet: the clock, the weather, the cheats and the garages go through the server's state)
}

// ---------------------------------------------------------------- the hideouts' garages
// One record per hideout (key 0x0100 + its index), the 4 stored cars as CGarages::aCarsInSafeHouses[h] holds them.
// The holder of a hideout's lock is the only client that turns its stored cars into vehicles and stores them back
// when its door has closed; the others keep the cars as data and move the door as the holder's.
enum { HIDEOUT_BYTES = sizeof(CStoredCar) * NUM_GARAGE_STORED_CARS, STORED_CAR_BYTES = sizeof(CStoredCar) };
static_assert(sizeof(CStoredCar) == 40, "CStoredCar as the records hold it");
static_assert(sizeof(CGarages::aCarsInSafeHouses[0]) == 160, "a hideout's record");

// (the game's door speeds, Garages.cpp: per 50 Hz step, times the hideouts' coefficient)
static const float DOOR_SPEED_HIDEOUT = 1.7f, DOOR_OPEN = 0.035f, DOOR_OPEN_ROTATED = 0.015f, DOOR_CLOSE = 0.04f, DOOR_CLOSE_ROTATED = 0.02f;
// (a lock asked for and not answered is asked again; a lock given while the player had gone is given back)
static const double LOCK_RETRY_MS = 5000.0, UNUSED_LOCK_MS = 2000.0;

struct Hideout
{
	bool known;         // a RECORD came for it
	uint8 holder;       // the lock's holder (NO_SLOT: free)
	int32 length;       // 0: nothing stored on the server yet (an empty hideout)
	uint8 data[HIDEOUT_BYTES];
	bool dirty;         // data for the game's array, copied in once the garage is closed here
	bool lockSent;
	double lockSentAt;
	double grantedAt;
	bool openedHere;    // its cars handed to the game here (restored, then the door opened): ours until stored
	bool pendingStore;  // stored here, not yet by the server (no link at the door's closing, or not answered yet)
	bool storeSent;
	double storeSentAt;
	uint8 pending[HIDEOUT_BYTES];
	double claimAt;     // the last claim of a car another player left in it
};
static Hideout hideouts[TOTAL_HIDEOUT_GARAGES];

// (the record says this client holds the lock: never while it has no slot, as during a reconnection)
static bool
HeldHere(int32 h)
{
	const Hideout &hd = hideouts[h];
	return LocalSlot() != NO_SLOT && hd.known && hd.holder == LocalSlot();
}

// (the lock asked for, again after LOCK_RETRY_MS if not answered)
static void
AskLock(int32 h)
{
	Hideout &hd = hideouts[h];
	if (!CanSend() || LocalSlot() == NO_SLOT || (hd.lockSent && Now() - hd.lockSentAt < LOCK_RETRY_MS))
		return;
	Out o(C_LOCK);
	o.U16(RECORD_GARAGE + h);
	hd.lockSent = true;
	hd.lockSentAt = Now();
}

static int32
HideoutIndex(const CGarage *garage)
{
	return CGarages::FindSafeHouseIndexForGarageType(garage->m_eGarageType);
}

static CGarage *
FindHideoutGarage(int32 h)
{
	for (uint32 i = 0; i < CGarages::NumGarages && i < NUM_GARAGES; i++)
		if (CGarages::FindSafeHouseIndexForGarageType(CGarages::aGarages[i].m_eGarageType) == h)
			return &CGarages::aGarages[i];
	return nil;
}

static int32
CarsIn(const uint8 *cars)
{
	int32 n = 0;
	for (int i = 0; i < NUM_GARAGE_STORED_CARS; i++) {
		int32 model;
		memcpy(&model, cars + i * STORED_CAR_BYTES, 4);
		n += model != 0;
	}
	return n;
}

// CStoredCar's bytes: i32 model, 3 f32 position, 3 f32 forward vector, i32 flags, i8 colours (2), radio, variations
// (2), bomb, 2 bytes of padding
enum { SC_MODEL = 0, SC_POS = 4, SC_FORWARD = 16, SC_FLAGS = 28, SC_RADIO = 34, SC_VARIATION = 35, SC_BOMB = 37, SC_PAD = 38 };
static const int32 STORED_FLAGS = 0x1F; // (CStoredCar's proofs)

// (records from the server: a slot kept only for a car, bike or boat this game has, inside that hideout as the game
// stores them, its forward vector a unit one; its other bytes within what the game's code reads: the proofs' flags,
// a radio station, variations of the model's 6 components; no bomb, as the bomb shops are closed)
static void
Sanitize(int32 h, uint8 *cars)
{
	CGarage *garage = FindHideoutGarage(h);
	for (int i = 0; i < NUM_GARAGE_STORED_CARS; i++) {
		uint8 *c = cars + i * STORED_CAR_BYTES;
		int32 model, flags;
		float v[6];
		memcpy(&model, c + SC_MODEL, 4);
		memcpy(v, c + SC_POS, sizeof(v));
		bool ok = garage != nil && model > 0 && model < MODELINFOSIZE && CModelInfo::GetModelInfo(model) &&
			(CModelInfo::IsCarModel(model) || CModelInfo::IsBikeModel(model) || CModelInfo::IsBoatModel(model));
		for (int k = 0; k < 6 && ok; k++)
			ok = isfinite(v[k]) && Abs(v[k]) < 10000.0f;
		ok = ok && garage->IsPointInsideGarage(CVector(v[0], v[1], v[2]));
		float length = ok ? Sqrt(SQR(v[3]) + SQR(v[4]) + SQR(v[5])) : 0.0f;
		ok = ok && length > 0.9f && length < 1.1f && Sqrt(SQR(v[3]) + SQR(v[4])) > 0.5f;
		if (!ok) {
			memset(c, 0, STORED_CAR_BYTES);
			continue;
		}
		for (int k = 3; k < 6; k++)
			v[k] /= length;
		memcpy(c + SC_FORWARD, v + 3, 3 * sizeof(float));
		memcpy(&flags, c + SC_FLAGS, 4);
		flags &= STORED_FLAGS;
		memcpy(c + SC_FLAGS, &flags, 4);
		if (c[SC_RADIO] > RADIO_OFF)
			c[SC_RADIO] = RADIO_OFF;
		for (int k = 0; k < 2; k++)
			if ((int8)c[SC_VARIATION + k] < -1 || (int8)c[SC_VARIATION + k] > 5)
				c[SC_VARIATION + k] = (uint8)-1;
		c[SC_BOMB] = 0;
		c[SC_PAD] = c[SC_PAD + 1] = 0;
	}
}

// (records to the server: what the game left unset zeroed: the padding, the bomb, an empty slot)
static void
Normalize(uint8 *cars)
{
	for (int i = 0; i < NUM_GARAGE_STORED_CARS; i++) {
		uint8 *c = cars + i * STORED_CAR_BYTES;
		int32 model;
		memcpy(&model, c + SC_MODEL, 4);
		if (model == 0) {
			memset(c, 0, STORED_CAR_BYTES);
			continue;
		}
		c[SC_PAD] = c[SC_PAD + 1] = 0;
		c[SC_BOMB] = 0;
	}
}

static void
CopyToGame(int32 h)
{
	Hideout &hd = hideouts[h];
	uint8 *cars = (uint8 *)&CGarages::aCarsInSafeHouses[h][0];
	memset(cars, 0, HIDEOUT_BYTES);
	if (hd.length > 0)
		memcpy(cars, hd.data, HIDEOUT_BYTES);
	Sanitize(h, cars);
	hd.dirty = false;
}

// (a record goes into the game's array when the garage is closed here and its cars are not out of it by our hand, nor
// stored here and not yet by the server)
static void
ApplyHideout(int32 h)
{
	Hideout &hd = hideouts[h];
	if (!hd.dirty || hd.openedHere || hd.pendingStore || !Ready())
		return;
	CGarage *garage = FindHideoutGarage(h);
	if (garage == nil || garage->m_eGarageState == GS_FULLYCLOSED)
		CopyToGame(h);
}

static void
SendKey(uint8 type, int32 h)
{
	Out o(type);
	o.U16(RECORD_GARAGE + h);
}

void
World_OnRecord(uint16 key, int32 holder, const uint8 *data, int32 length)
{
	if (key < RECORD_GARAGE || key >= RECORD_GARAGE + TOTAL_HIDEOUT_GARAGES)
		return;
	int32 h = key - RECORD_GARAGE;
	Hideout &hd = hideouts[h];
	bool wasMine = HeldHere(h);
	hd.known = true;
	hd.holder = holder >= 0 && holder < MAX_SLOTS ? (uint8)holder : (uint8)NO_SLOT;
	hd.length = length > 0 ? Min(length, (int32)HIDEOUT_BYTES) : 0;
	memset(hd.data, 0, HIDEOUT_BYTES);
	if (hd.length > 0 && data != nil)
		memcpy(hd.data, data, hd.length);
	hd.lockSent = false;
	if (HeldHere(h) && !wasMine)
		hd.grantedAt = Now();
	if (hd.pendingStore) {
		// (cars stored here: the game's array keeps them until the server has them, our STORE's record, or refused
		// them, our LOCK answered with another holder: then the server's data)
		bool stored = hd.holder == NO_SLOT && hd.length == HIDEOUT_BYTES && memcmp(hd.data, hd.pending, HIDEOUT_BYTES) == 0;
		bool refused = hd.storeSent && hd.holder != NO_SLOT && !HeldHere(h);
		if (!stored && !refused)
			return;
		hd.pendingStore = false;
		hd.storeSent = false;
		if (stored) {
			hd.dirty = false;
			return;
		}
	}
	hd.dirty = !hd.openedHere;
	// (a garage opened here whose lock the server no longer gives us, as after a reconnection: asked for again)
	if (hd.openedHere && hd.holder == NO_SLOT)
		AskLock(h);
	ApplyHideout(h);
}

// (CGarage::Update, the hideouts' case before its states: while another client holds the lock, the door moves as
// theirs and the garage stays closed here — no restore, no store; once free, the door comes down)
bool
HideoutMirrored(CGarage *garage)
{
	if (!Ready())
		return false;
	int32 h = HideoutIndex(garage);
	if (h < 0)
		return false;
	Hideout &hd = hideouts[h];
	if (hd.openedHere || garage->m_eGarageState != GS_FULLYCLOSED)
		return false;
	bool rotated = garage->m_bRotatedDoor;
	if (hd.known && hd.holder != NO_SLOT && !HeldHere(h)) {
		if (garage->m_fDoorPos < garage->m_fDoorHeight) {
			garage->m_fDoorPos = Min(garage->m_fDoorHeight, garage->m_fDoorPos +
				DOOR_SPEED_HIDEOUT * (rotated ? DOOR_OPEN_ROTATED : DOOR_OPEN) * CTimer::GetTimeStep());
			garage->UpdateDoorsHeight();
		}
		return true;
	}
	if (garage->m_fDoorPos > 0.0f) {
		garage->m_fDoorPos = Max(0.0f, garage->m_fDoorPos -
			DOOR_SPEED_HIDEOUT * (rotated ? DOOR_CLOSE_ROTATED : DOOR_CLOSE) * CTimer::GetTimeStep());
		garage->UpdateDoorsHeight();
	}
	return false;
}

// (the closed hideout's player is at its door: its lock asked for once; the stored cars restored and the door opened
// only once the record says we hold it, its cars copied in first)
bool
HideoutMayOpen(CGarage *garage)
{
	if (!Ready())
		return true;
	int32 h = HideoutIndex(garage);
	if (h < 0)
		return true;
	Hideout &hd = hideouts[h];
	if (hd.openedHere)
		return true; // (still restoring: some models were not loaded yet)
	if (hd.pendingStore)
		return false; // (its cars not with the server yet)
	if (HeldHere(h)) {
		CopyToGame(h);
		hd.openedHere = true;
		return true;
	}
	if (hd.known && hd.holder != NO_SLOT)
		return false;
	AskLock(h);
	return false;
}

// (the cars stored here sent: the lock asked for just before, the holder's LOCK accepted again, so that a lock lost
// meanwhile, as at a reconnection, is taken back if free; sent again after LOCK_RETRY_MS while not answered)
static void
SendStore(int32 h)
{
	Hideout &hd = hideouts[h];
	if (!CanSend() || LocalSlot() == NO_SLOT)
		return;
	SendKey(C_LOCK, h);
	{
		Out o(C_STORE);
		o.U16(RECORD_GARAGE + h);
		o.U16(HIDEOUT_BYTES);
		o.Raw(hd.pending, HIDEOUT_BYTES);
	}
	hd.storeSent = true;
	hd.storeSentAt = Now();
}

// (StoreAndRemoveCarsForThisHideout's end: the cars stored here kept until the server has them; no link, they wait for
// it, the game's array as the door left it)
void
HideoutStored(CGarage *garage, CStoredCar *cars)
{
	if (!Ready())
		return;
	int32 h = HideoutIndex(garage);
	if (h < 0)
		return;
	Hideout &hd = hideouts[h];
	if (!hd.openedHere && !HeldHere(h))
		return;
	hd.openedHere = false;
	hd.lockSent = false;
	uint8 *bytes = (uint8 *)cars;
	Normalize(bytes);
	memcpy(hd.pending, bytes, HIDEOUT_BYTES);
	hd.pendingStore = true;
	hd.storeSent = false;
	hd.dirty = false;
	SendStore(h);
}

// (an undriven copy of another client's vehicle inside a garage opened here, a car another player left in it: claimed,
// so that it is ours when the door closes and stored with the others; true while there is one)
static const double CLAIM_RETRY_MS = 1000.0;

static bool
ClaimParked(int32 h, CGarage *garage)
{
	Hideout &hd = hideouts[h];
	bool claim = CanSend() && Now() - hd.claimAt > CLAIM_RETRY_MS;
	bool any = false;
	int i = CPools::GetVehiclePool()->GetSize();
	while (i-- > 0) {
		CVehicle *veh = CPools::GetVehiclePool()->GetSlot(i);
		if (veh == nil || !IsPuppet(veh) || veh->pDriver != nil || veh->VehicleCreatedBy == MISSION_VEHICLE ||
		    !garage->IsPointInsideGarage(veh->GetPosition()))
			continue;
		NetEntity *ne = Find(veh);
		if (ne == nil || (ne->flags & (EF_DRIVEN | EF_MISSION)))
			continue;
		any = true;
		if (claim) {
			Out o(C_OWN);
			o.U32(ne->netid);
			o.U8(1);
			o.U8(LocalSlot());
		}
	}
	if (any && claim)
		hd.claimAt = Now();
	return any;
}

bool
GarageLeaveAlone(CVehicle *veh)
{
	return veh != nil && IsActive() && IsPuppet(veh);
}

// (the holder's door stays open while another player is in the garage or at its door, or a car another player left in
// it is not ours yet)
bool
HideoutKeepOpen(CGarage *garage)
{
	if (!Ready())
		return false;
	int32 h = HideoutIndex(garage);
	if (h >= 0 && hideouts[h].openedHere && ClaimParked(h, garage))
		return true;
	for (int32 slot = 0; slot < MAX_SLOTS; slot++) {
		RemotePlayer *p = Player(slot);
		if (p == nil || !p->present || p->puppet == nil)
			continue;
		CPed *ped = p->puppet;
		if (garage->IsEntityTouching3D(ped))
			return true;
		if (ped->bInVehicle && ped->m_pMyVehicle && garage->IsEntityTouching3D(ped->m_pMyVehicle))
			return true;
	}
	return false;
}

void
World_PreWorld(void)
{
	if (!Ready())
		return;
	for (int32 h = 0; h < TOTAL_HIDEOUT_GARAGES; h++) {
		Hideout &hd = hideouts[h];
		ApplyHideout(h);
		// (cars stored here and not by the server yet: sent once linked, again while not answered)
		if (hd.pendingStore && (!hd.storeSent || Now() - hd.storeSentAt > LOCK_RETRY_MS))
			SendStore(h);
		if (hd.openedHere) {
			// (its cars out by our hand and its lock not ours, as after a reconnection: asked for again while free)
			if (!HeldHere(h) && (!hd.known || hd.holder == NO_SLOT))
				AskLock(h);
			// (a car another player parked in it claimed as soon as it stands there)
			CGarage *garage = FindHideoutGarage(h);
			if (garage && garage->m_eGarageState != GS_FULLYCLOSED)
				ClaimParked(h, garage);
		}
		// (a lock given that the garage did not use — the player gone, or the garage full for the car — goes back)
		if (HeldHere(h) && !hd.openedHere && !hd.pendingStore && Now() - hd.grantedAt > UNUSED_LOCK_MS && CanSend()) {
			SendKey(C_UNLOCK, h);
			hd.holder = NO_SLOT;
			hd.lockSent = false;
		}
	}
}

// ---------------------------------------------------------------- the world loaded, left
// (the bomb shops closed for business: a car's bomb and its remote detonator cannot work across clients; the door left
// as it is, nobody shut in)
static void
CloseBombShops(void)
{
	for (uint32 i = 0; i < CGarages::NumGarages && i < NUM_GARAGES; i++) {
		CGarage &g = CGarages::aGarages[i];
		if (g.m_eGarageType < GARAGE_BOMBSHOP1 || g.m_eGarageType > GARAGE_BOMBSHOP3)
			continue;
		CGarages::ChangeGarageType(i, GARAGE_FOR_SCRIPT_TO_OPEN_AND_CLOSE, 0);
		g.m_eGarageState = g.m_fDoorPos > 0.0f ? GS_OPENED : GS_FULLYCLOSED;
	}
}

void
World_GameLoaded(void)
{
	// (a shared world's load only: the story's and the free mode's games keep their own garages and bomb shops)
	if (Mode() != MODE_JOINING && Mode() != MODE_ACTIVE && !IsActive() && !IsEntering())
		return;
	bLoaded = true;
	// (as the load left them: the cheats' flags reset by it, the threats and the player's gang read from it)
	for (int i = 0; i < NUM_PEDTYPES; i++)
		baseThreats[i] = CPedType::GetThreats(i);
	threatsChanged = false;
	baseGangModel1 = CGangs::GetGangPedModel1(GANG_PLAYER);
	baseGangModel2 = CGangs::GetGangPedModel2(GANG_PLAYER);
	baseGangWeapon1 = CGangs::GetGangInfo(GANG_PLAYER)->m_Weapon1;
	baseGangWeapon2 = CGangs::GetGangInfo(GANG_PLAYER)->m_Weapon2;
	chicksApplied = false;
	appliedBits = 0;
	appliedForced = WEATHER_RANDOM;
	// (big wheels left on by a world not left through World_Reset: the handling put back before the world's state)
	BigWheels(false);
	cheatsAsked = weatherAsked = rateAsked = false;
	// (the 12 hideouts as the server has them: no record, an empty hideout)
	for (int32 h = 0; h < TOTAL_HIDEOUT_GARAGES; h++) {
		Hideout &hd = hideouts[h];
		hd.openedHere = false;
		hd.lockSent = false;
		hd.pendingStore = false;
		hd.storeSent = false;
		CopyToGame(h);
	}
	CloseBombShops();
	ApplyWorld(true);
}

void
World_LinkReset(void)
{
	// (the server's locks are gone with the link: known again from the records it sends after WELCOME; cars restored
	// here stay ours to store, cars stored here wait to be sent, until the world is loaded again)
	for (int32 h = 0; h < TOTAL_HIDEOUT_GARAGES; h++) {
		Hideout &hd = hideouts[h];
		hd.known = false;
		hd.holder = NO_SLOT;
		hd.lockSent = false;
		hd.storeSent = false;
		hd.dirty = false;
	}
}

void
World_Reset(void)
{
	// (the world left: its locks as at a reconnection, the handling of its big wheels put back for the next game)
	World_LinkReset();
	for (int32 h = 0; h < TOTAL_HIDEOUT_GARAGES; h++)
		hideouts[h].pendingStore = false;
	BigWheels(false);
	cheatsAsked = weatherAsked = rateAsked = false;
}

// ---------------------------------------------------------------- tests
static void
TellHideout(int32 h)
{
	char line[160];
	Hideout &hd = hideouts[h];
	CGarage *garage = bLoaded ? FindHideoutGarage(h) : nil;
	snprintf(line, sizeof(line), "garage %d: %s holder %d, record %d cars (%d bytes), here %d cars, state %d, door %.2f%s%s",
		h, hd.known ? "known" : "unknown", hd.holder == NO_SLOT ? -1 : hd.holder, CarsIn(hd.data), hd.length,
		bLoaded ? CarsIn((uint8 *)&CGarages::aCarsInSafeHouses[h][0]) : 0, garage ? garage->m_eGarageState : -1,
		garage ? garage->m_fDoorPos : 0.0f, hd.openedHere ? ", opened here" : "", hd.pendingStore ? ", store pending" : "");
	Social_AddLine(LINE_NOTICE, line);
}

bool
World_TestCommand(const char *cmd)
{
	int a, b;
	char code[40];
	// (admins' requests, as the cheats': the server refuses them to the others — DENIED)
	if (sscanf(cmd, "time %d %d", &a, &b) == 2) {
		if (a < 0 || a > 23 || b < 0 || b > 59 || !CanSend())
			return true;
		Out o(C_WORLD_SET);
		o.U8(2);
		o.U16(a * 60 + b);
		return true;
	}
	if (sscanf(cmd, "weather %d", &a) == 1) {
		if (CanSend())
			SendVar(WV_FORCED_WEATHER, a >= 0 && a < WEATHER_TOTAL ? a : WEATHER_RANDOM);
		return true;
	}
	if (sscanf(cmd, "worldcheat %39s", code) == 1) {
		if (!World_CheatRequested(code))
			Social_AddLine(LINE_ERROR, "worldcheat: not a world cheat (or no shared world)");
		return true;
	}
	if (sscanf(cmd, "garage %d", &a) == 1) {
		if (a >= 0 && a < TOTAL_HIDEOUT_GARAGES)
			TellHideout(a);
		return true;
	}
	return false;
}

void
World_Debug(void)
{
	DebugAppend(",\"world\":{\"loaded\":%s,\"clock\":\"%02d:%02d\",\"day\":%d,\"rate\":%.1f,"
		"\"weather\":{\"old\":%d,\"new\":%d,\"forced\":%d,\"var\":%d,\"inList\":%d},\"cheats\":%d,\"applied\":%d,\"garages\":[",
		bLoaded ? "true" : "false", CClock::GetHours(), CClock::GetMinutes(), CStats::DaysPassed, World().msPerMinute,
		CWeather::OldWeatherType, CWeather::NewWeatherType, CWeather::ForcedWeatherType, ForcedWeather(),
		CWeather::WeatherTypeInList, CheatBits(), appliedBits);
	for (int32 h = 0; h < TOTAL_HIDEOUT_GARAGES; h++) {
		Hideout &hd = hideouts[h];
		CGarage *garage = bLoaded ? FindHideoutGarage(h) : nil;
		DebugAppend("%s{\"h\":%d,\"holder\":%d,\"record\":%d,\"here\":%d,\"state\":%d,\"opened\":%s,\"pending\":%s}", h ? "," : "",
			h, hd.known ? hd.holder : -2, CarsIn(hd.data), bLoaded ? CarsIn((uint8 *)&CGarages::aCarsInSafeHouses[h][0]) : 0,
			garage ? garage->m_eGarageState : -1, hd.openedHere ? "true" : "false", hd.pendingStore ? "true" : "false");
	}
	DebugAppend("]}");
}
}
#endif
